import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Subscript extends MarkupBase<"subscript"> {
    readonly content: readonly Markup[];
}
