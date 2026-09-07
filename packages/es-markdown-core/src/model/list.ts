import type { ListFlavor, OrderedListDelimiter, OrderedListVariant } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface List extends MarkupBase<"list"> {
    readonly flavor: ListFlavor;
    readonly start: number | null;
    readonly variant: OrderedListVariant | null;
    readonly delimiter: OrderedListDelimiter | null;
    readonly tight: boolean;
    readonly items: readonly ListItem[];
}

export interface ListItem extends MarkupBase<"listItem"> {
    readonly marker: string | null;
    readonly exampleLabel: string | null;
    readonly tasked: boolean;
    readonly completed: boolean;
    readonly content: readonly Markup[];
}
