import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/** An image whose children are its inline description. */
export interface Image extends MarkupBase<"image"> {
    /** The image source with backslash escapes and character references already
     * resolved, so it is not always the spelling the scope covers.
     *
     * Empty when the image writes empty parentheses, as `![alt]()` does.
     * Never null: an inline image always writes its `(…)`, so there is no
     * unwritten case to tell an empty one from. */
    readonly source: string;
    /** The title in quotes after the source, unescaped the same way; null when
     * the image states none. */
    readonly title: string | null;
    /** The image's alt text, kept as parsed inline content in source order
     * rather than flattened to a string. */
    readonly content: readonly Markup[];
}
