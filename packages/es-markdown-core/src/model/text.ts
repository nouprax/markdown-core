import type { MarkupBase } from "./base.js";

/** A run of literal inline text. */
export interface Text extends MarkupBase<"text"> {
    /** The decoded text.
     *
     * Entity references, backslash escapes, and — when `smartPunctuation` is
     * on — quotes and dashes are resolved here. The only decoded literal in
     * the tree; a raw HTML node and a formula carry the bytes that were
     * written. */
    readonly literal: string;
}
