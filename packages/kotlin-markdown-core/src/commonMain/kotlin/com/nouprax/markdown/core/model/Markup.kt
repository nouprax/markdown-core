package com.nouprax.markdown.core

/**
 * A node of the canonical Markdown value tree.
 *
 * Nodes are immutable values. Equality and hashing are O(1) and
 * allocation-free: two nodes are equal exactly when they have the same [id]
 * and the same [revision], which the engine guarantees implies identical AST
 * content (fields and descendants). Absolute source position is not content —
 * resolve it with [Document.scope] or receive it from [Walker] events.
 */
public sealed interface Markup {
    /**
     * Session-scoped identity: stable across incremental commits while the
     * node remains the same kind of thing at the same place.
     */
    public val id: MarkupID

    /**
     * The commit revision at which this node's own fields, child list, or
     * any descendant last changed. A pure positional shift caused by an edit
     * elsewhere never changes a node's revision.
     */
    public val revision: ULong

    public fun <Result> accept(visitor: Visitor<Result>): Result
}

internal fun markupEquals(
    node: Markup,
    other: Any?,
): Boolean = other is Markup && other.id == node.id && other.revision == node.revision

internal fun markupHashCode(node: Markup): Int = 31 * node.id.hashCode() + node.revision.hashCode()
