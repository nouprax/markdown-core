import type { Semantic } from "./model/semantic.js";

/**
 * One read of the text.
 *
 * `semantic` is the tree with policy applied. Every scope in it is counted
 * against the NORMALIZED source -- UTF-8 as fed, every NUL replaced by
 * U+FFFD, every line ending a single `\n` and every line having one -- which
 * the library does not hand back: a caller whose input can differ from it
 * applies the same normalization to its own copy before resolving a scope.
 *
 * A read is an immutable value the caller owns outright. It retains nothing
 * native, so it stays readable after every later feed and after the
 * `Document` that produced it is disposed. A mid-stream read is the
 * projection of the parse as it stands; the read `Document.seal` returns is
 * the whole text's.
 */
export interface Read {
    readonly semantic: Semantic;
    /** Returns the canonical diagnostic dump of `semantic`. */
    readonly dump: () => string;
}
