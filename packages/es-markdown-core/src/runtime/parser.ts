import { Concrete } from "../concrete.js";
import type { Document } from "../model/document.js";
import { ParseError } from "../parse-error.js";
import type { ParseOptions } from "../parse-options.js";
import { NodeDecoder } from "../wire/node-decoder.js";
import { native } from "./native.js";

interface OptionDescriptor {
    readonly name: keyof ParseOptions;
    readonly defaultValue: boolean;
    readonly mask: number;
}

const options = [
    { name: "smartPunctuation", defaultValue: true, mask: 1 << 0 },
    { name: "footnotes", defaultValue: true, mask: 1 << 1 },
    { name: "stripHTMLComments", defaultValue: true, mask: 1 << 2 },
    { name: "tables", defaultValue: true, mask: 1 << 3 },
    { name: "strikethrough", defaultValue: true, mask: 1 << 4 },
    { name: "autolinks", defaultValue: true, mask: 1 << 5 },
    { name: "taskLists", defaultValue: true, mask: 1 << 6 },
    { name: "formulas", defaultValue: true, mask: 1 << 7 },
    { name: "directives", defaultValue: true, mask: 1 << 8 }
] as const satisfies readonly OptionDescriptor[];

const utf8Encoder = new TextEncoder();

export function parseDocument(source: string, parseOptions: ParseOptions = {}): Document {
    if (typeof source !== "string") throw new TypeError("source must be a string");
    const flags = optionsMask(parseOptions);
    const bytes = utf8Encoder.encode(source);
    return withHeapBytes(bytes, (sourcePointer) =>
        copyOut((errorOutput) => native.es_document_parse(sourcePointer, bytes.length, flags, errorOutput))
    );
}

/**
 * Copies `bytes` into WASM memory for the duration of `action`, and frees the
 * copy on the way out. The pointer is valid for exactly `bytes.length` bytes
 * (one byte is still reserved for an empty input, because `malloc(0)` may
 * answer 0 and 0 is this boundary's failure value).
 */
export function withHeapBytes<Result>(bytes: Uint8Array, action: (pointer: number) => Result): Result {
    const pointer = allocate(Math.max(bytes.length, 1));
    try {
        new Uint8Array(native.memory.buffer, pointer, bytes.length).set(bytes);
        return action(pointer);
    } finally {
        native.free(pointer);
    }
}

/**
 * THE ONE WAY A DOCUMENT LEAVES WASM. `invoke` runs a native call that
 * answers with a document pointer or writes an error behind the given output
 * slot -- the one-shot parse, a session's `feed`, and its `finish` all answer
 * that way -- and this copies the document out as a value or throws the
 * `ParseError` behind the slot. Every native handle is released before it
 * returns, so a streamed document borrows nothing, exactly as a parsed one
 * does.
 */
export function copyOut(invoke: (errorOutput: number) => number): Document {
    const errorOutput = allocate(Uint32Array.BYTES_PER_ELEMENT);
    let documentPointer = 0;
    let errorPointer = 0;
    try {
        dataView().setUint32(errorOutput, 0, true);
        documentPointer = invoke(errorOutput);
        errorPointer = dataView().getUint32(errorOutput, true);

        const decoder = new NodeDecoder(native);
        try {
            if (!documentPointer) throw decoder.parseError(errorPointer);
            return withConcrete(
                decoder.decodeDocument(native.es_document_root(documentPointer)),
                readConcrete(documentPointer)
            );
        } finally {
            decoder.dispose();
        }
    } finally {
        if (documentPointer) native.es_document_free(documentPointer);
        if (errorPointer) native.es_error_free(errorPointer);
        native.free(errorOutput);
    }
}

/**
 * Copies the whole concrete view out of WASM memory, which the caller frees as
 * soon as this returns.
 *
 * Every array is read out in ONE crossing: the view is copied whole either way,
 * and a call per line or per region is tens of thousands of them on a document
 * of any size.
 */
/**
 * The root, given the source its scopes are counted against.
 *
 * Defined ON the decoded root rather than spread into a new object: `dump` is
 * not enumerable, and a spread would leave it behind. `concrete` is data and
 * enumerates.
 */
function withConcrete(root: Omit<Document, "concrete">, concrete: Concrete): Document {
    const document = root as Document;
    Object.defineProperty(document, "concrete", { enumerable: true, value: concrete });
    return document;
}

function readConcrete(documentPointer: number): Concrete {
    const sourceOutput = allocate(Uint32Array.BYTES_PER_ELEMENT * 2);
    let lineOutput = 0;
    try {
        native.es_document_source(documentPointer, sourceOutput, sourceOutput + 4);
        const sourcePointer = dataView().getUint32(sourceOutput, true);
        const sourceLength = dataView().getUint32(sourceOutput + 4, true);
        const source = new Uint8Array(native.memory.buffer, sourcePointer, sourceLength).slice();

        const lineCount = native.es_document_line_count(documentPointer);
        lineOutput = allocate(Math.max(lineCount, 1) * Uint32Array.BYTES_PER_ELEMENT);
        native.es_document_line_starts(documentPointer, lineOutput);
        const lineStarts = new Uint32Array(native.memory.buffer, lineOutput, lineCount).slice();

        return new Concrete(source, lineStarts);
    } finally {
        for (const pointer of [lineOutput, sourceOutput]) {
            if (pointer) native.free(pointer);
        }
    }
}

/** The option flags the C bridge reads, validated on the way: the one
 * checking and encoding of `ParseOptions`, whether a parse or a session is
 * about to read them. */
export function optionsMask(parseOptions: ParseOptions): number {
    if (parseOptions === null || typeof parseOptions !== "object") {
        throw new TypeError("options must be an object");
    }
    let flags = 0;
    for (const option of options) {
        const value = Object.hasOwn(parseOptions, option.name) ? parseOptions[option.name] : option.defaultValue;
        if (typeof value !== "boolean") throw new TypeError(`${option.name} must be a boolean`);
        if (value) flags |= option.mask;
    }
    return flags;
}

function allocate(size: number): number {
    const pointer = native.malloc(size);
    if (!pointer) throw new ParseError("allocationFailed", "failed to allocate WASM memory");
    return pointer;
}

function dataView(): DataView {
    return new DataView(native.memory.buffer);
}
