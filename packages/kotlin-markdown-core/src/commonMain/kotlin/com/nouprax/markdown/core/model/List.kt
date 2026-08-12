package com.nouprax.markdown.core

/**
 * A bullet or ordered list of [ListItem] values.
 *
 * The name is the construct's, not the collection's. A file that star-imports
 * this package resolves `List` to this class, so it has to import this one
 * under an alias or write `kotlin.collections.List` out in full.
 */
public class List internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** Bulleted or ordered — not which marker was written.
     *
     * `-`, `+` and `*` all read as [ListFlavor.BULLET], and changing the
     * marker starts a NEW list rather than changing this field. */
    public val flavor: ListFlavor,
    /** An ordered list's first number, as written; null for a bullet list. */
    public val start: Long?,
    /** Whether the list renders tight.
     *
     * The tree does not change shape for it — an item of a tight list still
     * holds a [Paragraph] — so a renderer that drops the spacing between
     * items must read this flag rather than infer it from the children. */
    public val tight: Boolean,
    /** The list's items in source order. */
    public val items: kotlin.collections.List<ListItem>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

/** One item of a [List], which is the only place one appears. */
public class ListItem internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** A task item's checkbox state.
     *
     * Null when no checkbox was written, which is how an unchecked box is
     * told from a plain item. */
    public val checked: Boolean?,
    /** The item's block content in source order. */
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
