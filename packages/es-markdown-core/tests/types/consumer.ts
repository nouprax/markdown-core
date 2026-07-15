import {
    Document,
    TreeDumper,
    visit,
    Walker,
    type Heading,
    type Markup,
    type Table,
    type TableCell,
    type TableRow,
    type Visitor
} from "@nouprax/es-markdown-core";
import type { Document as ParsedDocument } from "@nouprax/es-markdown-core";

const document = Document.parse("# typed", { tables: true });
const parsedDocument: ParsedDocument = document;
const diagnostic: string = document.dump();
const explicitDiagnostic: string = TreeDumper.dump(document);
void diagnostic;
void explicitDiagnostic;
void parsedDocument;
// @ts-expect-error Document values are created only by Document.parse
new Document();
const visitor: Visitor<string> = {
    visitDocument: (node) => node.kind,
    visitBlockQuote: (node) => node.kind,
    visitParagraph: (node) => node.kind,
    visitHeading(node: Heading) {
        return String(node.level);
    },
    visitThematicBreak: (node) => node.kind,
    visitList: (node) => node.kind,
    visitListItem: (node) => node.kind,
    visitCodeBlock: (node) => node.kind,
    visitHTMLBlock: (node) => node.kind,
    visitFormulaBlock: (node) => node.kind,
    visitTable: (node) => node.kind,
    visitTableRow: (node) => (node.isHeader ? "header" : "row"),
    visitTableCell: (node) => node.kind,
    visitDirectiveBlock: (node) => node.kind,
    visitFootnoteDefinition: (node) => node.kind,
    visitText: (node) => node.kind,
    visitSoftBreak: (node) => node.kind,
    visitLineBreak: (node) => node.kind,
    visitCode: (node) => node.kind,
    visitHTML: (node) => node.kind,
    visitFormula: (node) => node.kind,
    visitEmphasis: (node) => node.kind,
    visitStrong: (node) => node.kind,
    visitStrikethrough: (node) => node.kind,
    visitLink: (node) => node.kind,
    visitImage: (node) => node.kind,
    visitDirective: (node) => node.kind,
    visitFootnoteReference: (node) => node.kind
};
visit(document, visitor);
new Walker().walk(document, (_event, node) => visit(node, visitor));
// @ts-expect-error recursively readonly content cannot be replaced
document.content[0] = document;
// @ts-expect-error readonly scope values cannot be mutated
document.scope.start.line = 2;
// @ts-expect-error diagnostic methods cannot be replaced
document.dump = () => "replacement";

declare const table: Table;
const rowMarkup: Markup = table.header;
const row: TableRow = table.header;
const cellMarkup: Markup = row.cells[0]!;
const cell: TableCell = row.cells[0]!;
void rowMarkup;
void cellMarkup;
void cell;

// @ts-expect-error Visitor is exhaustive and requires one method per Markup kind
const incompleteVisitor: Visitor<string> = {
    visitDocument: (node) => node.kind
};
void incompleteVisitor;
