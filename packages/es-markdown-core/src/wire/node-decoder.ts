import { Attributes, type Record as AttributeRecord } from "../markup/attributes.js";
import type { Dimensions, Flow } from "../common/constraints.js";
import type { Metadata, MetadataValue } from "../markup/metadata.js";
import type { DirectiveLabel } from "../markup/directive-label.js";
import type { Citation } from "../markup/cite.js";
import type { Document } from "../markup/document.js";
import type { Specimen } from "../markup/specimen.js";
import type { Footnote } from "../markup/footnote.js";
import type { ListItem } from "../markup/list.js";
import type { Markup } from "../markup/markup.js";
import type { TableCell, TableColumn, TableRow } from "../markup/table.js";
import { ParseError, type ParseErrorCode } from "../common/parse-error.js";
import type {
    BibMode,
    CitationReferent,
    Destination,
    ListFlavor,
    OrderedListDelimiter,
    OrderedListVariant,
    Placement,
    Scope
} from "../markup/values.js";
import { kinds, type NativeKind } from "./kinds.js";
import {
    CalloutNode,
    CitationNode,
    CiteNode,
    CodeBlockNode,
    ContainerNode,
    CrossEmbeddedNode,
    CrossLinkNode,
    DefinitionListNode,
    DefinitionNode,
    DirectiveBlockNode,
    DirectiveNode,
    DocumentNode,
    EmbeddedNode,
    FootnoteNode,
    FormulaNode,
    HeadingNode,
    LinkNode,
    ListItemNode,
    ListNode,
    LiteralNode,
    MarkupNode,
    MetadataNode,
    SpecimenNode,
    TableCellNode,
    TableNode,
    TableRowNode
} from "./markup-nodes.js";

/*
 * MCB1 is an ES-only result ABI over WebAssembly linear memory. Native emits
 * fixed-width records in breadth-first order, so relationships always point
 * forward and this decoder can construct the immutable value tree bottom-up
 * in one pass over the records: a relation is checked where its owner reads
 * it, and every record has been built by the time an owner names it. It
 * performs no calls into Wasm and retains no view after decode returns; the
 * runtime frees the result immediately afterwards.
 *
 * Strings are one trailing UTF-8 blob that the decoder decodes once; a
 * string reference is an offset and a length in UTF-16 code units of that
 * decoded text, so a string is a slice and never a decode of its own.
 */

const magic = [0x4d, 0x43, 0x42, 0x31] as const;
export const transferHeaderSize = 80;
const nodeSize = 160;
const attributeSize = 16;
const columnSize = 16;
const noIndex = 0xffff_ffff;
type ValueKind = "metadataValue" | "definitionBody";
const valueKindBase = 0x100;
const valueKinds: readonly ValueKind[] = Object.freeze(["metadataValue", "definitionBody"]);
type DefinitionBody = { readonly body: readonly Markup[] };
type Decoded = DefinitionBody | Markup | MetadataValue;
/** The node kinds owned through their own relations, never through a content range. */
const ownedApart: ReadonlySet<string> = new Set(["metadata", "citation", "footnote", "specimen"]);
const isMarkup = (value: Decoded): value is Markup => value instanceof MarkupNode;
const isContent = (value: Decoded): value is Markup =>
    value instanceof MarkupNode && !ownedApart.has((value as MarkupNode<string>).kind);
const flows: readonly Flow[] = Object.freeze(["none", "left", "center", "right"]);

const header = {
    totalSize: 4,
    status: 8,
    errorCode: 12,
    errorOffset: 16,
    errorLength: 20,
    nodeCount: 24,
    edgeCount: 28,
    attributeCount: 32,
    columnCount: 36,
    nodesOffset: 40,
    edgesOffset: 44,
    attributesOffset: 48,
    columnsOffset: 52,
    stringsOffset: 56,
    stringsLength: 60,
    stringUnits: 64
} as const;

const nodeField = {
    kind: 0,
    flags: 4,
    scope: 8,
    childStart: 24,
    childCount: 28,
    fieldIndex: 32,
    auxiliaryStart: 36,
    auxiliaryCount: 40,
    scalar0: 44,
    integer2: 48,
    integer: 56,
    strings: 64,
    anchor: 96,
    classesStart: 104,
    classesCount: 108,
    recordsStart: 112,
    recordsCount: 116,
    metadata: 120,
    dimensions: 124,
    inheritedAttributes: 132
} as const;

interface Layout {
    readonly totalSize: number;
    readonly nodeCount: number;
    readonly edgeCount: number;
    readonly attributeCount: number;
    readonly columnCount: number;
    readonly nodesOffset: number;
    readonly edgesOffset: number;
    readonly attributesOffset: number;
    readonly columnsOffset: number;
    readonly stringsOffset: number;
    readonly stringsLength: number;
    readonly stringUnits: number;
}

/** Definition values decoded once into ordinary immutable JavaScript values. */
interface Resource {
    readonly dest: Destination;
    readonly title: string | null;
    readonly anchor: string | null;
    readonly attributes: Attributes;
}

