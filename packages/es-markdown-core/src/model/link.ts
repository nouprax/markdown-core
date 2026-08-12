import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * A hyperlink whose children are its inline caption.
 *
 * An autolink (`<https://example.com>`) arrives as a Link too, its caption
 * the single text node holding the URL.
 */
export interface Link extends MarkupBase<"link"> {
    /** The destination with backslash escapes and character references already
     * resolved, so it is not always the spelling the scope covers.
     *
     * Empty when the link writes empty parentheses, as `[text]()` and
     * `[text](<>)` both do. Never null: an inline link always writes its
     * `(…)`, so there is no unwritten case to tell an empty one from. */
    readonly destination: string;
    /** The title in quotes after the destination, unescaped the same way.
     *
     * Null when none is written, the empty string when one is written empty:
     * `[a](/u)` gives null and `[a](/u "")` gives `""`. An autolink writes no
     * title, so it gives null like any other link. */
    readonly title: string | null;
    /** The link's inline caption content in source order. */
    readonly content: readonly Markup[];
}
