import type { Flow } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Table extends MarkupBase<"table"> {
    readonly caption: TableCaption | null;
    readonly columns: readonly TableColumn[];
    readonly head: readonly TableRow[];
    readonly content: readonly TableRow[];
    readonly foot: readonly TableRow[];
}

export interface TableCaption extends MarkupBase<"tableCaption"> {
    readonly content: readonly Markup[];
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
    readonly flow: Flow;
    readonly relative: number | null;
}
