import { ParseError } from "../parse-error.js";
import type { ParseOptions } from "../parse-options.js";
import type { Scope } from "../values.js";
import { NodeDecoder } from "../wire/node-decoder.js";
import { native } from "./native.js";

interface OptionDescriptor {
    readonly name: keyof ParseOptions;
    readonly defaultValue: boolean;
    readonly mask: number;
}

const optionDescriptors = [
    { name: "smartPunctuation", defaultValue: true, mask: 1 << 0 },
    { name: "footnotes", defaultValue: true, mask: 1 << 1 },
    { name: "tables", defaultValue: true, mask: 1 << 2 },
    { name: "strikethrough", defaultValue: true, mask: 1 << 3 },
    { name: "autolinks", defaultValue: true, mask: 1 << 4 },
    { name: "taskLists", defaultValue: true, mask: 1 << 5 },
    { name: "formulas", defaultValue: true, mask: 1 << 6 },
    { name: "directives", defaultValue: true, mask: 1 << 7 },
    { name: "crossLinks", defaultValue: true, mask: 1 << 8 },
    { name: "embeds", defaultValue: true, mask: 1 << 9 }
] as const satisfies readonly OptionDescriptor[];

const utf8Encoder = new TextEncoder();

/**
 * One decoder for the module, not one per document.
 *
 * Its whole state is a 32-byte scratch block in WASM memory and a cached
 * DataView, and every call through it is synchronous, so a second one would
 * buy nothing and cost a malloc/free pair per document — including per edit
 * of a streaming document.
 */
export const decoder: NodeDecoder = new NodeDecoder(native);

/** One delta row: which node, and which parts of it differ. */
export interface RawDiff {
    readonly rawValue: number;
    readonly parts: number;
}

export interface RawDelta {
    readonly beforeRevision: number;
    readonly afterRevision: number;
    readonly diffs: readonly RawDiff[];
}

export interface RawDiagnostic {
    readonly code: number;
    readonly scope: Scope;
}

/** Normalizes one caller's options into the frozen record a document reports
 * as its own, plus the flag word the engine takes. */
export function normalizeOptions(parseOptions: ParseOptions): {
    readonly options: Readonly<Required<ParseOptions>>;
    readonly flags: number;
} {
    if (parseOptions === null || typeof parseOptions !== "object") {
        throw new TypeError("options must be an object");
    }
    const normalized: Partial<Record<keyof ParseOptions, boolean>> = {};
    let flags = 0;
    for (const option of optionDescriptors) {
        const value = Object.hasOwn(parseOptions, option.name) ? parseOptions[option.name] : option.defaultValue;
        if (typeof value !== "boolean") throw new TypeError(`${option.name} must be a boolean`);
        normalized[option.name] = value;
        if (value) flags |= option.mask;
    }
    return { options: normalized as Required<ParseOptions>, flags };
}

/** Copies `source` into WASM memory, runs `body` over it, and frees the copy
 * — the one place a JavaScript string crosses the boundary. */
function withSource<Result>(source: string, body: (bytes: number, length: number) => Result): Result {
    if (typeof source !== "string") throw new TypeError("markdown must be a string");
    const bytes = utf8Encoder.encode(source);
    const buffer = native.malloc(Math.max(bytes.length, 1));
    if (!buffer) throw new ParseError("allocationFailed", "failed to allocate WASM memory");
    try {
        new Uint8Array(native.memory.buffer, buffer, bytes.length).set(bytes);
        return body(buffer, bytes.length);
    } finally {
        native.free(buffer);
    }
}

function takeError(error: number): ParseError {
    const decoded = decoder.parseError(error);
    if (error) native.es_error_free(error);
    return decoded;
}

/**
 * The native document boundary: owns one `markdown_core_document` and
 * nothing else. All policy — the value tree, identity interning, the error
 * taxonomy beyond native failures — lives above.
 */
export class CDocument {
    private pointer: number;

    private constructor(pointer: number) {
        this.pointer = pointer;
    }

    /** Parses `source`; throws the native rejection. */
    static open(source: string, flags: number): CDocument {
        const scratch = decoder.scratchPointer;
        return new CDocument(
            withSource(source, (bytes, length) => {
                decoder.dataView().setUint32(scratch, 0, true);
                const document = native.es_document_open(bytes, length, flags, scratch);
                if (!document) throw takeError(decoder.dataView().getUint32(scratch, true));
                return document;
            })
        );
    }

