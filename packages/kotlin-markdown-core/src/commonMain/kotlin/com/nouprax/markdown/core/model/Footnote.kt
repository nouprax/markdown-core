package com.nouprax.markdown.core

public class FootnoteDefinition internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val label: String,
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

public class FootnoteReference internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val label: String,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
