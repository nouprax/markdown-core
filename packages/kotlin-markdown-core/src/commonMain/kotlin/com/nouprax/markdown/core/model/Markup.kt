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
 * sit are equal. That is what lets a mutation beyond a node leave every
 * reactive comparison below it untouched.
 */
public sealed interface Markup {
    /**
     * Series-scoped identity, unique within the series and never reused;
     * see [MarkupID].
     *
     * Stable across appends while the node remains the same kind
     * of thing at the same place.
     */
    public val id: MarkupID

    /**
     * The document revision at which this node's own fields, child list, or
     * any descendant last changed.
     *
     * A pure positional shift caused by a mutation elsewhere never changes
     * a node's revision.
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

/**
 * The node's children in document order — the one enumeration of what "the
 * children" means per kind, shared by the walker's traversal and the wire
 * mirror's subtree walk so the two can never disagree about structure.
 */
@JvmSynthetic
internal fun Markup.childValues(): kotlin.collections.List<Markup> =
    when (this) {
        is Document -> {
            content
        }

        is BlockQuote -> {
            content
        }

        is Paragraph -> {
            content
        }

        is Heading -> {
            content
        }

        is ThematicBreak -> {
            emptyList()
        }

        is List -> {
            items
        }

        is ListItem -> {
            content
        }

        is CodeBlock,
        is HTMLBlock,
        is FormulaBlock,
        -> {
            emptyList()
        }

        is Table -> {
            buildList {
                add(header)
                addAll(rows)
            }
        }

        is TableRow -> {
            cells
        }

        is TableCell -> {
            content
        }

        is DirectiveBlock -> {
            buildList {
                label?.let(::add)
                addAll(content)
            }
        }

        is DirectiveLabel -> {
            content
        }

        is FootnoteDefinition -> {
            content
        }

        is Text,
        is SoftBreak,
        is LineBreak,
        is Code,
        is HTML,
        is Formula,
        -> {
            emptyList()
        }

        is Emphasis -> {
            content
        }

        is Strong -> {
            content
        }

        is Strikethrough -> {
            content
        }

        is Link -> {
            content
        }

        is Image -> {
            content
        }

        is Directive -> {
            listOfNotNull(label)
        }

        is FootnoteReference -> {
            emptyList()
        }

        is ReferenceDefinition -> {
            emptyList()
        }

        is LinkReference -> {
            content
        }

        is ImageReference -> {
            content
        }

        is CrossLink -> {
            emptyList()
        }

        is Embed -> {
            emptyList()
        }
    }