export class Decoder {
    private readonly view: DataView;
    private readonly utf8 = new TextDecoder("utf-8", { fatal: false, ignoreBOM: true });
    private layout!: Layout;
    /** The string blob, decoded once. */
    private text = "";
    private values: Decoded[] = [];
    /** One byte per record: whether an owner has named it. */
    private claimed = new Uint8Array(0);
    private readonly resources = new Map<number, Resource>();

    constructor(private readonly bytes: Uint8Array) {
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    }

    decode(): Document {
        this.header();
        const { nodeCount, nodesOffset, stringsOffset, stringUnits, totalSize } = this.layout;
        this.text = this.utf8.decode(this.bytes.subarray(stringsOffset, totalSize));
        if (this.text.length !== stringUnits) {
            throw new Error("native result string blob does not decode to the units its references address");
        }
        const values = new Array<Decoded>(nodeCount);
        this.values = values;
        this.claimed = new Uint8Array(nodeCount);
        for (let index = nodeCount - 1; index >= 0; --index) {
            const offset = nodesOffset + index * nodeSize;
            const rawKind = this.uint(offset + nodeField.kind);
            if (rawKind >= valueKindBase) {
                if (index === 0) throw new Error("native result must contain exactly one document at its root");
                values[index] = this.valueRecord(index, offset, rawKind);
                continue;
            }
            const kind = kinds[rawKind];
            if (kind === undefined || kind === "none") {
                throw new Error(`native result contains unknown node kind ${rawKind}`);
            }
            if ((index === 0) !== (kind === "document")) {
                throw new Error("native result must contain exactly one document at its root");
            }
            values[index] = this.node(index, offset, kind);
        }
        if (this.claimed[0] !== 0) throw new Error("native result root has an incoming relation");
        for (let index = 1; index < nodeCount; ++index) {
            if (this.claimed[index] !== 1) throw new Error(`native result node ${index} is not uniquely owned`);
        }
        const document = values[0];
        if (!(document instanceof DocumentNode)) throw new Error("native result root is not a document");
        return document;
    }

    private header(): void {
        if (this.bytes.byteLength < transferHeaderSize) throw new Error("truncated native result header");
        for (const [index, expected] of magic.entries()) {
            const actual = this.bytes[index];
            if (actual !== expected) {
                throw new Error(`invalid native result at byte ${index}: expected ${expected}, got ${String(actual)}`);
            }
        }
        const totalSize = this.uint(header.totalSize);
        if (totalSize !== this.bytes.byteLength) throw new Error("native result length does not match its header");
        const status = this.uint(header.status);
        if (status === 1) throw this.error();
        if (status !== 0) throw new Error(`unsupported native result status ${status}`);
        if (
            this.uint(header.errorCode) !== 0 ||
            this.uint(header.errorOffset) !== 0 ||
            this.uint(header.errorLength) !== 0
        ) {
            throw new Error("successful native result carries an error payload");
        }

        const layout: Layout = {
            totalSize,
            nodeCount: this.uint(header.nodeCount),
            edgeCount: this.uint(header.edgeCount),
            attributeCount: this.uint(header.attributeCount),
            columnCount: this.uint(header.columnCount),
            nodesOffset: this.uint(header.nodesOffset),
            edgesOffset: this.uint(header.edgesOffset),
            attributesOffset: this.uint(header.attributesOffset),
            columnsOffset: this.uint(header.columnsOffset),
            stringsOffset: this.uint(header.stringsOffset),
            stringsLength: this.uint(header.stringsLength),
            stringUnits: this.uint(header.stringUnits)
        };
        if (layout.nodeCount === 0) throw new Error("native result contains no document node");
        const expectedEdges = this.sectionEnd(transferHeaderSize, layout.nodeCount, nodeSize, "node table");
        const expectedAttributes = this.sectionEnd(expectedEdges, layout.edgeCount, 4, "edge table");
        const expectedColumns = this.sectionEnd(
            expectedAttributes,
            layout.attributeCount,
            attributeSize,
            "attribute table"
        );
        const expectedStrings = this.sectionEnd(expectedColumns, layout.columnCount, columnSize, "column table");
        const expectedEnd = this.sectionEnd(expectedStrings, layout.stringsLength, 1, "string blob");
        if (
            layout.nodesOffset !== transferHeaderSize ||
            layout.edgesOffset !== expectedEdges ||
            layout.attributesOffset !== expectedAttributes ||
            layout.columnsOffset !== expectedColumns ||
            layout.stringsOffset !== expectedStrings ||
            expectedEnd !== layout.totalSize
        ) {
            throw new Error("native result sections are not canonical and contiguous");
        }
        this.layout = layout;
    }

    private error(): ParseError {
        const code = errorCode(this.int(header.errorCode));
        const offset = this.uint(header.errorOffset);
        const length = this.uint(header.errorLength);
        if (
            offset !== transferHeaderSize ||
            this.sectionEnd(offset, length, 1, "error message") !== this.bytes.length
        ) {
            throw new Error("invalid native result error payload");
        }
        return new ParseError(code, this.utf8.decode(this.bytes.subarray(offset, offset + length)));
    }

