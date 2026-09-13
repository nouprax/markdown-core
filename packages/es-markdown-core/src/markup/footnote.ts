import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/** A document-owned Markup definition. Its normalized ID is independent of display numbering. */
export interface Footnote extends MarkupBase<"footnote"> {
    readonly id: string;
    /** The parsed block content of the definition. */
    readonly content: readonly Markup[];
}
