import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Mark extends MarkupBase<"mark"> {
    readonly content: readonly Markup[];
}