    /**
     * The relations every record may carry only in the kinds that own them:
     * a metadata node belongs to the document, a singular field to a
     * directive or a table, and dimensions to an embed.
     */
    private relations(offset: number, kind: NativeKind | ValueKind): void {
        if (kind !== "document" && this.uint(offset + nodeField.metadata) !== noIndex) {
            throw new Error("invalid metadata relation");
        }
        if (
            kind !== "embedded" &&
            kind !== "crossEmbedded" &&
            (this.uint(offset + nodeField.dimensions) !== 0 || this.uint(offset + nodeField.dimensions + 4) !== 0)
        ) {
            throw new Error("dimensions require Embedded or CrossEmbedded");
        }
        if (
            kind !== "directive" &&
            kind !== "directiveBlock" &&
            kind !== "table" &&
            this.uint(offset + nodeField.fieldIndex) !== noIndex
        ) {
            throw new Error("node kind cannot own a singular field relation");
        }
    }

    private valueRecord(index: number, offset: number, rawKind: number): Decoded {
        const kind = valueKinds[rawKind - valueKindBase];
        if (kind === undefined) throw new Error(`native result contains unknown node kind ${rawKind}`);
        this.relations(offset, kind);
        const anchor = this.stringAt(offset + nodeField.anchor);
        if (anchor === "") throw new Error("empty normalized anchor");
        if (
            anchor !== null ||
            this.uint(offset + nodeField.classesCount) !== 0 ||
            this.uint(offset + nodeField.recordsCount) !== 0
        ) {
            throw new Error("non-node record carries Markup fields");
        }
        const flags = this.uint(offset + nodeField.flags);
        if (kind === "definitionBody") {
            this.flags(flags, 0, kind);
            return { body: this.content(index, offset) };
        }
        return this.metadataValue(offset, flags);
    }

