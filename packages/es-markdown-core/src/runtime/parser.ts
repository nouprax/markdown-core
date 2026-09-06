import type { Document } from "../model/document.js";
import { ParseError } from "../parse-error.js";
import { NodeDecoder, transferHeaderSize } from "../wire/node-decoder.js";
import { native, type NativeExports } from "./native.js";

const utf8Encoder = new TextEncoder();

export function parseDocument(source: string): Document {
    return parseDocumentWithNative(native, source);
}

/** Internal dependency boundary used to verify terminal native failures. */
export function parseDocumentWithNative(nativeExports: NativeExports, source: string): Document {
    if (typeof source !== "string") throw new TypeError("source must be a string");
    const bytes = utf8Encoder.encode(source);
    let sourcePointer = 0;
    let resultPointer = 0;
    try {
        sourcePointer = allocate(nativeExports, Math.max(bytes.length, 1));
        new Uint8Array(nativeExports.memory.buffer, sourcePointer, bytes.length).set(bytes);
        resultPointer = nativeExports.es_parse(sourcePointer, bytes.length);
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

function allocate(nativeExports: NativeExports, size: number): number {
    const pointer = nativeExports.malloc(size);
    if (!pointer) throw new ParseError("allocationFailed", "failed to allocate WASM memory");
    return pointer;
}
