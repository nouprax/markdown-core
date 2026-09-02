package com.nouprax.markdown.core

public class Directive internal constructor(
    public val name: String,
    public val attributes: kotlin.collections.List<DirectiveAttribute>?,
    /** Markup owned by the label field, not a generic child/content element. */
    public val label: DirectiveLabel?,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitDirective(this)
}
