import type { Semantic } from "../model/semantic.js";
import { ParseError } from "../parse-error.js";
import type { ParseOptions } from "../parse-options.js";
import type { Read } from "../read.js";
import { WireFrame } from "../wire/wire-decoder.js";
import { native } from "./native.js";
import { copyOut, discardOut, optionsMask, withHeapBytes, withHeapText } from "./parser.js";

// The well-known symbol `using` disposes through. Runtimes that predate
// `Symbol.dispose` (Node 20 before 20.5, older browsers) get the same
// registered symbol TypeScript's own `using` lowering falls back to, so the
// method is reachable either way and the class definition never throws.
const disposeSymbol: symbol = Symbol.dispose ?? Symbol.for("Symbol.dispose");

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
// The merged interface below adds only `[Symbol.dispose]`, and the prototype
// assignment after it installs exactly that member, so the declaration cannot
// outrun the value.
// eslint-disable-next-line @typescript-eslint/no-unsafe-declaration-merging
export class Document {
    // The native session. Zero once sealed or disposed, which is the same
    // "gone" this boundary uses everywhere; no native anything is part of
    // the public surface.
    #native: number;

    // THE PREVIOUS READ (#162): the semantic tree of the last payload this
    // document decoded, which the next feed's payload may be a DELTA
    // against -- the native side keeps the tree it wrote that payload from,
    // and names by position what did not move, so the values already built
    // here are handed into the new read instead of decoded again. Null
    // whenever there is nothing to be a delta against: before the first
    // feed (the constructor's discarded read is not a payload), and after a
    // feed that failed, whether natively or in the decoder -- the native
    // side leaves its baseline standing on its own failures and replaces it
    // on the decoder's, and asking for FULL is correct against either.
    #previous: Semantic | null = null;

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
    constructor(markdown?: string | Uint8Array | ParseOptions, options?: ParseOptions) {
        let initial: string | Uint8Array | undefined;
        let parseOptions: ParseOptions;
        if (markdown === undefined) {
            parseOptions = options ?? {};
        } else if (typeof markdown === "string" || markdown instanceof Uint8Array) {
            initial = markdown;
            parseOptions = options ?? {};
        } else if (options === undefined) {
            // The options-only form; `optionsMask` refuses anything that is
            // not a plain options object, `null` included.
            parseOptions = markdown;
        } else {
            throw new TypeError("markdown must be a string or a Uint8Array");
        }
        this.#native = native.es_session_new(optionsMask(parseOptions));
        if (!this.#native) throw new ParseError("allocationFailed", "failed to allocate a native session");
        if (initial === undefined) return;
        const session = this.#native;
        /* A string crosses through `withHeapText` in its one mandatory copy
         * (#147); a Uint8Array was always single-copy. */
        if (typeof initial === "string") {
            withHeapText(initial, (chunkPointer, length) =>
                discardOut((output) => native.es_session_advance(session, chunkPointer, length, output, output + 4))
            );
            return;
        }
        withHeapBytes(initial, (chunkPointer) =>
            discardOut((output) => native.es_session_advance(session, chunkPointer, initial.length, output, output + 4))
        );
    }

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
    feed(chunk: string | Uint8Array): Read {
        if (typeof chunk === "string") {
            const session = this.#live();
            return withHeapText(chunk, (chunkPointer, length) =>
                this.#read((request, output) =>
                    native.es_session_feed(session, chunkPointer, length, request, output, output + 4)
                )
            );
        }
        if (!(chunk instanceof Uint8Array)) throw new TypeError("chunk must be a string or a Uint8Array");
        const session = this.#live();
        return withHeapBytes(chunk, (chunkPointer) =>
            this.#read((request, output) =>
                native.es_session_feed(session, chunkPointer, chunk.length, request, output, output + 4)
            )
        );
    }

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
    seal(): Read {
        const session = this.#live();
        const sealed = this.#read((request, output) => native.es_session_finish(session, request, output, output + 4));
        this.dispose();
        return sealed;
    }

    /**
     * Hands back the native memory of a stream abandoned before `seal`.
     * Idempotent, and the end of the object: every later `feed` and `seal`
     * throws rather than touching freed memory. The reads already returned
     * are values and keep reading.
     */
    dispose(): void {
        this.#previous = null;
        if (!this.#native) return;
        native.es_session_free(this.#native);
        this.#native = 0;
    }

    #live(): number {
        if (!this.#native) throw new Error("the document is sealed or disposed");
        return this.#native;
    }

    /** One read through the wire: DELTA is asked for exactly when a previous
     * read is in hand, the previous read is cleared before the call so that
     * any failure leaves the next request FULL, and a decoded read becomes
     * the previous one. */
    #read(invoke: (request: number, output: number) => number): Read {
        const previous = this.#previous;
        this.#previous = null;
        const request = previous === null ? WireFrame.full : WireFrame.delta;
        const read = copyOut((output) => invoke(request, output), previous);
        this.#previous = read.semantic;
        return read;
    }
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-declaration-merging -- see the class.
export interface Document {
    /** `using document = new Document()` disposes it at scope exit. */
    [Symbol.dispose](): void;
}

// Installed by name rather than declared in the class: a computed member
// would evaluate `Symbol.dispose` inside the class definition and crash the
// module on a runtime that predates it, and `--isolatedDeclarations` refuses
// the computed key besides.
Object.defineProperty(Document.prototype, disposeSymbol, {
    configurable: true,
    writable: true,
    value(this: Document): void {
        this.dispose();
    }
});
