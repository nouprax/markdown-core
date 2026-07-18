package com.nouprax.markdown.core

public class LineBreak internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitLineBreak(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
