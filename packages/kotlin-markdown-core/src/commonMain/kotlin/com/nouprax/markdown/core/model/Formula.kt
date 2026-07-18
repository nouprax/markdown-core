package com.nouprax.markdown.core

public class Formula internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val mode: PlacementMode,
    public val literal: String,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitFormula(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
