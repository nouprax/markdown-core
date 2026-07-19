import {
    Document,
    MarkupSession,
    MarkupDumper,
    visit,
    MarkupWalker,
    type Commit,
    type Delta,
    type FootnoteInfo,
    type Heading,
    type Markup,
    type MarkupID,
    type Scope,
    type Table,
    type TableCell,
    type TableRow,
    type MarkupVisitor
} from "@nouprax/es-markdown-core";
import type { Document as ParsedDocument } from "@nouprax/es-markdown-core";

const document = Document.parse("# typed", { tables: true });
const parsedDocument: ParsedDocument = document;
const diagnostic: string = document.dump();
const explicitDiagnostic: string = MarkupDumper.dump(document);
const subtreeDiagnostic: string = MarkupDumper.dump(document, document.content[0]);
void diagnostic;
void explicitDiagnostic;
void subtreeDiagnostic;
void parsedDocument;
// @ts-expect-error Document values are created only by Document.parse
new Document();
const visitor: MarkupVisitor<string> = {
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
    visitFootnoteDefinition: (node) => node.label,
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
    visitFootnoteReference: (node) => node.label
};
visit(document, visitor);
new MarkupWalker().walk(document, (_event, node, scope) => {
    const resolved: Scope = scope;
    void resolved;
    visit(node, visitor);
});
new MarkupWalker().walk(document, document.content[0], (_event, node) => visit(node, visitor));
// @ts-expect-error recursively readonly content cannot be replaced
document.content[0] = document;
// @ts-expect-error diagnostic methods cannot be replaced
document.dump = () => "replacement";
// @ts-expect-error the scope mediator cannot be replaced
document.scope = () => {
    throw new Error("replacement");
};
// @ts-expect-error nodes do not store scopes
void document.content[1].scope;

const identity: MarkupID = document.id;
const lineage: bigint = identity.lineage;
const rawValue: number = identity.rawValue;
const revision: number = document.revision;
const documentScope: Scope = document.scope(document);
void lineage;
void rawValue;
void revision;
void documentScope;

const session = new MarkupSession({ tables: true });
const snapshot: ParsedDocument = session.document;
const commit: Commit = session.commit();
const delta: Delta = commit.delta;
const added: readonly MarkupID[] = delta.added;
const currentValue: Markup | null = session.node(document.id);
const info: FootnoteInfo | null = session.footnote(document.id);
const sessionLineage: bigint = session.lineage;
void snapshot;
void added;
void currentValue;
void info;
void sessionLineage;
// @ts-expect-error session options are immutable for the session lifetime
session.options.tables = false;
session.close();

declare const table: Table;
const rowMarkup: Markup = table.header;
const row: TableRow = table.header;
const cellMarkup: Markup = row.cells[0]!;
const cell: TableCell = row.cells[0]!;
void rowMarkup;
void cellMarkup;
void cell;

// @ts-expect-error MarkupVisitor is exhaustive and requires one method per Markup kind
const incompleteVisitor: MarkupVisitor<string> = {
    visitDocument: (node) => node.kind
};
void incompleteVisitor;
