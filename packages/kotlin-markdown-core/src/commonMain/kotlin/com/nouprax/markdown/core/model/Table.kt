package com.nouprax.markdown.core

public class TableCell internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitTableCell(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

public class TableRow internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val isHeader: Boolean,
    public val cells: kotlin.collections.List<TableCell>,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitTableRow(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

public class Table internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val alignments: kotlin.collections.List<TableAlignment>,
    public val header: TableRow,
    public val rows: kotlin.collections.List<TableRow>,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitTable(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
