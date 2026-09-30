import { Attributes } from "../markup/attributes.js";
import type { Record as AttributeRecord } from "../markup/attributes.js";
import type { Dimensions, Flow } from "../common/constraints.js";
import type { Metadata, MetadataListItem, MetadataScalar, MetadataValue } from "../markup/metadata.js";
import type { MarkupBase } from "../markup/base.js";
import type { DirectiveLabel } from "../markup/directive-label.js";
import type { Citation } from "../markup/cite.js";
import type { Document } from "../markup/document.js";
import type { Footnote } from "../markup/footnote.js";
import type { Specimen } from "../markup/specimen.js";
import type { ListItem } from "../markup/list.js";
import type { Markup } from "../markup/markup.js";
import type { TableCaption, TableCell, TableColumn, TableRow } from "../markup/table.js";
import type { Definition } from "../markup/definition-list.js";
import { ParseError, type ParseErrorCode } from "../common/parse-error.js";
import { MarkupDumper } from "../visitor/markup-dumper.js";
import { nodeAt, scopeOf } from "../visitor/document-queries.js";
import type {
    BibMode,
    CitationReferent,
    Destination,
    Extent,
    ListFlavor,
    OrderedListDelimiter,
    OrderedListVariant,
    Placement,
    Position,
    TextUnit
} from "../markup/values.js";
import { contentKinds, kinds, type NativeKind } from "./kinds.js";

/*
 * The MCB3 reader (docs/architecture/wire-format.md). Records arrive in
 * post-order, so each node is built bottom-up from the nodes already on the
 * stack, without recursion and without another call into WebAssembly. The
 * document's definition tables follow its record. The reader retains no view
 * of the message after decode returns.
 */

const magic = [0x4d, 0x43, 0x42, 0x33] as const;
/** The magic, the u32 message length and the u8 status. */
export const headerSize = 9;
/** The byte offset of the u32 message length. */
export const lengthOffset = 4;

const flows: readonly Flow[] = ["none", "left", "center", "right"];
const placements: readonly Placement[] = ["embedded", "standalone"];
const bibModes: readonly BibMode[] = ["normal", "authorInText", "suppressAuthor"];
const flavors: readonly ListFlavor[] = ["bullet", "ordered"];
const errorCodes: readonly ParseErrorCode[] = ["internal", "invalidArgument", "allocationFailed", "internal"];

/** The document's members that are not contract fields: the decoder adds them
 * once the definition tables are read. */
type DocumentMembers = "unit" | "footnotes" | "specimens" | "footnote" | "specimen" | "scope" | "nodeAt" | "dump";
type MarkupValue = Markup extends infer Node ? (Node extends Document ? Omit<Document, DocumentMembers> : Node) : never;
type Base<Kind extends Markup["kind"]> = MarkupBase<Kind>;

/** Ids are exact JavaScript numbers: every one is below 2^53. */
const idLimit = 2 ** 53;

/** A reference definition's shared values, decoded once however often it is used. */
interface Resource {
    readonly dest: Destination;
    readonly title: string | null;
    readonly anchor: string | null;
    readonly attributes: Attributes;
}

export class Decoder {
    private readonly view: DataView;
    private readonly utf8 = new TextDecoder("utf-8", { fatal: false });
    private offset = 0;
    private readonly stack: Markup[] = [];
    private readonly resources: Resource[] = [];
    /** Every Footnote and Specimen by id, for the definition tables. */
    private readonly definitions = new Map<number, Footnote | Specimen>();

    /** `unit` is the text unit of the document the message decodes to. */
    constructor(
        private readonly bytes: Uint8Array,
        private readonly unit: TextUnit
    ) {
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    }

    decode(): Document {
        this.header();
        // A Document is no field's node, so its record is the last one.
        let root: Markup | undefined;
        while (root?.kind !== "document" && this.offset < this.bytes.byteLength) {
            root = this.record();
            this.stack.push(root);
        }
        this.stack.pop();
        if (root?.kind !== "document" || this.stack.length !== 0) {
            throw new Error("native result is not one document tree");
        }
        const footnotes = this.table("footnote") as Footnote[];
        const specimens = this.table("specimen") as Specimen[];
        if (this.offset !== this.bytes.byteLength)
            throw new Error("native result has bytes after its definition tables");
        return this.document(root, footnotes, specimens);
    }

