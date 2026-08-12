package com.nouprax.markdown.core

/** Struck-through inline content, written `~text~` or `~~text~~`. */
public class Strikethrough internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The struck-through inline content in source order. */
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
