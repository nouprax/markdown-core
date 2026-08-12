import type { MarkupBase } from "./base.js";

/** A hard line break, written as a backslash or two or more spaces before the newline.
 *
 * It renders as an explicit new line rather than as collapsible whitespace. */
export type LineBreak = MarkupBase<"lineBreak">;
