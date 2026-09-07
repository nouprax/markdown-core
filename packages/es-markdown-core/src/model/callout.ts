import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { CalloutFold } from "../values.js";

/**
 * Every `>` container. A plain quoted block is a callout without metadata:
 * `variant` is `null`, `fold` is `"none"` and `title` is `null`. The callouts
 * module's metadata rule, which fills them in, lands with `O8`.
 */
export interface Callout extends MarkupBase<"callout"> {
    /** The authored type as written, or `null` when the container has no metadata line. */
    readonly variant: string | null;
    readonly fold: CalloutFold;
    /** The title's inline content, owned by the callout and never part of `content`; `null` when no title was authored. */
    readonly title: readonly Markup[] | null;
    readonly content: readonly Markup[];
}
