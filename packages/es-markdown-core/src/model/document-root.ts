import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface DocumentRoot extends MarkupBase<"document"> {
    readonly content: readonly Markup[];
}
