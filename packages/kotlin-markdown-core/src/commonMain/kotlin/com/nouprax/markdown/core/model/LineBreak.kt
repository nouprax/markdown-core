package com.nouprax.markdown.core

public class LineBreak internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
