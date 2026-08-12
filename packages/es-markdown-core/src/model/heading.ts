import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/** An ATX or setext heading; the node does not record which form was written. */
export interface Heading extends MarkupBase<"heading"> {
    /** The heading level, 1 through 6. */
    readonly level: number;
    /** The heading's inline content in source order. */
    readonly content: readonly Markup[];
}
