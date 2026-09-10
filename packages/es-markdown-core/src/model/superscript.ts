import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Superscript extends MarkupBase<"superscript"> {
    readonly content: readonly Markup[];
}
