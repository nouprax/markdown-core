import type { MarkupBase } from "./base.js";
import type { Destination, Dimensions } from "../values.js";

/** A workspace transclusion. The label is absent only when no separator was written. */
export interface CrossEmbedded extends MarkupBase<"crossEmbedded"> {
    readonly dest: Destination;
    /** Raw prefix after a valid dimension suffix; null only when no separator was authored. */
    readonly label: string | null;
    /** Authored dimensions, absent for an invalid or missing suffix. */
    readonly dimensions: Dimensions | null;
}
