import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";

export interface FormulaBlock extends MarkupBase<"formulaBlock"> {
    readonly mode: PlacementMode;
    readonly literal: string;
}
