package com.nouprax.markdown.core

/**
 * A hard line break, written as two or more trailing spaces or as a trailing
 * backslash.
 *
 * It renders as an explicit new line, where a [SoftBreak] renders as
 * collapsible whitespace.
 */
public class LineBreak internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
