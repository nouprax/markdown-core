import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { Destination } from "../values.js";

export interface Image extends MarkupBase<"image"> {
    /** Required, for the reason `Link.dest` is. */
    readonly dest: Destination;
    /** Optional. */
    readonly title: string | null;
    readonly width: number | null;
    readonly height: number | null;
    readonly content: readonly Markup[];
}
