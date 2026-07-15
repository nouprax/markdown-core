import type { ListFlavor } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface List extends MarkupBase<"list"> {
    readonly flavor: ListFlavor;
    readonly start: number | null;
    readonly tight: boolean;
    readonly items: readonly ListItem[];
}

export interface ListItem extends MarkupBase<"listItem"> {
    readonly checked: boolean | null;
    readonly content: readonly Markup[];
}
