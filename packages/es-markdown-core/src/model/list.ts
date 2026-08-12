import type { ListFlavor } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

export interface List extends MarkupBase<"list"> {
    readonly flavor: ListFlavor;
    /** The number the list's first item was written with — `3.` gives 3.
     *
     * Null for a bullet list, and never null for an ordered one. */
    readonly start: number | null;
    /** True when the list is tight: a renderer drops the paragraph wrapping
     * inside its items.
     *
     * A blank line between two items, or between two blocks within one item,
     * makes the list loose. */
    readonly tight: boolean;
    /** The list's items in source order. */
    readonly items: readonly ListItem[];
}

export interface ListItem extends MarkupBase<"listItem"> {
    /** Whether a task-list item's box is checked.
     *
     * Null for an item that carries no checkbox, which is every item when
     * the `taskLists` option is off. */
    readonly checked: boolean | null;
    /** The item's block content in source order. */
    readonly content: readonly Markup[];
}
