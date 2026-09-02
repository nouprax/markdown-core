import type { MarkupBase } from "./base.js";
import type { DirectiveAttribute } from "./directive-attribute.js";
import type { DirectiveLabel } from "./directive-label.js";

export interface Directive extends MarkupBase<"directive"> {
    readonly name: string;
    /** Sorted by name, or `null` when the source wrote no `{...}` at all. */
    readonly attributes: readonly DirectiveAttribute[] | null;
    /** Markup owned by the label field, not a generic child/content element. */
    readonly label: DirectiveLabel | null;
}
