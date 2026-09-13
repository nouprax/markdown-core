import {
    Document,
    Attributes,
    type Record,
    type Metadata,
    type MetadataValue,
    type MetadataScalar,
    type MetadataListItem,
    type Embedded,
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
    document: (node) => node.kind,
    callout: (node) => node.kind,
    paragraph: (node) => node.kind,
    heading(heading) {
        const inferred: Heading = heading;
        return String(inferred.level);
    },
    thematicBreak: (node) => node.kind,
    list: (node) => node.kind,
    listItem: (node) => node.kind,
    codeBlock: (node) => node.kind,
    htmlBlock: (node) => node.kind,
    formulaBlock: (node) => node.kind,
    table: (node) => node.kind,
    tableCaption: (node) => node.kind,
    tableRow: () => "row",
    tableCell: (node) => node.kind,
    directiveBlock: (node) => node.kind,
    directiveLabel: (node) => node.kind,
    text: (node) => node.kind,
    softBreak: (node) => node.kind,
    lineBreak: (node) => node.kind,
    code: (node) => node.kind,
    html: (node) => node.kind,
    comment: (node) => node.kind,
    crossLink: (node) => node.kind,
    crossEmbedded: (node) => node.kind,
    formula: (node) => node.kind,
    emphasis: (node) => node.kind,
    strong: (node) => node.kind,
    strikethrough: (node) => node.kind,
    mark: (node) => node.kind,
    insertion: (node) => node.kind,
    span: (node) => node.kind,
    superscript: (node) => node.kind,
    subscript: (node) => node.kind,
    definitionList: (node) => node.kind,
    definition: (node) => node.kind,
    link(link) {
        // @ts-expect-error the inferred Link parameter has no dimensions
        void link.dimensions;
        return link.kind;
    },
    embedded(embedded) {
        const inferred: Embedded = embedded;
        const dimensions: Dimensions | null = inferred.dimensions;
        // @ts-expect-error Embedded is not a Heading
        void embedded.level;
        void dimensions;
        return embedded.kind;
    },
    directive: (node) => node.kind,
    cite: (node) => node.kind
};
const result: string = visit(document, visitor);
const explicit: string = visit<string>(document, visitor);
void [result, explicit];
// @ts-expect-error a document cannot be passed to the Embedded callback
visitor.embedded(document);
const mismatchedVisitor: Visitor<string> = {
    ...visitor,
    // @ts-expect-error the Embedded handler cannot accept only Headings
    embedded: (heading: Heading) => String(heading.level)
};
void mismatchedVisitor;
const { embedded: omitted, ...remaining } = visitor;
// @ts-expect-error even one missing kind makes a visitor incomplete
const missingEmbedded: Visitor<string> = remaining;
void [omitted, missingEmbedded];
const walkingVisitor: WalkingVisitor = {
    ...visitor,
    heading(heading, phase) {
        const inferred: Heading = heading;
        const inferredPhase: WalkPhase = phase;
        void [inferred.level, inferredPhase];
    },
    // The scoped values are not `Markup`: they arrive through their own
    // callbacks and never through a kind case.
    citation(citation, phase) {
        const inferred: Citation = citation;
        const referent: CitationReferent = inferred.referent;
        const inferredPhase: WalkPhase = phase;
        // @ts-expect-error a Citation remains a scoped value without a kind
        void citation.kind;
        void [referent, inferredPhase];
    },
    specimen(specimen, phase) {
        const inferred: Specimen = specimen;
        const inferredPhase: WalkPhase = phase;
        void [inferred, inferredPhase];
    },
    footnote(footnote, phase) {
        const inferred: Footnote = footnote;
        const inferredPhase: WalkPhase = phase;
        void [inferred.id, inferredPhase];
    }
};
walk(document, walkingVisitor);
const { citation: omittedCitation, ...remainingWalking } = walkingVisitor;
// @ts-expect-error value callbacks are required as well as Markup callbacks
const missingCitation: WalkingVisitor = remainingWalking;
void [omittedCitation, missingCitation];
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
    document: (node) => node.kind
};
void incompleteVisitor;

// @ts-expect-error WalkingVisitor is exhaustive and requires one method per Markup kind
const incompleteWalkingVisitor: WalkingVisitor = {
    document: (node, phase) => {
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
declare const image: Embedded;
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
