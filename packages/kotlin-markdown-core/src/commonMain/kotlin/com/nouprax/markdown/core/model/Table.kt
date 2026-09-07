package com.nouprax.markdown.core

/** A logical column, with an authored width share when present. */
public class TableColumn internal constructor(
    public val alignment: TableAlignment,
    public val relative: Double?,
)

/** Inline or block content as parsed, without paragraph normalization. */
public class TableCell internal constructor(
    public val rowspan: Int,
    public val colspan: Int,
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitTableCell(this)
}

public class TableRow internal constructor(
    public val cells: kotlin.collections.List<TableCell>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitTableRow(this)
}

public class Table internal constructor(
    public val columns: kotlin.collections.List<TableColumn>,
    public val head: kotlin.collections.List<TableRow>,
    public val content: kotlin.collections.List<TableRow>,
    public val foot: kotlin.collections.List<TableRow>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitTable(this)
}
