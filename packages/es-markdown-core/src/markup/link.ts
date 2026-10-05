import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { Destination } from "./values.js";

export interface Link extends MarkupBase<"link"> {
    /**
     * Required: `[a]()` and `[a](<>)` wrote a destination and wrote nothing in
     * it, so both answer `{ kind: "url", value: "" }`. A reference occurrence
     * answers `{ kind: "reference", label }`, the normalized label it names
     * (`Document.reference`).
     */
    readonly dest: Destination;
    /** Optional: `[a](/u)` wrote no title, `[a](/u "")` wrote an empty one.
     * A reference occurrence writes none; its `Reference` holds the title. */
    readonly title: string | null;
    readonly content: readonly Markup[];
}