    private node(index: number, offset: number, kind: NativeKind): Markup {
        this.relations(offset, kind);
        const scope = this.scope(offset);
        const anchor = this.stringAt(offset + nodeField.anchor);
        if (anchor === "") throw new Error("empty normalized anchor");
        const attributes = this.attributes(offset + nodeField.anchor);
        const flags = this.uint(offset + nodeField.flags);
        switch (kind) {
            case "document": {
                this.flags(flags, 0, kind);
                const content = this.content(index, offset);
                const metadata = this.documentMetadata(index, offset);
                const definitions = this.definitions(index, offset);
                return new DocumentNode(
                    scope,
                    anchor,
                    attributes,
                    content,
                    metadata,
                    definitions.footnotes,
                    definitions.specimens
                );
            }
            case "cite": {
                this.flags(flags, 0, kind);
                const citations = this.edgeRange(index, offset + nodeField.childStart, "citation", "child edge range");
                for (const item of citations) {
                    if (!(item instanceof CitationNode)) throw new Error("cite contains a non-citation record");
                }
                if (citations.length === 0) throw new Error("native result gives a cite no items");
                return new CiteNode(scope, anchor, attributes, citations as Citation[]);
            }
            case "paragraph":
            case "emphasis":
            case "strong":
            case "strikethrough":
            case "mark":
            case "insertion":
            case "span":
            case "superscript":
            case "subscript":
            case "directiveLabel":
            case "tableCaption":
                this.flags(flags, 0, kind);
                return new ContainerNode(kind, scope, anchor, attributes, this.content(index, offset));
            case "heading": {
                this.flags(flags, 0, kind);
                const level = this.int(offset + nodeField.scalar0);
                if (level < 1 || level > 6) throw new Error(`native result contains invalid heading level ${level}`);
                return new HeadingNode(scope, anchor, attributes, level, this.content(index, offset));
            }
            case "thematicBreak":
            case "softBreak":
            case "lineBreak":
                this.flags(flags, 0, kind);
                this.leaf(offset, kind);
                return new MarkupNode(kind, scope, anchor, attributes);
            case "callout": {
                // The variant is the first string slot and the fold marker the
                // scalar; the auxiliary range names the title's nodes in the
                // edge table, a node-valued list beside the content, and an
                // empty range is no title because a present title holds at
                // least one node.
                this.flags(flags, 0, kind);
                const title =
                    this.uint(offset + nodeField.auxiliaryCount) === 0
                        ? null
                        : this.markupRange(
                              index,
                              offset + nodeField.auxiliaryStart,
                              "callout title",
                              "callout title range",
                              "native result callout title is not ordinary content"
                          );
                return new CalloutNode(
                    scope,
                    anchor,
                    attributes,
                    this.string(offset, 0),
                    this.nullableBoolean(this.int(offset + nodeField.scalar0), "callout fold marker"),
                    title,
                    this.content(index, offset)
                );
            }
            case "list":
                return this.list(index, offset, scope, anchor, attributes, flags);
            case "listItem":
                this.flags(flags, 0, kind);
                return new ListItemNode(scope, anchor, attributes, this.string(offset, 0), this.content(index, offset));
            case "codeBlock":
                this.flags(flags, 0b11, kind);
                this.leaf(offset, kind);
                return new CodeBlockNode(
                    scope,
                    anchor,
                    attributes,
                    this.string(offset, 0),
                    this.string(offset, 1),
                    this.required(offset, 2),
                    (flags & 1) !== 0,
                    (flags & 2) !== 0
                );
            case "htmlBlock":
            case "text":
            case "code":
            case "html":
            case "comment":
            case "formulaBlock":
                this.flags(flags, 0, kind);
                this.leaf(offset, kind);
                return new LiteralNode(kind, scope, anchor, attributes, this.required(offset, 0));
            case "formula":
                this.flags(flags, 0, kind);
                this.leaf(offset, kind);
                return new FormulaNode(
                    scope,
                    anchor,
                    attributes,
                    this.placement(this.int(offset + nodeField.scalar0)),
                    this.required(offset, 0)
                );
            case "table":
                return this.table(index, offset, scope, anchor, attributes, flags);
            case "definitionList": {
                this.flags(flags, 0, kind);
                const definitions = this.content(index, offset);
                for (const definition of definitions) {
                    if (!(definition instanceof DefinitionNode)) throw new Error("invalid definition list child");
                }
                if (definitions.length === 0) throw new Error("empty definition list");
                return new DefinitionListNode(scope, anchor, attributes, definitions as DefinitionNode[]);
            }
            case "definition": {
                this.flags(flags, 1, kind);
                const bodies = this.edgeRange(index, offset + nodeField.childStart, "body", "child edge range");
                if (bodies.length === 0) throw new Error("definition has no bodies");
                const content = new Array<readonly Markup[]>(bodies.length);
                for (const [at, value] of bodies.entries()) {
                    if (!("body" in value)) throw new Error("invalid definition body");
                    content[at] = value.body;
                }
                const term = this.markupRange(
                    index,
                    offset + nodeField.auxiliaryStart,
                    "term",
                    "term range",
                    "invalid definition term"
                );
                return new DefinitionNode(scope, anchor, attributes, term, content, (flags & 1) !== 0);
            }
            case "directiveBlock":
                this.flags(flags, 0, kind);
                return new DirectiveBlockNode(
                    scope,
                    anchor,
                    attributes,
                    this.string(offset, 0),
                    this.directiveLabel(index, offset),
                    this.content(index, offset)
                );
            case "directive": {
                this.flags(flags, 0, kind);
                const name = this.string(offset, 0);
                const label = this.directiveLabel(index, offset);
                if (name === null) throw new Error("inline directive requires a name");
                if (this.content(index, offset).length !== 0) {
                    throw new Error("inline directive contains block content");
                }
                return new DirectiveNode(scope, anchor, attributes, name, label);
            }
            case "crossLink":
            case "crossEmbedded": {
                this.flags(flags, 0, kind);
                this.leaf(offset, kind);
                if (this.int(offset + nodeField.scalar0) !== 2) {
                    throw new Error("cross reference requires a cross destination");
                }
                const label = this.string(offset, 2);
                const dest = this.destination(offset);
                if (kind === "crossLink") return new CrossLinkNode(scope, anchor, attributes, dest, label);
                const dimensions = this.dimensions(offset);
                if (dimensions !== null && label === null) throw new Error("dimensions require an authored label");
                return new CrossEmbeddedNode(scope, anchor, attributes, dest, label, dimensions);
            }
            case "link":
            case "embedded": {
                this.flags(flags, 0, kind);
                const resource = this.resource(index, offset);
                const merged = mergeAttributes(resource.attributes, attributes);
                const content = this.content(index, offset);
                if (kind === "link") {
                    return new LinkNode(
                        scope,
                        anchor ?? resource.anchor,
                        merged,
                        resource.dest,
                        resource.title,
                        content
                    );
                }
                return new EmbeddedNode(
                    scope,
                    anchor ?? resource.anchor,
                    merged,
                    resource.dest,
                    resource.title,
                    this.dimensions(offset),
                    content
                );
            }
            case "tableRow": {
                this.flags(flags, 0, kind);
                const cells = this.content(index, offset);
                for (const cell of cells) {
                    if (!(cell instanceof TableCellNode)) throw new Error("table row contains a non-cell node");
                }
                return new TableRowNode(scope, anchor, attributes, cells as TableCell[]);
            }
            case "tableCell": {
                this.flags(flags, 0, kind);
                const rowspan = this.integer(offset + nodeField.integer, "table scalar");
                const colspan = this.integer(offset + nodeField.integer2, "table scalar");
                if (rowspan < 1 || colspan < 1) throw new Error("invalid table cell spans");
                return new TableCellNode(scope, anchor, attributes, rowspan, colspan, this.content(index, offset));
            }
            case "citation": {
                // A citation (M4): its referent branch is the scalar, the bib
                // mode the integer and its key or id the first string; its
                // prefix is its child range and its suffix its auxiliary range.
                this.flags(flags, 0, kind);
                const suffix =
                    this.uint(offset + nodeField.auxiliaryCount) === 0
                        ? []
                        : this.markupRange(
                              index,
                              offset + nodeField.auxiliaryStart,
                              "suffix",
                              "suffix range",
                              "native result suffix is not ordinary content"
                          );
                return new CitationNode(
                    scope,
                    anchor,
                    attributes,
                    this.referent(offset),
                    this.content(index, offset),
                    suffix
                );
            }
            case "footnote":
                // A footnote (M4): its id is the first string and its content its child range.
                this.flags(flags, 0, kind);
                return new FootnoteNode(
                    scope,
                    anchor,
                    attributes,
                    this.required(offset, 0),
                    this.content(index, offset)
                );
            case "specimen":
                this.flags(flags, 1, kind);
                return new SpecimenNode(
                    scope,
                    anchor,
                    attributes,
                    this.string(offset, 0),
                    (flags & 1) === 0 ? null : this.integer(offset + nodeField.integer, "specimen start"),
                    this.content(index, offset)
                );
            case "metadata":
                return this.metadata(index, offset, scope, anchor, attributes, flags);
        }
    }

