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
    MarkupDumper,
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
    type MarkupVisitor,
    type MarkupVisitPhase
} from "@nouprax/es-markdown-core";

const document: Document = Document.parse("# typed");
// @ts-expect-error the dialect has no switches: parse takes the source and nothing else
Document.parse("# typed", { tables: true });
const dump: string = document.dump();
const explicitDump: string = MarkupDumper.dump(document);
void dump;
void explicitDump;
const visitor: MarkupVisitor = {
    citation: (node) => {
        void node.kind;
    },
    footnote: (node) => {
        void node.kind;
    },
    specimen: (node) => {
        void node.kind;
    },
    metadata: (node) => {
        void node.kind;
    },

    document: (node) => {
        void node.kind;
    },
    callout: (node) => {
        void node.kind;
    },
    paragraph: (node) => {
        void node.kind;
    },
    heading(heading) {
        const inferred: Heading = heading;
        void inferred.level;
    },
    thematicBreak: (node) => {
        void node.kind;
    },
    list: (node) => {
        void node.kind;
    },
    listItem: (node) => {
        void node.kind;
    },
    codeBlock: (node) => {
        void node.kind;
    },
    htmlBlock: (node) => {
        void node.kind;
    },
    formulaBlock: (node) => {
        void node.kind;
    },
    table: (node) => {
        void node.kind;
    },
    tableCaption: (node) => {
        void node.kind;
    },
    tableRow: () => undefined,
    tableCell: (node) => {
        void node.kind;
    },
    directiveBlock: (node) => {
        void node.kind;
    },
    directiveLabel: (node) => {
        void node.kind;
    },
    text: (node) => {
        void node.kind;
    },
    softBreak: (node) => {
        void node.kind;
    },
    lineBreak: (node) => {
        void node.kind;
    },
    code: (node) => {
        void node.kind;
    },
    html: (node) => {
        void node.kind;
    },
    comment: (node) => {
        void node.kind;
    },
    crossLink: (node) => {
        void node.kind;
    },
    crossEmbedded: (node) => {
        void node.kind;
    },
    formula: (node) => {
        void node.kind;
    },
    emphasis: (node) => {
        void node.kind;
    },
    strong: (node) => {
        void node.kind;
    },
    strikethrough: (node) => {
        void node.kind;
    },
    mark: (node) => {
        void node.kind;
    },
    insertion: (node) => {
        void node.kind;
    },
    span: (node) => {
        void node.kind;
    },
    superscript: (node) => {
        void node.kind;
    },
    subscript: (node) => {
        void node.kind;
    },
    definitionList: (node) => {
        void node.kind;
    },
    definition: (node) => {
        void node.kind;
    },
    link(link) {
        // @ts-expect-error the inferred Link parameter has no dimensions
        void link.dimensions;
        void link.kind;
    },
    embedded(embedded) {
        const inferred: Embedded = embedded;
        const dimensions: Dimensions | null = inferred.dimensions;
        // @ts-expect-error Embedded is not a Heading
        void embedded.level;
        void dimensions;
        void embedded.kind;
    },
    directive: (node) => {
        void node.kind;
    },
    cite: (node) => {
        void node.kind;
    }
};
walk(document, visitor);
// @ts-expect-error node-level dispatch is not public
document.accept(visitor);
// @ts-expect-error a document cannot be passed to the Embedded callback
visitor.embedded(document, "enter");
const mismatchedVisitor: MarkupVisitor = {
    ...visitor,
    // @ts-expect-error the Embedded handler cannot accept only Headings
    embedded: (heading: Heading) => {
        void heading.level;
    }
};
void mismatchedVisitor;
const { embedded: omitted, ...remaining } = visitor;
// @ts-expect-error even one missing kind makes a visitor incomplete
const missingEmbedded: MarkupVisitor = remaining;
void [omitted, missingEmbedded];
const walkingVisitor: MarkupVisitor = {
    document: () => undefined,
    callout: () => undefined,
    paragraph: () => undefined,
    thematicBreak: () => undefined,
    list: () => undefined,
    listItem: () => undefined,
    codeBlock: () => undefined,
    htmlBlock: () => undefined,
    formulaBlock: () => undefined,
    table: () => undefined,
    tableCaption: () => undefined,
    tableRow: () => undefined,
    tableCell: () => undefined,
    directiveBlock: () => undefined,
    directiveLabel: () => undefined,
    text: () => undefined,
    softBreak: () => undefined,
    lineBreak: () => undefined,
    code: () => undefined,
    html: () => undefined,
    crossLink: () => undefined,
    crossEmbedded: () => undefined,
    comment: () => undefined,
    formula: () => undefined,
    emphasis: () => undefined,
    strong: () => undefined,
    strikethrough: () => undefined,
    mark: () => undefined,
    insertion: () => undefined,
    span: () => undefined,
    superscript: () => undefined,
    subscript: () => undefined,
    link: () => undefined,
    embedded: () => undefined,
    directive: () => undefined,
    cite: () => undefined,
    definitionList: () => undefined,
    definition: () => undefined,
    metadata: () => undefined,
    heading(heading, phase) {
        const inferred: Heading = heading;
        const inferredPhase: MarkupVisitPhase = phase;
        void [inferred.level, inferredPhase];
    },
    // Owned elements use the same discriminated Markup union.
    citation(citation, phase) {
        const inferred: Citation = citation;
        const referent: CitationReferent = inferred.referent;
        const inferredPhase: MarkupVisitPhase = phase;
        const node: Markup = citation;
        const kind: "citation" = node.kind;
        void kind;
        void [referent, inferredPhase];
    },
    specimen(specimen, phase) {
        const inferred: Specimen = specimen;
        const inferredPhase: MarkupVisitPhase = phase;
        void [inferred, inferredPhase];
    },
    footnote(footnote, phase) {
        const inferred: Footnote = footnote;
        const inferredPhase: MarkupVisitPhase = phase;
        void [inferred.id, inferredPhase];
    }
};
walk(document, walkingVisitor);
// @ts-expect-error every callback is a no-result callback, even in a mixed object
walk(document, { ...walkingVisitor, text: () => 1 });
// @ts-expect-error the phase domain is closed
visitor.document(document, "unknown");
const { citation: omittedCitation, ...remainingWalking } = walkingVisitor;
// @ts-expect-error every Markup callback is required
const missingCitation: MarkupVisitor = remainingWalking;
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

// @ts-expect-error MarkupVisitor is exhaustive and requires one method per Markup kind
const incompleteVisitor: MarkupVisitor = {
    document: (node) => {
        void node.kind;
    }
};
void incompleteVisitor;

// @ts-expect-error MarkupVisitor is exhaustive and requires one method per Markup kind
const incompleteWalkingVisitor: MarkupVisitor = {
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
    kind: "metadata",
    anchor: null,
    attributes: empty,
    dump() {
        return MarkupDumper.dump(this);
    },
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

// @ts-expect-error dispatch is internal; walk is the only visitor execution API
import { visit } from "@nouprax/es-markdown-core";
void visit;
