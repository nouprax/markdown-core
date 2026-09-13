import type { Attributes, Dimensions, Metadata, MetadataValue } from "../values.js";
import type { MarkupBase } from "../model/base.js";
import type { DirectiveLabel } from "../model/directive-label.js";
import type { Citation } from "../model/cite.js";
import type { Document } from "../model/document.js";
import type { Specimen } from "../model/specimen.js";
import type { Footnote } from "../model/footnote.js";
import type { ListItem } from "../model/list.js";
import type { Markup } from "../model/markup.js";
import type { TableCell, TableRow } from "../model/table.js";
import { ParseError, type ParseErrorCode } from "../parse-error.js";
import { TreeDumper } from "../tree-dumper.js";
import type { BibMode, CitationReferent, Destination, ListFlavor, Placement, Scope, Flow } from "../values.js";
import { kinds, type NativeKind } from "./kinds.js";

/*
 * MCB1 is an ES-only result ABI over WebAssembly linear memory. Native emits
 * fixed-width records in breadth-first order, so relationships always point
 * forward and this decoder can construct the immutable value tree bottom-up.
 * It performs no calls into Wasm and retains no view after decodeDocument
 * returns; the runtime frees the result immediately afterwards.
 */

const magic = [0x4d, 0x43, 0x42, 0x31] as const;
export const transferHeaderSize = 64;
const nodeSize = 160;
const attributeSize = 16;
const columnSize = 16;
const noIndex = 0xffff_ffff;
/**
 * The scoped values travel as records above the node-kind space (M4):
 * a citation's prefix is its child range and its suffix its auxiliary range,
 * a footnote's content is its child range, a cite's items are its child
 * range, and the document's definitions are its auxiliary range.
 */
type ValueKind = "definitionBody" | "citation" | "footnote" | "specimen" | "metadata" | "metadataValue";
const valueKindBase = 0x100;
const valueKinds: readonly ValueKind[] = Object.freeze([
    "citation",
    "footnote",
    "specimen",
    "metadata",
    "metadataValue",
    "definitionBody"
]);
type DefinitionBody = { readonly body: readonly Markup[] };
type Decoded = DefinitionBody | Markup | Citation | Footnote | Specimen | Metadata | MetadataValue;
const isMarkup = (value: Decoded): value is Markup => "kind" in value && "scope" in value;

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
    stringsLength: 60
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

type MarkupValue = Markup extends infer Node ? (Node extends Markup ? Omit<Node, "dump"> : never) : never;
type MarkupValueOf<Kind extends Markup["kind"]> = Extract<MarkupValue, { readonly kind: Kind }>;

interface ResultLayout {
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
}

/** Definition values decoded once into ordinary immutable JavaScript values. */
interface Resource {
    readonly dest: Destination;
    readonly title: string | null;
    readonly anchor: string | null;
    readonly attributes: Attributes;
}

interface NodeRecord {
    readonly index: number;
    readonly offset: number;
    readonly kind: NativeKind | ValueKind;
    readonly flags: number;
    readonly scope: Scope;
    readonly childStart: number;
    readonly childCount: number;
    readonly fieldIndex: number;
    readonly auxiliaryStart: number;
    readonly auxiliaryCount: number;
    readonly scalar0: number;
    readonly integer: bigint;
    readonly integer2: bigint;
}

export class NodeDecoder {
    private readonly view: DataView;
    private readonly utf8Decoder = new TextDecoder("utf-8", { fatal: false });
    private layout!: ResultLayout;
    private values: readonly (Decoded | undefined)[] = [];
    private readonly resources = new Map<number, Resource>();

    constructor(private readonly bytes: Uint8Array) {
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    }

    decodeDocument(): Document {
        this.readHeader();
        this.validateTopology();
        const values: (Decoded | undefined)[] = Array.from({ length: this.layout.nodeCount });
        this.values = values;
        for (let remaining = this.layout.nodeCount; remaining > 0; --remaining) {
            const index = remaining - 1;
            const record = this.readRecord(index);
            if (record.kind === "citation") values[index] = this.citation(record);
            else if (record.kind === "footnote") values[index] = this.footnote(record);
            else if (record.kind === "specimen") values[index] = this.specimen(record);
            else if (record.kind === "definitionBody") {
                this.flags(record, 0);
                values[index] = { body: this.content(record) };
            } else if (record.kind === "metadata") values[index] = this.metadata(record);
            else if (record.kind === "metadataValue") values[index] = this.metadataValue(record);
            else values[index] = this.markup(this.value(record));
        }
        const document = values[0];
        if (document === undefined || !isMarkup(document) || document.kind !== "document") {
            throw new Error("native result root is not a document");
        }
        return document;
    }

