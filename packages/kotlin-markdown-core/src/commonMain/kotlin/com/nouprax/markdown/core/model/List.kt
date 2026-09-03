package com.nouprax.markdown.core

public class List internal constructor(
    public val flavor: ListFlavor,
    public val start: Long?,
    public val tight: Boolean,
    public val items: kotlin.collections.List<ListItem>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitList(this)
}

public class ListItem internal constructor(
    public val checked: Boolean?,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitListItem(this)
}
