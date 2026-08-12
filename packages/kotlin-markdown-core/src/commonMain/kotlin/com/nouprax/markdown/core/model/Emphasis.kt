package com.nouprax.markdown.core

/** Emphasized (typically italic) inline content. */
public class Emphasis internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The emphasized inline content in source order. */
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
