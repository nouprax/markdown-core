import type { Markup } from "../model/markup.js";

/** Previous child value → current child value for one bubbled parent. */
type ChildReplacements = ReadonlyMap<Markup, Markup>;

function replaced<T extends Markup>(children: readonly T[], replacements: ChildReplacements): readonly T[] {
    let result: T[] | null = null;
    let index = 0;
    for (const child of children) {
        const replacement = replacements.get(child);
        if (replacement !== undefined) {
            result ??= children.slice();
            result[index] = replacement as T;
        }
        index += 1;
    }
    return result ?? children;
}

function replacementFor<T extends Markup>(value: T, replacements: ChildReplacements): T {
    const replacement = replacements.get(value);
    if (replacement === undefined) return value;
    return replacement as T;
}

function tableHeader(previous: Extract<Markup, { readonly kind: "table" }>, replacements: ChildReplacements) {
    const header = replacementFor(previous.header, replacements);
    if (header.kind !== "tableRow") {
        throw new Error("table relink replaced the header with a non-row node");
    }
    return header;
}

function directiveLabel(
    previous: Extract<Markup, { readonly kind: "directive" | "directiveBlock" }>,
    replacements: ChildReplacements
) {
    if (previous.label === null) return null;
    const label = replacementFor(previous.label, replacements);
    if (label.kind !== "directiveLabel") {
        throw new Error("directive relink replaced the label with a non-label node");
    }
    return label;
}

/**
 * Rebuilds a `bubbled` node from its previous snapshot value: the delta
 * contract guarantees its fields and direct child id list are unchanged, so
 * the new value is the previous one at the new revision with the replaced
 * child values in place. Every child field gets one replacement-lookup pass
 * and at most one linear copy; lookup is O(1) expected even when one parent
 * has many changed children.
 */
export function relink(previous: Markup, revision: number, replacements: ChildReplacements): Markup {
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
        case "directiveLabel":
            return { ...previous, revision, content: replaced(previous.content, replacements) };
        case "list":
            return { ...previous, revision, items: replaced(previous.items, replacements) };
        case "table":
            return {
                ...previous,
                revision,
                header: tableHeader(previous, replacements),
                rows: replaced(previous.rows, replacements)
            };
        case "tableRow":
            return { ...previous, revision, cells: replaced(previous.cells, replacements) };
        case "directiveBlock":
            return {
                ...previous,
                revision,
                label: directiveLabel(previous, replacements),
                content: replaced(previous.content, replacements)
            };
        case "directive":
            return {
                ...previous,
                revision,
                label: directiveLabel(previous, replacements)
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
