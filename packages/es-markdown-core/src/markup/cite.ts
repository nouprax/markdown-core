import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { CitationReferent } from "./values.js";

/** A Markup node owned by Cite.citations, with inline prefix and suffix
 * content. A `footnote` referent with a `note` target owns its inline note's
 * Footnote, visited before the prefix. */
export interface Citation extends MarkupBase<"citation"> {
    readonly referent: CitationReferent;
    readonly prefix: readonly Markup[];
    readonly suffix: readonly Markup[];
}

/**
 * A citation cluster: one or more items in source order. An inherited
 * `[^label]` call is a one-item cite whose referent names the footnote by label
 * and whose affixes are empty; the items remain in the citations field.
 */
export interface Cite extends MarkupBase<"cite"> {
    readonly citations: readonly Citation[];
}
