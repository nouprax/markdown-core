import type { TableAlignment } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/** A pipe table.
 *
 * The delimiter row that declares the alignments is not a row here — it
 * becomes `alignments` — so the tree holds only `header` and `rows`. */
export interface Table extends MarkupBase<"table"> {
    /** One entry per column, so its length is the table's width. */
    readonly alignments: readonly TableAlignment[];
    readonly header: TableRow;
    /** The body rows in source order.
     *
     * The header is not among them. */
    readonly rows: readonly TableRow[];
}

export interface TableRow extends MarkupBase<"tableRow"> {
    /** True only for the row `Table.header` holds; every row in `Table.rows`
     * has it false. */
    readonly isHeader: boolean;
    /** The row's cells, one per column: a short row is padded with empty
     * cells and whatever a long row writes past the last column is dropped,
     * so this always has as many entries as `Table.alignments`. */
    readonly cells: readonly TableCell[];
}

export interface TableCell extends MarkupBase<"tableCell"> {
    /** The cell's inline content in source order. */
    readonly content: readonly Markup[];
}
