import type { Markup } from "./model/markup.js";

/** One required, precisely typed callback for every markup kind. */
export type Visitor<Result> = {
    [Node in Markup as Node["kind"]]: (this: void, node: Node) => Result;
};

/**
 * Dispatches one markup to its kind's required callback.
 * Each branch narrows the node and exposes a concrete call site for the
 * JavaScript engine to optimize.
 */
export function visit<Result>(node: Markup, visitor: Visitor<Result>): Result {
    switch (node.kind) {
        case "document":
            return visitor.document(node);
        case "callout":
            return visitor.callout(node);
        case "paragraph":
            return visitor.paragraph(node);
        case "heading":
            return visitor.heading(node);
        case "thematicBreak":
            return visitor.thematicBreak(node);
        case "list":
            return visitor.list(node);
        case "listItem":
            return visitor.listItem(node);
        case "codeBlock":
            return visitor.codeBlock(node);
        case "htmlBlock":
            return visitor.htmlBlock(node);
        case "formulaBlock":
            return visitor.formulaBlock(node);
        case "table":
            return visitor.table(node);
        case "tableCaption":
            return visitor.tableCaption(node);
        case "tableRow":
            return visitor.tableRow(node);
        case "tableCell":
            return visitor.tableCell(node);
        case "directiveBlock":
            return visitor.directiveBlock(node);
        case "directiveLabel":
            return visitor.directiveLabel(node);
        case "text":
            return visitor.text(node);
        case "softBreak":
            return visitor.softBreak(node);
        case "lineBreak":
            return visitor.lineBreak(node);
        case "code":
            return visitor.code(node);
        case "html":
            return visitor.html(node);
        case "crossLink":
            return visitor.crossLink(node);
        case "crossEmbedded":
            return visitor.crossEmbedded(node);
        case "comment":
            return visitor.comment(node);
        case "formula":
            return visitor.formula(node);
        case "emphasis":
            return visitor.emphasis(node);
        case "strong":
            return visitor.strong(node);
        case "strikethrough":
            return visitor.strikethrough(node);
        case "mark":
            return visitor.mark(node);
        case "insertion":
            return visitor.insertion(node);
        case "span":
            return visitor.span(node);
        case "superscript":
            return visitor.superscript(node);
        case "definitionList":
            return visitor.definitionList(node);
        case "definition":
            return visitor.definition(node);
        case "subscript":
            return visitor.subscript(node);
        case "link":
            return visitor.link(node);
        case "embedded":
            return visitor.embedded(node);
        case "directive":
            return visitor.directive(node);
        case "cite":
            return visitor.cite(node);
    }
    return unreachable(node);
}

function unreachable(value: never): never {
    throw new Error(`unreachable markup ${String(value)}`);
}
