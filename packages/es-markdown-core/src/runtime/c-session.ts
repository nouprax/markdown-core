import { ParseError } from "../parse-error.js";
import type { ParseOptions } from "../parse-options.js";
import type { ScopeEntry } from "../session/scope-resolver.js";
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
    { name: "stripHTMLComments", defaultValue: true, mask: 1 << 2 },
    { name: "tables", defaultValue: true, mask: 1 << 3 },
    { name: "strikethrough", defaultValue: true, mask: 1 << 4 },
    { name: "autolinks", defaultValue: true, mask: 1 << 5 },
    { name: "taskLists", defaultValue: true, mask: 1 << 6 },
    { name: "formulas", defaultValue: true, mask: 1 << 7 },
    { name: "dollarFormulaDelimiters", defaultValue: true, mask: 1 << 8 },
    { name: "latexFormulaDelimiters", defaultValue: true, mask: 1 << 9 },
    { name: "directives", defaultValue: true, mask: 1 << 10 }
] as const satisfies readonly OptionDescriptor[];

const utf8Encoder = new TextEncoder();

/** One session commit's raw identity sets, before interning. */
export interface RawDelta {
    readonly beforeRevision: number;
    readonly afterRevision: number;
    readonly added: readonly number[];
    readonly removed: readonly number[];
    readonly changed: readonly number[];
    readonly bubbled: readonly number[];
}

export interface RawFootnoteInfo {
    readonly definition: number;
    readonly number: number;
    readonly referenceOrdinal: number;
    readonly referenceCount: number;
}

/**
 * The native session boundary: owns one `markdown_core_session`, the decoder
 * (and its per-session scratch) that reads from it, and nothing else. All
 * policy — mirrors, snapshots, resolvers, error taxonomy beyond native
 * failures — lives above, in `MarkupSession` and the one-shot parse.
 */
export class CSession {
    readonly options: Readonly<Required<ParseOptions>>;
    readonly decoder: NodeDecoder;
    private pointer: number;

    constructor(parseOptions: ParseOptions) {
        const normalized: Partial<Record<keyof ParseOptions, boolean>> = {};
        let flags = 0;
        for (const option of optionDescriptors) {
            const value = Object.hasOwn(parseOptions, option.name) ? parseOptions[option.name] : option.defaultValue;
            if (typeof value !== "boolean") throw new TypeError(`${option.name} must be a boolean`);
            normalized[option.name] = value;
            if (value) flags |= option.mask;
        }
        this.options = normalized as Required<ParseOptions>;
        this.decoder = new NodeDecoder(native);
        try {
            const scratch = this.decoder.scratchPointer;
            this.decoder.dataView().setUint32(scratch, 0, true);
            this.pointer = native.es_session_open(flags, scratch);
            if (!this.pointer) throw this.takeError(this.decoder.dataView().getUint32(scratch, true));
        } catch (failure) {
            this.decoder.dispose();
            throw failure;
        }
    }

    /** Queues the replacement of stored-text bytes `[byteStart, byteEnd)`
     * with `replacement`'s UTF-8 bytes. Throws the native rejection; the
     * session stays usable. */
    edit(byteStart: number, byteEnd: number, replacement: string): void {
        const session = this.requirePointer();
        const bytes = utf8Encoder.encode(replacement);
        const buffer = native.malloc(Math.max(bytes.length, 1));
        if (!buffer) throw new ParseError("allocationFailed", "failed to allocate WASM memory");
        try {
            new Uint8Array(native.memory.buffer, buffer, bytes.length).set(bytes);
            const scratch = this.decoder.scratchPointer;
            this.decoder.dataView().setUint32(scratch, 0, true);
            if (!native.es_session_edit(session, byteStart, byteEnd, buffer, bytes.length, scratch)) {
                throw this.takeError(this.decoder.dataView().getUint32(scratch, true));
            }
        } finally {
            native.free(buffer);
        }
    }

