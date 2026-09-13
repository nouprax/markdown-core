import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Heading extends MarkupBase<"heading"> {
    readonly level: number;
    readonly content: readonly Markup[];
}
