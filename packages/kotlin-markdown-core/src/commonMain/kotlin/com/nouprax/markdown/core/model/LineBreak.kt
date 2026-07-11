package com.nouprax.markdown.core

public class LineBreak internal constructor(
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitLineBreak(this)
}
