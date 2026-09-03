package com.nouprax.markdown.core

public class HTMLBlock internal constructor(
    public val literal: String,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitHTMLBlock(this)
}
