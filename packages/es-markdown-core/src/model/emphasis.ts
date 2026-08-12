import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/** Emphasized (typically italic) inline content. */
export interface Emphasis extends MarkupBase<"emphasis"> {
    /** The emphasized inline content in source order. */
    readonly content: readonly Markup[];
}
