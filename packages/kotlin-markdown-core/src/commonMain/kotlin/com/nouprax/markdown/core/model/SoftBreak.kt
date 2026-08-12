package com.nouprax.markdown.core

/**
 * A soft line break: a source newline that renders as collapsible whitespace
 * rather than as a new line.
 *
 * [LineBreak] is the hard form.
 */
public class SoftBreak internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