    private header(): void {
        if (this.bytes.byteLength < headerSize) throw new Error("truncated native result header");
        for (const [index, expected] of magic.entries()) {
            const actual = this.u8();
            if (actual !== expected) {
                throw new Error(`invalid native result at byte ${index}: expected ${expected}, got ${actual}`);
            }
        }
        if (this.u32() !== this.bytes.byteLength) throw new Error("native result length does not match its header");
        const status = this.u8();
        if (status === 1) throw this.error();
        if (status !== 0) throw new Error(`unsupported native result status ${status}`);
    }

    private error(): ParseError {
        const code = errorCodes[this.u32()] ?? "internal";
        const message = this.string();
        if (this.offset !== this.bytes.byteLength) throw new Error("invalid native result error payload");
        return new ParseError(code, message);
    }

    // ---- Records -----------------------------------------------------------

    private record(): Markup {
        const ordinal = this.u8();
        const kind = kinds[ordinal];
        if (kind === undefined || kind === "none")
            throw new Error(`native result contains unknown node kind ${ordinal}`);
        const id = this.id();
        const extent = this.extent();
        const anchor = this.optional(() => this.string());
        const attributes = this.attributes();
        const node = this.fields(kind, { kind, id, extent, anchor, attributes }) as Markup;
        if (node.kind === "footnote" || node.kind === "specimen") this.definitions.set(id, node);
        return node;
    }

