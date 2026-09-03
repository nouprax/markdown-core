import type { Scope } from "../values.js";

export interface MarkupBase<Kind extends string> {
    readonly kind: Kind;
    readonly scope: Scope;
    /** Returns the canonical debug dump for this markup subtree. */
    readonly dump: () => string;
}
