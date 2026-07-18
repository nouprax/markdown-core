import type { Document } from "../model/document.js";
import type { MarkupID } from "../model/markup-id.js";

/** The result of one session commit: the new immutable snapshot and the
 * exact difference from the previous revision. */
export interface Commit {
    readonly document: Document;
    readonly changes: Delta;
}

/**
 * The id sets of one commit. The four arrays are disjoint: `added` and
 * `removed` list nodes that appeared and disappeared, `changed` lists nodes
 * whose own fields or direct child list changed, and `bubbled` lists
 * ancestors whose revision advanced only because a descendant changed. Ids
 * of removed nodes are retired and never reused. A pure positional shift is
 * not a change and produces no entry.
 */
export interface Delta {
    readonly beforeRevision: number;
    readonly afterRevision: number;
    readonly added: readonly MarkupID[];
    readonly removed: readonly MarkupID[];
    readonly changed: readonly MarkupID[];
    readonly bubbled: readonly MarkupID[];
}
