package com.nouprax.markdown.core

/** A run of literal inline text. */
public class Text internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The decoded text.
     *
     * Entity references and backslash escapes are already resolved, so
     * `&amp;` arrives here as `&`. A renderer targeting HTML escapes it
     * again rather than passing it through. */
    public val literal: String,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
