package com.nouprax.markdown.core

public class Formula internal constructor(
    public val mode: PlacementMode,
    public val literal: String,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitFormula(this)
}
