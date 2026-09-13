import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Strong extends MarkupBase<"strong"> {
    readonly content: readonly Markup[];
}
