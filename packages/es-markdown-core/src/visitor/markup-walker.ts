import type { Markup } from "../markup/markup.js";
import type { MarkupVisitPhase, MarkupVisitor } from "./markup-visitor.js";

/* The pending visits as two parallel stacks, a node and its phase at the
 * same index, so scheduling a visit allocates nothing per node. */
interface Actions {
    readonly nodes: Markup[];
    readonly phases: MarkupVisitPhase[];
}

/** Walks markup depth first with an explicit stack, reporting both phases to the visitor. */
export function walk(root: Markup, visitor: MarkupVisitor): void {
    const actions: Actions = { nodes: [root], phases: ["enter"] };
    while (actions.nodes.length > 0) {
        const node = actions.nodes.pop()!;
        const phase = actions.phases.pop()!;
        if (phase === "enter") {
            actions.nodes.push(node);
            actions.phases.push("exit");
        }
        dispatch(node, visitor, phase, actions);
    }
}

// The mapped union keeps the kind and its node correlated during indexed dispatch.
function dispatch<Kind extends Markup["kind"]>(
    node: { [Key in Kind]: Extract<Markup, { kind: Key }> }[Kind],
    visitor: MarkupVisitor,
    phase: MarkupVisitPhase,
    actions: Actions
): void {
    visitor[node.kind](node, phase);
    if (phase === "enter") schedule[node.kind](node, actions);
}

function enter(node: Markup, actions: Actions): void {
    actions.nodes.push(node);
    actions.phases.push("enter");
}

function push(nodes: readonly Markup[], actions: Actions): void {
    for (let index = nodes.length - 1; index >= 0; index -= 1) {
        enter(nodes[index]!, actions);
    }
}

// Named fields determine traversal order. Every kind must specify its schedule.
const schedule: {
    [Kind in Markup["kind"]]: (node: Extract<Markup, { kind: Kind }>, actions: Actions) => void;
} = {
    document(node, actions) {
        push(node.specimens, actions);
        push(node.footnotes, actions);
        push(node.content, actions);
        if (node.metadata !== null) enter(node.metadata, actions);
    },
    callout(node, actions) {
        push(node.content, actions);
        if (node.title !== null) push(node.title, actions);
    },
    paragraph(node, actions) {
        push(node.content, actions);
    },
    heading(node, actions) {
        push(node.content, actions);
    },
    list(node, actions) {
        push(node.items, actions);
    },
    listItem(node, actions) {
        push(node.content, actions);
    },
    table(node, actions) {
        push(node.foot, actions);
        push(node.content, actions);
        push(node.head, actions);
        if (node.caption !== null) enter(node.caption, actions);
    },
    tableCaption(node, actions) {
        push(node.content, actions);
    },
    tableRow(node, actions) {
        push(node.cells, actions);
    },
    tableCell(node, actions) {
        push(node.content, actions);
    },
    directiveBlock(node, actions) {
        push(node.content, actions);
        if (node.label !== null) enter(node.label, actions);
    },
    directiveLabel(node, actions) {
        push(node.content, actions);
    },
    emphasis(node, actions) {
        push(node.content, actions);
    },
    strong(node, actions) {
        push(node.content, actions);
    },
    strikethrough(node, actions) {
        push(node.content, actions);
    },
    mark(node, actions) {
        push(node.content, actions);
    },
    insertion(node, actions) {
        push(node.content, actions);
    },
    span(node, actions) {
        push(node.content, actions);
    },
    superscript(node, actions) {
        push(node.content, actions);
    },
    subscript(node, actions) {
        push(node.content, actions);
    },
    link(node, actions) {
        push(node.content, actions);
    },
    embedded(node, actions) {
        push(node.content, actions);
    },
    directive(node, actions) {
        if (node.label !== null) enter(node.label, actions);
    },
    cite(node, actions) {
        push(node.citations, actions);
    },
    definitionList(node, actions) {
        push(node.definitions, actions);
    },
    definition(node, actions) {
        for (let index = node.content.length - 1; index >= 0; index -= 1) push(node.content[index]!, actions);
        push(node.term, actions);
    },
    citation(node, actions) {
        push(node.suffix, actions);
        push(node.prefix, actions);
    },
    footnote(node, actions) {
        push(node.content, actions);
    },
    specimen(node, actions) {
        push(node.content, actions);
    },
    thematicBreak: () => undefined,
    codeBlock: () => undefined,
    htmlBlock: () => undefined,
    formulaBlock: () => undefined,
    text: () => undefined,
    softBreak: () => undefined,
    lineBreak: () => undefined,
    code: () => undefined,
    html: () => undefined,
    crossLink: () => undefined,
    crossEmbedded: () => undefined,
    comment: () => undefined,
    formula: () => undefined,
    metadata: () => undefined
};
