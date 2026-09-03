import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { ReferenceForm } from "../values.js";

/**
 * A link reference. It carries NO destination: the destination is stated once,
 * at the definition, and `identifier` is what names it. `form` records which
 * of `[text][label]`, `[label][]` and `[label]` the source wrote; all three
 * resolve identically, so nothing else on the node recovers it.
 */
export interface LinkReference extends MarkupBase<"linkReference"> {
    readonly label: string;
    readonly identifier: string;
    readonly form: ReferenceForm;
    readonly content: readonly Markup[];
}

/** An image reference. As `LinkReference`; the content is parsed alt text. */
export interface ImageReference extends MarkupBase<"imageReference"> {
    readonly label: string;
    readonly identifier: string;
    readonly form: ReferenceForm;
    readonly content: readonly Markup[];
}
