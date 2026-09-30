import type { Document } from "../markup/document.js";
import type { TextUnit } from "../markup/values.js";
import { MarkdownCoreError } from "../common/markdown-core-error.js";
import { Decoder, lengthOffset } from "../wire/node-decoder.js";
import { native, type NativeExports } from "./native.js";

const utf8Encoder = new TextEncoder();

export function parseDocument(source: string, unit: TextUnit): Document {
    return parseDocumentWithNative(native, source, unit);
}

/** Internal dependency boundary used to verify terminal native failures. */
export function parseDocumentWithNative(nativeExports: NativeExports, source: string, unit: TextUnit): Document {
    const bytes = utf8Encoder.encode(source);
    let sourcePointer = 0;
    let resultPointer = 0;
    try {
        // malloc(0) may return NULL, which would read as allocation failure,
        // so an empty source still takes one byte.
        sourcePointer = allocate(nativeExports, Math.max(bytes.length, 1));
        new Uint8Array(nativeExports.memory.buffer, sourcePointer, bytes.length).set(bytes);
        resultPointer = nativeExports.markdown_core_wire_parse(sourcePointer, bytes.length);
        if (!resultPointer) throw new MarkdownCoreError("allocationFailed");

        // Parsing may grow memory, which detaches every pre-call view. Take
        // fresh views, then decode in place without another Wasm call. No
        // view escapes this try.
        const length = new DataView(nativeExports.memory.buffer).getUint32(resultPointer + lengthOffset, true);
        return new Decoder(new Uint8Array(nativeExports.memory.buffer, resultPointer, length), unit).decode();
    } finally {
        if (resultPointer) nativeExports.markdown_core_wire_free(resultPointer);
        if (sourcePointer) nativeExports.free(sourcePointer);
    }
}

function allocate(nativeExports: NativeExports, size: number): number {
    const pointer = nativeExports.malloc(size);
    if (!pointer) throw new MarkdownCoreError("allocationFailed");
    return pointer;
}
