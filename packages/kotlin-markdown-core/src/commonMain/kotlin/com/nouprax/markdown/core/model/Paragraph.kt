package com.nouprax.markdown.core

public class Paragraph internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitParagraph(this)
}
