import type { Identity, Scope } from "../values.js";
export interface MarkupBase<Kind extends string> {
    readonly kind: Kind;
    /**
     * The node's identity (docs/STREAMING.md §4 D4): the name a consumer
     * tracks this element by across a stream's feeds -- the render key. A
     * block keeps its identity across feeds however the bytes arrive; an
     * inline's is stable exactly as long as its owning block's inline list is.
     */
    readonly id: Identity;
    readonly scope: Scope;
    /** Returns the canonical diagnostic dump for this markup subtree. */
    readonly dump: () => string;
}
