import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Strikethrough extends MarkupBase<"strikethrough"> {
    readonly content: readonly Markup[];
}
