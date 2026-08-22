package com.nouprax.markdown.core

public class Directive internal constructor(
    public val name: String,
    public val attributes: kotlin.collections.List<DirectiveAttribute>?,
    public val label: DirectiveLabel?,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitDirective(this)
}
