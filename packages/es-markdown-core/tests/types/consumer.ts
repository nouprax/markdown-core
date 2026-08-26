import {
    Concrete,
    Document,
    TreeDumper,
    visit,
    Walker,
    type Heading,
    type Markup,
    type Read,
    type Semantic,
    type Table,
    type TableCell,
    type TableRow,
    type Visitor
} from "@nouprax/es-markdown-core";

const read: Read = new Document("# typed", { tables: true }).seal();
const semantic: Semantic = read.semantic;
const concrete: Concrete = read.concrete;
const diagnostic: string = read.dump();
const explicitDiagnostic: string = TreeDumper.dump(semantic);
void diagnostic;
void explicitDiagnostic;
// The source a scope is counted against is bytes and a line index, and both
// outlive the WASM handle.
const source: Uint8Array = concrete.source;
const offset: number = concrete.offset(1);
const lines: number = concrete.lines;
void source;
void offset;
void lines;
// @ts-expect-error the source is readonly
concrete.source = source;
// @ts-expect-error the read's views are readonly
read.concrete = concrete;
// The stream hands out `Read` values, and a chunk is a string or raw UTF-8
// bytes -- nothing else crosses.
const streaming: Document = new Document({ tables: false });
const updated: Read = streaming.feed("# streamed");
const fedBytes: Read = streaming.feed(new Uint8Array([35, 32, 104, 105, 10]));
const sealed: Read = streaming.seal();
const disposal: void = streaming.dispose();
void updated;
void fedBytes;
void sealed;
void disposal;
// @ts-expect-error a chunk is a string or a Uint8Array
streaming.feed(42);

const visitor: Visitor<string> = {
    visitSemantic: (node) => node.kind,
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
visit(semantic, visitor);
new Walker().walk(semantic, (_event, node) => visit(node, visitor));
// @ts-expect-error recursively readonly content cannot be replaced
semantic.content[0] = semantic;
// @ts-expect-error readonly scope values cannot be mutated
semantic.scope.start.line = 2;
// @ts-expect-error diagnostic methods cannot be replaced
semantic.dump = () => "replacement";

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
    visitSemantic: (node) => node.kind
};
void incompleteVisitor;
