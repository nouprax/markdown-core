@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.jvm.JvmSynthetic

/**
 * A node of the canonical Markdown value tree.
 *
 * Nodes are immutable values. Equality and hashing are O(1) and
 * allocation-free: two nodes are equal exactly when they have the same [id]
 * and the same [revision], which the engine guarantees implies identical AST
 * content (fields and descendants).
 *
 * [scope] is on every node and is deliberately NOT part of that: absolute
 * source position is not content, so two nodes differing only in where they
 * sit are equal. That is what lets an edit above a node leave every reactive
 * comparison below it untouched.
 */
public sealed interface Markup {
    /**
     * Series-scoped identity, unique within the series and never reused;
     * see [MarkupID].
     *
     * Stable across edits while the node remains the same kind
     * of thing at the same place.
     */
    public val id: MarkupID

    /**
     * The commit revision at which this node's own fields, child list, or
     * any descendant last changed.
     *
     * A pure positional shift caused by an edit elsewhere never changes a
     * node's revision.
     */
    public val revision: ULong

    /**
     * The node's absolute source extent, both bounds inclusive of the
     * construct's own markers.
     *
     * A property OF the node, not of a lookup: a document is an immutable
     * projection of one text, so a node in it does not move.
     */
    public val scope: Scope

    /** [revision] as a bit-preserving signed value: the Java view of the
     * unsigned accessor, whose mangled name Java sources cannot write. */
    public fun revisionBits(): Long = revision.toLong()

    public fun <Result> accept(visitor: MarkupVisitor<Result>): Result
}

@JvmSynthetic
internal fun markupEquals(
    node: Markup,
    other: Any?,
): Boolean = other is Markup && other.id == node.id && other.revision == node.revision

@JvmSynthetic
internal fun markupHashCode(node: Markup): Int = 31 * node.id.hashCode() + node.revision.hashCode()
