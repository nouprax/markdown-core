import {
    Document,
    Attributes,
    type Record,
    type Metadata,
    type MetadataRecord,
    type MetadataValue,
    type MetadataScalar,
    type MetadataListItem,
    type Image,
    TreeDumper,
    visit,
    walk,
    type Citation,
    type CitationReferent,
    type Footnote,
    type Specimen,
    type Heading,
    type Markup,
    type Table,
    type TableCell,
    type TableRow,
    type Visitor,
    type WalkingVisitor,
    type WalkPhase
} from "@nouprax/es-markdown-core";

const document: Document = Document.parse("# typed");
// @ts-expect-error the dialect has no switches: parse takes the source and nothing else
Document.parse("# typed", { tables: true });
const dump: string = document.dump();
const explicitDump: string = TreeDumper.dump(document);
void dump;
void explicitDump;
const visitor: Visitor<string> = {
    visitDocument: (node) => node.kind,
    visitCallout: (node) => node.kind,
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
    visitTableRow: () => "row",
    visitTableCell: (node) => node.kind,
    visitDirectiveBlock: (node) => node.kind,
    visitDirectiveLabel: (node) => node.kind,
    visitText: (node) => node.kind,
    visitSoftBreak: (node) => node.kind,
    visitLineBreak: (node) => node.kind,
    visitCode: (node) => node.kind,
    visitHTML: (node) => node.kind,
    visitComment: (node) => node.kind,
    visitCrossLink: (node) => node.kind,
    visitFormula: (node) => node.kind,
    visitEmphasis: (node) => node.kind,
    visitStrong: (node) => node.kind,
    visitStrikethrough: (node) => node.kind,
    visitLink: (node) => node.kind,
    visitImage: (node) => node.kind,
    visitDirective: (node) => node.kind,
    visitCite: (node) => node.kind
};
visit(document, visitor);
const walkingVisitor: WalkingVisitor = {
    ...visitor,
    visitHeading(node: Heading, phase: WalkPhase) {
        void node.level;
        void phase;
    },
    // The scoped values are not `Markup`: they arrive through their own
    // callbacks and never through a kind case.
    visitCitation(value: Citation, phase: WalkPhase) {
        const referent: CitationReferent = value.referent;
        void referent;
        void phase;
    },
    visitSpecimen(value: Specimen, phase: WalkPhase) {
        void value;
        void phase;
    },
    visitFootnote(value: Footnote, phase: WalkPhase) {
        void value.id;
        void phase;
    }
};
walk(document, walkingVisitor);
// @ts-expect-error recursively readonly content cannot be replaced
document.content[0] = document;
// @ts-expect-error readonly scope values cannot be mutated
document.scope.start.line = 2;
// @ts-expect-error dump methods cannot be replaced
document.dump = () => "replacement";

declare const table: Table;
const rowMarkup: Markup = table.head[0]!;
const row: TableRow = table.head[0]!;
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

// @ts-expect-error WalkingVisitor is exhaustive and requires one method per Markup kind
const incompleteWalkingVisitor: WalkingVisitor = {
    visitDocument: (node, phase) => {
        void node;
        void phase;
    }
};
void incompleteWalkingVisitor;

const anchor: string | null = document.anchor;
const attributes: Attributes = document.attributes;
const empty: Attributes = Attributes.empty;
const record: Record = { name: "k", value: "1" };
const scalar: MetadataScalar = { kind: "number", value: "9007199254740993" };
const listItem: MetadataListItem = { kind: "text", value: "" };
const metadataValue: MetadataValue = { kind: "scalar", value: scalar };
const metadataRecord: MetadataRecord = { name: "k", value: metadataValue, scope: document.scope };
const metadata: Metadata = { records: [metadataRecord], scope: document.scope };
const parsedMetadata: Metadata | null = document.metadata;
void [anchor, attributes, empty, record, listItem, metadata, parsedMetadata];
declare const image: Image;
const dimensions: readonly (number | null)[] = [image.width, image.height];
void dimensions;
// @ts-expect-error inherited attributes are recursively readonly
attributes.classes[0] = "replacement";
// @ts-expect-error metadata collections are recursively readonly
metadata.records[0] = metadataRecord;
// @ts-expect-error an attribute record is a value, not Markup
const recordMarkup: Markup = record;
void recordMarkup;
