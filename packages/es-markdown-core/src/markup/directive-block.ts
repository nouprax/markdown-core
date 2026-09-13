import type { MarkupBase } from "./base.js";
import type { DirectiveLabel } from "./directive-label.js";
import type { Markup } from "./markup.js";

export interface DirectiveBlock extends MarkupBase<"directiveBlock"> {
    readonly name: string | null;
    /** Markup owned by the label field, never an element of `content`. */
    readonly label: DirectiveLabel | null;
    readonly content: readonly Markup[];
}
