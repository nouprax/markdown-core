import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
export interface Image extends MarkupBase<"image"> {
    /** Required, for the reason `Link.destination` is. */
    readonly source: string;
    /** Optional. */
    readonly title: string | null;
    readonly content: readonly Markup[];
}
