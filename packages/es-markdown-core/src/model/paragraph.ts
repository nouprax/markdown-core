import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Paragraph extends MarkupBase<"paragraph"> {
    /** The paragraph's inline content in source order. */
    readonly content: readonly Markup[];
}
