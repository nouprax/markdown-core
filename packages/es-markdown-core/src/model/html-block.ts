import type { MarkupBase } from "./base.js";

/** A block of raw HTML, passed through unparsed. */
export interface HTMLBlock extends MarkupBase<"htmlBlock"> {
    readonly literal: string;
    /** True when the literal is one complete comment, so consumers without an
     * HTML parser can skip comment material by this bit alone. */
    readonly comment: boolean;
}
