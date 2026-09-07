import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { CitationReferent, Scope } from "../values.js";

/**
 * One item of a `Cite`: a scoped value, not a `Markup` kind. It is written,
 * so it has a scope, and it owns its affixes, but it is never a child of a
 * node. `prefix` and `suffix` are inline content, empty when absent.
 */
export interface Citation {
    readonly scope: Scope;
    readonly referent: CitationReferent;
    readonly prefix: readonly Markup[];
    readonly suffix: readonly Markup[];
}

/**
 * A citation cluster: one or more items in source order. An inherited
 * `[^label]` call is a one-item cite whose referent names the footnote by id
 * and whose affixes are empty; the items are values, never children.
 */
export interface Cite extends MarkupBase<"cite"> {
    readonly citations: readonly Citation[];
}