    private list(
        index: number,
        offset: number,
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        flags: number
    ): ListNode {
        this.flags(flags, 0x3ff, "list");
        const flavor = this.flavor(this.int(offset + nodeField.scalar0));
        const start = (flags & 1) === 0 ? null : this.integer(offset + nodeField.integer, "list start");
        if (flavor === "bullet" && start !== null) throw new Error("native result gives a bullet list a start");
        const items = this.content(index, offset);
        for (const item of items) {
            if (!(item instanceof ListItemNode)) throw new Error("list contains a non-item node");
        }
        const variantRaw = (flags >> 2) & 0x7;
        const lowercased = (flags & (1 << 9)) !== 0;
        let variant: OrderedListVariant | null = null;
        let delimiter: OrderedListDelimiter | null = null;
        if (start !== null) {
            if (variantRaw === 1) variant = "decimal";
            else if (variantRaw === 2) variant = { kind: "alpha", lowercased };
            else if (variantRaw === 3) variant = { kind: "roman", lowercased };
            else if (variantRaw === 4) variant = "default";
            const delimiterRaw = (flags >> 5) & 0x7;
            if (delimiterRaw === 1) delimiter = "period";
            else if (delimiterRaw === 2) delimiter = { kind: "parenthesis", closed: (flags & (1 << 8)) !== 0 };
            else if (delimiterRaw === 3) delimiter = "default";
            if (variant === null || delimiter === null) throw new Error("invalid ordered list facts");
        }
        return new ListNode(
            scope,
            anchor,
            attributes,
            flavor,
            start,
            variant,
            delimiter,
            (flags & 2) !== 0,
            items as ListItem[]
        );
    }

    private table(
        index: number,
        offset: number,
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        flags: number
    ): TableNode {
        this.flags(flags, 0, "table");
        const columnStart = this.uint(offset + nodeField.auxiliaryStart);
        const columnCount = this.uint(offset + nodeField.auxiliaryCount);
        this.range(columnStart, columnCount, this.layout.columnCount, "table column range");
        if (columnCount === 0) throw new Error("table has no columns");
        const columns = new Array<TableColumn>(columnCount);
        for (let at = 0; at < columnCount; ++at) {
            const column = this.layout.columnsOffset + (columnStart + at) * columnSize;
            const flow = this.flow(this.uint(column));
            const present = this.uint(column + 4);
            if (present > 1) throw new Error("invalid table column width presence");
            const value = this.view.getFloat64(column + 8, true);
            if (present && (!Number.isFinite(value) || value <= 0)) throw new Error("invalid table column width");
            columns[at] = { flow, relative: present ? value : null };
        }
        const rows = this.content(index, offset);
        for (const row of rows) {
            if (!(row instanceof TableRowNode)) throw new Error("table contains a non-row node");
        }
        const headCount = this.int(offset + nodeField.scalar0);
        const contentCount = this.integer(offset + nodeField.integer2, "table scalar");
        const footCount = this.integer(offset + nodeField.integer, "table scalar");
        if (
            headCount < 0 ||
            contentCount < 0 ||
            footCount < 0 ||
            headCount + contentCount + footCount !== rows.length
        ) {
            throw new Error("invalid table row groups");
        }
        const caption = this.ownedField(index, offset, "table caption field contains a non-caption node");
        if (caption !== null && !(caption instanceof ContainerNode && caption.kind === "tableCaption")) {
            throw new Error("table caption field contains a non-caption node");
        }
        return new TableNode(
            scope,
            anchor,
            attributes,
            caption as ContainerNode<"tableCaption"> | null,
            columns,
            (rows as TableRow[]).slice(0, headCount),
            (rows as TableRow[]).slice(headCount, headCount + contentCount),
            (rows as TableRow[]).slice(headCount + contentCount)
        );
    }

