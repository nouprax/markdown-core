import type { Metadata } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { Specimen } from "./specimen.js";
import type { Footnote } from "./footnote.js";
import type { Markup } from "./markup.js";

/**
 * The immutable semantic root returned by a parse.
 */
export interface Document extends MarkupBase<"document"> {
    readonly metadata: Metadata | null;
    readonly content: readonly Markup[];
    /**
     * Every winning or unreferenced footnote definition as a document-owned
     * `Footnote` value, ordered by scope start; visited after `content` and
     * never counted among its children.
     */
    readonly footnotes: readonly Footnote[];
    /** Definitions in scope order, visited after footnotes and never counted as children. */
    readonly specimens: readonly Specimen[];
}
