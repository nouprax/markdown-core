import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface DirectiveBlock extends MarkupBase<"directiveBlock"> {
    readonly name: string;
    readonly attributes: string | null;
    readonly label: readonly Markup[] | null;
    readonly content: readonly Markup[];
}
