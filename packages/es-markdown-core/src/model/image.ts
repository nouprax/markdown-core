import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Image extends MarkupBase<"image"> {
    readonly source: string | null;
    readonly title: string | null;
    readonly content: readonly Markup[];
}
