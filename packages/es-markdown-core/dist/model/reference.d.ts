import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { Identity, ReferenceForm } from "../values.js";
/**
 * A link reference. It carries NO destination: the destination is stated once,
 * at the definition, and `definition` names it -- the identity of the
 * `ReferenceDefinition` this reference resolved to, the first definition of
 * its label in document order. `form` records which of `[text][label]`,
 * `[label][]` and `[label]` the source wrote; all three resolve identically,
 * so nothing else on the node recovers it.
 */
export interface LinkReference extends MarkupBase<"linkReference"> {
    readonly label: string;
    readonly form: ReferenceForm;
    readonly definition: Identity;
    readonly content: readonly Markup[];
}
/** An image reference. As `LinkReference`; the content is parsed alt text. */
export interface ImageReference extends MarkupBase<"imageReference"> {
    readonly label: string;
    readonly form: ReferenceForm;
    readonly definition: Identity;
    readonly content: readonly Markup[];
}
