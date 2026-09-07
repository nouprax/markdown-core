import type { MarkupBase } from "../model/base.js";
import type { DirectiveAttribute } from "../model/directive-attribute.js";
import type { DirectiveLabel } from "../model/directive-label.js";
import type { Citation } from "../model/cite.js";
import type { Document } from "../model/document.js";
import type { Footnote } from "../model/footnote.js";
import type { ListItem } from "../model/list.js";
import type { Markup } from "../model/markup.js";
import type { TableCell, TableRow } from "../model/table.js";
import { ParseError, type ParseErrorCode } from "../parse-error.js";
import { TreeDumper } from "../tree-dumper.js";
import type {
    BibMode,
    CitationReferent,
    Destination,
    ListFlavor,
    PlacementMode,
    Scope,
    TableAlignment
} from "../values.js";
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
const nodeSize = 96;
const attributeSize = 16;
const noIndex = 0xffff_ffff;
/**
 * The two scoped values travel as records above the node-kind space (M4):
 * a citation's prefix is its child range and its suffix its auxiliary range,
 * a footnote's content is its child range, a cite's items are its child
 * range, and the document's footnotes are its auxiliary range.
 */
type ValueKind = "citation" | "footnote";
const valueKindBase = 0x100;
const valueKinds: readonly ValueKind[] = Object.freeze(["citation", "footnote"]);
type Decoded = Markup | Citation | Footnote;
const isMarkup = (value: Decoded): value is Markup => "kind" in value;

const header = {
    totalSize: 4,
    status: 8,
    errorCode: 12,
    errorOffset: 16,
    errorLength: 20,
    nodeCount: 24,
    edgeCount: 28,
    attributeCount: 32,
    alignmentCount: 36,
    nodesOffset: 40,
    edgesOffset: 44,
    attributesOffset: 48,
    alignmentsOffset: 52,
    stringsOffset: 56,
    stringsLength: 60
} as const;

const nodeField = {
    kind: 0,
    flags: 4,
    scope: 8,
    childStart: 24,
    childCount: 28,
    labelIndex: 32,
    auxiliaryStart: 36,
    auxiliaryCount: 40,
    scalar0: 44,
    integer: 56,
    strings: 64
} as const;

type MarkupValue = Markup extends infer Node ? (Node extends Markup ? Omit<Node, "dump"> : never) : never;
type MarkupValueOf<Kind extends Markup["kind"]> = Extract<MarkupValue, { readonly kind: Kind }>;

interface ResultLayout {
    readonly totalSize: number;
    readonly nodeCount: number;
    readonly edgeCount: number;
    readonly attributeCount: number;
    readonly alignmentCount: number;
    readonly nodesOffset: number;
    readonly edgesOffset: number;
    readonly attributesOffset: number;
    readonly alignmentsOffset: number;
    readonly stringsOffset: number;
    readonly stringsLength: number;
}

/** A destination and title materialized once and shared by every occurrence. */
interface Resource {
    readonly dest: Destination;
    readonly title: string | null;
}

