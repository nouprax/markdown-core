package com.nouprax.markdown.core

public class ThematicBreak internal constructor(
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitThematicBreak(this)
}
