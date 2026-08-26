import type { Markup } from "./model/markup.js";
/** Produces the canonical diagnostic tree for immutable Markdown markup. */
export declare class TreeDumper {
    private constructor();
    /** Returns the canonical diagnostic dump for `root` and its descendants. */
    static dump(root: Markup): string;
}
