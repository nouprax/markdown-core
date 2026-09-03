import type { Document } from "../model/document.js";
import { ParseError } from "../parse-error.js";
import type { ParseOptions } from "../parse-options.js";
import { NodeDecoder, transferHeaderSize } from "../wire/node-decoder.js";
import { native, type NativeExports } from "./native.js";

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
    return parseDocumentWithNative(native, source, parseOptions);
}

/** Internal dependency boundary used to verify terminal native failures. */
export function parseDocumentWithNative(
    nativeExports: NativeExports,
    source: string,
    parseOptions: ParseOptions = {}
): Document {
    validateInput(source, parseOptions);
    const bytes = utf8Encoder.encode(source);
    let sourcePointer = 0;
    let resultPointer = 0;
    try {
        sourcePointer = allocate(nativeExports, Math.max(bytes.length, 1));
        new Uint8Array(nativeExports.memory.buffer, sourcePointer, bytes.length).set(bytes);
        resultPointer = nativeExports.es_parse(sourcePointer, bytes.length, optionsMask(parseOptions));
        if (!resultPointer) throw new ParseError("allocationFailed", "failed to allocate native AST result");

        // es_parse may grow memory, which detaches every pre-call view. Take a
        // fresh header view, validate its size against the current heap, then
        // decode in place without another Wasm call. No view escapes this try.
        const memorySize = nativeExports.memory.buffer.byteLength;
        if (resultPointer > memorySize - transferHeaderSize) {
            throw new Error("native result header lies outside WebAssembly memory");
        }
        const totalSize = new DataView(nativeExports.memory.buffer).getUint32(resultPointer + 4, true);
        if (totalSize < transferHeaderSize || totalSize > memorySize - resultPointer) {
            throw new Error("native result lies outside WebAssembly memory");
        }
        return new NodeDecoder(new Uint8Array(nativeExports.memory.buffer, resultPointer, totalSize)).decodeDocument();
    } finally {
        if (resultPointer) nativeExports.es_result_free(resultPointer);
        if (sourcePointer) nativeExports.free(sourcePointer);
    }
}

function validateInput(source: string, parseOptions: ParseOptions): void {
    if (typeof source !== "string") throw new TypeError("source must be a string");
    if (parseOptions === null || typeof parseOptions !== "object") {
        throw new TypeError("options must be an object");
    }
}

function optionsMask(parseOptions: ParseOptions): number {
    let flags = 0;
    for (const option of options) {
        const value = Object.hasOwn(parseOptions, option.name) ? parseOptions[option.name] : option.defaultValue;
        if (typeof value !== "boolean") throw new TypeError(`${option.name} must be a boolean`);
        if (value) flags |= option.mask;
    }
    return flags;
}

function allocate(nativeExports: NativeExports, size: number): number {
    const pointer = nativeExports.malloc(size);
    if (!pointer) throw new ParseError("allocationFailed", "failed to allocate WASM memory");
    return pointer;
}