    private readHeader(): void {
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
        if (status === 1) throw this.parseError();
        if (status !== 0) throw new Error(`unsupported native result status ${status}`);
        if (
            this.uint(header.errorCode) !== 0 ||
            this.uint(header.errorOffset) !== 0 ||
            this.uint(header.errorLength) !== 0
        ) {
            throw new Error("successful native result carries an error payload");
        }

        const layout: ResultLayout = {
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
            stringsLength: this.uint(header.stringsLength)
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

    private parseError(): ParseError {
        const code = errorCode(this.int(header.errorCode));
        const offset = this.uint(header.errorOffset);
        const length = this.uint(header.errorLength);
        if (
            offset !== transferHeaderSize ||
            this.sectionEnd(offset, length, 1, "error message") !== this.bytes.length
        ) {
            throw new Error("invalid native result error payload");
        }
        return new ParseError(code, this.utf8Decoder.decode(this.bytes.subarray(offset, offset + length)));
    }

    private readRecord(index: number): NodeRecord {
        const offset = this.layout.nodesOffset + index * nodeSize;
        const rawKind = this.uint(offset + nodeField.kind);
        const kind = rawKind >= valueKindBase ? valueKinds[rawKind - valueKindBase] : kinds[rawKind];
        if (!kind || kind === "none") throw new Error(`native result contains unknown node kind ${rawKind}`);
        return {
            index,
            offset,
            kind,
            flags: this.uint(offset + nodeField.flags),
            scope: {
                start: { line: this.int(offset + nodeField.scope), column: this.int(offset + nodeField.scope + 4) },
                end: { line: this.int(offset + nodeField.scope + 8), column: this.int(offset + nodeField.scope + 12) }
            },
            childStart: this.uint(offset + nodeField.childStart),
            childCount: this.uint(offset + nodeField.childCount),
            fieldIndex: this.uint(offset + nodeField.fieldIndex),
            auxiliaryStart: this.uint(offset + nodeField.auxiliaryStart),
            auxiliaryCount: this.uint(offset + nodeField.auxiliaryCount),
            scalar0: this.int(offset + nodeField.scalar0),
            integer2: this.view.getBigInt64(offset + nodeField.integer2, true),
            integer: this.view.getBigInt64(offset + nodeField.integer, true)
        };
    }

    private validateTopology(): void {
        const incoming = new Uint8Array(this.layout.nodeCount);
        for (let index = 0; index < this.layout.nodeCount; ++index) {
            const record = this.readRecord(index);
            if ((index === 0) !== (record.kind === "document")) {
                throw new Error("native result must contain exactly one document at its root");
            }
            this.range(record.childStart, record.childCount, this.layout.edgeCount, "child edge range");
            for (let offset = 0; offset < record.childCount; ++offset) {
                this.recordRelation(record, this.edge(record.childStart + offset), incoming, "child");
            }
            const anchor = this.stringAt(record.offset + nodeField.anchor);
            if (anchor === "") throw new Error("empty normalized anchor");
            if (
                this.uint(record.offset) >= valueKindBase &&
                (anchor !== null ||
                    this.uint(record.offset + nodeField.classesCount) !== 0 ||
                    this.uint(record.offset + nodeField.recordsCount) !== 0)
            )
                throw new Error("scoped value carries Markup fields");
            const metadata = this.uint(record.offset + nodeField.metadata);
            if (metadata !== noIndex) {
                if (record.kind !== "document" || this.readRecord(metadata).kind !== "metadata")
                    throw new Error("invalid metadata relation");
                this.recordRelation(record, metadata, incoming, "metadata");
            }
            if (
                record.kind !== "embedded" &&
                record.kind !== "crossEmbedded" &&
                (this.uint(record.offset + nodeField.dimensions) !== 0 ||
                    this.uint(record.offset + nodeField.dimensions + 4) !== 0)
            )
                throw new Error("dimensions require Embedded or CrossEmbedded");
            if (record.fieldIndex !== noIndex) {
                if (record.kind !== "directive" && record.kind !== "directiveBlock" && record.kind !== "table") {
                    throw new Error("node kind cannot own a singular field relation");
                }
                this.recordRelation(record, record.fieldIndex, incoming, "owned node");
            }
            if (record.kind === "callout" && record.auxiliaryCount !== 0) {
                // The title's nodes are owned through the auxiliary range, as
                // a directive's label is through its index; a present title
                // holds at least one node, so the range's count is its presence.
                this.range(record.auxiliaryStart, record.auxiliaryCount, this.layout.edgeCount, "callout title range");
                for (let offset = 0; offset < record.auxiliaryCount; ++offset) {
                    this.recordRelation(record, this.edge(record.auxiliaryStart + offset), incoming, "title");
                }
            }
            // The document's definitions and a citation's suffix are owned
            // through the auxiliary range as well (M4).
            const auxiliary =
                record.kind === "document"
                    ? "definitions"
                    : record.kind === "citation"
                      ? "suffix"
                      : record.kind === "definition"
                        ? "term"
                        : null;
            if (auxiliary !== null && record.auxiliaryCount !== 0) {
                this.range(record.auxiliaryStart, record.auxiliaryCount, this.layout.edgeCount, `${auxiliary} range`);
                for (let offset = 0; offset < record.auxiliaryCount; ++offset) {
                    this.recordRelation(record, this.edge(record.auxiliaryStart + offset), incoming, auxiliary);
                }
            }
        }
        if (incoming[0] !== 0) throw new Error("native result root has an incoming relation");
        for (let index = 1; index < incoming.length; ++index) {
            if (incoming[index] !== 1) throw new Error(`native result node ${index} is not uniquely owned`);
        }
    }

    private recordRelation(record: NodeRecord, target: number, incoming: Uint8Array, field: string): void {
        if (target <= record.index || target >= incoming.length) {
            throw new Error(`native result ${field} relation does not point forward`);
        }
        if (incoming[target] !== 0) throw new Error(`native result node ${target} has multiple owners`);
        incoming[target] = 1;
    }

    private markup(value: MarkupValue): Markup {
        Object.defineProperty(value, "dump", {
            enumerable: false,
            value(this: Markup): string {
                return TreeDumper.dump(this);
            }
        });
        return value as Markup;
    }

    private value(record: NodeRecord): MarkupValue {
        // The scoped values are decoded by their owners, never as nodes,
        // so the kind dispatch below is over Markup kinds alone.
        const kind = record.kind;
        if (
            kind === "citation" ||
            kind === "footnote" ||
            kind === "specimen" ||
            kind === "metadata" ||
            kind === "metadataValue" ||
            kind === "definitionBody"
        ) {
            throw new Error(`native result places a ${kind} value where a node belongs`);
        }
        const base = this.base(record);
        switch (kind) {
            case "document":
                this.flags(record, 0);
                return {
                    ...base,
                    content: this.content(record),
                    metadata: this.documentMetadata(record),
                    ...this.definitions(record)
                } as MarkupValue;
            case "cite":
                this.flags(record, 0);
                return { ...base, citations: this.citations(record) } as MarkupValue;
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
                this.flags(record, 0);
                return { ...base, content: this.content(record) } as MarkupValue;
            case "heading": {
                this.flags(record, 0);
                const level = record.scalar0;
                if (level < 1 || level > 6) throw new Error(`native result contains invalid heading level ${level}`);
                return { ...base, level, content: this.content(record) } as MarkupValue;
            }
            case "thematicBreak":
            case "softBreak":
            case "lineBreak":
                this.flags(record, 0);
                this.leaf(record);
                return base as MarkupValue;
            case "callout":
                return this.callout(record);
            case "list":
                return this.list(record);
            case "listItem": {
                this.flags(record, 0);
                const marker = this.string(record, 0);
                return {
                    ...base,
                    marker,
                    get tasked() {
                        return marker !== null;
                    },
                    get completed() {
                        return marker !== null && marker !== " ";
                    },
                    content: this.content(record)
                } as MarkupValue;
            }
            case "codeBlock":
                this.flags(record, 0b11);
                this.leaf(record);
                return {
                    ...base,
                    info: this.string(record, 0),
                    language: this.string(record, 1),
                    literal: this.requiredString(record, 2),
                    fenced: (record.flags & 1) !== 0,
                    closed: (record.flags & 2) !== 0
                } as MarkupValue;
            case "htmlBlock":
            case "text":
            case "code":
            case "html":
            case "comment":
                this.flags(record, 0);
                this.leaf(record);
                return { ...base, literal: this.requiredString(record, 0) } as MarkupValue;
            case "formulaBlock":
                this.flags(record, 0);
                this.leaf(record);
                return { ...base, literal: this.requiredString(record, 0) } as MarkupValue;
            case "formula":
                this.flags(record, 0);
                this.leaf(record);
                return {
                    ...base,
                    mode: this.placement(record.scalar0),
                    literal: this.requiredString(record, 0)
                } as MarkupValue;
            case "tableCaption":
                this.flags(record, 0);
                return { ...this.base(record, "tableCaption"), content: this.content(record) };
            case "table":
                return this.table(record);
            case "definitionList": {
                this.flags(record, 0);
                const definitions = this.content(record).map((child) => {
                    if (child.kind !== "definition") throw new Error("invalid definition list child");
                    return child;
                });
                if (definitions.length === 0) throw new Error("empty definition list");
                return { ...base, definitions } as MarkupValue;
            }
            case "definition": {
                this.flags(record, 1);
                const term = this.edgeRange(record.auxiliaryStart, record.auxiliaryCount, "term").map((value) => {
                    if (!isMarkup(value)) throw new Error("invalid definition term");
                    return value;
                });
                const content = this.edgeRange(record.childStart, record.childCount, "body").map((value) => {
                    if (!("body" in value)) throw new Error("invalid definition body");
                    return value.body;
                });
                if (content.length === 0) throw new Error("definition has no bodies");
                return { ...base, term, content, compact: (record.flags & 1) !== 0 } as MarkupValue;
            }
            case "directiveBlock":
                return { ...base, ...this.directiveFields(record) } as MarkupValue;
            case "directive": {
                const fields = this.directiveFields(record);
                if (fields.name === null) throw new Error("inline directive requires a name");
                if (fields.content.length !== 0) throw new Error("inline directive contains block content");
                return {
                    ...base,
                    name: fields.name,
                    label: fields.label
                } as MarkupValue;
            }
            case "crossLink":
            case "crossEmbedded": {
                this.flags(record, 0);
                this.leaf(record);
                if (record.scalar0 !== 2) throw new Error("cross reference requires a cross destination");
                const label = this.string(record, 2);
                const fields = { ...base, dest: this.destination(record), label };
                if (kind === "crossEmbedded") {
                    const dimensions = this.dimensions(record);
                    if (dimensions !== null && label === null) throw new Error("dimensions require an authored label");
                    return { ...fields, dimensions } as MarkupValue;
                }
                return fields as MarkupValue;
            }
            case "link":
            case "embedded": {
                this.flags(record, 0);
                const resource = this.resource(record);
                return {
                    ...base,
                    dest: resource.dest,
                    title: resource.title,
                    ...(kind === "embedded"
                        ? {
                              dimensions: this.dimensions(record)
                          }
                        : {}),
                    content: this.content(record)
                } as MarkupValue;
            }
            case "tableRow":
                return this.tableRow(record);
            case "tableCell": {
                this.flags(record, 0);
                const rowspan = this.safeInteger(record.integer, "table scalar");
                const colspan = this.safeInteger(record.integer2, "table scalar");
                if (rowspan < 1 || colspan < 1) throw new Error("invalid table cell spans");
                return { ...base, rowspan, colspan, content: this.content(record) } as MarkupValue;
            }
        }
    }

    /**
     * The variant is the first string slot and the fold marker the scalar; the
     * auxiliary range names the title's nodes in the edge table, a node-valued
     * list beside the content, and an empty range is no title because a
     * present title holds at least one node.
     */
    private callout(record: NodeRecord): MarkupValueOf<"callout"> {
        this.flags(record, 0);
        let title: readonly Markup[] | null = null;
        if (record.auxiliaryCount !== 0) {
            this.range(record.auxiliaryStart, record.auxiliaryCount, this.layout.edgeCount, "callout title range");
            title = this.edgeRange(record.auxiliaryStart, record.auxiliaryCount, "callout title").map((value) => {
                if (!isMarkup(value)) throw new Error("native result callout title is a value, not a node");
                return value;
            });
        }
        return {
            ...this.base(record, "callout"),
            variant: this.string(record, 0),
            collapsed: this.nullableBoolean(record.scalar0, "callout fold marker"),
            title,
            content: this.content(record)
        };
    }

    private list(record: NodeRecord): MarkupValueOf<"list"> {
        this.flags(record, 0x3ff);
        const flavor = this.listFlavor(record.scalar0);
        const start = (record.flags & 1) === 0 ? null : this.safeInteger(record.integer, "list start");
        if (flavor === "bullet" && start !== null) throw new Error("native result gives a bullet list a start");
        const children = this.content(record);
        if (!children.every((child): child is ListItem => child.kind === "listItem")) {
            throw new Error("list contains a non-item node");
        }
        const variantRaw = (record.flags >> 2) & 0x7;
        const lowercased = (record.flags & (1 << 9)) !== 0;
        const variant =
            start === null
                ? null
                : variantRaw === 1
                  ? "decimal"
                  : variantRaw === 2
                    ? { kind: "alpha" as const, lowercased }
                    : variantRaw === 3
                      ? { kind: "roman" as const, lowercased }
                      : variantRaw === 4
                        ? "default"
                        : null;
        const delimiterRaw = (record.flags >> 5) & 0x7;
        const delimiter =
            start === null
                ? null
                : delimiterRaw === 1
                  ? "period"
                  : delimiterRaw === 2
                    ? { kind: "parenthesis" as const, closed: (record.flags & (1 << 8)) !== 0 }
                    : delimiterRaw === 3
                      ? "default"
                      : null;
        if (start !== null && (variant === null || delimiter === null)) throw new Error("invalid ordered list facts");
        return {
            ...this.base(record, "list"),
            flavor,
            start,
            variant,
            delimiter,
            tight: (record.flags & 2) !== 0,
            items: children
        };
    }

    private table(record: NodeRecord): MarkupValueOf<"table"> {
        this.flags(record, 0);
        this.range(record.auxiliaryStart, record.auxiliaryCount, this.layout.columnCount, "table column range");
        if (record.auxiliaryCount === 0) throw new Error("table has no columns");
        const columns = Array.from({ length: record.auxiliaryCount }, (_, index) => {
            const offset = this.layout.columnsOffset + (record.auxiliaryStart + index) * columnSize;
            const flow = this.flow(this.uint(offset));
            const present = this.uint(offset + 4);
            if (present > 1) throw new Error("invalid table column width presence");
            const value = this.view.getFloat64(offset + 8, true);
            if (present && (!Number.isFinite(value) || value <= 0)) throw new Error("invalid table column width");
            return { flow, relative: present ? value : null };
        });
        const rows = this.content(record);
        if (!rows.every((child): child is TableRow => child.kind === "tableRow")) {
            throw new Error("table contains a non-row node");
        }
        const headCount = record.scalar0;
        const contentCount = this.safeInteger(record.integer2, "table scalar");
        const footCount = this.safeInteger(record.integer, "table scalar");
        if (
            headCount < 0 ||
            contentCount < 0 ||
            footCount < 0 ||
            headCount + contentCount + footCount !== rows.length
        ) {
            throw new Error("invalid table row groups");
        }
        const caption = record.fieldIndex === noIndex ? null : this.values[record.fieldIndex];
        if (caption !== null && (caption === undefined || !isMarkup(caption) || caption.kind !== "tableCaption")) {
            throw new Error("table caption field contains a non-caption node");
        }
        return {
            ...this.base(record, "table"),
            caption,
            columns,
            head: rows.slice(0, headCount),
            content: rows.slice(headCount, headCount + contentCount),
            foot: rows.slice(headCount + contentCount)
        };
    }

    private tableRow(record: NodeRecord): Omit<TableRow, "dump"> {
        this.flags(record, 0);
        const cells = this.content(record);
        if (!cells.every((child): child is TableCell => child.kind === "tableCell")) {
            throw new Error("table row contains a non-cell node");
        }
        return { ...this.base(record, "tableRow"), cells };
    }

    private directiveFields(record: NodeRecord): {
        readonly name: string | null;
        readonly label: DirectiveLabel | null;
        readonly content: readonly Markup[];
    } {
        this.flags(record, 0);
        return {
            name: this.string(record, 0),
            label: this.directiveLabel(record),
            content: this.content(record)
        };
    }

    private directiveLabel(record: NodeRecord): DirectiveLabel | null {
        if (record.fieldIndex === noIndex) return null;
        const label = this.values[record.fieldIndex];
        if (label === undefined || !isMarkup(label) || label.kind !== "directiveLabel") {
            throw new Error("directive label field contains a non-label node");
        }
        return label;
    }

    private content(record: NodeRecord): readonly Markup[] {
        return this.edgeRange(record.childStart, record.childCount, "child").map((value) => {
            if (!isMarkup(value)) throw new Error("native result child is a value, not a node");
            return value;
        });
    }

    private edgeRange(start: number, count: number, field: string): readonly Decoded[] {
        return Array.from({ length: count }, (_, index) => {
            const value = this.values[this.edge(start + index)];
            if (!value) throw new Error(`native result ${field} was not constructed`);
            return value;
        });
    }

    /**
     * A citation (M4): its referent branch is the scalar, the bib mode the
     * integer and its key or id the first string; its prefix is its child
     * range and its suffix its auxiliary range.
     */
    private citation(record: NodeRecord): Citation {
        this.flags(record, 0);
        let suffix: readonly Markup[] = [];
        if (record.auxiliaryCount !== 0) {
            this.range(record.auxiliaryStart, record.auxiliaryCount, this.layout.edgeCount, "suffix range");
            suffix = this.edgeRange(record.auxiliaryStart, record.auxiliaryCount, "suffix").map((value) => {
                if (!isMarkup(value)) throw new Error("native result suffix is a value, not a node");
                return value;
            });
        }
        return { scope: record.scope, referent: this.referent(record), prefix: this.content(record), suffix };
    }

    private referent(record: NodeRecord): CitationReferent {
        switch (record.scalar0) {
            case 1:
                return { kind: "bib", key: this.requiredString(record, 0), mode: this.bibMode(record.integer) };
            case 2:
                return { kind: "footnote", id: this.requiredString(record, 0) };
            case 3:
                return { kind: "specimen", id: this.requiredString(record, 0) };
            default:
                throw new Error(`native result contains unknown referent kind ${String(record.scalar0)}`);
        }
    }

    private bibMode(value: bigint): BibMode {
        if (value === 1n) return "normal";
        if (value === 2n) return "authorInText";
        if (value === 3n) return "suppressAuthor";
        throw new Error(`native result contains invalid bib mode ${String(value)}`);
    }

    /** A footnote (M4): its id is the first string and its content its child range. */
    private footnote(record: NodeRecord): Footnote {
        this.flags(record, 0);
        return { scope: record.scope, id: this.requiredString(record, 0), content: this.content(record) };
    }

    private citations(record: NodeRecord): readonly Citation[] {
        const items = this.edgeRange(record.childStart, record.childCount, "citation").map((value) => {
            if (isMarkup(value) || !("referent" in value)) throw new Error("cite contains a non-citation record");
            return value;
        });
        if (items.length === 0) throw new Error("native result gives a cite no items");
        return items;
    }

    private specimen(record: NodeRecord): Specimen {
        this.flags(record, 1);
        return {
            scope: record.scope,
            id: this.string(record, 0),
            start: (record.flags & 1) === 0 ? null : this.safeInteger(record.integer, "specimen start"),
            content: this.content(record)
        };
    }

    private definitions(record: NodeRecord): { footnotes: readonly Footnote[]; specimens: readonly Specimen[] } {
        if (record.auxiliaryCount === 0) return { footnotes: [], specimens: [] };
        this.range(record.auxiliaryStart, record.auxiliaryCount, this.layout.edgeCount, "definitions range");
        const footnotes: Footnote[] = [];
        const specimens: Specimen[] = [];
        for (const value of this.edgeRange(record.auxiliaryStart, record.auxiliaryCount, "definition")) {
            if (isMarkup(value) || !("id" in value))
                throw new Error("document definitions contain a non-definition record");
            if ("start" in value) specimens.push(value);
            else {
                if (specimens.length !== 0) throw new Error("document footnotes follow specimens");
                footnotes.push(value);
            }
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
    private resource(record: NodeRecord): Resource {
        const first = this.safeInteger(record.integer, "resource index");
        if (first < 0 || first > record.index) {
            throw new Error("native result resource does not name its first occurrence");
        }
        let resource = this.resources.get(first);
        if (resource === undefined) {
            const definition = this.readRecord(first);
            if ((definition.kind !== "link" && definition.kind !== "embedded") || definition.integer !== BigInt(first))
                throw new Error("invalid definition resource");
            const offset = definition.offset + nodeField.inheritedAttributes;
            const anchor = this.stringAt(offset);
            if (anchor === "") throw new Error("empty normalized anchor");
            resource = {
                dest: this.destination(definition),
                title: this.string(definition, 2),
                anchor,
                attributes: this.attributes(offset)
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
    private destination(record: NodeRecord): Destination {
        switch (record.scalar0) {
            case 1:
                return { kind: "url", value: this.requiredString(record, 0) };
            case 2:
                return { kind: "cross", path: this.requiredString(record, 0), anchor: this.string(record, 1) };
            default:
                throw new Error(`native result contains unknown destination kind ${String(record.scalar0)}`);
        }
    }

    private string(record: NodeRecord, slot: number): string | null {
        return this.stringAt(record.offset + nodeField.strings + slot * 8);
    }

    private requiredString(record: NodeRecord, slot: number): string {
        const value = this.string(record, slot);
        if (value === null) throw new Error("native result is missing a required string");
        return value;
    }

    private requiredStringAt(referenceOffset: number): string {
        const value = this.stringAt(referenceOffset);
        if (value === null) throw new Error("native result is missing a required string");
        return value;
    }

    private stringAt(referenceOffset: number): string | null {
        const offset = this.uint(referenceOffset);
        const length = this.uint(referenceOffset + 4);
        if (offset === noIndex) {
            if (length !== 0) throw new Error("absent native string has a nonzero length");
            return null;
        }
        if (
            offset < this.layout.stringsOffset ||
            this.sectionEnd(offset, length, 1, "string") > this.layout.totalSize
        ) {
            throw new Error("native result string lies outside the string blob");
        }
        return this.utf8Decoder.decode(this.bytes.subarray(offset, offset + length));
    }

    private edge(index: number): number {
        return this.uint(this.layout.edgesOffset + index * 4);
    }

    private base<Kind extends Markup["kind"]>(
        record: NodeRecord,
        kind: Kind = record.kind as Kind
    ): Omit<MarkupBase<Kind>, "dump"> {
        const primaryAnchor = this.stringAt(record.offset + nodeField.anchor);
        const primary = this.attributes(record.offset + nodeField.anchor);
        if (kind !== "link" && kind !== "embedded")
            return { kind, scope: record.scope, anchor: primaryAnchor, attributes: primary };
        const inherited = this.resource(record);
        // Keep ordinary JS arrays. Definition-only occurrences reuse the native
        // immutable values; a local sequence creates its own merged array.
        const attributes = Object.freeze({
            classes: primary.classes.length
                ? Object.freeze([...inherited.attributes.classes, ...primary.classes])
                : inherited.attributes.classes,
            records: primary.records.length
                ? Object.freeze([...inherited.attributes.records, ...primary.records])
                : inherited.attributes.records
        });
        return { kind, scope: record.scope, anchor: primaryAnchor ?? inherited.anchor, attributes };
    }

    private attributes(offset: number): Attributes {
        const classes = this.attributeRange(this.uint(offset + 8), this.uint(offset + 12));
        if (classes.some((value) => value.name !== "" || value.value.length === 0))
            throw new Error("class has a record name");
        const records = this.attributeRange(this.uint(offset + 16), this.uint(offset + 20));
        if (records.some((value) => value.name === "id" || value.name === "class" || value.name.length === 0))
            throw new Error("invalid normalized attribute record");
        return Object.freeze({
            classes: Object.freeze(classes.map((value) => value.value)),
            records: Object.freeze(records)
        });
    }
    private attributeRange(start: number, count: number): readonly { readonly name: string; readonly value: string }[] {
        this.range(start, count, this.layout.attributeCount, "attribute range");
        return Array.from({ length: count }, (_, index) => {
            const offset = this.layout.attributesOffset + (start + index) * attributeSize;
            return Object.freeze({ name: this.requiredStringAt(offset), value: this.requiredStringAt(offset + 8) });
        });
    }
    private dimensions(record: NodeRecord): Dimensions | null {
        // The value occupies two u32s. Zero width encodes absence and requires
        // zero height; otherwise width is required and zero height is optional.
        const width = this.uint(record.offset + nodeField.dimensions);
        const height = this.uint(record.offset + nodeField.dimensions + 4);
        if (width > 0x7fffffff || height > 0x7fffffff || (width === 0 && height !== 0))
            throw new Error("invalid dimensions");
        return width === 0 ? null : { width, height: height === 0 ? null : height };
    }
    private documentMetadata(record: NodeRecord): Metadata | null {
        const index = this.uint(record.offset + nodeField.metadata);
        if (index === noIndex) return null;
        const value = this.values[index];
        if (!value || "kind" in value || !("title" in value)) throw new Error("invalid document metadata");
        return value;
    }
    private metadata(record: NodeRecord): Metadata {
        this.flags(record, 0x3ff);
        const values = this.edgeRange(record.childStart, record.childCount, "metadata fields");
        let cursor = 0;
        const field = (bit: number): MetadataValue | null => {
            if ((record.flags & (1 << bit)) === 0) return null;
            const value = values[cursor++];
            if (!value || "scope" in value || !("kind" in value) || (value.kind !== "scalar" && value.kind !== "list"))
                throw new Error("invalid metadata field value");
            return value;
        };
        const metadata: Metadata = {
            scope: record.scope,
            name: field(0),
            title: field(1),
            subtitle: field(2),
            time: field(3),
            date: field(4),
            authors: field(5),
            keywords: field(6),
            abstract: field(7),
            state: field(8),
            comment: field(9)
        };
        if (cursor !== values.length) throw new Error("invalid metadata field count");
        return metadata;
    }
    private metadataValue(record: NodeRecord): MetadataValue {
        this.leaf(record);
        let value: MetadataValue;
        if (record.scalar0 === 1) {
            switch (record.flags) {
                case 0:
                    value = { kind: "scalar", value: { kind: "null" } };
                    break;
                case 1:
                    if (record.integer !== 0n && record.integer !== 1n) throw new Error("invalid metadata boolean");
                    value = { kind: "scalar", value: { kind: "bool", value: record.integer === 1n } };
                    break;
                case 2:
                    value = { kind: "scalar", value: { kind: "number", value: this.requiredString(record, 1) } };
                    break;
                case 3:
                    value = { kind: "scalar", value: { kind: "text", value: this.requiredString(record, 1) } };
                    break;
                default:
                    throw new Error("invalid metadata scalar");
            }
        } else if (record.scalar0 === 2) {
            this.flags(record, 0);
            value = {
                kind: "list",
                items: this.attributeRange(record.auxiliaryStart, record.auxiliaryCount).map((item) => {
                    if (item.name !== "number" && item.name !== "text") throw new Error("invalid metadata item");
                    return { kind: item.name, value: item.value };
                })
            };
        } else throw new Error("invalid metadata value");
        return value;
    }

    private flags(record: NodeRecord, allowed: number): void {
        if ((record.flags & ~allowed) !== 0) throw new Error(`native result contains invalid flags for ${record.kind}`);
    }

    private leaf(record: NodeRecord): void {
        if (record.childCount !== 0) throw new Error(`native result gives leaf ${record.kind} child relations`);
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

    private safeInteger(value: bigint, field: string): number {
        const number = Number(value);
        if (!Number.isSafeInteger(number)) throw new Error(`native ${field} exceeds JavaScript integer precision`);
        return number;
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

    private listFlavor(value: number): ListFlavor {
        if (value === 1) return "bullet";
        if (value === 2) return "ordered";
        throw new Error(`native result contains invalid list flavor ${value}`);
    }

    private flow(value: number): Flow {
        const flows: readonly Flow[] = ["none", "left", "center", "right"];
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

function errorCode(value: number): ParseErrorCode {
    if (value === 1) return "invalidArgument";
    if (value === 2) return "allocationFailed";
    return "internal";
}
