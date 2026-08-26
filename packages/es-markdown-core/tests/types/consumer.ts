import {
    Concrete,
    Document,
    Session,
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

const document: Document = Document.parse("# typed", { tables: true });
const concrete: Concrete = document.concrete;
const diagnostic: string = document.dump();
const explicitDiagnostic: string = TreeDumper.dump(document);
void diagnostic;
void explicitDiagnostic;
// The source a scope is counted against is bytes and a line index, and both
// outlive the WASM handle.
const source: Uint8Array = concrete.source;
const lineStart: number = concrete.lineStart(1);
const lineCount: number = concrete.lineCount;
void source;
void lineStart;
void lineCount;
// @ts-expect-error the source is readonly
concrete.source = source;
// @ts-expect-error the concrete view is readonly
document.concrete = concrete;
// The stream hands out the same document values the one-shot parse does, and
// a chunk is a string or raw UTF-8 bytes -- nothing else crosses.
const session: Session = new Session({ tables: false });
const updated: Document = session.feed("# streamed");
const fedBytes: Document = session.feed(new Uint8Array([35, 32, 104, 105, 10]));
const sealed: Document = session.finish();
const disposal: void = session.dispose();
void updated;
void fedBytes;
void sealed;
void disposal;
// @ts-expect-error a chunk is a string or a Uint8Array
session.feed(42);

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
    visitDirectiveLabel: (node) => node.kind,
    visitFootnoteDefinition: (node) => node.kind,
    visitReferenceDefinition: (node) => node.kind,
    visitLinkReference: (node) => node.kind,
    visitImageReference: (node) => node.kind,
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
