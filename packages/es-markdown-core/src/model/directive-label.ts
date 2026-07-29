import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/** The optional inline-markup label owned by a directive. */
export interface DirectiveLabel extends MarkupBase<"directiveLabel"> {
    readonly content: readonly Markup[];
}
