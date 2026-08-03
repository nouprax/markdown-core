import type { MarkupID } from "../model/markup-id.js";

/**
 * Session-answered footnote presentation state for one node.
 *
 * The tree is source-faithful: definitions stay at their source position
 * whether referenced or not, and references always carry their label.
 * Numbering, first-use order, resolution state, and back-reference ordinals
 * are queries against the session's committed revision. When a commit
 * changes only these answers, the affected nodes are reported `changed`
 * with a revision bump and identical dump content.
 */
export interface FootnoteInfo {
    /** The label's winning definition (for a definition: its own id unless
     * an earlier definition shadows it). Every node this can be asked about
     * has one: a reference exists only where its label is defined, and a
     * definition defines its own. */
    readonly definition: MarkupID | null;
    /** The label's 1-based first-use ordinal; null for a definition no
     * reference names. */
    readonly number: number | null;
    /** For a reference: its 1-based position among the label's references
     * in document order. null for definitions. */
    readonly referenceOrdinal: number | null;
    /** How many references share the label. */
    readonly referenceCount: number;
}
