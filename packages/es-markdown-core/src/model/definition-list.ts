import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface DefinitionList extends MarkupBase<"definitionList"> {
    readonly definitions: readonly Definition[];
}

export interface Definition extends MarkupBase<"definition"> {
    readonly term: readonly Markup[];
    readonly content: readonly (readonly Markup[])[];
    readonly compact: boolean;
}
