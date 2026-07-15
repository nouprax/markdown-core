import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Link extends MarkupBase<"link"> {
    readonly destination: string | null;
    readonly title: string | null;
    readonly content: readonly Markup[];
}
