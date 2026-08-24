import type { Concrete } from "../concrete.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * A parse: the tree, and the bytes its scopes are counted against.
 *
 * Every node carries a `scope`: a pair of BOUNDARIES saying which
 * line-and-column range the element occupies, not a range a substring is taken
 * with. `concrete` is what those numbers are counted against — the NORMALIZED
 * source and its line index — because they are not counted against the string
 * that was passed to `parse`.
 *
 * In C the two are siblings, because a `markdown_core_document` is a handle and
 * the root is a node it lends out. Here they are not: the handle is gone by the
 * time `Document.parse` returns, the tree is a value, and the source hangs off
 * the root whose scopes index it.
 */
export interface Document extends MarkupBase<"document"> {
    readonly content: readonly Markup[];
    readonly concrete: Concrete;

}