    private directiveLabel(index: number, offset: number): DirectiveLabel | null {
        const label = this.ownedField(index, offset, "directive label field contains a non-label node");
        if (label !== null && !(label instanceof ContainerNode && label.kind === "directiveLabel")) {
            throw new Error("directive label field contains a non-label node");
        }
        return label as ContainerNode<"directiveLabel"> | null;
    }

    /** The node a singular field relation names, claimed by its owner. */
    private ownedField(index: number, offset: number, message: string): Decoded | null {
        const target = this.uint(offset + nodeField.fieldIndex);
        if (target === noIndex) return null;
        const value = this.claim(index, target, "owned node");
        if (!isMarkup(value)) throw new Error(message);
        return value;
    }

    private content(index: number, offset: number): Markup[] {
        return this.markupRange(
            index,
            offset + nodeField.childStart,
            "child",
            "child edge range",
            "native result child is not ordinary content"
        );
    }

    /** An edge range whose every node is ordinary content. */
    private markupRange(index: number, at: number, field: string, rangeName: string, message: string): Markup[] {
        const values = this.edgeRange(index, at, field, rangeName);
        for (const value of values) {
            if (!isContent(value)) throw new Error(message);
        }
        return values as Markup[];
    }

    /**
     * The records an edge range at `at` (a start then a count) names, each
     * claimed by this owner: the range lies in the edge table, every edge
     * points forward past `index`, and no record is named twice.
     */
    private edgeRange(index: number, at: number, field: string, rangeName: string): Decoded[] {
        const start = this.uint(at);
        const count = this.uint(at + 4);
        this.range(start, count, this.layout.edgeCount, rangeName);
        const values = new Array<Decoded>(count);
        for (let offset = 0; offset < count; ++offset) {
            values[offset] = this.claim(index, this.edge(start + offset), field);
        }
        return values;
    }

    private claim(index: number, target: number, field: string): Decoded {
        if (target <= index || target >= this.values.length) {
            throw new Error(`native result ${field} relation does not point forward`);
        }
        if (this.claimed[target] !== 0) throw new Error(`native result node ${target} has multiple owners`);
        this.claimed[target] = 1;
        const value = this.values[target];
        if (value === undefined) throw new Error(`native result ${field} was not constructed`);
        return value;
    }

    private referent(offset: number): CitationReferent {
        const branch = this.int(offset + nodeField.scalar0);
        switch (branch) {
            case 1:
                return {
                    kind: "bib",
                    key: this.required(offset, 0),
                    mode: this.bibMode(this.integer(offset + nodeField.integer, "bib mode"))
                };
            case 2:
                return { kind: "footnote", id: this.required(offset, 0) };
            case 3:
                return { kind: "specimen", id: this.required(offset, 0) };
            default:
                throw new Error(`native result contains unknown referent kind ${String(branch)}`);
        }
    }

    private bibMode(value: number): BibMode {
        if (value === 1) return "normal";
        if (value === 2) return "authorInText";
        if (value === 3) return "suppressAuthor";
        throw new Error(`native result contains invalid bib mode ${String(value)}`);
    }

    private definitions(
        index: number,
        offset: number
    ): { readonly footnotes: readonly Footnote[]; readonly specimens: readonly Specimen[] } {
        // The document's definitions are its auxiliary range: footnotes then
        // specimens, in scope order (M4).
        if (this.uint(offset + nodeField.auxiliaryCount) === 0) return { footnotes: [], specimens: [] };
        const footnotes: Footnote[] = [];
        const specimens: Specimen[] = [];
        for (const value of this.edgeRange(
            index,
            offset + nodeField.auxiliaryStart,
            "definition",
            "definitions range"
        )) {
            if (value instanceof SpecimenNode) specimens.push(value);
            else if (value instanceof FootnoteNode) {
                if (specimens.length !== 0) throw new Error("document footnotes follow specimens");
                footnotes.push(value);
            } else throw new Error("document definitions contain a non-definition record");
        }
        return { footnotes, specimens };
    }

    /**
     * Every occurrence of one reference definition reads through one resource
     * in the C tree, and a link or image record's integer names the first
     * node that did. Each record carries the same string references, so the
     * resource decodes from whichever occurrence is met first, and every
     * record naming that index shares the one value.
     */
    private resource(index: number, offset: number): Resource {
        const first = this.integer(offset + nodeField.integer, "resource index");
        if (first < 0 || first > index) {
            throw new Error("native result resource does not name its first occurrence");
        }
        let resource = this.resources.get(first);
        if (resource === undefined) {
            const definition = this.layout.nodesOffset + first * nodeSize;
            const kind = kinds[this.uint(definition + nodeField.kind)];
            if (
                (kind !== "link" && kind !== "embedded") ||
                this.integer(definition + nodeField.integer, "resource index") !== first
            ) {
                throw new Error("invalid definition resource");
            }
            const inherited = definition + nodeField.inheritedAttributes;
            const anchor = this.stringAt(inherited);
            if (anchor === "") throw new Error("empty normalized anchor");
            resource = {
                dest: this.destination(definition),
                title: this.string(definition, 2),
                anchor,
                attributes: this.attributes(inherited)
            };
            this.resources.set(first, resource);
        }
        return resource;
    }

