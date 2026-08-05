package com.nouprax.markdown.core

public class HTML internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val literal: String,
) : Markup {
    /** True when the literal is one complete comment; the same rule as
     * [HTMLBlock.comment]. */
    public val comment: Boolean get() = htmlLiteralIsComment(literal)

    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
