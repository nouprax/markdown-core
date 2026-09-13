package com.nouprax.markdown.core

public class Insertion internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(
        visitor: Visitor<Result>,
        phase: MarkupWalkPhase,
    ): Result = visitor.visit(this, phase)
}
