import type { TableAlignment } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Table extends MarkupBase<"table"> {
    readonly columns: readonly TableColumn[];
    readonly head: readonly TableRow[];
    readonly content: readonly TableRow[];
    readonly foot: readonly TableRow[];
}

export interface TableRow extends MarkupBase<"tableRow"> {
    readonly cells: readonly TableCell[];
}

export interface TableCell extends MarkupBase<"tableCell"> {
    readonly rowspan: number;
    readonly colspan: number;
    /** Inline or block content as parsed, without paragraph normalization. */
    readonly content: readonly Markup[];
}

export interface TableColumn {
    readonly alignment: TableAlignment;
    readonly relative: number | null;
}
