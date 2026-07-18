import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface FootnoteDefinition extends MarkupBase<"footnoteDefinition"> {
    readonly label: string;
    readonly content: readonly Markup[];
}

export interface FootnoteReference extends MarkupBase<"footnoteReference"> {
    readonly label: string;
}
