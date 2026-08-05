import type { MarkupBase } from "./base.js";

export interface HTML extends MarkupBase<"html"> {
    readonly literal: string;
    /** True when the literal is one complete comment; the same rule as
     * `HTMLBlock.comment`. */
    readonly comment: boolean;
}
