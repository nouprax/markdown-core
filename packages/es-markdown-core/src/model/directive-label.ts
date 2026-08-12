import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/** The optional inline-markup label owned by a directive. */
export interface DirectiveLabel extends MarkupBase<"directiveLabel"> {
    /** The label's inline content in source order.
     *
     * Empty for an explicit `[]`; a directive that wrote no label at all has a
     * null `label` instead. */
    readonly content: readonly Markup[];
}
