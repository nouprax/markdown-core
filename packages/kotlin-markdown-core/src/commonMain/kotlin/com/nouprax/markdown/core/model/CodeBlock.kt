package com.nouprax.markdown.core

public class CodeBlock internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val mode: PlacementMode,
    public val info: String?,
    public val language: String?,
    public val literal: String,
    public val fenced: Boolean,
    public val closed: Boolean,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