    /**
     * Hands this document new text and returns the successor together with
     * the caller-owned delta pointer (release with `deltaFree`).
     *
     * CONSUMES the receiver on every path, success or not: the native edit
     * clears it either way, so this instance stops owning anything the
     * moment the call is made.
     */
    edit(source: string): { readonly document: CDocument; readonly delta: number } {
        const owned = this.requirePointer();
        this.pointer = 0;
        const scratch = decoder.scratchPointer;
        const commit = withSource(source, (bytes, length) => {
            const view = decoder.dataView();
            view.setUint32(scratch, 0, true);
            view.setUint32(scratch + 4, 0, true);
            view.setUint32(scratch + 8, 0, true);
            if (!native.es_document_edit(owned, bytes, length, scratch, scratch + 8)) {
                throw takeError(decoder.dataView().getUint32(scratch + 8, true));
            }
            const after = decoder.dataView();
            return { document: after.getUint32(scratch, true), delta: after.getUint32(scratch + 4, true) };
        });
        if (!commit.document || !commit.delta) {
            throw new Error("the native edit succeeded without a document and a delta");
        }
        return { document: new CDocument(commit.document), delta: commit.delta };
    }

    rootPointer(): number {
        const root = native.es_document_root(this.requirePointer());
        if (!root) throw new Error("the native document has no root");
        return root;
    }

    diagnostics(): readonly RawDiagnostic[] {
        const document = this.requirePointer();
        const scratch = decoder.scratchPointer;
        decoder.dataView().setUint32(scratch, 0, true);
        const count = native.es_document_diagnostics(document, scratch);
        if (!Number.isSafeInteger(count) || count < 0) {
            throw new Error(`the native document returned diagnostic count ${count}`);
        }
        const view = decoder.dataView();
        const data = view.getUint32(scratch, true);
        if (!data) {
            if (count !== 0) throw new Error("the native document returned an invalid diagnostic array");
            return [];
        }
        const rowBytes = 20;
        if (count * rowBytes > view.byteLength - data) {
            throw new Error("the native document returned an out-of-bounds diagnostic array");
        }
        const rows: RawDiagnostic[] = [];
        for (let index = 0; index < count; index += 1) {
            const row = data + index * rowBytes;
            rows.push({
                code: view.getInt32(row, true),
                scope: {
                    start: { line: view.getInt32(row + 4, true), column: view.getInt32(row + 8, true) },
                    end: { line: view.getInt32(row + 12, true), column: view.getInt32(row + 16, true) }
                }
            });
        }
        return rows;
    }

    series(): bigint {
        // The WASM boundary delivers i64 as a signed bigint; the salt is an
        // unsigned 64-bit value.
        return BigInt.asUintN(64, native.es_document_series(this.requirePointer()));
    }

    /** Reads one delta's rows, in the order the facade defines. */
    readDelta(delta: number): RawDelta {
        const scratch = decoder.scratchPointer;
        const revisions = {
            beforeRevision: decoder.toSafeNumber(native.es_delta_revision(delta, 0), "delta revision"),
            afterRevision: decoder.toSafeNumber(native.es_delta_revision(delta, 1), "delta revision")
        };
        decoder.dataView().setUint32(scratch, 0, true);
        const count = native.es_delta_diffs(delta, scratch);
        if (!Number.isSafeInteger(count) || count < 0) {
            throw new Error(`the native delta returned row count ${count}`);
        }
        const view = decoder.dataView();
        const data = view.getUint32(scratch, true);
        if (!data) {
            if (count !== 0) throw new Error("the native delta returned an invalid row array");
            return { ...revisions, diffs: [] };
        }
        const rowBytes = 16;
        if (count * rowBytes > view.byteLength - data) {
            throw new Error("the native delta returned an out-of-bounds row array");
        }
        const diffs: RawDiff[] = [];
        for (let index = 0; index < count; index += 1) {
            const row = data + index * rowBytes;
            diffs.push({
                rawValue: decoder.toSafeNumber(view.getBigUint64(row, true), "node id"),
                parts: view.getUint32(row + 8, true)
            });
        }
        return { ...revisions, diffs };
    }

    deltaFree(delta: number): void {
        native.es_delta_free(delta);
    }

    /** Releases the native parse. Idempotent, and a no-op once `edit` has
     * handed the parse to a successor. */
    free(): void {
        if (!this.pointer) return;
        native.es_document_free(this.pointer);
        this.pointer = 0;
    }

    get released(): boolean {
        return this.pointer === 0;
    }

    private requirePointer(): number {
        if (!this.pointer) throw new Error("the native document has been released");
        return this.pointer;
    }
}
