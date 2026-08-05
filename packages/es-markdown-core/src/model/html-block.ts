import type { MarkupBase } from "./base.js";

export interface HTMLBlock extends MarkupBase<"htmlBlock"> {
    readonly literal: string;
    /** True when the literal is one complete comment, so consumers without an
     * HTML parser can skip comment material by this bit alone. */
    readonly comment: boolean;
}
