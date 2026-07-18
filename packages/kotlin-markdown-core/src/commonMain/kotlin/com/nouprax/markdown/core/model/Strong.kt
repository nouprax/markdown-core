package com.nouprax.markdown.core

public class Strong internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitStrong(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
