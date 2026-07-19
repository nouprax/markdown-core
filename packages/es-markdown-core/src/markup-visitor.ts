import type { BlockQuote } from "./model/block-quote.js";
import type { CodeBlock } from "./model/code-block.js";
import type { Code } from "./model/code.js";
import type { DirectiveBlock } from "./model/directive-block.js";
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
import type { SoftBreak } from "./model/soft-break.js";
import type { Strikethrough } from "./model/strikethrough.js";
import type { Strong } from "./model/strong.js";
import type { Table, TableCell, TableRow } from "./model/table.js";
import type { Text } from "./model/text.js";
import type { ThematicBreak } from "./model/thematic-break.js";

export interface MarkupVisitor<Result> {
    visitDocument(this: void, node: Document): Result;
    visitBlockQuote(this: void, node: BlockQuote): Result;
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
    visitFootnoteDefinition(this: void, node: FootnoteDefinition): Result;
    visitText(this: void, node: Text): Result;
    visitSoftBreak(this: void, node: SoftBreak): Result;
    visitLineBreak(this: void, node: LineBreak): Result;
    visitCode(this: void, node: Code): Result;
    visitHTML(this: void, node: HTML): Result;
    visitFormula(this: void, node: Formula): Result;
    visitEmphasis(this: void, node: Emphasis): Result;
    visitStrong(this: void, node: Strong): Result;
    visitStrikethrough(this: void, node: Strikethrough): Result;
    visitLink(this: void, node: Link): Result;
    visitImage(this: void, node: Image): Result;
    visitDirective(this: void, node: Directive): Result;
    visitFootnoteReference(this: void, node: FootnoteReference): Result;
}

export function visit<Result>(node: Markup, visitor: MarkupVisitor<Result>): Result {
    switch (node.kind) {
        case "document":
            return visitor.visitDocument(node);
        case "blockQuote":
            return visitor.visitBlockQuote(node);
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
        case "footnoteDefinition":
            return visitor.visitFootnoteDefinition(node);
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
        case "formula":
            return visitor.visitFormula(node);
        case "emphasis":
            return visitor.visitEmphasis(node);
        case "strong":
            return visitor.visitStrong(node);
        case "strikethrough":
            return visitor.visitStrikethrough(node);
        case "link":
            return visitor.visitLink(node);
        case "image":
            return visitor.visitImage(node);
        case "directive":
            return visitor.visitDirective(node);
        case "footnoteReference":
            return visitor.visitFootnoteReference(node);
    }
    return unreachable(node);
}

function unreachable(value: never): never {
    throw new Error(`unreachable markup ${String(value)}`);
}
