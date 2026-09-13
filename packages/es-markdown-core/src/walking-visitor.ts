import type { Citation } from "./model/cite.js";
import type { Footnote } from "./model/footnote.js";
import type { Markup } from "./model/markup.js";
import type { Specimen } from "./model/specimen.js";

/** The phase of a depth-first markup walk. */
export type WalkPhase = "entering" | "exiting";

/**
 * An exhaustive, node-kind-dispatched observer for a depth-first markup walk.
 *
 * There is no untyped callback, optional handler, or default implementation.
 * Markup-valued fields are not projected into a generic children collection:
 * each node-kind traversal branch schedules its own typed relations.
 */
export type WalkingVisitor = {
    [Node in Markup as Node["kind"]]: (this: void, node: Node, phase: WalkPhase) => void;
} & {
    /** A citation is a scoped value outside the markup union. */
    citation: (this: void, citation: Citation, phase: WalkPhase) => void;
    /** A footnote is a scoped value outside the markup union. */
    footnote: (this: void, footnote: Footnote, phase: WalkPhase) => void;
    /** A specimen is a scoped value outside the markup union. */
    specimen: (this: void, specimen: Specimen, phase: WalkPhase) => void;
};

type WalkAction =
    | { readonly kind: "markup"; readonly node: Markup; readonly phase: WalkPhase }
    | { readonly kind: "citation"; readonly value: Citation; readonly phase: WalkPhase }
    | { readonly kind: "footnote"; readonly value: Footnote; readonly phase: WalkPhase }
    | { readonly kind: "specimen"; readonly value: Specimen; readonly phase: WalkPhase };

/**
 * Walks `root` and all of its owned markup depth first.
 *
 * An explicit action stack keeps the JavaScript call-stack depth independent
 * of document depth. Every node receives `entering` before its typed relations
 * and `exiting` after them.
 */
