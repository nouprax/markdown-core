import type { Callout } from "./model/callout.js";
import type { Cite } from "./model/cite.js";
import type { CodeBlock } from "./model/code-block.js";
import type { Code } from "./model/code.js";
import type { Comment } from "./model/comment.js";
import type { CrossLink } from "./model/cross-link.js";
import type { CrossEmbedded } from "./model/cross-embedded.js";
import type { DirectiveBlock } from "./model/directive-block.js";
import type { DirectiveLabel } from "./model/directive-label.js";
import type { Directive } from "./model/directive.js";
import type { Document } from "./model/document.js";
import type { Emphasis } from "./model/emphasis.js";
import type { FormulaBlock } from "./model/formula-block.js";
import type { Formula } from "./model/formula.js";
import type { Heading } from "./model/heading.js";
import type { HTMLBlock } from "./model/html-block.js";
import type { HTML } from "./model/html.js";
import type { Media } from "./model/media.js";
import type { LineBreak } from "./model/line-break.js";
import type { Link } from "./model/link.js";
import type { List, ListItem } from "./model/list.js";
import type { Markup } from "./model/markup.js";
import type { Paragraph } from "./model/paragraph.js";
import type { SoftBreak } from "./model/soft-break.js";
import type { Strikethrough } from "./model/strikethrough.js";
import type { Mark } from "./model/mark.js";
import type { Insertion } from "./model/insertion.js";
import type { Strong } from "./model/strong.js";
import type { Table, TableCell, TableRow } from "./model/table.js";
import type { Text } from "./model/text.js";
import type { ThematicBreak } from "./model/thematic-break.js";

export interface Visitor<Result> {
    visitDocument(this: void, node: Document): Result;
    visitCallout(this: void, node: Callout): Result;
    visitParagraph(this: void, node: Paragraph): Result;
    visitHeading(this: void, node: Heading): Result;
    visitThematicBreak(this: void, node: ThematicBreak): Result;
    visitList(this: void, node: List): Result;
    visitListItem(this: void, node: ListItem): Result;
    visitCodeBlock(this: void, node: CodeBlock): Result;
    visitHTMLBlock(this: void, node: HTMLBlock): Result;
    visitFormulaBlock(this: void, node: FormulaBlock): Result;
    visitTable(this: void, node: Table): Result;
    visitTableRow(this: void, node: TableRow): Result;
    visitTableCell(this: void, node: TableCell): Result;
    visitDirectiveBlock(this: void, node: DirectiveBlock): Result;
    visitDirectiveLabel(this: void, node: DirectiveLabel): Result;
    visitText(this: void, node: Text): Result;
    visitSoftBreak(this: void, node: SoftBreak): Result;
    visitLineBreak(this: void, node: LineBreak): Result;
    visitCode(this: void, node: Code): Result;
    visitHTML(this: void, node: HTML): Result;
    visitComment(this: void, node: Comment): Result;
    visitCrossLink(this: void, node: CrossLink): Result;
    visitCrossEmbedded(this: void, node: CrossEmbedded): Result;
    visitFormula(this: void, node: Formula): Result;
    visitEmphasis(this: void, node: Emphasis): Result;
    visitStrong(this: void, node: Strong): Result;
    visitStrikethrough(this: void, node: Strikethrough): Result;
    visitMark(this: void, node: Mark): Result;
    visitInsertion(this: void, node: Insertion): Result;
    visitLink(this: void, node: Link): Result;
    visitMedia(this: void, node: Media): Result;
    visitDirective(this: void, node: Directive): Result;
    visitCite(this: void, node: Cite): Result;
}

export function visit<Result>(node: Markup, visitor: Visitor<Result>): Result {
    switch (node.kind) {
        case "document":
            return visitor.visitDocument(node);
        case "callout":
            return visitor.visitCallout(node);
        case "paragraph":
            return visitor.visitParagraph(node);
        case "heading":
            return visitor.visitHeading(node);
        case "thematicBreak":
            return visitor.visitThematicBreak(node);
        case "list":
            return visitor.visitList(node);
        case "listItem":
            return visitor.visitListItem(node);
        case "codeBlock":
            return visitor.visitCodeBlock(node);
        case "htmlBlock":
            return visitor.visitHTMLBlock(node);
        case "formulaBlock":
            return visitor.visitFormulaBlock(node);
        case "table":
            return visitor.visitTable(node);
        case "tableRow":
            return visitor.visitTableRow(node);
        case "tableCell":
            return visitor.visitTableCell(node);
        case "directiveBlock":
            return visitor.visitDirectiveBlock(node);
        case "directiveLabel":
            return visitor.visitDirectiveLabel(node);
        case "text":
            return visitor.visitText(node);
        case "softBreak":
            return visitor.visitSoftBreak(node);
        case "lineBreak":
            return visitor.visitLineBreak(node);
        case "code":
            return visitor.visitCode(node);
        case "html":
            return visitor.visitHTML(node);
        case "crossLink":
            return visitor.visitCrossLink(node);
        case "crossEmbedded":
            return visitor.visitCrossEmbedded(node);
        case "comment":
            return visitor.visitComment(node);
        case "formula":
            return visitor.visitFormula(node);
        case "emphasis":
            return visitor.visitEmphasis(node);
        case "strong":
            return visitor.visitStrong(node);
        case "strikethrough":
            return visitor.visitStrikethrough(node);
        case "mark":
            return visitor.visitMark(node);
        case "insertion":
            return visitor.visitInsertion(node);
        case "link":
            return visitor.visitLink(node);
        case "media":
            return visitor.visitMedia(node);
        case "directive":
            return visitor.visitDirective(node);
        case "cite":
            return visitor.visitCite(node);
    }
    return unreachable(node);
}

function unreachable(value: never): never {
    throw new Error(`unreachable markup ${String(value)}`);
}
