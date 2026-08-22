import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Directive extends MarkupBase<"directive"> {
    readonly name: string;
    readonly attributes: string | null;
    readonly label: readonly Markup[] | null;
}
