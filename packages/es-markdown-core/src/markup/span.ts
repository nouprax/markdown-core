import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Span extends MarkupBase<"span"> {
    readonly content: readonly Markup[];
}
