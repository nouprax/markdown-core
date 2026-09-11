package com.nouprax.markdown.core

public class Definition internal constructor(
    public val term: kotlin.collections.List<Markup>,
    public val content: kotlin.collections.List<kotlin.collections.List<Markup>>,
    public val compact: Boolean,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitDefinition(this)
}
