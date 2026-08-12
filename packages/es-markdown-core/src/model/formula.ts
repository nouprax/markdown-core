import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";

/** A formula written inside inline content.
 *
 * Present only when the `formulas` option is on. */
export interface Formula extends MarkupBase<"formula"> {
    /** `standalone` for a display formula, `embedded` for one that runs with
     * the surrounding text.
     *
     * Either way the node is inline content, and {@link FormulaBlock} is the
     * kind for a formula that occupies a block of its own. */
    readonly mode: PlacementMode;
    /** The formula source between the delimiters.
     *
     * The delimiters are not part of it, though `scope` covers them. */
    readonly literal: string;
}
