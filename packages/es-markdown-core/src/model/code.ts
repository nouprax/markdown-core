import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";

/** An inline code span. */
export interface Code extends MarkupBase<"code"> {
    /** Always `embedded`.
     *
     * The standalone form of code is {@link CodeBlock}, a different kind. */
    readonly mode: PlacementMode;
    /** The text between the backticks, normalized as CommonMark requires:
     * every line ending becomes one space, and one leading and one trailing
     * space are dropped together when the span holds anything other than
     * spaces.
     *
     * Nothing in it is decoded — an entity or a backslash inside a span is
     * literal text. */
    readonly literal: string;
}
