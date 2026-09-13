package com.nouprax.markdown.core

public class HTML internal constructor(
    public val literal: String,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(
        visitor: Visitor<Result>,
        phase: MarkupWalkPhase,
    ): Result = visitor.visit(this, phase)
}
