import type { MarkupBase } from "./base.js";
import type { Destination } from "../values.js";

/** A workspace link. The label is absent only when no separator was written. */
export interface CrossLink extends MarkupBase<"crossLink"> {
    readonly dest: Destination;
    /** The complete raw authored label; null only when no separator was authored. */
    readonly label: string | null;
}
