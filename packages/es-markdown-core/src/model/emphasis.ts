import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Emphasis extends MarkupBase<"emphasis"> {
    readonly content: readonly Markup[];
}
