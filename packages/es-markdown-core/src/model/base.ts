import type { MarkupID } from "./markup-id.js";
import type { Scope } from "../values.js";

/**
 * Common surface of every canonical Markdown value-tree node.
 *
 * Nodes are immutable plain values. Equality is O(1): two nodes have the same
 * content exactly when they have the same `id` and the same `revision`, which
 * the engine guarantees implies identical AST content (fields and
 * descendants). {@link MarkupID} is interned, so `a.id === b.id && a.revision ===
 * b.revision` is that comparison.
 *
 * `scope` is deliberately outside it: absolute source position is not content,
 * so two nodes differing only in where they sit have the same content. That is
 * what lets a boundary moved by an append leave every reactive comparison
 * under an unchanged node untouched.
 */
export interface MarkupBase<Kind extends string> {
    readonly kind: Kind;
    /** Series-scoped identity: stable across appends while the node remains
     * the same kind of thing at the same place. */
    readonly id: MarkupID;
    /** The revision at which this node's own fields, child list, or any
     * descendant last changed.
     *
     * A pure positional shift caused by a change elsewhere never changes a
     * node's revision. */
    readonly revision: number;
    /** The node's absolute source extent, both bounds inclusive of the
     * construct's own markers.
     *
     * A property OF the node, not of a lookup: a document is an immutable
     * projection of one text, so a node in it does not move. */
    readonly scope: Scope;
}
