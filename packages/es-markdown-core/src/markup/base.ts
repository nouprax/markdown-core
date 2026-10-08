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
    /** Its own source ranges, in source order; touching runs are one run,
     * and the source between two runs is not its own. None when it is a
     * block whose own source is its range; every inline node has runs. */
    readonly runs: readonly Run[];
    readonly anchor: string | null;
    readonly attributes: Attributes;
}
