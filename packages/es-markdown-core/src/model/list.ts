import type { ListFlavor, OrderedListDelimiter, OrderedListStyle } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface List extends MarkupBase<"list"> {
    readonly flavor: ListFlavor;
    readonly start: number | null;
    readonly style: OrderedListStyle | null;
    readonly delimiter: OrderedListDelimiter | null;
    readonly tight: boolean;
    readonly items: readonly ListItem[];
}

export interface ListItem extends MarkupBase<"listItem"> {
    readonly marker: string | null;
    readonly exampleLabel: string | null;
    readonly isTask: boolean;
    readonly isComplete: boolean;
    readonly content: readonly Markup[];
}
