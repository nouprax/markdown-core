import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * A definition where it was written. A referenced definition `[^x]: body` is
 * a block in the content that holds it; an inline note `^[body]` is owned by
 * its Citation's `footnote` referent, with a null label and direct inline
 * content. Duplicates and unused definitions remain. Display numbering is
 * the renderer's.
 */
export interface Footnote extends MarkupBase<"footnote"> {
    /** The normalized label, or null for an inline note. */
    readonly label: string | null;
    /** The parsed content of the definition. */
    readonly content: readonly Markup[];
}
