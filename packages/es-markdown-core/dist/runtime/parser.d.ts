import type { Read } from "../read.js";
import type { ParseOptions } from "../parse-options.js";
/**
 * Copies `bytes` into WASM memory for the duration of `action`, and frees the
 * copy on the way out. The pointer is valid for exactly `bytes.length` bytes
 * (one byte is still reserved for an empty input, because `malloc(0)` may
 * answer 0 and 0 is this boundary's failure value).
 */
export declare function withHeapBytes<Result>(bytes: Uint8Array, action: (pointer: number) => Result): Result;
/**
 * THE ONE WAY A READ LEAVES WASM. `invoke` runs a native call that writes an
 * MKC6 payload behind the given output slot -- a document's `feed` and its
 * `seal` both answer that way -- and this copies the payload out in ONE
 * crossing, releases the native buffer, and decodes the copy: a `Read` value,
 * or the `ParseError` the payload carried. Nothing native survives the call.
 */
export declare function copyOut(invoke: (output: number) => number): Read;
/**
 * Runs a native call whose read is DISCARDED -- the `Document` constructor's
 * initial feed -- so an error still surfaces and a healthy tree is not
 * decoded just to be thrown away. Only the payload's envelope is read.
 */
export declare function discardOut(invoke: (output: number) => number): void;
/** The option flags the C bridge reads, validated on the way: the one
 * checking and encoding of `ParseOptions`, whether a parse or a session is
 * about to read them. */
export declare function optionsMask(parseOptions: ParseOptions): number;
