import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * The root of the semantic tree -- the view with policy applied, which may
 * omit bytes: a fence, a bullet and a reference definition's punctuation are
 * in no literal anywhere. It is an ordinary `Markup` node (its `kind` stays
 * `"document"`, the name the C enum and the canonical dump use): nothing but
 * its `content` and its `scope`, like every node under it. What it does NOT
 * carry is the text its scopes are counted against -- a root detached from
 * its `Concrete` is not self-interpreting, which is why the two travel
 * together as a `Read` and never alone.
 */
export interface Semantic extends MarkupBase<"document"> {
    readonly content: readonly Markup[];
}
