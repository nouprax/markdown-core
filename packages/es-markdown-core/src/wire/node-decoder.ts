import type { MarkupBase } from "../model/base.js";
import type { DirectiveAttribute } from "../model/directive-attribute.js";
import type { DirectiveLabel } from "../model/directive-label.js";
import type { Document } from "../model/document.js";
import type { ListItem } from "../model/list.js";
import type { Markup } from "../model/markup.js";
import type { TableCell, TableRow } from "../model/table.js";
import { ParseError, type ParseErrorCode } from "../parse-error.js";
import { TreeDumper } from "../tree-dumper.js";
import type { ListFlavor, PlacementMode, ReferenceForm, Scope, TableAlignment } from "../values.js";
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

interface NodeRecord {
    readonly index: number;
    readonly offset: number;
    readonly kind: NativeKind;
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
    private values: readonly (Markup | undefined)[] = [];

    constructor(private readonly bytes: Uint8Array) {
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    }

    decodeDocument(): Document {
        this.readHeader();
        this.validateTopology();
        const values: (Markup | undefined)[] = Array.from({ length: this.layout.nodeCount });
        this.values = values;
        for (let remaining = this.layout.nodeCount; remaining > 0; --remaining) {
            const index = remaining - 1;
            values[index] = this.markup(this.value(this.readRecord(index)));
        }
        const document = values[0];
        if (document?.kind !== "document") throw new Error("native result root is not a document");
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
        const kind = kinds[rawKind];
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
        const base = this.base(record);
        switch (record.kind) {
            case "document":
            case "blockQuote":
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
            case "list":
                return this.list(record);
            case "listItem":
                this.flags(record, 0);
                return {
                    ...base,
                    checked: this.nullableBoolean(record.scalar0, "list item checked state"),
                    content: this.content(record)
                } as MarkupValue;
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
            case "footnoteDefinition":
                this.flags(record, 0);
                return { ...base, ...this.association(record), content: this.content(record) } as MarkupValue;
            case "footnoteReference":
                this.flags(record, 0);
                this.leaf(record);
                return { ...base, ...this.association(record) } as MarkupValue;
            case "referenceDefinition":
                this.flags(record, 0);
                this.leaf(record);
                return {
                    ...base,
                    ...this.association(record),
                    destination: this.requiredString(record, 2),
                    title: this.string(record, 3)
                } as MarkupValue;
            case "linkReference":
            case "imageReference":
                this.flags(record, 0);
                return {
                    ...base,
                    ...this.association(record),
                    form: this.referenceForm(record.scalar0),
                    content: this.content(record)
                } as MarkupValue;
            case "link":
                this.flags(record, 0);
                return {
                    ...base,
                    destination: this.requiredString(record, 0),
                    title: this.string(record, 1),
                    content: this.content(record)
                } as MarkupValue;
            case "image":
                this.flags(record, 0);
                return {
                    ...base,
                    source: this.requiredString(record, 0),
                    title: this.string(record, 1),
                    content: this.content(record)
                } as MarkupValue;
            case "tableRow":
                return this.tableRow(record);
            case "tableCell":
                this.flags(record, 0);
                return { ...base, content: this.content(record) } as MarkupValue;
        }
    }

    private list(record: NodeRecord): MarkupValueOf<"list"> {
        this.flags(record, 0b11);
        const flavor = this.listFlavor(record.scalar0);
        const start = (record.flags & 1) === 0 ? null : this.safeInteger(record.integer, "list start");
        if (flavor === "bullet" && start !== null) throw new Error("native result gives a bullet list a start");
        const children = this.content(record);
        if (!children.every((child): child is ListItem => child.kind === "listItem")) {
            throw new Error("list contains a non-item node");
        }
        return { ...this.base(record, "list"), flavor, start, tight: (record.flags & 2) !== 0, items: children };
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
        if (label?.kind !== "directiveLabel") throw new Error("directive label field contains a non-label node");
        return label;
    }

    private content(record: NodeRecord): readonly Markup[] {
        return Array.from({ length: record.childCount }, (_, index) => {
            const node = this.values[this.edge(record.childStart + index)];
            if (!node) throw new Error("native result child was not constructed");
            return node;
        });
    }

    private association(record: NodeRecord): { readonly label: string; readonly identifier: string } {
        return { label: this.requiredString(record, 0), identifier: this.requiredString(record, 1) };
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

    private referenceForm(value: number): ReferenceForm {
        if (value === 1) return "full";
        if (value === 2) return "collapsed";
        if (value === 3) return "shortcut";
        throw new Error(`native result contains invalid reference form ${value}`);
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