    /**
     * Commits the pending text. A `ParseError` is the native transactional
     * failure: the tree is unchanged at the previous revision and the commit
     * may be retried. On success returns the caller-owned delta pointer
     * (release with `deltaFree`), or 0 when `withChanges` declined it.
     */
    commit(withChanges: boolean): number {
        const session = this.requirePointer();
        const scratch = this.decoder.scratchPointer;
        const view = this.decoder.dataView();
        view.setUint32(scratch, 0, true);
        view.setUint32(scratch + 4, 0, true);
        if (!native.es_session_commit(session, withChanges ? scratch : 0, scratch + 4)) {
            throw this.takeError(this.decoder.dataView().getUint32(scratch + 4, true));
        }
        if (!withChanges) return 0;
        const changes = this.decoder.dataView().getUint32(scratch, true);
        if (!changes) throw new Error("native commit succeeded without producing a delta");
        return changes;
    }

    readDelta(changes: number): RawDelta {
        return {
            beforeRevision: this.decoder.toSafeNumber(native.es_delta_revision(changes, 0), "delta revision"),
            afterRevision: this.decoder.toSafeNumber(native.es_delta_revision(changes, 1), "delta revision"),
            added: this.readIds((output) => native.es_delta_ids(changes, 0, output)),
            removed: this.readIds((output) => native.es_delta_ids(changes, 1, output)),
            changed: this.readIds((output) => native.es_delta_ids(changes, 2, output)),
            bubbled: this.readIds((output) => native.es_delta_ids(changes, 3, output))
        };
    }

    deltaFree(changes: number): void {
        native.es_delta_free(changes);
    }

    rootPointer(): number {
        const root = native.es_document_root(native.es_session_document(this.requirePointer()));
        if (!root) throw new Error("native session has no committed document root");
        return root;
    }

    rootIdentity(): { readonly rawValue: number; readonly revision: number } {
        const root = this.rootPointer();
        return {
            rawValue: this.decoder.toSafeNumber(native.es_node_id(root), "node id"),
            revision: this.decoder.toSafeNumber(native.es_node_revision(root), "node revision")
        };
    }

    scopeTable(): Map<number, ScopeEntry> {
        return this.decoder.scopeTable(this.rootPointer());
    }

    lineage(): bigint {
        // The WASM boundary delivers i64 as a signed bigint; the salt is an
        // unsigned 64-bit value.
        return BigInt.asUintN(64, native.es_session_lineage(this.requirePointer()));
    }

    revision(): number {
        return this.decoder.toSafeNumber(native.es_session_revision(this.requirePointer()), "session revision");
    }

    length(): number {
        return native.es_session_length(this.requirePointer());
    }

    footnoteInfo(rawValue: number): RawFootnoteInfo | null {
        const session = this.requirePointer();
        const scratch = this.decoder.scratchPointer;
        if (!native.es_session_footnote_info(session, BigInt(rawValue), scratch)) return null;
        const view = this.decoder.dataView();
        const field = (slot: number): number =>
            this.decoder.toSafeNumber(view.getBigUint64(scratch + slot * 8, true), "footnote answer");
        return {
            definition: field(0),
            number: field(1),
            referenceOrdinal: field(2),
            referenceCount: field(3)
        };
    }

    footnotes(): readonly number[] {
        const session = this.requirePointer();
        return this.readIds((output) => native.es_session_footnotes(session, output));
    }

    footnoteReferences(definition: number): readonly number[] {
        const session = this.requirePointer();
        return this.readIds((output) => native.es_session_footnote_references(session, BigInt(definition), output));
    }

    free(): void {
        if (!this.pointer) return;
        native.es_session_free(this.pointer);
        this.pointer = 0;
        this.decoder.dispose();
    }

    private readIds(fill: (output: number) => number): readonly number[] {
        const scratch = this.decoder.scratchPointer;
        this.decoder.dataView().setUint32(scratch, 0, true);
        const count = fill(scratch);
        if (!Number.isSafeInteger(count) || count < 0) throw new Error(`native session returned id count ${count}`);
        const view = this.decoder.dataView();
        const data = view.getUint32(scratch, true);
        if (!data) {
            if (count !== 0) throw new Error("native session returned an invalid id array");
            return [];
        }
        const ids: number[] = [];
        for (let index = 0; index < count; index += 1) {
            ids.push(this.decoder.toSafeNumber(view.getBigUint64(data + index * 8, true), "node id"));
        }
        return ids;
    }

    private takeError(error: number): ParseError {
        const decoded = this.decoder.parseError(error);
        if (error) native.es_error_free(error);
        return decoded;
    }

    private requirePointer(): number {
        if (!this.pointer) throw new Error("the native session has been released");
        return this.pointer;
    }
}
