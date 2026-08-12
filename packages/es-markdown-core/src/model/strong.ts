import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/** Strongly emphasized (typically bold) inline content. */
export interface Strong extends MarkupBase<"strong"> {
    /** The strongly emphasized inline content in source order. */
    readonly content: readonly Markup[];
}
