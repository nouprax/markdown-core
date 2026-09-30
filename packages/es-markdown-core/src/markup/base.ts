import type { Attributes } from "./attributes.js";
import type { Extent } from "./values.js";

export interface MarkupBase<Kind extends string> {
    readonly kind: Kind;
    /** Unique within the document across every owned relation, and numbered
     * from 1 in canonical walk order by a parse. Always below 2^53, so it is
     * an exact number, usable directly as a list key. */
    readonly id: number;
    readonly extent: Extent;
    readonly anchor: string | null;
    readonly attributes: Attributes;
}
