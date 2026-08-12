package com.nouprax.markdown.core

/** A run of raw inline HTML, passed through unparsed. */
public class HTML internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** True when the literal is one complete comment.
     *
     * The same bit as [HTMLBlock.comment], and from the same place. */
    public val comment: Boolean,
    public val literal: String,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
