package com.nouprax.markdown.core

/**
 * The optional inline label owned by a [Directive] or [DirectiveBlock].
 *
 * - A present-but-empty label is represented by a [DirectiveLabel] whose
 *   [content] is empty.
 * - An absent label is represented by a null parent property.
 */
public class DirectiveLabel internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The label's content in source order.
     *
     * Inline nodes only, on a [DirectiveBlock]'s label as much as on a
     * [Directive]'s. */
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
