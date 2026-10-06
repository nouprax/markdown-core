import type { Attributes } from "./attributes.js";
import type { Extent, Piece, Run } from "./values.js";

export interface MarkupBase<Kind extends string> {
    readonly kind: Kind;
    /** Unique within the document across every owned relation, and numbered
     * from 1 in canonical walk order by a parse. Always below 2^53, so it is
     * an exact number, usable directly as a list key. */
    readonly id: number;
    readonly extent: Extent;
    /** The node's own parts of its range, one per line, when other bytes
     * separate them; none when its range is one piece. */
    readonly pieces: readonly Piece[];
    /** Where its first relation was read from when that relation is an
     * inline root's content; none otherwise. */
    readonly runs: readonly Run[];
    readonly anchor: string | null;
    readonly attributes: Attributes;
}
