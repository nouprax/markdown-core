package com.nouprax.markdown.core

/**
 * A directive's bracketed label. Its scope spans the brackets, so a label
 * written empty is still a place in the source.
 */
public class DirectiveLabel internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitDirectiveLabel(this)
}
