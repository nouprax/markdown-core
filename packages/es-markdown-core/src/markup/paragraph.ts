import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Paragraph extends MarkupBase<"paragraph"> {
    readonly content: readonly Markup[];
}
