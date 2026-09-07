import type { Callout } from "./model/callout.js";
import type { Citation, Cite } from "./model/cite.js";
import type { CodeBlock } from "./model/code-block.js";
import type { Code } from "./model/code.js";
import type { Comment } from "./model/comment.js";
import type { DirectiveBlock } from "./model/directive-block.js";
import type { DirectiveLabel } from "./model/directive-label.js";
import type { Directive } from "./model/directive.js";
import type { Document } from "./model/document.js";
import type { Emphasis } from "./model/emphasis.js";
import type { Footnote } from "./model/footnote.js";
import type { FormulaBlock } from "./model/formula-block.js";
import type { Formula } from "./model/formula.js";
import type { Heading } from "./model/heading.js";
import type { HTMLBlock } from "./model/html-block.js";
import type { HTML } from "./model/html.js";
import type { Image } from "./model/image.js";
import type { LineBreak } from "./model/line-break.js";
import type { Link } from "./model/link.js";
import type { List, ListItem } from "./model/list.js";
import type { Markup } from "./model/markup.js";
import type { Paragraph } from "./model/paragraph.js";
import type { SoftBreak } from "./model/soft-break.js";
import type { Strikethrough } from "./model/strikethrough.js";
import type { Strong } from "./model/strong.js";
import type { Table, TableCell, TableRow } from "./model/table.js";
import type { Text } from "./model/text.js";
import type { ThematicBreak } from "./model/thematic-break.js";
import { visit, type Visitor } from "./visitor.js";

/** The phase of a depth-first markup walk. */
export type WalkPhase = "entering" | "exiting";

/**
 * An exhaustive, node-kind-dispatched observer for a depth-first markup walk.
 *
 * There is no untyped callback, optional handler, or default implementation.
 * Markup-valued fields are not projected into a generic children collection:
 * each node-kind traversal branch schedules its own typed relations.
 */
export interface WalkingVisitor {
    visitDocument(this: void, node: Document, phase: WalkPhase): void;
    visitCallout(this: void, node: Callout, phase: WalkPhase): void;
    visitParagraph(this: void, node: Paragraph, phase: WalkPhase): void;
    visitHeading(this: void, node: Heading, phase: WalkPhase): void;
    visitThematicBreak(this: void, node: ThematicBreak, phase: WalkPhase): void;
    visitList(this: void, node: List, phase: WalkPhase): void;
    visitListItem(this: void, node: ListItem, phase: WalkPhase): void;
    visitCodeBlock(this: void, node: CodeBlock, phase: WalkPhase): void;
    visitHTMLBlock(this: void, node: HTMLBlock, phase: WalkPhase): void;
    visitFormulaBlock(this: void, node: FormulaBlock, phase: WalkPhase): void;
    visitTable(this: void, node: Table, phase: WalkPhase): void;
    visitTableRow(this: void, node: TableRow, phase: WalkPhase): void;
    visitTableCell(this: void, node: TableCell, phase: WalkPhase): void;
    visitDirectiveBlock(this: void, node: DirectiveBlock, phase: WalkPhase): void;
    visitDirectiveLabel(this: void, node: DirectiveLabel, phase: WalkPhase): void;
    visitText(this: void, node: Text, phase: WalkPhase): void;
    visitSoftBreak(this: void, node: SoftBreak, phase: WalkPhase): void;
    visitLineBreak(this: void, node: LineBreak, phase: WalkPhase): void;
    visitCode(this: void, node: Code, phase: WalkPhase): void;
    visitHTML(this: void, node: HTML, phase: WalkPhase): void;
    visitComment(this: void, node: Comment, phase: WalkPhase): void;
    visitFormula(this: void, node: Formula, phase: WalkPhase): void;
    visitEmphasis(this: void, node: Emphasis, phase: WalkPhase): void;
    visitStrong(this: void, node: Strong, phase: WalkPhase): void;
    visitStrikethrough(this: void, node: Strikethrough, phase: WalkPhase): void;
    visitLink(this: void, node: Link, phase: WalkPhase): void;
    visitImage(this: void, node: Image, phase: WalkPhase): void;
    visitDirective(this: void, node: Directive, phase: WalkPhase): void;
    visitCite(this: void, node: Cite, phase: WalkPhase): void;
    /** A value callback: a `Citation` is a scoped value, not a `Markup` kind. */
    visitCitation(this: void, value: Citation, phase: WalkPhase): void;
    /** A value callback: a `Footnote` is a scoped value, not a `Markup` kind. */
    visitFootnote(this: void, value: Footnote, phase: WalkPhase): void;
}

