import type { Document } from "./document.js";
import type { MarkupID } from "./markup-id.js";

/** What `document.edit(...)` returns: the document the new text describes,
 * and what changed on the way there. */
export interface Commit {
    readonly document: Document;
    readonly delta: Delta;
}

/**
 * Which parts of a node's projection differ.
 *
 * There is no lifecycle flag. A node that no longer exists has no parts, so
 * every flag is false and `retired` is true, which is how a consumer tells a
 * deletion from a change. A node that did not exist before differs from
 * absence in every part it has, so it carries all of them.
 */
export interface DiffParts {
    /** True when the node no longer exists. */
    readonly retired: boolean;
    /** True when the node's kind or one of its scalar fields differs. */
    readonly value: boolean;
    /** True when the node's canonical text differs. */
    readonly text: boolean;
    /** True when the node's own child list differs. */
    readonly children: boolean;
    /** True when something below the node differs. Carried by every ancestor
     * of a change, so a renderer can stop at the first node whose own parts
     * are all false. */
    readonly descendant: boolean;
}

/** One node whose projection differs between two documents. */
export interface Diff {
    readonly markup: MarkupID;
    readonly parts: DiffParts;
}

/**
 * The difference between two documents: one list, in order.
 *
 * The order is the new document's postorder over the surviving nodes — a
 * node always follows its own children — with each retired node emitted
 * where it was found, before its former parent's row. A consumer rebuilding
 * immutable values bottom-up therefore reads the list once, front to back.
 */
export interface Delta {
    readonly beforeRevision: number;
    readonly afterRevision: number;
    readonly diffs: readonly Diff[];
}
