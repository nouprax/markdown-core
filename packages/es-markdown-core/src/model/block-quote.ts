import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface BlockQuote extends MarkupBase<"blockQuote"> {
    /** The quotation's block content in source order. */
    readonly content: readonly Markup[];
}
