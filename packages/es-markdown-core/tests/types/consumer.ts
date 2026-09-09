import {
    Document,
    Attributes,
    type Record,
    type Metadata,
    type MetadataValue,
    type MetadataScalar,
    type MetadataListItem,
    type Media,
    type CrossLink,
    type CrossEmbedded,
    type Dimensions,
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
    visitCrossEmbedded: (node) => node.kind,
    visitFormula: (node) => node.kind,
    visitEmphasis: (node) => node.kind,
    visitStrong: (node) => node.kind,
    visitStrikethrough: (node) => node.kind,
    visitMark: (node) => node.kind,
    visitLink: (node) => node.kind,
    visitMedia: (node) => node.kind,
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
const metadata: Metadata = {
    name: metadataValue,
    title: null,
    subtitle: null,
    time: null,
    date: null,
    authors: null,
    keywords: null,
    abstract: null,
    state: null,
    comment: null,
    scope: document.scope
};
const parsedMetadata: Metadata | null = document.metadata;
void [anchor, attributes, empty, record, listItem, metadata, parsedMetadata];
declare const image: Media;
const dimensions: Dimensions | null = image.dimensions;
const standaloneSize: Dimensions = { width: 640, height: null };
void [dimensions, standaloneSize];
// @ts-expect-error dimensions require width
const heightOnly: Dimensions = { height: 480 };
// @ts-expect-error dimensions are values, not Markup
const sizeNode: Markup = standaloneSize;
void [heightOnly, sizeNode];
// @ts-expect-error dimensions are immutable
standaloneSize.width = 800;
// @ts-expect-error inherited attributes are recursively readonly
attributes.classes[0] = "replacement";
// @ts-expect-error metadata fields are readonly
metadata.name = metadataValue;
// @ts-expect-error an attribute record is a value, not Markup
const recordMarkup: Markup = record;
void recordMarkup;

declare const crossLink: CrossLink;
declare const crossEmbedded: CrossEmbedded;
const embeddedSize: Dimensions | null = crossEmbedded.dimensions;
void embeddedSize;
// @ts-expect-error ordinary cross links have no dimensions
void crossLink.dimensions;
// @ts-expect-error the node kind replaces the old embedded flag
void crossLink.embedded;
// @ts-expect-error CrossEmbedded also has no redundant embedded flag
void crossEmbedded.embedded;
