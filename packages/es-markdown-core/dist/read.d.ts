import type { Concrete } from "./concrete.js";
import type { Semantic } from "./model/semantic.js";
/**
 * One read of the text, under two total views.
 *
 * `semantic` is the tree with policy applied, which may omit bytes;
 * `concrete` omits nothing. Every byte of the source is in exactly one region
 * of the concrete view and every region has exactly one owner in the tree, so
 * the pair is complete -- and it is CLOSED: every scope in `semantic` is
 * counted against `concrete`, and nothing outside this value is needed to
 * resolve one.
 *
 * A read is an immutable value the caller owns outright. It retains nothing
 * native, so it stays readable after every later feed and after the
 * `Document` that produced it is disposed. A mid-stream read is the
 * projection of the parse as it stands; the read `Document.seal` returns is
 * the whole text's.
 */
export interface Read {
    readonly semantic: Semantic;
    readonly concrete: Concrete;
    /** Returns the canonical diagnostic dump of `semantic`. */
    readonly dump: () => string;
}
