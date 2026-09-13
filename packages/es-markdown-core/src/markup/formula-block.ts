import type { MarkupBase } from "./base.js";

export interface FormulaBlock extends MarkupBase<"formulaBlock"> {
    readonly literal: string;
}
