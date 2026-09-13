import type { Markup } from "./model/markup.js";

/** One required, precisely typed callback for every markup kind. */
export type Visitor<Result> = {
    [Node in Markup as Node["kind"]]: (this: void, node: Node, phase: MarkupWalkPhase) => Result;
};

/**
 * Dispatches one markup to its kind's required callback.
 * Each branch narrows the node and exposes a concrete call site for the
 * JavaScript engine to optimize.
 */
export function visit<Result>(node: Markup, visitor: Visitor<Result>, phase: MarkupWalkPhase = "entering"): Result {
    switch (node.kind) {
        case "document":
            return visitor.document(node, phase);
        case "callout":
            return visitor.callout(node, phase);
        case "paragraph":
            return visitor.paragraph(node, phase);
        case "heading":
            return visitor.heading(node, phase);
        case "thematicBreak":
            return visitor.thematicBreak(node, phase);
        case "list":
            return visitor.list(node, phase);
        case "listItem":
            return visitor.listItem(node, phase);
        case "codeBlock":
            return visitor.codeBlock(node, phase);
        case "htmlBlock":
            return visitor.htmlBlock(node, phase);
        case "formulaBlock":
            return visitor.formulaBlock(node, phase);
        case "table":
            return visitor.table(node, phase);
        case "tableCaption":
            return visitor.tableCaption(node, phase);
        case "tableRow":
            return visitor.tableRow(node, phase);
        case "tableCell":
            return visitor.tableCell(node, phase);
        case "directiveBlock":
            return visitor.directiveBlock(node, phase);
        case "directiveLabel":
            return visitor.directiveLabel(node, phase);
        case "text":
            return visitor.text(node, phase);
        case "softBreak":
            return visitor.softBreak(node, phase);
        case "lineBreak":
            return visitor.lineBreak(node, phase);
        case "code":
            return visitor.code(node, phase);
        case "html":
            return visitor.html(node, phase);
        case "crossLink":
            return visitor.crossLink(node, phase);
        case "crossEmbedded":
            return visitor.crossEmbedded(node, phase);
        case "comment":
            return visitor.comment(node, phase);
        case "formula":
            return visitor.formula(node, phase);
        case "emphasis":
            return visitor.emphasis(node, phase);
        case "strong":
            return visitor.strong(node, phase);
        case "strikethrough":
            return visitor.strikethrough(node, phase);
        case "mark":
            return visitor.mark(node, phase);
        case "insertion":
            return visitor.insertion(node, phase);
        case "span":
            return visitor.span(node, phase);
        case "superscript":
            return visitor.superscript(node, phase);
        case "definitionList":
            return visitor.definitionList(node, phase);
        case "definition":
            return visitor.definition(node, phase);
        case "subscript":
            return visitor.subscript(node, phase);
        case "link":
            return visitor.link(node, phase);
        case "embedded":
            return visitor.embedded(node, phase);
        case "directive":
            return visitor.directive(node, phase);
        case "citation":
            return visitor.citation(node, phase);
        case "footnote":
            return visitor.footnote(node, phase);
        case "specimen":
            return visitor.specimen(node, phase);
        case "metadata":
            return visitor.metadata(node, phase);
        case "cite":
            return visitor.cite(node, phase);
    }
    return unreachable(node);
}

function unreachable(value: never): never {
    throw new Error(`unreachable markup ${String(value)}`);
}

/** The phase supplied to a markup visit. */
export type MarkupWalkPhase = "entering" | "exiting";

type WalkAction = { readonly node: Markup; readonly phase: MarkupWalkPhase };

/**
 * Walks owned markup depth first, reporting both phases through the same visitor.
 * An explicit stack keeps call-stack depth independent of document depth.
 * `undefined` requires callbacks without results; TypeScript's `void` would
 * also accept value-returning callbacks and silently discard their results.
 */
export function walk(root: Markup, visitor: Visitor<undefined>): void {
    const actions: WalkAction[] = [{ node: root, phase: "entering" }];
    while (actions.length > 0) {
        const action = actions.pop()!;
        visit(action.node, visitor, action.phase);
        if (action.phase === "exiting") continue;
        actions.push({ node: action.node, phase: "exiting" });
        scheduleOwnedMarkup(action.node, actions);
    }
}

// Typed ownership fields determine traversal order; no flattened children projection is built.
function scheduleOwnedMarkup(node: Markup, actions: WalkAction[]): void {
    const schedule = (nodes: readonly Markup[]): void => {
        for (let index = nodes.length - 1; index >= 0; index -= 1) {
            actions.push({ node: nodes[index]!, phase: "entering" });
        }
    };
    switch (node.kind) {
        case "document":
            schedule(node.specimens);
            schedule(node.footnotes);
            schedule(node.content);
            if (node.metadata !== null) actions.push({ node: node.metadata, phase: "entering" });
            return;
        case "callout":
            schedule(node.content);
            if (node.title !== null) schedule(node.title);
            return;
        case "paragraph":
            schedule(node.content);
            return;
        case "heading":
            schedule(node.content);
            return;
        case "list":
            schedule(node.items);
            return;
        case "listItem":
            schedule(node.content);
            return;
        case "table":
            schedule(node.foot);
            schedule(node.content);
            schedule(node.head);
            if (node.caption !== null) actions.push({ node: node.caption, phase: "entering" });
            return;
        case "tableCaption":
            schedule(node.content);
            return;
        case "tableRow":
            schedule(node.cells);
            return;
        case "tableCell":
            schedule(node.content);
            return;
        case "directiveBlock":
            schedule(node.content);
            if (node.label !== null) actions.push({ node: node.label, phase: "entering" });
            return;
        case "directiveLabel":
            schedule(node.content);
            return;
        case "emphasis":
            schedule(node.content);
            return;
        case "strong":
            schedule(node.content);
            return;
        case "strikethrough":
            schedule(node.content);
            return;
        case "mark":
            schedule(node.content);
            return;
        case "insertion":
            schedule(node.content);
            return;
        case "span":
            schedule(node.content);
            return;
        case "superscript":
            schedule(node.content);
            return;
        case "subscript":
            schedule(node.content);
            return;
        case "link":
            schedule(node.content);
            return;
        case "embedded":
            schedule(node.content);
            return;
        case "directive":
            if (node.label !== null) actions.push({ node: node.label, phase: "entering" });
            return;
        case "cite":
            schedule(node.citations);
            return;
        case "definitionList":
            schedule(node.definitions);
            return;
        case "definition":
            for (let index = node.content.length - 1; index >= 0; index -= 1) schedule(node.content[index]!);
            schedule(node.term);
            return;
        case "citation":
            schedule(node.suffix);
            schedule(node.prefix);
            return;
        case "footnote":
            schedule(node.content);
            return;
        case "specimen":
            schedule(node.content);
            return;
        case "thematicBreak":
        case "codeBlock":
        case "htmlBlock":
        case "formulaBlock":
        case "text":
        case "softBreak":
        case "lineBreak":
        case "code":
        case "html":
        case "crossLink":
        case "crossEmbedded":
        case "comment":
        case "formula":
        case "metadata":
            return;
    }
    unreachable(node);
}
