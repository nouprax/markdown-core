import type { Document } from "../markup/document.js";
import { ParseError } from "../common/parse-error.js";
import { Decoder, transferHeaderSize } from "../wire/node-decoder.js";
import { native, type NativeExports } from "./native.js";

const utf8Encoder = new TextEncoder();

export function parseDocument(source: string): Document {
    return parseDocumentWithNative(native, source);
}

/** Internal dependency boundary used to verify terminal native failures. */
export function parseDocumentWithNative(nativeExports: NativeExports, source: string): Document {
    if (typeof source !== "string") throw new TypeError("source must be a string");
    let sourcePointer = 0;
    let resultPointer = 0;
    try {
        // The source is encoded straight into the heap, never into a byte
        // array of its own first. A reservation of one byte per UTF-16 code
        // unit takes an ASCII source whole; where non-ASCII text made the
        // bytes outnumber the units, the rest follows into a reservation of
        // three bytes per unit, the most UTF-8 needs (a lone surrogate is
        // U+FFFD, as `encode` writes it).
        const initial = Math.max(source.length + 16, 1);
        sourcePointer = allocate(nativeExports, initial);
        let { read, written } = utf8Encoder.encodeInto(
            source,
            new Uint8Array(nativeExports.memory.buffer, sourcePointer, initial)
        );
        if (read < source.length) {
            const capacity = written + (source.length - read) * 3;
            const grown = allocate(nativeExports, capacity);
            new Uint8Array(nativeExports.memory.buffer, grown, written).set(
                new Uint8Array(nativeExports.memory.buffer, sourcePointer, written)
            );
            nativeExports.free(sourcePointer);
            sourcePointer = grown;
            const rest = utf8Encoder.encodeInto(
                source.slice(read),
                new Uint8Array(nativeExports.memory.buffer, grown + written, capacity - written)
            );
            read += rest.read;
            written += rest.written;
            if (read !== source.length) throw new Error("source did not fit its UTF-8 reservation");
        }
        resultPointer = nativeExports.es_parse(sourcePointer, written);
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
        return new Decoder(new Uint8Array(nativeExports.memory.buffer, resultPointer, totalSize)).decode();
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
