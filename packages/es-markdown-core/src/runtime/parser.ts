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
    try {
        sourcePointer = allocateBytes(nativeExports, bytes);
        return decodeMessage(nativeExports, nativeExports.markdown_core_wire_parse(sourcePointer, bytes.length), unit);
    } finally {
        if (sourcePointer) nativeExports.free(sourcePointer);
    }
}

/** Decodes and releases one owned message: the document it carries, or its
 * failure thrown as a `MarkdownCoreError`. Zero is a message that could not
 * be allocated. */
export function decodeMessage(nativeExports: NativeExports, message: number, unit: TextUnit): Document {
    if (!message) throw new MarkdownCoreError("allocationFailed");
    try {
        // A call may grow memory, which detaches every pre-call view. Take
        // fresh views, then decode in place without another Wasm call. No
        // view escapes this try.
        const length = new DataView(nativeExports.memory.buffer).getUint32(message + lengthOffset, true);
        return new Decoder(new Uint8Array(nativeExports.memory.buffer, message, length), unit).decode();
    } finally {
        nativeExports.markdown_core_wire_free(message);
    }
}

/** `bytes` copied into the module's memory. malloc(0) may return NULL, which
 * would read as allocation failure, so no bytes still take one. */
export function allocateBytes(nativeExports: NativeExports, bytes: Uint8Array): number {
    const pointer = allocate(nativeExports, Math.max(bytes.length, 1));
    new Uint8Array(nativeExports.memory.buffer, pointer, bytes.length).set(bytes);
    return pointer;
}

export function allocate(nativeExports: NativeExports, size: number): number {
    const pointer = nativeExports.malloc(size);
    if (!pointer) throw new MarkdownCoreError("allocationFailed");
    return pointer;
}
