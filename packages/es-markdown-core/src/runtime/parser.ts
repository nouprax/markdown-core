import { ParseError } from "../parse-error.js";
import type { Read } from "../read.js";
import type { ParseOptions } from "../parse-options.js";
import { decodeDiscarded, decodeRead } from "../wire/wire-decoder.js";
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

const utf8Encoder = new TextEncoder();

/**
 * Encodes `text` straight into WASM memory for the duration of `action`
 * (#147): `TextEncoder.encodeInto` writes into a view over the allocation,
 * so the string crosses in its one mandatory copy instead of being encoded
 * into a throwaway array that a second copy then moves. The allocation is
 * sized for the worst case -- three bytes per UTF-16 code unit covers every
 * code point, since an astral character spends two units on its four bytes
 * -- and `action` is told the length actually written.
 */
export function withHeapText<Result>(text: string, action: (pointer: number, length: number) => Result): Result {
    const capacity = Math.max(text.length * 3, 1);
    const pointer = allocate(capacity);
    try {
        const { written } = utf8Encoder.encodeInto(text, new Uint8Array(native.memory.buffer, pointer, capacity));
        return action(pointer, written);
    } finally {
        native.free(pointer);
    }
}

/**
 * THE ONE WAY A READ LEAVES WASM. `invoke` runs a native call that writes an
 * MKC6 payload behind the given output slot -- a document's `feed` and its
 * `seal` both answer that way -- and this copies the payload out in ONE
 * crossing, releases the native buffer, and decodes the copy: a `Read` value,
 * or the `ParseError` the payload carried. Nothing native survives the call.
 */
export function copyOut(invoke: (output: number) => number): Read {
    const { semantic, concrete } = decodePayload(invoke, decodeRead);
    return makeRead(semantic, concrete);
}

/**
 * Runs a native call whose read is DISCARDED -- the `Document` constructor's
 * initial feed -- so an error still surfaces and a healthy tree is not
 * decoded just to be thrown away. Only the payload's envelope is read.
 */
export function discardOut(invoke: (output: number) => number): void {
    decodePayload(invoke, decodeDiscarded);
}

/** Runs the native call and DECODES its payload off a view over WASM memory,
 * freeing the native buffer only after the decode returns (#147). The old
 * shape `.slice()`d the whole payload first, purely because the free ran
 * before the decode -- but the decoder is pure JS, nothing re-enters WASM
 * mid-decode to grow or detach the buffer, and everything it returns owns
 * its bytes (strings, plain values, and the Concrete source's one necessary
 * owning copy), so the full-payload copy bought nothing. A zero return from
 * `invoke` is the one failure with no payload to decode: the buffer itself
 * could not be built. */
function decodePayload<Result>(invoke: (output: number) => number, decode: (payload: Uint8Array) => Result): Result {
    const output = allocate(2 * Uint32Array.BYTES_PER_ELEMENT);
    let payloadPointer = 0;
    try {
        const view = dataView();
        view.setUint32(output, 0, true);
        view.setUint32(output + 4, 0, true);
        if (!invoke(output)) {
            throw new ParseError("allocationFailed", "failed to serialize the native document");
        }
        const after = dataView();
        payloadPointer = after.getUint32(output, true);
        const payloadLength = after.getUint32(output + 4, true);
        return decode(new Uint8Array(native.memory.buffer, payloadPointer, payloadLength));
    } finally {
        if (payloadPointer) native.es_wire_free(payloadPointer);
        native.free(output);
    }
}

/**
 * The pair, sealed shut: `semantic` and `concrete` are data and enumerate;
 * `dump` is a convenience and does not.
 */
function makeRead(semantic: Read["semantic"], concrete: Read["concrete"]): Read {
    const read = {
        semantic,
        concrete
    } as { semantic: Read["semantic"]; concrete: Read["concrete"]; dump?: () => string };
    Object.defineProperty(read, "dump", {
        enumerable: false,
        value: () => semantic.dump()
    });
    return read as Read;
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
