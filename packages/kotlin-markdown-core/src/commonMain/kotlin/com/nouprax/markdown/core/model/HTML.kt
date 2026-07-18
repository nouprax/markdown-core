package com.nouprax.markdown.core

public class HTML internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val literal: String,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitHTML(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
