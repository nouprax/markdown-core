import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * A footnote definition (`[^label]: …`) and the blocks that make up the note.
 *
 * Numbering and back-references are presentation rather than content: nothing
 * on the node says which footnote this is or where it was referenced from.
 */
export interface FootnoteDefinition extends MarkupBase<"footnoteDefinition"> {
    /** The label between `[^` and `]`, exactly as written and not normalized.
     *
     * A reference and a definition are paired case-folded, trimmed, and with
     * inner whitespace collapsed, so comparing two of these strings byte for
     * byte is a stricter test than the one that matched them. */
    readonly label: string;
    /** The definition's block content in source order. */
    readonly content: readonly Markup[];
}

/**
 * A reference (`[^label]`) to a footnote definition.
 *
 * There is no unresolved form: `[^x]` where nothing defines `x` is not a
 * footnote at all but the literal text the author typed, so a reference that
 * reaches the tree is one the document defines.
 */
export interface FootnoteReference extends MarkupBase<"footnoteReference"> {
    /** The label between `[^` and `]`, exactly as written and not normalized.
     *
     * A reference and a definition are paired case-folded, trimmed, and with
     * inner whitespace collapsed, so comparing two of these strings byte for
     * byte is a stricter test than the one that matched them. */
    readonly label: string;
}
