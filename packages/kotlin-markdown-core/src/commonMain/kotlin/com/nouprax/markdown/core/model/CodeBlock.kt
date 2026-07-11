package com.nouprax.markdown.core

public class CodeBlock internal constructor(
    public val mode: PlacementMode,
    public val info: String?,
    public val language: String?,
    public val literal: String,
    public val fenced: Boolean,
    public val closed: Boolean,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitCodeBlock(this)
}
