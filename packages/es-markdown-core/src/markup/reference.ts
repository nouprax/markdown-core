import type { MarkupBase } from "./base.js";
import type { Destination } from "./values.js";

/**
 * A link reference definition `[label]: /url "title"` where it was written: a
 * leaf block. Its anchor and attributes are the ones the definition states.
 * Duplicates and unused definitions remain.
 */
export interface Reference extends MarkupBase<"reference"> {
    /** The normalized label. */
    readonly label: string;
    /** The `url` destination the definition states. */
    readonly dest: Destination;
    /** Optional: absent and empty remain distinct. */
    readonly title: string | null;
}
