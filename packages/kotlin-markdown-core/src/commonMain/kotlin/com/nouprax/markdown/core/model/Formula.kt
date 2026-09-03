package com.nouprax.markdown.core

public class Formula internal constructor(
    public val mode: PlacementMode,
    public val literal: String,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitFormula(this)
}
