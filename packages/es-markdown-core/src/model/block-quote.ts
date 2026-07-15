import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface BlockQuote extends MarkupBase<"blockQuote"> {
    readonly content: readonly Markup[];
}
