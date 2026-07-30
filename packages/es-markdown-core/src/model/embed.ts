import type { MarkupBase } from "./base.js";

/** An embed written as `![[reference]]`. */
export interface Embed extends MarkupBase<"embed"> {
    /** The source-faithful reference between the delimiters. */
    readonly reference: string;
}
