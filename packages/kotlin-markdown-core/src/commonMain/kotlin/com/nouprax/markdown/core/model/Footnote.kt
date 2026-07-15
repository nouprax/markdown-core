package com.nouprax.markdown.core

public class FootnoteDefinition internal constructor(
    public val id: String,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitFootnoteDefinition(this)
}

public class FootnoteReference internal constructor(
    public val id: String,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitFootnoteReference(this)
}
