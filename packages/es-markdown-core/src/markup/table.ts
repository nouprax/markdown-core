import type { Flow } from "../common/constraints.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface Table extends MarkupBase<"table"> {
    readonly columns: readonly TableColumn[];
    readonly head: readonly TableRow[];
    readonly content: readonly TableRow[];
}

export interface TableRow extends MarkupBase<"tableRow"> {
    readonly cells: readonly TableCell[];
}

export interface TableCell extends MarkupBase<"tableCell"> {
    /** Inline content as parsed. */
    readonly content: readonly Markup[];
}

export interface TableColumn {
    readonly flow: Flow;
}
