package com.nouprax.markdown.core

public class List internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val flavor: ListFlavor,
    public val start: Long?,
    public val tight: Boolean,
    public val items: kotlin.collections.List<ListItem>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

public class ListItem internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val checked: Boolean?,
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
