import type { Document } from "../model/document.js";
import { ParseError } from "../parse-error.js";
import type { ParseOptions } from "../parse-options.js";
import { native } from "./native.js";
import { copyOut, optionsMask, withHeapBytes } from "./parser.js";

const utf8Encoder = new TextEncoder();

/**
 * A parse fed in pieces (docs/STREAMING.md §4 D5).
 *
 * A session owns one native parser for its whole life. `feed` hands it the
 * next chunk and returns THE DOCUMENT AFTER THOSE BYTES — a mid-stream
 * projection: a trailing line whose ending has not arrived is not yet in it,
 * and an open construct is projected as it stands (a list still open has not
 * settled its tightness). `finish` ends the stream: the pending line is
 * processed, every construct closes, and the sealed document comes back —
 * equal, concrete view and all, to what `Document.parse` returns for the
 * same bytes.
 *
 * Every document either call returns is built exactly the way `parse` builds
 * one: the native handle is released before the call returns, so the result
 * is a value that borrows nothing. It stays readable after every later feed,
 * after `finish`, and after the session is disposed.
 *
 * The session itself is a native resource mid-parse, not a value, and this
 * runtime has no destructor to hand it to: `dispose` is the one call that
 * takes the native memory back, it is idempotent, and it is owed exactly
 * once — after `finish` as much as after an abandoned stream.
 */
export class Session {
    // The native session, private the way `parse`'s handle is scoped to
    // `parse`: no native anything is part of the public surface. Zero once
    // disposed, which is the same "gone" this boundary uses everywhere.
    #native: number;

    /**
     * Opens a session.
     *
     * @param options which constructs to recognise, read exactly as
     * `Document.parse` reads them. Everything, by default.
     * @throws {ParseError} when the native session cannot be created, which
     * is an allocation failure and nothing finer.
     */
    constructor(options: ParseOptions = {}) {
        this.#native = native.es_session_new(optionsMask(options));
        if (!this.#native) throw new ParseError("allocationFailed", "failed to allocate a native session");
    }

    /**
     * Feeds exactly the bytes of `chunk` and returns the document after them.
     *
     * A `Uint8Array` chunk is raw UTF-8 and may end anywhere: inside a
     * multi-byte sequence, between a CR and its LF — the stream repairs both,
     * which is a split no string could even spell. A `string` chunk feeds its
     * UTF-8 bytes, exactly as the byte form takes them; a producer of string
     * pieces never splits a character, so the byte form's one extra power is
     * not needed there. An empty chunk is a legal feed: the document as it
     * stands.
     *
     * @param chunk the next piece of the stream.
     * @returns the document after those bytes, as a value the caller keeps.
     * An incomplete trailing line is not yet in it.
     * @throws {ParseError} `invalidArgument` once `finish` has sealed the
     * stream, `allocationFailed` when the projection could not be built.
     * Text is never a failure: it produces a document.
     */
    feed(chunk: string | Uint8Array): Document {
        const bytes = typeof chunk === "string" ? utf8Encoder.encode(chunk) : chunk;
        if (!(bytes instanceof Uint8Array)) throw new TypeError("chunk must be a string or a Uint8Array");
        const session = this.#live();
        return withHeapBytes(bytes, (chunkPointer) =>
            copyOut((errorOutput) => native.es_session_feed(session, chunkPointer, bytes.length, errorOutput))
        );
    }

    /**
     * Ends the stream and returns the sealed document.
     *
     * The pending line is processed and every construct closes, so the result
     * equals `Document.parse` of the concatenated chunks. It also ends the
     * session's parse: after it returns, `feed` and a second `finish` throw
     * `invalidArgument`, and `dispose` is all that remains.
     *
     * @returns the sealed document.
     * @throws {ParseError} `invalidArgument` for a session already finished,
     * `allocationFailed` when the final projection could not be built.
     */
    finish(): Document {
        const session = this.#live();
        return copyOut((errorOutput) => native.es_session_finish(session, errorOutput));
    }

    /**
     * Hands the session's native memory back. Idempotent, and the end of the
     * object: every later `feed` and `finish` throws rather than touching
     * freed memory. The documents already returned are values and keep
     * reading.
     */
    dispose(): void {
        if (!this.#native) return;
        native.es_session_free(this.#native);
        this.#native = 0;
    }

    #live(): number {
        if (!this.#native) throw new Error("native session has been disposed");
        return this.#native;
    }
}