    /**
     * A tagged `Destination`: the branch is the record's scalar, and the
     * branch's own strings follow -- the url, or the path and the optional
     * anchor -- so a field of the other branch is never read.
     */
    private destination(offset: number): Destination {
        const branch = this.int(offset + nodeField.scalar0);
        switch (branch) {
            case 1:
                return { kind: "url", value: this.required(offset, 0) };
            case 2:
                return { kind: "cross", path: this.required(offset, 0), anchor: this.string(offset, 1) };
            default:
                throw new Error(`native result contains unknown destination kind ${String(branch)}`);
        }
    }

    private string(offset: number, slot: number): string | null {
        return this.stringAt(offset + nodeField.strings + slot * 8);
    }

    private required(offset: number, slot: number): string {
        const value = this.string(offset, slot);
        if (value === null) throw new Error("native result is missing a required string");
        return value;
    }

    private requiredAt(referenceOffset: number): string {
        const value = this.stringAt(referenceOffset);
        if (value === null) throw new Error("native result is missing a required string");
        return value;
    }

    /** A string reference: a start and a length in UTF-16 code units of the decoded blob. */
    private stringAt(referenceOffset: number): string | null {
        const start = this.uint(referenceOffset);
        const length = this.uint(referenceOffset + 4);
        if (start === noIndex) {
            if (length !== 0) throw new Error("absent native string has a nonzero length");
            return null;
        }
        const end = start + length;
        if (end > this.text.length) throw new Error("native result string lies outside the string blob");
        return this.text.slice(start, end);
    }

    private edge(index: number): number {
        return this.uint(this.layout.edgesOffset + index * 4);
    }

    private scope(offset: number): Scope {
        const at = offset + nodeField.scope;
        return {
            start: { line: this.int(at), column: this.int(at + 4) },
            end: { line: this.int(at + 8), column: this.int(at + 12) }
        };
    }

    /**
     * The attributes an anchor reference at `offset` leads: the class range
     * and the record range follow it. Two empty ranges are the one shared
     * empty value; anything else is built and frozen once.
     */
    private attributes(offset: number): Attributes {
        const classStart = this.uint(offset + 8);
        const classCount = this.uint(offset + 12);
        const recordStart = this.uint(offset + 16);
        const recordCount = this.uint(offset + 20);
        this.range(classStart, classCount, this.layout.attributeCount, "attribute range");
        this.range(recordStart, recordCount, this.layout.attributeCount, "attribute range");
        if (classCount === 0 && recordCount === 0) return Attributes.empty;
        const classes = new Array<string>(classCount);
        for (let at = 0; at < classCount; ++at) {
            const entry = this.layout.attributesOffset + (classStart + at) * attributeSize;
            const value = this.requiredAt(entry + 8);
            if (this.requiredAt(entry) !== "" || value.length === 0) throw new Error("class has a record name");
            classes[at] = value;
        }
        const records = this.attributeRange(recordStart, recordCount);
        for (const record of records) {
            if (record.name === "id" || record.name === "class" || record.name.length === 0) {
                throw new Error("invalid normalized attribute record");
            }
        }
        return Object.freeze({ classes: Object.freeze(classes), records: Object.freeze(records) });
    }

    private attributeRange(start: number, count: number): AttributeRecord[] {
        this.range(start, count, this.layout.attributeCount, "attribute range");
        const records = new Array<AttributeRecord>(count);
        for (let at = 0; at < count; ++at) {
            const entry = this.layout.attributesOffset + (start + at) * attributeSize;
            records[at] = Object.freeze({ name: this.requiredAt(entry), value: this.requiredAt(entry + 8) });
        }
        return records;
    }

    private dimensions(offset: number): Dimensions | null {
        // The value occupies two u32s. Zero width encodes absence and requires
        // zero height; otherwise width is required and zero height is optional.
        const width = this.uint(offset + nodeField.dimensions);
        const height = this.uint(offset + nodeField.dimensions + 4);
        if (width > 0x7fffffff || height > 0x7fffffff || (width === 0 && height !== 0)) {
            throw new Error("invalid dimensions");
        }
        return width === 0 ? null : { width, height: height === 0 ? null : height };
    }

    private documentMetadata(index: number, offset: number): Metadata | null {
        const target = this.uint(offset + nodeField.metadata);
        if (target === noIndex) return null;
        const value = this.claim(index, target, "metadata");
        if (!(value instanceof MetadataNode)) throw new Error("invalid metadata relation");
        return value;
    }

