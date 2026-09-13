package com.nouprax.markdown.core

public class Directive internal constructor(
    public val name: String,
    /** Markup owned by the label field, not a generic child/content element. */
    public val label: DirectiveLabel?,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visit(this)
}
