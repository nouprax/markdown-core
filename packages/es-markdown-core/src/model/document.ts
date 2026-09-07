import type { MarkupBase } from "./base.js";
import type { Footnote } from "./footnote.js";
import type { Markup } from "./markup.js";

/**
 * The immutable semantic root returned by a parse.
 */
export interface Document extends MarkupBase<"document"> {
    readonly content: readonly Markup[];
    /**
     * Every winning or unreferenced footnote definition as a document-owned
     * `Footnote` value, ordered by scope start; visited after `content` and
     * never counted among its children.
     */
    readonly footnotes: readonly Footnote[];
}
