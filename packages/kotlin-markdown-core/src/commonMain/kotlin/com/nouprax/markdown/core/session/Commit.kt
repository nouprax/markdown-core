package com.nouprax.markdown.core

/**
 * The result of one session commit: the new immutable snapshot and the exact
 * difference from the previous revision.
 */
public class Commit internal constructor(
    public val document: Document,
    public val changes: Delta,
)

/**
 * The id sets of one commit. The four lists are disjoint: [added] and
 * [removed] list nodes that appeared and disappeared, [changed] lists nodes
 * whose own fields or direct child list changed, and [bubbled] lists
 * ancestors whose revision advanced only because a descendant changed. Ids
 * of removed nodes are retired and never reused. A pure positional shift is
 * not a change and produces no entry.
 */
public class Delta internal constructor(
    public val beforeRevision: ULong,
    public val afterRevision: ULong,
    public val added: kotlin.collections.List<MarkupID>,
    public val removed: kotlin.collections.List<MarkupID>,
    public val changed: kotlin.collections.List<MarkupID>,
    public val bubbled: kotlin.collections.List<MarkupID>,
) {
    override fun equals(other: Any?): Boolean =
        other is Delta &&
            other.beforeRevision == beforeRevision &&
            other.afterRevision == afterRevision &&
            other.added == added &&
            other.removed == removed &&
            other.changed == changed &&
            other.bubbled == bubbled

    override fun hashCode(): Int {
        var result = beforeRevision.hashCode()
        result = 31 * result + afterRevision.hashCode()
        result = 31 * result + added.hashCode()
        result = 31 * result + removed.hashCode()
        result = 31 * result + changed.hashCode()
        result = 31 * result + bubbled.hashCode()
        return result
    }
}
