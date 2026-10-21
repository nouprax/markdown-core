import type { MarkupBase } from "./base.js";

/** A `#name` topic: an inline leaf whose name excludes the `#`. */
export interface Topic extends MarkupBase<"topic"> {
    /** The name as written, without the `#`. */
    readonly name: string;
}
