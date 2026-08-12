import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";

/** A formula standing alone as its own block.
 *
 * Present only when the `formulas` option is on. */
export interface FormulaBlock extends MarkupBase<"formulaBlock"> {
    /** Always `standalone`.
     *
     * A formula embedded in inline content is a {@link Formula}, a different
     * kind. */
    readonly mode: PlacementMode;
    /** The formula source between the delimiters.
     *
     * The delimiters are inside `scope` and outside this. */
    readonly literal: string;
}
