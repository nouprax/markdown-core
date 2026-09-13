package com.nouprax.markdown.core

public class Formula internal constructor(
    public val mode: Placement,
    public val literal: String,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visit(this)
}
