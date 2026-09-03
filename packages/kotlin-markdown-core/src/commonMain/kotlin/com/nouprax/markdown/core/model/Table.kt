package com.nouprax.markdown.core

public class TableCell internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitTableCell(this)
}

public class TableRow internal constructor(
    public val isHeader: Boolean,
    public val cells: kotlin.collections.List<TableCell>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitTableRow(this)
}

public class Table internal constructor(
    public val alignments: kotlin.collections.List<TableAlignment>,
    public val header: TableRow,
    public val rows: kotlin.collections.List<TableRow>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitTable(this)
}
