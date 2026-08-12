package com.nouprax.markdown.core

public class TableCell internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** The cell's inline content in source order.
     *
     * Empty for a cell that was never written — the padding that squares a
     * short row off. */
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

public class TableRow internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** True for the row [Table.header] holds, false for every row in
     * [Table.rows]. */
    public val isHeader: Boolean,
    /** The row's cells in column order, always exactly as many as the table
     * has columns.
     *
     * A source row that wrote fewer is padded with empty cells and one that
     * wrote more has the surplus dropped, so a column index is valid in every
     * row of the table. */
    public val cells: kotlin.collections.List<TableCell>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

/**
 * A pipe table, from the tables extension.
 *
 * A table has one width, fixed by the delimiter row that declared it:
 * [alignments], [header]'s cells, and each row's cells all have that many
 * entries. A column index is therefore an index into any of them.
 */
public class Table internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    public val alignments: kotlin.collections.List<TableAlignment>,
    public val header: TableRow,
    /** The body rows in source order.
     *
     * [header] is not among them. */
    public val rows: kotlin.collections.List<TableRow>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
