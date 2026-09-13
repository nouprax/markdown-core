import type { MarkupBase } from "./base.js";
import type { DirectiveLabel } from "./directive-label.js";

export interface Directive extends MarkupBase<"directive"> {
    readonly name: string;
    /** Markup owned by the label field, not a generic child/content element. */
    readonly label: DirectiveLabel | null;
}
