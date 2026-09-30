import type { Document } from "../markup/document.js";
import type { TextUnit } from "../markup/values.js";
import { ParseError } from "../common/parse-error.js";
import { Decoder, headerSize, lengthOffset } from "../wire/node-decoder.js";
import { native, type NativeExports } from "./native.js";

const utf8Encoder = new TextEncoder();

export function parseDocument(source: string, unit: TextUnit): Document {
    return parseDocumentWithNative(native, source, unit);
}

/** Internal dependency boundary used to verify terminal native failures. */
export function parseDocumentWithNative(nativeExports: NativeExports, source: string, unit: TextUnit): Document {
    if (typeof source !== "string") throw new TypeError("source must be a string");
    if (unit !== "utf8" && unit !== "utf16") throw new TypeError('unit must be "utf8" or "utf16"');
    const bytes = utf8Encoder.encode(source);
    let sourcePointer = 0;
    let resultPointer = 0;
    try {
        sourcePointer = allocate(nativeExports, Math.max(bytes.length, 1));
        new Uint8Array(nativeExports.memory.buffer, sourcePointer, bytes.length).set(bytes);
        resultPointer = nativeExports.markdown_core_wire_parse(sourcePointer, bytes.length);
        if (!resultPointer) throw new ParseError("allocationFailed", "failed to allocate native AST result");

        // Parsing may grow memory, which detaches every pre-call view. Take a
        // fresh header view, validate its size against the current heap, then
        // decode in place without another Wasm call. No view escapes this try.
        const memorySize = nativeExports.memory.buffer.byteLength;
        if (resultPointer > memorySize - headerSize) {
            throw new Error("native result header lies outside WebAssembly memory");
        }
        const totalSize = new DataView(nativeExports.memory.buffer).getUint32(resultPointer + lengthOffset, true);
        if (totalSize < headerSize || totalSize > memorySize - resultPointer) {
            throw new Error("native result lies outside WebAssembly memory");
        }
        return new Decoder(new Uint8Array(nativeExports.memory.buffer, resultPointer, totalSize), unit).decode();
    } finally {
        if (resultPointer) nativeExports.markdown_core_wire_free(resultPointer);
        if (sourcePointer) nativeExports.free(sourcePointer);
    }
}

function allocate(nativeExports: NativeExports, size: number): number {
    const pointer = nativeExports.malloc(size);
    if (!pointer) throw new ParseError("allocationFailed", "failed to allocate WASM memory");
    return pointer;
}