type WalkAction =
    | { readonly kind: "markup"; readonly node: Markup; readonly phase: WalkPhase }
    | { readonly kind: "citation"; readonly value: Citation; readonly phase: WalkPhase }
    | { readonly kind: "footnote"; readonly value: Footnote; readonly phase: WalkPhase };

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
    const visitCitation = (value: Citation): void => {
        walkingVisitor.visitCitation(value, phase);
        if (phase === "entering") {
            actions.push({ kind: "citation", value, phase: "exiting" });
            schedule(value.suffix);
            schedule(value.prefix);
        }
    };
    const visitFootnote = (value: Footnote): void => {
        walkingVisitor.visitFootnote(value, phase);
        if (phase === "entering") {
            actions.push({ kind: "footnote", value, phase: "exiting" });
            schedule(value.content);
        }
    };

    // Each callback owns the schedule for that node kind. This is traversal
    // control flow, not a public iterator or a generic child projection.
    const driver: Visitor<void> = {
        visitDocument: (node) => {
            walkingVisitor.visitDocument(node, phase);
            scheduleExit(node);
            if (phase === "entering") {
                // The footnotes are visited after the content.
                scheduleFootnotes(node.footnotes);
                schedule(node.content);
            }
        },
        visitCallout: (node) => {
            walkingVisitor.visitCallout(node, phase);
            scheduleExit(node);
            if (phase === "entering") {
                schedule(node.content);
                // The title is a node-valued field, visited before the content.
                if (node.title !== null) schedule(node.title);
            }
        },
        visitParagraph: (node) => {
            walkingVisitor.visitParagraph(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitHeading: (node) => {
            walkingVisitor.visitHeading(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitThematicBreak: (node) => {
            walkingVisitor.visitThematicBreak(node, phase);
            scheduleExit(node);
        },
        visitList: (node) => {
            walkingVisitor.visitList(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.items);
        },
        visitListItem: (node) => {
            walkingVisitor.visitListItem(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitCodeBlock: (node) => {
            walkingVisitor.visitCodeBlock(node, phase);
            scheduleExit(node);
        },
        visitHTMLBlock: (node) => {
            walkingVisitor.visitHTMLBlock(node, phase);
            scheduleExit(node);
        },
        visitFormulaBlock: (node) => {
            walkingVisitor.visitFormulaBlock(node, phase);
            scheduleExit(node);
        },
        visitTable: (node) => {
            walkingVisitor.visitTable(node, phase);
            scheduleExit(node);
            if (phase === "entering") {
                schedule(node.rows);
                actions.push({ kind: "markup", node: node.header, phase: "entering" });
            }
        },
        visitTableRow: (node) => {
            walkingVisitor.visitTableRow(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.cells);
        },
        visitTableCell: (node) => {
            walkingVisitor.visitTableCell(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitDirectiveBlock: (node) => {
            walkingVisitor.visitDirectiveBlock(node, phase);
            scheduleExit(node);
            if (phase === "entering") {
                schedule(node.content);
                if (node.label !== null) actions.push({ kind: "markup", node: node.label, phase: "entering" });
            }
        },
        visitDirectiveLabel: (node) => {
            walkingVisitor.visitDirectiveLabel(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitText: (node) => {
            walkingVisitor.visitText(node, phase);
            scheduleExit(node);
        },
        visitSoftBreak: (node) => {
            walkingVisitor.visitSoftBreak(node, phase);
            scheduleExit(node);
        },
        visitLineBreak: (node) => {
            walkingVisitor.visitLineBreak(node, phase);
            scheduleExit(node);
        },
        visitCode: (node) => {
            walkingVisitor.visitCode(node, phase);
            scheduleExit(node);
        },
        visitHTML: (node) => {
            walkingVisitor.visitHTML(node, phase);
            scheduleExit(node);
        },
        visitComment: (node) => {
            walkingVisitor.visitComment(node, phase);
            scheduleExit(node);
        },
        visitFormula: (node) => {
            walkingVisitor.visitFormula(node, phase);
            scheduleExit(node);
        },
        visitEmphasis: (node) => {
            walkingVisitor.visitEmphasis(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitStrong: (node) => {
            walkingVisitor.visitStrong(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitStrikethrough: (node) => {
            walkingVisitor.visitStrikethrough(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitLink: (node) => {
            walkingVisitor.visitLink(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitImage: (node) => {
            walkingVisitor.visitImage(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitDirective: (node) => {
            walkingVisitor.visitDirective(node, phase);
            scheduleExit(node);
            if (phase === "entering" && node.label !== null) {
                actions.push({ kind: "markup", node: node.label, phase: "entering" });
            }
        },
        visitCite: (node) => {
            walkingVisitor.visitCite(node, phase);
            scheduleExit(node);
            if (phase === "entering") scheduleCitations(node.citations);
        }
    };

    while (actions.length > 0) {
        const action = actions.pop()!;
        phase = action.phase;
        if (action.kind === "markup") visit(action.node, driver);
        else if (action.kind === "citation") visitCitation(action.value);
        else visitFootnote(action.value);
    }
}
