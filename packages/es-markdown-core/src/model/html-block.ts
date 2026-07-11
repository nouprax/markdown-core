import type { MarkupBase } from "./base.js";

export interface HTMLBlock extends MarkupBase<"htmlBlock"> {
    readonly literal: string;
}
