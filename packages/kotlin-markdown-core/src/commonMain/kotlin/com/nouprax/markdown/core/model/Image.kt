package com.nouprax.markdown.core

public class Image internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val source: String?,
    public val title: String?,
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
