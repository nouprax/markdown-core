import {
    Document,
    MarkupDumper,
    visit,
    MarkupWalker,
    type Directive,
    type DirectiveBlock,
    type DirectiveLabel,
    type CrossLink,
    DiagnosticCode,
    type Diagnostic,
    type Embed,
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

const document = Document("# typed", { tables: true });
const parsedDocument: ParsedDocument = document;
const diagnostic: string = document.dump();
const explicitDiagnostic: string = MarkupDumper.dump(document);
const subtreeDiagnostic: string = MarkupDumper.dump(document, document.content[0]);
void diagnostic;
void explicitDiagnostic;
void subtreeDiagnostic;
void parsedDocument;
// @ts-expect-error a document is parsed from text, not constructed empty
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
    visitDirectiveLabel: (node) => node.kind,
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
    visitFootnoteReference: (node) => node.label,
    visitReferenceDefinition: (node) => node.destination ?? node.label,
    visitLinkReference: (node) => node.form,
    visitImageReference: (node) => node.label,
    visitCrossLink: (node) => node.reference,
    visitEmbed: (node) => node.reference
};
visit(document, visitor);
new MarkupWalker().walk(document, (_event, node, scope) => {
    const resolved: Scope = scope;
    void resolved;
    visit(node, visitor);
});
new MarkupWalker().walk(document, document.content[0], (_event, node) => visit(node, visitor));
// The scope-free structural overload dispatches the visitor per node.
new MarkupWalker().walk(document, visitor);
const statefulVisitor: MarkupVisitor<void> & { kind: string } = {
    ...visitor,
    kind: "visitor-state"
};
new MarkupWalker().walk(document, statefulVisitor);
const callableVisitor: MarkupVisitor<void> & (() => void) = Object.assign(() => {}, visitor);
new MarkupWalker().walk(document, callableVisitor);
// @ts-expect-error recursively readonly content cannot be replaced
document.content[0] = document;
// @ts-expect-error diagnostic methods cannot be replaced
document.dump = () => "replacement";
// @ts-expect-error a node's extent is read-only, like every other field
document.content[1].scope = { start: { line: 1, column: 1 }, end: { line: 1, column: 1 } };

const identity: MarkupID = document.id;
const series: string = identity.series;
const rawValue: number = identity.rawValue;
const revision: number = document.revision;
const documentScope: Scope = document.scope;
const nodeScope: Scope = document.content[1]!.scope;
void nodeScope;
void series;
void rawValue;
void revision;
void documentScope;

const successor: ParsedDocument = document.append(" and appended");
const successorRevision: number = successor.revision;
const currentValue: Markup | null = successor.node(document.id);
const diagnostics: readonly Diagnostic[] = successor.diagnostics;
const attributeCode: DiagnosticCode = DiagnosticCode.directiveAttributes;
void attributeCode;
void successorRevision;
void currentValue;
void diagnostics;
// @ts-expect-error options are immutable for a document's whole series
document.options.tables = false;
const appended: ParsedDocument = successor.append(" and more");
const appendedRevision: number = appended.revision;
void appendedRevision;
// @ts-expect-error append takes the chunk's text
successor.append();
// @ts-expect-error a chunk is a string, not bytes
successor.append(new Uint8Array(1));
// @ts-expect-error a chain grows one way — append; replacing the text is a new document
successor.edit("# typed again");
appended.close();
successor.close();

declare const table: Table;
const rowMarkup: Markup = table.header;
const row: TableRow = table.header;
const cellMarkup: Markup = row.cells[0]!;
const cell: TableCell = row.cells[0]!;
void rowMarkup;
void cellMarkup;
void cell;

declare const directive: Directive;
declare const directiveBlock: DirectiveBlock;
declare const crossLink: CrossLink;
declare const embed: Embed;
const inlineLabel: DirectiveLabel | null = directive.label;
const blockLabel: DirectiveLabel | null = directiveBlock.label;
const labelMarkup: Markup | null = inlineLabel;
const labelContent: readonly Markup[] | undefined = blockLabel?.content;
void labelMarkup;
void labelContent;
const crossLinkReference: string = crossLink.reference;
const embedReference: string = embed.reference;
void crossLinkReference;
void embedReference;
// @ts-expect-error directive labels are immutable typed child properties
directive.label = null;

// @ts-expect-error MarkupVisitor is exhaustive and requires one method per Markup kind
const incompleteVisitor: MarkupVisitor<string> = {
    visitDocument: (node) => node.kind
};
void incompleteVisitor;
