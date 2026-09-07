import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { Destination } from "../values.js";

export interface Link extends MarkupBase<"link"> {
    /**
     * Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing in
     * it, so both answer `{ kind: "url", value: "" }`. A reference occurrence
     * answers the destination its definition stated, and every occurrence of
     * one definition shares the same value.
     */
    readonly dest: Destination;
    /** Optional: `[a](/u)` wrote no title, `[a](/u "")` wrote an empty one. */
    readonly title: string | null;
    readonly content: readonly Markup[];
}
