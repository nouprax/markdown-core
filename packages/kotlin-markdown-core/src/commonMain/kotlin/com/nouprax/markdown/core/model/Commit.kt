@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

/**
 * What [Document.edit] returns: the document the new text describes, and what
 * changed on the way there.
 */
public class Commit internal constructor(
    public val document: Document,
    public val delta: Delta,
)

/**
 * Which parts of a node's projection differ.
 *
 * There is no lifecycle flag. A node that no longer exists has no parts, so
 * its set is EMPTY, and that is how a consumer tells a deletion from a
 * change. A node that did not exist before differs from absence in every part
 * it has, so it carries all of them.
 */
public class DiffParts internal constructor(
    /** The raw bit pattern, as the engine reports it.
     *
     * A signed Int and not an unsigned one: four bits are defined, and an
     * unsigned property would reach Java only through a name Java cannot
     * write. */
    public val rawValue: Int,
) {
    /** True when this entry reports a node that no longer exists. */
    public val isRetired: Boolean get() = rawValue == 0

    /** True when one of the node's own fields differs, string-valued ones
     * included.
     *
     * A link's destination and title, a reference's label, a directive's
     * name and attributes are all [value], not [text]. Never a kind change —
     * paired nodes always share a kind, because a changed kind is a
     * retirement and a creation rather than a difference. */
    public val value: Boolean get() = has(VALUE)

    /** True when the node's canonical text differs. */
    public val text: Boolean get() = has(TEXT)

    /** True when the node's own child list differs. */
    public val children: Boolean get() = has(CHILDREN)

    /** True when something below the node differs.
     *
     * Reported together with whatever of the node's own differs, never
     * instead of it. Every node above a change carries some part, so a
     * subtree whose root the delta does not name holds no change at all. */
    public val descendant: Boolean get() = has(DESCENDANT)

    private fun has(bit: Int): Boolean = rawValue and bit != 0

    override fun equals(other: Any?): Boolean = other is DiffParts && other.rawValue == rawValue

    override fun hashCode(): Int = rawValue

    override fun toString(): String =
        buildList {
            if (value) add("value")
            if (text) add("text")
            if (children) add("children")
            if (descendant) add("descendant")
        }.joinToString(prefix = "DiffParts(", postfix = ")")

    private companion object {
        const val VALUE = 1
        const val TEXT = 2
        const val CHILDREN = 4
        const val DESCENDANT = 8
    }
}

/** One node whose projection differs between two documents. */
public class Diff internal constructor(
    /** Which node. */
    public val markup: MarkupID,
    /** What about it differs; [DiffParts.isRetired] when it no longer exists. */
    public val parts: DiffParts,
) {
    override fun equals(other: Any?): Boolean = other is Diff && other.markup == markup && other.parts == parts

    override fun hashCode(): Int = 31 * markup.hashCode() + parts.hashCode()
}

/**
 * The difference between two documents: one list, in order.
 *
 * The order is the new document's postorder over the surviving nodes — a node
 * always follows its own children — with each retired node emitted where it
 * was found, before its former parent's row. A consumer rebuilding immutable
 * values bottom-up therefore reads the list once, front to back.
 */
public class Delta internal constructor(
    public val beforeRevision: ULong,
    public val afterRevision: ULong,
    public val diffs: kotlin.collections.List<Diff>,
) {
    /** [beforeRevision] as a bit-preserving signed value for Java callers. */
    public fun beforeRevisionBits(): Long = beforeRevision.toLong()

    /** [afterRevision] as a bit-preserving signed value for Java callers. */
    public fun afterRevisionBits(): Long = afterRevision.toLong()

    override fun equals(other: Any?): Boolean =
        other is Delta &&
            other.beforeRevision == beforeRevision &&
            other.afterRevision == afterRevision &&
            other.diffs == diffs

    override fun hashCode(): Int {
        var result = beforeRevision.hashCode()
        result = 31 * result + afterRevision.hashCode()
        result = 31 * result + diffs.hashCode()
        return result
    }
}
