import type { MarkupID } from "./markup-id.js";

/**
 * Common surface of every canonical Markdown value-tree node.
 *
 * Nodes are immutable plain values. Equality is O(1): two nodes are equal
 * exactly when they have the same `id` and the same `revision`, which the
 * engine guarantees implies identical AST content (fields and descendants).
 * An unchanged node is additionally the same object across consecutive
 * snapshots. Absolute source position is not content — resolve it with
 * `Document.scope` or receive it from `Walker` events.
 */
export interface MarkupBase<Kind extends string> {
    readonly kind: Kind;
    /** Session-scoped identity: stable across incremental commits while the
     * node remains the same kind of thing at the same place. */
    readonly id: MarkupID;
    /** The commit revision at which this node's own fields, child list, or
     * any descendant last changed. A pure positional shift caused by an edit
     * elsewhere never changes a node's revision. */
    readonly revision: number;
}
