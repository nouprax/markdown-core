import type { Document } from "../model/document.js";
import { ParseError } from "../parse-error.js";
import type { ParseOptions } from "../parse-options.js";
import { NodeDecoder } from "../wire/node-decoder.js";
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
    let errorOutput = 0;
    let documentPointer = 0;
    let errorPointer = 0;
    try {
        sourcePointer = allocate(nativeExports, Math.max(bytes.length, 1));
        errorOutput = allocate(nativeExports, Uint32Array.BYTES_PER_ELEMENT);
        new Uint8Array(nativeExports.memory.buffer, sourcePointer, bytes.length).set(bytes);
        dataView(nativeExports).setUint32(errorOutput, 0, true);
        documentPointer = nativeExports.es_document_parse(
            sourcePointer,
            bytes.length,
            optionsMask(parseOptions),
            errorOutput
        );
        errorPointer = dataView(nativeExports).getUint32(errorOutput, true);

        const decoder = new NodeDecoder(nativeExports);
        try {
            if (!documentPointer) throw decoder.parseError(errorPointer);
            return decoder.decodeDocument(nativeExports.es_document_root(documentPointer));
        } finally {
            decoder.dispose();
        }
    } finally {
        if (documentPointer) nativeExports.es_document_free(documentPointer);
        if (errorPointer) nativeExports.es_error_free(errorPointer);
        if (errorOutput) nativeExports.free(errorOutput);
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

function dataView(nativeExports: NativeExports): DataView {
    return new DataView(nativeExports.memory.buffer);
}
