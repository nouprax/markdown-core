import type { Attributes, Scope } from "../values.js";

export interface MarkupBase<Kind extends string> {
    readonly kind: Kind;
    readonly scope: Scope;
    readonly anchor: string | null;
    readonly attributes: Attributes;
    /** Returns the canonical debug dump for this markup subtree. */
    readonly dump: () => string;
}
