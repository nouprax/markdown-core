package com.nouprax.markdown.core

public class SoftBreak internal constructor(
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitSoftBreak(this)
}
