package com.nouprax.markdown.core

public class HTMLBlock internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val literal: String,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitHTMLBlock(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
