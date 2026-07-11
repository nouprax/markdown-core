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
    { name: "dollarFormulaDelimiters", defaultValue: true, mask: 1 << 8 },
    { name: "latexFormulaDelimiters", defaultValue: true, mask: 1 << 9 },
    { name: "directives", defaultValue: true, mask: 1 << 10 }
] as const satisfies readonly OptionDescriptor[];

const utf8Encoder = new TextEncoder();

export function parseDocument(source: string, parseOptions: ParseOptions = {}): Document {
    validateInput(source, parseOptions);
    const bytes = utf8Encoder.encode(source);
    let sourcePointer = 0;
    let errorOutput = 0;
    let documentPointer = 0;
    let errorPointer = 0;
    try {
        sourcePointer = allocate(Math.max(bytes.length, 1));
        errorOutput = allocate(Uint32Array.BYTES_PER_ELEMENT);
        new Uint8Array(native.memory.buffer, sourcePointer, bytes.length).set(bytes);
        dataView().setUint32(errorOutput, 0, true);
        documentPointer = native.es_document_parse(sourcePointer, bytes.length, optionsMask(parseOptions), errorOutput);
        errorPointer = dataView().getUint32(errorOutput, true);

        const decoder = new NodeDecoder(native);
        try {
            if (!documentPointer) throw decoder.parseError(errorPointer);
            return decoder.decodeDocument(native.es_document_root(documentPointer));
        } finally {
            decoder.dispose();
        }
    } finally {
        if (documentPointer) native.es_document_free(documentPointer);
        if (errorPointer) native.es_error_free(errorPointer);
        if (errorOutput) native.free(errorOutput);
        if (sourcePointer) native.free(sourcePointer);
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

function allocate(size: number): number {
    const pointer = native.malloc(size);
    if (!pointer) throw new ParseError("allocationFailed", "failed to allocate WASM memory");
    return pointer;
}

function dataView(): DataView {
    return new DataView(native.memory.buffer);
}
