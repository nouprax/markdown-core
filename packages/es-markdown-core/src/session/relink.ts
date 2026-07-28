import type { Markup } from "../model/markup.js";

/** One child value swap inside a relinked parent: `previous` is the child's
 * value in the parent's previous snapshot, `next` its value now. */
export interface ChildSwap {
    readonly previous: Markup;
    readonly next: Markup;
}

function swapped<T extends Markup>(children: readonly T[], swaps: readonly ChildSwap[]): readonly T[] {
    let result: T[] | null = null;
    for (const swap of swaps) {
        const index = children.indexOf(swap.previous as T);
        if (index >= 0) {
            result ??= children.slice();
            result[index] = swap.next as T;
        }
    }
    return result ?? children;
}

/**
 * Rebuilds a `bubbled` node from its previous snapshot value: the delta
 * contract guarantees its fields and direct child id list are unchanged, so
 * the new value is the previous one at the new revision with the swapped
 * child values in place. No native traversal happens here — this is the
 * "relink bubbled" step of the documented O(delta) mirror update.
 */
export function relink(previous: Markup, revision: number, swaps: readonly ChildSwap[]): Markup {
    switch (previous.kind) {
        case "document":
        case "blockQuote":
        case "paragraph":
        case "heading":
        case "listItem":
        case "footnoteDefinition":
        case "emphasis":
        case "strong":
        case "strikethrough":
        case "link":
        case "image":
        case "tableCell":
            return { ...previous, revision, content: swapped(previous.content, swaps) };
        case "list":
            return { ...previous, revision, items: swapped(previous.items, swaps) };
        case "table": {
            const header = swaps.find((swap) => swap.previous === previous.header)?.next;
            if (header !== undefined && header.kind !== "tableRow") {
                throw new Error("table relink replaced the header with a non-row node");
            }
            return {
                ...previous,
                revision,
                header: header ?? previous.header,
                rows: swapped(previous.rows, swaps)
            };
        }
        case "tableRow":
            return { ...previous, revision, cells: swapped(previous.cells, swaps) };
        case "directiveBlock":
            return {
                ...previous,
                revision,
                label: previous.label === null ? null : swapped(previous.label, swaps),
                content: swapped(previous.content, swaps)
            };
        case "directive":
            return {
                ...previous,
                revision,
                label: previous.label === null ? null : swapped(previous.label, swaps)
            };
        case "thematicBreak":
        case "codeBlock":
        case "htmlBlock":
        case "formulaBlock":
        case "text":
        case "softBreak":
        case "lineBreak":
        case "code":
        case "html":
        case "formula":
        case "footnoteReference":
            // Leaves have no descendants to bubble from; tolerated for
            // robustness against a wider-than-necessary delta.
            return { ...previous, revision };
    }
}