    private metadata(
        index: number,
        offset: number,
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        flags: number
    ): MetadataNode {
        this.flags(flags, 0x3ff, "metadata");
        const values = this.edgeRange(index, offset + nodeField.childStart, "metadata fields", "child edge range");
        let cursor = 0;
        const field = (bit: number): MetadataValue | null => {
            if ((flags & (1 << bit)) === 0) return null;
            const value = values[cursor++];
            if (
                value === undefined ||
                isMarkup(value) ||
                !("kind" in value) ||
                (value.kind !== "scalar" && value.kind !== "list")
            ) {
                throw new Error("invalid metadata field value");
            }
            return value;
        };
        const metadata = new MetadataNode(
            scope,
            anchor,
            attributes,
            field(0),
            field(1),
            field(2),
            field(3),
            field(4),
            field(5),
            field(6),
            field(7),
            field(8),
            field(9)
        );
        if (cursor !== values.length) throw new Error("invalid metadata field count");
        return metadata;
    }

    private metadataValue(offset: number, flags: number): MetadataValue {
        this.leaf(offset, "metadataValue");
        const branch = this.int(offset + nodeField.scalar0);
        if (branch === 1) {
            switch (flags) {
                case 0:
                    return { kind: "scalar", value: { kind: "null" } };
                case 1: {
                    const value = this.integer(offset + nodeField.integer, "metadata boolean");
                    if (value !== 0 && value !== 1) throw new Error("invalid metadata boolean");
                    return { kind: "scalar", value: { kind: "bool", value: value === 1 } };
                }
                case 2:
                    return { kind: "scalar", value: { kind: "number", value: this.required(offset, 1) } };
                case 3:
                    return { kind: "scalar", value: { kind: "text", value: this.required(offset, 1) } };
                default:
                    throw new Error("invalid metadata scalar");
            }
        }
        if (branch !== 2) throw new Error("invalid metadata value");
        this.flags(flags, 0, "metadataValue");
        const entries = this.attributeRange(
            this.uint(offset + nodeField.auxiliaryStart),
            this.uint(offset + nodeField.auxiliaryCount)
        );
        const items = new Array<{ readonly kind: "number" | "text"; readonly value: string }>(entries.length);
        for (const [at, item] of entries.entries()) {
            if (item.name !== "number" && item.name !== "text") throw new Error("invalid metadata item");
            items[at] = { kind: item.name, value: item.value };
        }
        return { kind: "list", items };
    }

    private flags(flags: number, allowed: number, kind: string): void {
        if ((flags & ~allowed) !== 0) throw new Error(`native result contains invalid flags for ${kind}`);
    }

    private leaf(offset: number, kind: string): void {
        if (this.uint(offset + nodeField.childCount) !== 0) {
            throw new Error(`native result gives leaf ${kind} child relations`);
        }
    }

    private range(start: number, count: number, limit: number, field: string): void {
        if (start > limit || count > limit - start) throw new Error(`native result contains an invalid ${field}`);
    }

    private sectionEnd(start: number, count: number, width: number, field: string): number {
        const end = start + count * width;
        if (!Number.isSafeInteger(end) || end > this.bytes.length) {
            throw new Error(`native result ${field} exceeds its allocation`);
        }
        return end;
    }

    /** A signed 64-bit field as a JavaScript integer, from its two halves. */
    private integer(offset: number, field: string): number {
        const value = this.view.getInt32(offset + 4, true) * 0x1_0000_0000 + this.view.getUint32(offset, true);
        if (!Number.isSafeInteger(value)) throw new Error(`native ${field} exceeds JavaScript integer precision`);
        return value;
    }

    private nullableBoolean(value: number, field: string): boolean | null {
        if (value === -1) return null;
        if (value === 0) return false;
        if (value === 1) return true;
        throw new Error(`native result contains invalid ${field} ${value}`);
    }

    private placement(value: number): Placement {
        if (value === 1) return "embedded";
        if (value === 2) return "standalone";
        throw new Error(`native result contains invalid placement mode ${value}`);
    }

    private flavor(value: number): ListFlavor {
        if (value === 1) return "bullet";
        if (value === 2) return "ordered";
        throw new Error(`native result contains invalid list flavor ${value}`);
    }

    private flow(value: number): Flow {
        const flow = flows[value];
        if (flow === undefined) throw new Error(`native result contains invalid table flow ${value}`);
        return flow;
    }

    private uint(offset: number): number {
        return this.view.getUint32(offset, true);
    }

    private int(offset: number): number {
        return this.view.getInt32(offset, true);
    }
}

/**
 * A link or image's attributes are its definition's, then its own. A side
 * without any keeps the other's value as it is; otherwise the merged
 * sequence is one new frozen value.
 */
function mergeAttributes(inherited: Attributes, primary: Attributes): Attributes {
    if (primary === Attributes.empty) return inherited;
    if (inherited === Attributes.empty) return primary;
    return Object.freeze({
        classes: primary.classes.length ? Object.freeze([...inherited.classes, ...primary.classes]) : inherited.classes,
        records: primary.records.length ? Object.freeze([...inherited.records, ...primary.records]) : inherited.records
    });
}

function errorCode(value: number): ParseErrorCode {
    if (value === 1) return "invalidArgument";
    if (value === 2) return "allocationFailed";
    return "internal";
}