export function walk(root: Markup, walkingVisitor: WalkingVisitor): void {
    const actions: WalkAction[] = [{ kind: "markup", node: root, phase: "entering" }];
    let phase: WalkPhase = "entering";

    const scheduleExit = (node: Markup): void => {
        if (phase === "entering") actions.push({ kind: "markup", node, phase: "exiting" });
    };
    const schedule = (nodes: readonly Markup[]): void => {
        for (let index = nodes.length - 1; index >= 0; index -= 1) {
            actions.push({ kind: "markup", node: nodes[index]!, phase: "entering" });
        }
    };
    // The scoped values are scheduled like nodes and receive their own
    // callbacks; each descends into its markup arrays in declared field order.
    const scheduleCitations = (items: readonly Citation[]): void => {
        for (let index = items.length - 1; index >= 0; index -= 1) {
            actions.push({ kind: "citation", value: items[index]!, phase: "entering" });
        }
    };
    const scheduleFootnotes = (footnotes: readonly Footnote[]): void => {
        for (let index = footnotes.length - 1; index >= 0; index -= 1) {
            actions.push({ kind: "footnote", value: footnotes[index]!, phase: "entering" });
        }
    };
    const scheduleSpecimens = (specimens: readonly Specimen[]): void => {
        for (let index = specimens.length - 1; index >= 0; index -= 1) {
            actions.push({ kind: "specimen", value: specimens[index]!, phase: "entering" });
        }
    };
    const citation = (value: Citation): void => {
        walkingVisitor.citation(value, phase);
        if (phase === "entering") {
            actions.push({ kind: "citation", value, phase: "exiting" });
            schedule(value.suffix);
            schedule(value.prefix);
        }
    };
    const footnote = (value: Footnote): void => {
        walkingVisitor.footnote(value, phase);
        if (phase === "entering") {
            actions.push({ kind: "footnote", value, phase: "exiting" });
            schedule(value.content);
        }
    };
    const specimen = (value: Specimen): void => {
        walkingVisitor.specimen(value, phase);
        if (phase === "entering") {
            actions.push({ kind: "specimen", value, phase: "exiting" });
            schedule(value.content);
        }
    };

    // Each branch reports its node and schedules that node's typed relations.
    // Dispatch directly without allocating an intermediate table of callbacks.
    const markup = (node: Markup): void => {
        switch (node.kind) {
            case "document": {
                walkingVisitor.document(node, phase);
                scheduleExit(node);
                if (phase === "entering") {
                    // The footnotes are visited after the content.
                    scheduleSpecimens(node.specimens);
                    scheduleFootnotes(node.footnotes);
                    schedule(node.content);
                }

                return;
            }
            case "callout": {
                walkingVisitor.callout(node, phase);
                scheduleExit(node);
                if (phase === "entering") {
                    schedule(node.content);
                    // The title is a node-valued field, visited before the content.
                    if (node.title !== null) schedule(node.title);
                }

                return;
            }
            case "paragraph": {
                walkingVisitor.paragraph(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "heading": {
                walkingVisitor.heading(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "thematicBreak": {
                walkingVisitor.thematicBreak(node, phase);
                scheduleExit(node);

                return;
            }
            case "list": {
                walkingVisitor.list(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.items);

                return;
            }
            case "listItem": {
                walkingVisitor.listItem(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "codeBlock": {
                walkingVisitor.codeBlock(node, phase);
                scheduleExit(node);

                return;
            }
            case "htmlBlock": {
                walkingVisitor.htmlBlock(node, phase);
                scheduleExit(node);

                return;
            }
            case "formulaBlock": {
                walkingVisitor.formulaBlock(node, phase);
                scheduleExit(node);

                return;
            }
            case "table": {
                walkingVisitor.table(node, phase);
                scheduleExit(node);
                if (phase === "entering") {
                    schedule(node.foot);
                    schedule(node.content);
                    schedule(node.head);
                    if (node.caption) schedule([node.caption]);
                }

                return;
            }
            case "tableCaption": {
                walkingVisitor.tableCaption(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "tableRow": {
                walkingVisitor.tableRow(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.cells);

                return;
            }
            case "tableCell": {
                walkingVisitor.tableCell(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "directiveBlock": {
                walkingVisitor.directiveBlock(node, phase);
                scheduleExit(node);
                if (phase === "entering") {
                    schedule(node.content);
                    if (node.label !== null) actions.push({ kind: "markup", node: node.label, phase: "entering" });
                }

                return;
            }
            case "directiveLabel": {
                walkingVisitor.directiveLabel(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "text": {
                walkingVisitor.text(node, phase);
                scheduleExit(node);

                return;
            }
            case "softBreak": {
                walkingVisitor.softBreak(node, phase);
                scheduleExit(node);

                return;
            }
            case "lineBreak": {
                walkingVisitor.lineBreak(node, phase);
                scheduleExit(node);

                return;
            }
            case "code": {
                walkingVisitor.code(node, phase);
                scheduleExit(node);

                return;
            }
            case "html": {
                walkingVisitor.html(node, phase);
                scheduleExit(node);

                return;
            }
            case "crossLink": {
                walkingVisitor.crossLink(node, phase);
                scheduleExit(node);

                return;
            }
            case "crossEmbedded": {
                walkingVisitor.crossEmbedded(node, phase);
                scheduleExit(node);

                return;
            }
            case "comment": {
                walkingVisitor.comment(node, phase);
                scheduleExit(node);

                return;
            }
            case "formula": {
                walkingVisitor.formula(node, phase);
                scheduleExit(node);

                return;
            }
            case "emphasis": {
                walkingVisitor.emphasis(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "strong": {
                walkingVisitor.strong(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "strikethrough": {
                walkingVisitor.strikethrough(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "mark": {
                walkingVisitor.mark(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "insertion": {
                walkingVisitor.insertion(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "span": {
                walkingVisitor.span(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "superscript": {
                walkingVisitor.superscript(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "definitionList": {
                walkingVisitor.definitionList(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.definitions);

                return;
            }
            case "definition": {
                walkingVisitor.definition(node, phase);
                scheduleExit(node);
                if (phase === "entering") {
                    for (let index = node.content.length - 1; index >= 0; --index) schedule(node.content[index]!);
                    schedule(node.term);
                }

                return;
            }
            case "subscript": {
                walkingVisitor.subscript(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "link": {
                walkingVisitor.link(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "embedded": {
                walkingVisitor.embedded(node, phase);
                scheduleExit(node);
                if (phase === "entering") schedule(node.content);

                return;
            }
            case "directive": {
                walkingVisitor.directive(node, phase);
                scheduleExit(node);
                if (phase === "entering" && node.label !== null) {
                    actions.push({ kind: "markup", node: node.label, phase: "entering" });
                }

                return;
            }
            case "cite": {
                walkingVisitor.cite(node, phase);
                scheduleExit(node);
                if (phase === "entering") scheduleCitations(node.citations);

                return;
            }
        }
        node satisfies never;
        throw new Error(`unreachable markup ${String(node)}`);
    };

    while (actions.length > 0) {
        const action = actions.pop()!;
        phase = action.phase;
        if (action.kind === "markup") markup(action.node);
        else if (action.kind === "citation") citation(action.value);
        else if (action.kind === "footnote") footnote(action.value);
        else specimen(action.value);
    }
}