interface NodeRecord {
    readonly index: number;
    readonly offset: number;
    readonly kind: NativeKind | ValueKind;
    readonly flags: number;
    readonly scope: Scope;
    readonly childStart: number;
    readonly childCount: number;
    readonly labelIndex: number;
    readonly auxiliaryStart: number;
    readonly auxiliaryCount: number;
    readonly scalar0: number;
    readonly integer: bigint;
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
            alignmentCount: this.uint(header.alignmentCount),
            nodesOffset: this.uint(header.nodesOffset),
            edgesOffset: this.uint(header.edgesOffset),
            attributesOffset: this.uint(header.attributesOffset),
            alignmentsOffset: this.uint(header.alignmentsOffset),
            stringsOffset: this.uint(header.stringsOffset),
            stringsLength: this.uint(header.stringsLength)
        };
        if (layout.nodeCount === 0) throw new Error("native result contains no document node");
        const expectedEdges = this.sectionEnd(transferHeaderSize, layout.nodeCount, nodeSize, "node table");
        const expectedAttributes = this.sectionEnd(expectedEdges, layout.edgeCount, 4, "edge table");
        const expectedAlignments = this.sectionEnd(
            expectedAttributes,
            layout.attributeCount,
            attributeSize,
            "attribute table"
        );
        const expectedStrings = this.sectionEnd(expectedAlignments, layout.alignmentCount, 1, "alignment table");
        const expectedEnd = this.sectionEnd(expectedStrings, layout.stringsLength, 1, "string blob");
        if (
            layout.nodesOffset !== transferHeaderSize ||
            layout.edgesOffset !== expectedEdges ||
            layout.attributesOffset !== expectedAttributes ||
            layout.alignmentsOffset !== expectedAlignments ||
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
            labelIndex: this.uint(offset + nodeField.labelIndex),
            auxiliaryStart: this.uint(offset + nodeField.auxiliaryStart),
            auxiliaryCount: this.uint(offset + nodeField.auxiliaryCount),
            scalar0: this.int(offset + nodeField.scalar0),
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
            if (record.labelIndex !== noIndex) {
                if (record.kind !== "directive" && record.kind !== "directiveBlock") {
                    throw new Error("only a directive may own a label relation");
                }
                this.recordRelation(record, record.labelIndex, incoming, "label");
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
            // The document's footnotes and a citation's suffix are owned
            // through the auxiliary range as well (M4).
            const auxiliary = record.kind === "document" ? "footnotes" : record.kind === "citation" ? "suffix" : null;
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
        // The two scoped values are decoded by their owners, never as nodes,
        // so the kind dispatch below is over Markup kinds alone.
        const kind = record.kind;
        if (kind === "citation" || kind === "footnote") {
            throw new Error(`native result places a ${kind} value where a node belongs`);
        }
        const base = this.base(record);
        switch (kind) {
            case "document":
                this.flags(record, 0);
                return { ...base, content: this.content(record), footnotes: this.footnotes(record) } as MarkupValue;
            case "cite":
                this.flags(record, 0);
                return { ...base, citations: this.citations(record) } as MarkupValue;
            case "paragraph":
            case "emphasis":
            case "strong":
            case "strikethrough":
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
                    exampleLabel: this.string(record, 1),
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
            case "table":
                return this.table(record);
            case "directiveBlock":
                return { ...base, ...this.directiveFields(record) } as MarkupValue;
            case "directive": {
                const fields = this.directiveFields(record);
                if (fields.content.length !== 0) throw new Error("inline directive contains block content");
                return {
                    ...base,
                    name: fields.name,
                    attributes: fields.attributes,
                    label: fields.label
                } as MarkupValue;
            }
            case "link":
            case "image": {
                this.flags(record, 0);
                const resource = this.resource(record);
                return {
                    ...base,
                    dest: resource.dest,
                    title: resource.title,
                    content: this.content(record)
                } as MarkupValue;
            }
            case "tableRow":
                return this.tableRow(record);
            case "tableCell":
                this.flags(record, 0);
                return { ...base, content: this.content(record) } as MarkupValue;
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
        this.flags(record, 0x1ff);
        const flavor = this.listFlavor(record.scalar0);
        const start = (record.flags & 1) === 0 ? null : this.safeInteger(record.integer, "list start");
        if (flavor === "bullet" && start !== null) throw new Error("native result gives a bullet list a start");
        const children = this.content(record);
        if (!children.every((child): child is ListItem => child.kind === "listItem")) {
            throw new Error("list contains a non-item node");
        }
        const variant = start === null ? null : ((record.flags >> 2) & 0x7) === 1 ? "decimal" : null;
        const delimiterRaw = (record.flags >> 5) & 0x7;
        const delimiter =
            start === null
                ? null
                : delimiterRaw === 1
                  ? "period"
                  : delimiterRaw === 2
                    ? { kind: "parenthesis" as const, closed: (record.flags & (1 << 8)) !== 0 }
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
        this.range(record.auxiliaryStart, record.auxiliaryCount, this.layout.alignmentCount, "table alignment range");
        const alignments = Array.from({ length: record.auxiliaryCount }, (_, index) =>
            this.tableAlignment(this.bytes[this.layout.alignmentsOffset + record.auxiliaryStart + index]!)
        );
        const content = this.content(record);
        if (!content.every((child): child is TableRow => child.kind === "tableRow")) {
            throw new Error("table contains a non-row node");
        }
        const headers = content.filter((row) => row.isHeader);
        if (headers.length !== 1) throw new Error(`table contains ${headers.length} header rows`);
        return {
            ...this.base(record, "table"),
            alignments,
            header: headers[0]!,
            rows: content.filter((row) => !row.isHeader)
        };
    }

    private tableRow(record: NodeRecord): Omit<TableRow, "dump"> {
        this.flags(record, 1);
        const content = this.content(record);
        if (!content.every((child): child is TableCell => child.kind === "tableCell")) {
            throw new Error("table row contains a non-cell node");
        }
        return { ...this.base(record, "tableRow"), isHeader: record.flags !== 0, cells: content };
    }

    private directiveFields(record: NodeRecord): {
        readonly name: string;
        readonly attributes: readonly DirectiveAttribute[] | null;
        readonly label: DirectiveLabel | null;
        readonly content: readonly Markup[];
    } {
        this.flags(record, 1);
        let attributes: readonly DirectiveAttribute[] | null = null;
        if ((record.flags & 1) !== 0) {
            this.range(record.auxiliaryStart, record.auxiliaryCount, this.layout.attributeCount, "attribute range");
            attributes = Object.freeze(
                Array.from({ length: record.auxiliaryCount }, (_, index) => {
                    const offset = this.layout.attributesOffset + (record.auxiliaryStart + index) * attributeSize;
                    return { name: this.requiredStringAt(offset), value: this.requiredStringAt(offset + 8) };
                })
            );
        } else if (record.auxiliaryCount !== 0) {
            throw new Error("directive without an attribute container carries attributes");
        }
        return {
            name: this.requiredString(record, 0),
            attributes,
            label: this.directiveLabel(record),
            content: this.content(record)
        };
    }

    private directiveLabel(record: NodeRecord): DirectiveLabel | null {
        if (record.labelIndex === noIndex) return null;
        const label = this.values[record.labelIndex];
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

    private footnotes(record: NodeRecord): readonly Footnote[] {
        if (record.auxiliaryCount === 0) return [];
        this.range(record.auxiliaryStart, record.auxiliaryCount, this.layout.edgeCount, "footnotes range");
        return this.edgeRange(record.auxiliaryStart, record.auxiliaryCount, "footnote").map((value) => {
            if (isMarkup(value) || !("id" in value))
                throw new Error("document footnotes contain a non-footnote record");
            return value;
        });
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
            resource = { dest: this.destination(record), title: this.string(record, 2) };
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
        return { kind, scope: record.scope };
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

    private placement(value: number): PlacementMode {
        if (value === 1) return "embedded";
        if (value === 2) return "standalone";
        throw new Error(`native result contains invalid placement mode ${value}`);
    }

    private listFlavor(value: number): ListFlavor {
        if (value === 1) return "bullet";
        if (value === 2) return "ordered";
        throw new Error(`native result contains invalid list flavor ${value}`);
    }

    private tableAlignment(value: number): TableAlignment {
        const alignments: readonly TableAlignment[] = ["none", "left", "center", "right"];
        const alignment = alignments[value];
        if (alignment === undefined) throw new Error(`native result contains invalid table alignment ${value}`);
        return alignment;
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
