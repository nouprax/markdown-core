import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * Every `>` container. A plain quoted block is a callout without metadata:
 * `variant`, `collapsed` and `title` are `null`. The callouts module's
 * metadata rule, which fills them in, lands with `O8`.
 */
export interface Callout extends MarkupBase<"callout"> {
    /** The authored type as written, or `null` when the container has no metadata line. */
    readonly variant: string | null;
    /** The fold marker: `null` when no `+` or `-` was authored, `false` for `+`, which opens expanded, and `true` for `-`. */
    readonly collapsed: boolean | null;
    /** The title's inline content, owned by the callout and never part of `content`; `null` when no title was authored, and never empty. */
    readonly title: readonly Markup[] | null;
    readonly content: readonly Markup[];
}
