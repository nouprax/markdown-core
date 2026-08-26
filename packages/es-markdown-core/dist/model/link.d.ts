import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
export interface Link extends MarkupBase<"link"> {
    /**
     * Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing in
     * it, so both answer `""`. A link with no destination at all is a
     * `LinkReference`.
     */
    readonly destination: string;
    /** Optional: `[a](/u)` wrote no title, `[a](/u "")` wrote an empty one. */
    readonly title: string | null;
    readonly content: readonly Markup[];
}
