import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * The immutable semantic root returned by a parse.
 */
export interface Document extends MarkupBase<"document"> {
    readonly content: readonly Markup[];
}
