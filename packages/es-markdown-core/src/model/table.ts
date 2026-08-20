import type { TableAlignment } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Table extends MarkupBase<"table"> {
    readonly alignments: readonly TableAlignment[];
    readonly header: TableRow;
    readonly rows: readonly TableRow[];
}

export interface TableRow extends MarkupBase<"tableRow"> {
    readonly isHeader: boolean;
    readonly cells: readonly TableCell[];
}

export interface TableCell extends MarkupBase<"tableCell"> {
    readonly content: readonly Markup[];
}
