import type { MarkupBase } from "./base.js";

/** A cross-link written as `[[reference]]`. */
export interface CrossLink extends MarkupBase<"crossLink"> {
    /** The source-faithful reference between the delimiters. */
    readonly reference: string;
}
