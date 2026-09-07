import type { MarkupBase } from "./base.js";
import type { Destination } from "../values.js";

/** An authored workspace reference. The label is absent only when no separator was written. */
export interface CrossLink extends MarkupBase<"crossLink"> {
    readonly embedded: boolean;
    readonly dest: Destination;
    readonly label: string | null;
}
