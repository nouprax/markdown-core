import type { BlockQuote } from "./model/block-quote.js";
import type { CodeBlock } from "./model/code-block.js";
import type { Code } from "./model/code.js";
import type { DirectiveBlock } from "./model/directive-block.js";
import type { DirectiveLabel } from "./model/directive-label.js";
import type { Directive } from "./model/directive.js";
import type { Document } from "./model/document.js";
import type { Emphasis } from "./model/emphasis.js";
import type { FootnoteDefinition, FootnoteReference } from "./model/footnote.js";
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
import type { ReferenceDefinition } from "./model/reference-definition.js";
import type { ImageReference, LinkReference } from "./model/reference.js";
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
    visitBlockQuote(this: void, node: BlockQuote, phase: WalkPhase): void;
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
    visitFootnoteDefinition(this: void, node: FootnoteDefinition, phase: WalkPhase): void;
    visitReferenceDefinition(this: void, node: ReferenceDefinition, phase: WalkPhase): void;
    visitLinkReference(this: void, node: LinkReference, phase: WalkPhase): void;
    visitImageReference(this: void, node: ImageReference, phase: WalkPhase): void;
    visitText(this: void, node: Text, phase: WalkPhase): void;
    visitSoftBreak(this: void, node: SoftBreak, phase: WalkPhase): void;
    visitLineBreak(this: void, node: LineBreak, phase: WalkPhase): void;
    visitCode(this: void, node: Code, phase: WalkPhase): void;
    visitHTML(this: void, node: HTML, phase: WalkPhase): void;
    visitFormula(this: void, node: Formula, phase: WalkPhase): void;
    visitEmphasis(this: void, node: Emphasis, phase: WalkPhase): void;
    visitStrong(this: void, node: Strong, phase: WalkPhase): void;
    visitStrikethrough(this: void, node: Strikethrough, phase: WalkPhase): void;
    visitLink(this: void, node: Link, phase: WalkPhase): void;
    visitImage(this: void, node: Image, phase: WalkPhase): void;
    visitDirective(this: void, node: Directive, phase: WalkPhase): void;
    visitFootnoteReference(this: void, node: FootnoteReference, phase: WalkPhase): void;
}

interface WalkAction {
    readonly node: Markup;
    readonly phase: WalkPhase;
}

/**
 * Walks `root` and all of its owned markup depth first.
 *
 * An explicit action stack keeps the JavaScript call-stack depth independent
 * of document depth. Every node receives `entering` before its typed relations
 * and `exiting` after them.
 */
export function walk(root: Markup, walkingVisitor: WalkingVisitor): void {
    const actions: WalkAction[] = [{ node: root, phase: "entering" }];
    let phase: WalkPhase = "entering";

    const scheduleExit = (node: Markup): void => {
        if (phase === "entering") actions.push({ node, phase: "exiting" });
    };
    const schedule = (nodes: readonly Markup[]): void => {
        for (let index = nodes.length - 1; index >= 0; index -= 1) {
            actions.push({ node: nodes[index]!, phase: "entering" });
        }
    };

    // Each callback owns the schedule for that node kind. This is traversal
    // control flow, not a public iterator or a generic child projection.
    const driver: Visitor<void> = {
        visitDocument: (node) => {
            walkingVisitor.visitDocument(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitBlockQuote: (node) => {
            walkingVisitor.visitBlockQuote(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
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
                actions.push({ node: node.header, phase: "entering" });
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
                if (node.label !== null) actions.push({ node: node.label, phase: "entering" });
            }
        },
        visitDirectiveLabel: (node) => {
            walkingVisitor.visitDirectiveLabel(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitFootnoteDefinition: (node) => {
            walkingVisitor.visitFootnoteDefinition(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitReferenceDefinition: (node) => {
            walkingVisitor.visitReferenceDefinition(node, phase);
            scheduleExit(node);
        },
        visitLinkReference: (node) => {
            walkingVisitor.visitLinkReference(node, phase);
            scheduleExit(node);
            if (phase === "entering") schedule(node.content);
        },
        visitImageReference: (node) => {
            walkingVisitor.visitImageReference(node, phase);
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
                actions.push({ node: node.label, phase: "entering" });
            }
        },
        visitFootnoteReference: (node) => {
            walkingVisitor.visitFootnoteReference(node, phase);
            scheduleExit(node);
        }
    };

    while (actions.length > 0) {
        const action = actions.pop()!;
        phase = action.phase;
        visit(action.node, driver);
    }
}
