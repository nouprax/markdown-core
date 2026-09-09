import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Insertion extends MarkupBase<"insertion"> {
    readonly content: readonly Markup[];
}
