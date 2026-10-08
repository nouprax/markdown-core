import type { Attributes } from "./attributes.js";
import type { Extent, Run } from "./values.js";

export interface MarkupBase<Kind extends string> {
    readonly kind: Kind;
    /** Unique within the document across every owned relation, and numbered
     * from 1 by a parse in the order its nodes complete, the document last.
     * Always below 2^53, so it is
     * an exact number, usable directly as a list key. */
    readonly id: number;
    readonly extent: Extent;
    /** The source it read: the runs its first relation was read from when
     * that relation is an inline root's content, and runs that decode no
     * bytes over its own source, between which lies source that is not its
     * own. None when
     * its own source is its range and it has no inline content. */
    readonly runs: readonly Run[];
    readonly anchor: string | null;
    readonly attributes: Attributes;
}
