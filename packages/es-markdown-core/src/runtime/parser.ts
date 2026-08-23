import { Concrete } from "../concrete.js";
import type { DocumentRoot } from "../model/document-root.js";
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

/** The two views, before `Document` wraps them: the class lives one
 * module up so that the parser does not import what imports it. */
export interface ParsedViews {
    readonly semantic: DocumentRoot;
    readonly concrete: Concrete;
}

export function parseDocument(source: string, parseOptions: ParseOptions = {}): ParsedViews {
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
            return {
                semantic: decoder.decodeDocument(native.es_document_root(documentPointer)),
                concrete: readConcrete(documentPointer)
            };
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

/**
 * Copies the whole concrete view out of WASM memory, which the caller frees as
 * soon as this returns.
 *
 * Every array is read out in ONE crossing: the view is copied whole either way,
 * and a call per line or per region is tens of thousands of them on a document
 * of any size.
 */
function readConcrete(documentPointer: number): Concrete {
    const viewOutput = allocate(Uint32Array.BYTES_PER_ELEMENT * 2);
    let lineOutput = 0;
    let regionOutput = 0;
    let offsetOutput = 0;
    let pathOutput = 0;
    try {
        native.es_document_source(documentPointer, viewOutput, viewOutput + 4);
        const sourcePointer = dataView().getUint32(viewOutput, true);
        const sourceLength = dataView().getUint32(viewOutput + 4, true);
        const source = new Uint8Array(native.memory.buffer, sourcePointer, sourceLength).slice();

        const lineCount = native.es_document_line_count(documentPointer);
        lineOutput = allocate(Math.max(lineCount, 1) * Uint32Array.BYTES_PER_ELEMENT);
        native.es_document_line_starts(documentPointer, lineOutput);
        const lineStarts = new Uint32Array(native.memory.buffer, lineOutput, lineCount).slice();

        const regionCount = native.es_document_region_count(documentPointer);
        regionOutput = allocate(Math.max(regionCount * 3, 1) * Uint32Array.BYTES_PER_ELEMENT);
        native.es_document_regions(documentPointer, regionOutput);
        const regions = new Uint32Array(native.memory.buffer, regionOutput, regionCount * 3).slice();

        // Sizing first: the same call fills the offsets it refuses to write
        // paths for, so the last one is how many the paths need.
        offsetOutput = allocate((regionCount + 1) * Uint32Array.BYTES_PER_ELEMENT);
        native.es_document_owner_paths(documentPointer, 0, 0, offsetOutput, regionCount + 1);
        const ownerOffsets = new Uint32Array(native.memory.buffer, offsetOutput, regionCount + 1).slice();
        const pathLength = ownerOffsets[regionCount]!;
        pathOutput = allocate(Math.max(pathLength, 1) * Int32Array.BYTES_PER_ELEMENT);
        if (!native.es_document_owner_paths(documentPointer, pathOutput, pathLength, offsetOutput, regionCount + 1)) {
            throw new ParseError("internal", "the concrete view has an owner outside the semantic tree");
        }
        const ownerPaths = new Int32Array(native.memory.buffer, pathOutput, pathLength).slice();

        return new Concrete(source, lineStarts, regions, ownerPaths, ownerOffsets);
    } finally {
        for (const pointer of [pathOutput, offsetOutput, regionOutput, lineOutput, viewOutput]) {
            if (pointer) native.free(pointer);
        }
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
