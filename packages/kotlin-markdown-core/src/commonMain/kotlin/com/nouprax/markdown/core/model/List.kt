package com.nouprax.markdown.core

public class List internal constructor(
    public val flavor: ListFlavor,
    public val start: Long?,
    public val variant: OrderedListVariant?,
    public val delimiter: OrderedListDelimiter?,
    public val tight: Boolean,
    public val items: kotlin.collections.List<ListItem>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visit(this)
}

public class ListItem internal constructor(
    public val marker: String?,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    public val tasked: Boolean get() = marker != null
    public val completed: Boolean get() = marker != null && marker != " "

    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visit(this)
}
