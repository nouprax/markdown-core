import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * A directive's bracketed label. Its scope spans the brackets, so a label
 * written empty is still a place in the source.
 */
export interface DirectiveLabel extends MarkupBase<"directiveLabel"> {
    readonly content: readonly Markup[];
}
