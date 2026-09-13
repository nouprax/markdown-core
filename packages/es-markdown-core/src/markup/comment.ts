import type { MarkupBase } from "./base.js";

/**
 * A comment: an inline HTML comment token, or an HTML block that opened with
 * `<!--` and closed on a `-->` line. The one kind valid in both block and
 * inline content; the parent records which. `literal` is the bytes between
 * the delimiters, exactly as written.
 */
export interface Comment extends MarkupBase<"comment"> {
    readonly literal: string;
}
