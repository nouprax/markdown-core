import type { ParseOptions } from "../parse-options.js";
import type { Read } from "../read.js";
/**
 * The living document: the one entry into this parser, fed in pieces and
 * answering with `Read` values (docs/STREAMING.md §4 D5, under 3.0's names).
 *
 * `feed` hands it the next chunk and returns THE READ AFTER THOSE BYTES — a
 * mid-stream projection: a trailing line whose ending has not arrived is not
 * yet in it, and an open construct is projected as it stands (a list still
 * open has not settled its tightness). `seal` ends the stream: the pending
 * line is processed, every construct closes, and the sealed read comes back —
 * the whole text's, identical for the same bytes however they were fed — and
 * the native shell goes with it: a sealed document IS a disposed one.
 *
 * Every read either call returns is a value that borrows nothing: the native
 * handle behind it is released before the call returns, so it stays readable
 * after every later feed, after `seal`, and after the document is disposed.
 *
 * The document itself is a native resource mid-parse, not a value, and this
 * runtime has no destructor to hand it to: `dispose` (also `Symbol.dispose`,
 * so `using` scopes one) takes the native memory back from a stream that is
 * abandoned instead of sealed. It is idempotent, and it takes nothing else
 * with it — every read this document returned remains a plain value.
 */
export declare class Document {
    #private;
    /**
     * Opens a document; with `markdown`, feeds it in the same step — exactly
     * `new Document(options)` followed by one `feed` whose returned read is
     * discarded — and, being discarded by contract, never built: the native
     * side takes the bytes without projecting or serializing a read nothing
     * would decode. The whole-text parse is `new Document(markdown).seal()`.
     *
     * @param markdown the first piece of the stream, or the whole text.
     * @param options which constructs to recognise. Everything, by default.
     * @throws {ParseError} when the native session cannot be created, which
     * is an allocation failure and nothing finer.
     */
    constructor(markdown?: string | Uint8Array | ParseOptions, options?: ParseOptions);
    /**
     * Feeds exactly the bytes of `chunk` and returns the read after them.
     *
     * A `Uint8Array` chunk is raw UTF-8 and may end anywhere: inside a
     * multi-byte sequence, between a CR and its LF — the stream repairs both,
     * which is a split no string could even spell. A `string` chunk feeds its
     * UTF-8 bytes, exactly as the byte form takes them; a producer of string
     * pieces never splits a character, so the byte form's one extra power is
     * not needed there. An empty chunk is a legal feed: the read as it
     * stands.
     *
     * @param chunk the next piece of the stream.
     * @returns the read after those bytes, as a value the caller keeps. An
     * incomplete trailing line is not yet in it.
     * @throws {ParseError} `allocationFailed` when the projection could not
     * be built. Text is never a failure: it produces a read.
     */
    feed(chunk: string | Uint8Array): Read;
    /**
     * Ends the stream, returns the sealed read, and releases the native
     * shell.
     *
     * The pending line is processed and every construct closes, so the
     * result is identical for the same bytes however they were fed. Sealing
     * IS disposing: after it returns, `feed` and a second `seal` throw
     * rather than touching freed memory.
     *
     * @returns the sealed read.
     * @throws {ParseError} `allocationFailed` when the final projection
     * could not be built; the shell then remains for `dispose`.
     */
    seal(): Read;
    /**
     * Hands back the native memory of a stream abandoned before `seal`.
     * Idempotent, and the end of the object: every later `feed` and `seal`
     * throws rather than touching freed memory. The reads already returned
     * are values and keep reading.
     */
    dispose(): void;
}
export interface Document {
    /** `using document = new Document()` disposes it at scope exit. */
    [Symbol.dispose](): void;
}
