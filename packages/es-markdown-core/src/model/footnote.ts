import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * A footnote definition. `identifier` KEEPS the leading `^` that `label` does
 * not carry, so a footnote and a link definition of one name cannot collide in
 * a consumer's single map.
 */
export interface FootnoteDefinition extends MarkupBase<"footnoteDefinition"> {
    readonly label: string;
    readonly identifier: string;
    readonly content: readonly Markup[];
}

/** A footnote call. There is one footnote syntax, so it carries no form. */
export interface FootnoteReference extends MarkupBase<"footnoteReference"> {
    readonly label: string;
    readonly identifier: string;
}
