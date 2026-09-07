package com.nouprax.markdown.core

public class FormulaBlock internal constructor(
    public val literal: String,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitFormulaBlock(this)
}
