import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * The root of the semantic tree -- the view with policy applied, which may
 * omit bytes: a fence, a bullet and a reference definition's punctuation are
 * in no literal anywhere. It is an ordinary `Markup` node (its `kind` stays
 * `"document"`, the name the C enum and the canonical dump use): nothing but
 * its `content` and its `scope`, like every node under it. Its scopes are
 * counted against the normalized source (see `Read`), which the library does
 * not hand back.
 */
export interface Semantic extends MarkupBase<"document"> {
    readonly content: readonly Markup[];
}
