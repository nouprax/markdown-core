package com.nouprax.markdown.core

public class DirectiveBlock internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val mode: PlacementMode,
    public val name: String,
    public val attributes: String?,
    public val label: kotlin.collections.List<Markup>?,
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
