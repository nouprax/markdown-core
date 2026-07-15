package com.nouprax.markdown.core

public class Directive internal constructor(
    public val mode: PlacementMode,
    public val name: String,
    public val attributes: String?,
    public val label: kotlin.collections.List<Markup>?,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitDirective(this)
}
