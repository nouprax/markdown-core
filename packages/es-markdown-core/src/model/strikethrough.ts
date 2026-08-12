import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/** Struck-through inline content, written `~text~` or `~~text~~`.
 *
 * Present only when the `strikethrough` option is on. */
export interface Strikethrough extends MarkupBase<"strikethrough"> {
    /** The struck-through inline content in source order. */
    readonly content: readonly Markup[];
}
