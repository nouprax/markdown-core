package com.nouprax.markdown.core

public class Link internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val destination: String?,
    public val title: String?,
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitLink(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
