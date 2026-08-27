import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { Identity } from "../values.js";
/**
 * A footnote definition. `norm` is the match key the label folds to, and it
 * KEEPS the leading `^` that `label` does not carry, so a footnote and a link
 * definition of one name cannot collide in a consumer's single map.
 */
export interface FootnoteDefinition extends MarkupBase<"footnoteDefinition"> {
    readonly label: string;
    readonly norm: string;
    readonly content: readonly Markup[];
}
/**
 * A footnote call. There is one footnote syntax, so it carries no form.
 * `definition` is the identity of the `FootnoteDefinition` it resolved to --
 * the first definition of its label in document order.
 */
export interface FootnoteReference extends MarkupBase<"footnoteReference"> {
    readonly label: string;
    readonly definition: Identity;
}