    /** A kind's fields in the contract's order; node-valued fields read their counts. */
    private fields(kind: NativeKind, base: Base<NativeKind>): MarkupValue {
        switch (kind) {
            case "document": {
                const content = this.count();
                const metadata = this.presence();
                const nodes = this.nodes(content + metadata);
                return {
                    ...(base as Base<"document">),
                    content: nodes.content(content),
                    metadata: nodes.optional("metadata", metadata) as Metadata | null
                };
            }
            case "callout": {
                const variant = this.optional(() => this.string());
                const collapsed = this.optional(() => this.bool());
                const title = this.optional(() => this.count());
                const content = this.count();
                const nodes = this.nodes((title ?? 0) + content);
                return {
                    ...(base as Base<"callout">),
                    variant,
                    collapsed,
                    title: title === null ? null : nodes.content(title),
                    content: nodes.content(content)
                };
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
            case "tableCaption": {
                const content = this.count();
                return { ...base, content: this.nodes(content).content(content) } as MarkupValue;
            }
            case "heading": {
                const level = this.int();
                const content = this.count();
                return { ...(base as Base<"heading">), level, content: this.nodes(content).content(content) };
            }
            case "thematicBreak":
            case "softBreak":
            case "lineBreak":
                return base as MarkupValue;
            case "list": {
                const flavor = this.index(flavors);
                const start = this.optional(() => this.int());
                const variant = this.optional(() => this.variant());
                const delimiter = this.optional(() => this.delimiter());
                const tight = this.bool();
                const items = this.count();
                return {
                    ...(base as Base<"list">),
                    flavor,
                    start,
                    variant,
                    delimiter,
                    tight,
                    items: this.nodes(items).typed("listItem", items) as ListItem[]
                };
            }
            case "listItem": {
                const marker = this.optional(() => this.string());
                const content = this.count();
                return {
                    ...(base as Base<"listItem">),
                    marker,
                    get tasked() {
                        return marker !== null;
                    },
                    get completed() {
                        return marker !== null && marker !== " ";
                    },
                    content: this.nodes(content).content(content)
                };
            }
            case "codeBlock":
                return {
                    ...(base as Base<"codeBlock">),
                    info: this.optional(() => this.string()),
                    language: this.optional(() => this.string()),
                    literal: this.string(),
                    fenced: this.bool(),
                    closed: this.bool()
                };
            case "htmlBlock":
            case "formulaBlock":
            case "text":
            case "code":
            case "html":
            case "comment":
                return { ...base, literal: this.string() } as MarkupValue;
            case "formula":
                return { ...(base as Base<"formula">), mode: this.index(placements), literal: this.string() };
            case "table": {
                const caption = this.presence();
                const columns = this.list(() => this.column());
                const head = this.count();
                const content = this.count();
                const foot = this.count();
                const nodes = this.nodes(caption + head + content + foot);
                return {
                    ...(base as Base<"table">),
                    caption: nodes.optional("tableCaption", caption) as TableCaption | null,
                    columns,
                    head: nodes.typed("tableRow", head) as TableRow[],
                    content: nodes.typed("tableRow", content) as TableRow[],
                    foot: nodes.typed("tableRow", foot) as TableRow[]
                };
            }
            case "tableRow": {
                const cells = this.count();
                return {
                    ...(base as Base<"tableRow">),
                    cells: this.nodes(cells).typed("tableCell", cells) as TableCell[]
                };
            }
            case "tableCell": {
                const rowspan = this.int();
                const colspan = this.int();
                const content = this.count();
                return {
                    ...(base as Base<"tableCell">),
                    rowspan,
                    colspan,
                    content: this.nodes(content).content(content)
                };
            }
            case "directiveBlock": {
                const name = this.optional(() => this.string());
                const label = this.presence();
                const content = this.count();
                const nodes = this.nodes(label + content);
                return {
                    ...(base as Base<"directiveBlock">),
                    name,
                    label: nodes.optional("directiveLabel", label) as DirectiveLabel | null,
                    content: nodes.content(content)
                };
            }
            case "directive": {
                const name = this.string();
                const label = this.presence();
                return {
                    ...(base as Base<"directive">),
                    name,
                    label: this.nodes(label).optional("directiveLabel", label) as DirectiveLabel | null
                };
            }
            case "crossLink":
                return {
                    ...(base as Base<"crossLink">),
                    dest: this.destination(),
                    label: this.optional(() => this.string())
                };
            case "crossEmbedded":
                return {
                    ...(base as Base<"crossEmbedded">),
                    dest: this.destination(),
                    label: this.optional(() => this.string()),
                    dimensions: this.optional(() => this.dimensions())
                };
            case "link": {
                const resource = this.resource();
                const content = this.count();
                return {
                    ...this.inheriting(base as Base<"link">, resource),
                    dest: resource.dest,
                    title: resource.title,
                    content: this.nodes(content).content(content)
                };
            }
            case "embedded": {
                const resource = this.resource();
                const dimensions = this.optional(() => this.dimensions());
                const content = this.count();
                return {
                    ...this.inheriting(base as Base<"embedded">, resource),
                    dest: resource.dest,
                    title: resource.title,
                    dimensions,
                    content: this.nodes(content).content(content)
                };
            }
            case "cite": {
                const citations = this.count();
                return {
                    ...(base as Base<"cite">),
                    citations: this.nodes(citations).typed("citation", citations) as Citation[]
                };
            }
            case "definitionList": {
                const definitions = this.count();
                return {
                    ...(base as Base<"definitionList">),
                    definitions: this.nodes(definitions).typed("definition", definitions) as Definition[]
                };
            }
            case "definition": {
                const term = this.count();
                const bodies = this.list(() => this.count());
                const compact = this.bool();
                const nodes = this.nodes(bodies.reduce((sum, count) => sum + count, term));
                return {
                    ...(base as Base<"definition">),
                    term: nodes.content(term),
                    content: bodies.map((count) => nodes.content(count)),
                    compact
                };
            }
            case "citation": {
                const written = this.referent();
                const prefix = this.count();
                const suffix = this.count();
                // An inline note's Footnote is the first node the record takes.
                const nodes = this.nodes((written === null ? 1 : 0) + prefix + suffix);
                const referent: CitationReferent = written ?? {
                    kind: "footnote",
                    target: { kind: "note", footnote: nodes.optional("footnote", 1) as Footnote }
                };
                return {
                    ...(base as Base<"citation">),
                    referent,
                    prefix: nodes.content(prefix),
                    suffix: nodes.content(suffix)
                };
            }
            case "footnote": {
                const label = this.optional(() => this.string());
                const content = this.count();
                return { ...(base as Base<"footnote">), label, content: this.nodes(content).content(content) };
            }
            case "specimen": {
                const label = this.optional(() => this.string());
                const start = this.optional(() => this.int());
                const content = this.count();
                return { ...(base as Base<"specimen">), label, start, content: this.nodes(content).content(content) };
            }
            case "metadata": {
                const value = (): MetadataValue | null => this.optional(() => this.metadataValue());
                return {
                    ...(base as Base<"metadata">),
                    name: value(),
                    title: value(),
                    subtitle: value(),
                    time: value(),
                    date: value(),
                    authors: value(),
                    keywords: value(),
                    abstract: value(),
                    state: value(),
                    comment: value()
                };
            }
        }
    }

    // ---- Definition tables -------------------------------------------------

    /** A table's definitions in source order; each id names a node of `kind`. */
    private table(kind: "footnote" | "specimen"): (Footnote | Specimen)[] {
        return this.list(() => {
            const id = this.id();
            const node = this.definitions.get(id);
            if (node?.kind !== kind) throw new Error(`native result ${kind} table names no ${kind} node ${id}`);
            return node;
        });
    }

    /**
     * The document with its members: the unit, the tables and the label
     * lookups, built here, once, over data the document carries. They are not
     * enumerable, so the document's enumerable fields stay its contract fields.
     */
    private document(root: Omit<Document, DocumentMembers>, footnotes: Footnote[], specimens: Specimen[]): Document {
        const footnoteFor = firstByLabel(footnotes);
        const specimenFor = firstByLabel(specimens);
        const member = (value: unknown): PropertyDescriptor => ({ value, enumerable: false });
        return Object.defineProperties(root, {
            unit: member(this.unit),
            footnotes: member(footnotes),
            specimens: member(specimens),
            footnote: member((label: string): Footnote | null => footnoteFor.get(label) ?? null),
            specimen: member((label: string): Specimen | null => specimenFor.get(label) ?? null),
            scope: member(function (this: Document, node: Markup, source: string) {
                return scopeOf(this, node, source);
            }),
            nodeAt: member(function (this: Document, position: Position, source: string) {
                return nodeAt(this, position, source);
            }),
            dump: member(function (this: Document, nodeOrSource: Markup | string, source?: string) {
                return typeof nodeOrSource === "string"
                    ? MarkupDumper.dump(this, nodeOrSource)
                    : MarkupDumper.dump(this, nodeOrSource, source as string);
            })
        }) as Document;
    }

    /** The `count` nodes on top of the stack, handed out in field order. */
    private nodes(count: number): Nodes {
        if (count > this.stack.length) throw new Error("native result record names more nodes than precede it");
        return new Nodes(this.stack.splice(this.stack.length - count, count));
    }

    // ---- Shared resources --------------------------------------------------

    private resource(): Resource {
        const ordinal = this.u32();
        if (ordinal < this.resources.length) return this.resources[ordinal] as Resource;
        if (ordinal !== this.resources.length) throw new Error(`native result names unknown resource ${ordinal}`);
        const resource: Resource = {
            dest: this.destination(),
            title: this.optional(() => this.string()),
            anchor: this.optional(() => this.string()),
            attributes: this.attributes()
        };
        this.resources.push(resource);
        return resource;
    }

    /** The occurrence's anchor wins; the definition's classes and records come first. */
    private inheriting<Kind extends "link" | "embedded">(base: Base<Kind>, resource: Resource): Base<Kind> {
        const primary = base.attributes;
        const inherited = resource.attributes;
        const attributes =
            primary.classes.length === 0 && primary.records.length === 0
                ? inherited
                : Object.freeze({
                      classes: Object.freeze([...inherited.classes, ...primary.classes]),
                      records: Object.freeze([...inherited.records, ...primary.records])
                  });
        return { ...base, anchor: base.anchor ?? resource.anchor, attributes };
    }

    // ---- Values ------------------------------------------------------------

    private extent(): Extent {
        return { lead: this.i32(), span: this.u32() };
    }

    private attributes(): Attributes {
        const classes = this.list(() => this.string());
        const records = this.list((): AttributeRecord => Object.freeze({ name: this.string(), value: this.string() }));
        if (classes.length === 0 && records.length === 0) return Attributes.empty;
        return Object.freeze({ classes: Object.freeze(classes), records: Object.freeze(records) });
    }

    private destination(): Destination {
        switch (this.branch(2)) {
            case 0:
                return { kind: "url", value: this.string() };
            default:
                return { kind: "cross", path: this.string(), anchor: this.optional(() => this.string()) };
        }
    }

    /** The referent as written, or null for an inline note: its `Footnote`
     * is a node of the record, which the record takes off the stack. */
    private referent(): CitationReferent | null {
        switch (this.branch(3)) {
            case 0:
                return { kind: "bib", key: this.string(), mode: this.index(bibModes) };
            case 1:
                return this.branch(2) === 0
                    ? { kind: "footnote", target: { kind: "label", value: this.string() } }
                    : null;
            default:
                return { kind: "specimen", label: this.string() };
        }
    }

    private variant(): OrderedListVariant {
        switch (this.branch(4)) {
            case 0:
                return "decimal";
            case 1:
                return { kind: "alpha", lowercased: this.bool() };
            case 2:
                return { kind: "roman", lowercased: this.bool() };
            default:
                return "default";
        }
    }

    private delimiter(): OrderedListDelimiter {
        switch (this.branch(3)) {
            case 0:
                return "period";
            case 1:
                return { kind: "parenthesis", closed: this.bool() };
            default:
                return "default";
        }
    }

    private column(): TableColumn {
        return { flow: this.index(flows), relative: this.optional(() => this.double()) };
    }

    private dimensions(): Dimensions {
        return { width: this.int(), height: this.optional(() => this.int()) };
    }

    private metadataValue(): MetadataValue {
        if (this.branch(2) === 0) return { kind: "scalar", value: this.metadataScalar() };
        return { kind: "list", items: this.list(() => this.metadataListItem()) };
    }

    private metadataScalar(): MetadataScalar {
        switch (this.branch(4)) {
            case 0:
                return { kind: "null" };
            case 1:
                return { kind: "bool", value: this.bool() };
            case 2:
                return { kind: "number", value: this.string() };
            default:
                return { kind: "text", value: this.string() };
        }
    }

    private metadataListItem(): MetadataListItem {
        return this.branch(2) === 0 ? { kind: "number", value: this.string() } : { kind: "text", value: this.string() };
    }

    // ---- Primitives --------------------------------------------------------

    private optional<T>(read: () => T): T | null {
        return this.bool() ? read() : null;
    }

    /** The node count of a `K?` field. */
    private presence(): number {
        return this.bool() ? 1 : 0;
    }

    private list<T>(read: () => T): T[] {
        const count = this.count();
        // Every item occupies at least one byte, so a count the message cannot
        // hold is rejected before anything is allocated for it.
        if (count > this.bytes.byteLength - this.offset) throw new Error("native result count exceeds the message");
        return Array.from({ length: count }, read);
    }

    private count(): number {
        return this.u32();
    }

    private branch(count: number): number {
        const branch = this.u8();
        if (branch >= count) throw new Error(`native result contains invalid branch ${branch}`);
        return branch;
    }

    private index<T>(values: readonly T[]): T {
        const index = this.u8();
        const value = values[index];
        if (value === undefined) throw new Error(`native result contains invalid enum index ${index}`);
        return value;
    }

    private bool(): boolean {
        const value = this.u8();
        if (value > 1) throw new Error(`native result contains invalid boolean ${value}`);
        return value === 1;
    }

    private string(): string {
        const length = this.u32();
        const start = this.take(length);
        return this.utf8.decode(this.bytes.subarray(start, start + length));
    }

    /** Int, which a JavaScript number carries only while it is a safe integer. */
    private int(): number {
        const start = this.take(8);
        const value = this.view.getInt32(start + 4, true) * 0x1_0000_0000 + this.view.getUint32(start, true);
        if (!Number.isSafeInteger(value)) throw new Error("native integer exceeds JavaScript integer precision");
        return value;
    }

    /** A u64 node id, which a JavaScript number carries exactly below 2^53. */
    private id(): number {
        const start = this.take(8);
        const value = this.view.getUint32(start + 4, true) * 0x1_0000_0000 + this.view.getUint32(start, true);
        if (value >= idLimit) throw new Error("native node id is not below 2^53");
        return value;
    }

    private double(): number {
        return this.view.getFloat64(this.take(8), true);
    }

    private u8(): number {
        return this.view.getUint8(this.take(1));
    }

    private u32(): number {
        return this.view.getUint32(this.take(4), true);
    }

    private i32(): number {
        return this.view.getInt32(this.take(4), true);
    }

    /** Advances past `length` bytes and returns where they start. */
    private take(length: number): number {
        const start = this.offset;
        if (length > this.bytes.byteLength - start) throw new Error("truncated native result");
        this.offset = start + length;
        return start;
    }
}

/** One record's nodes, handed to its fields in order; each field checks the kinds it accepts. */
class Nodes {
    private next = 0;

    constructor(private readonly nodes: readonly Markup[]) {}

    content(count: number): readonly Markup[] {
        return this.take(count, (node) => contentKinds.has(node.kind), "content");
    }

    typed(kind: NativeKind, count: number): readonly Markup[] {
        return this.take(count, (node) => node.kind === kind, kind);
    }

    optional(kind: NativeKind, count: number): Markup | null {
        return count === 0 ? null : (this.typed(kind, 1)[0] as Markup);
    }

    private take(count: number, accepts: (node: Markup) => boolean, field: string): readonly Markup[] {
        const taken = this.nodes.slice(this.next, this.next + count);
        this.next += count;
        for (const node of taken) {
            if (!accepts(node)) throw new Error(`native result places a ${node.kind} node in a ${field} field`);
        }
        return taken;
    }
}

/** The first node of each label, in the table's source order. A null label
 * is an inline note's or an anonymous definition's and names nothing. */
function firstByLabel<Node extends Footnote | Specimen>(nodes: readonly Node[]): ReadonlyMap<string, Node> {
    const map = new Map<string, Node>();
    for (const node of nodes) {
        if (node.label !== null && !map.has(node.label)) map.set(node.label, node);
    }
    return map;
}
