import type { MarkupBase } from "../model/base.js";
import type { DirectiveAttribute } from "../model/directive-attribute.js";
import type { DirectiveLabel } from "../model/directive-label.js";
import type { ListItem } from "../model/list.js";
import type { Markup } from "../model/markup.js";
import type { Semantic } from "../model/semantic.js";
import type { TableCell, TableRow } from "../model/table.js";
import { ParseError, type ParseErrorCode } from "../parse-error.js";
import { TreeDumper } from "../tree-dumper.js";
import type { Identity, ListFlavor, PlacementMode, ReferenceForm, Scope, TableAlignment } from "../values.js";
import { kinds, type NativeKind } from "./kinds.js";

/* THE ONE DECODE PASS. The native side answers every read with one MKC8
 * payload -- the envelope's magic and status, then the facade's own canonical
 * wire -- and this file turns that one buffer into the same plain values the
 * per-field walk used to build, with the boundary crossed once per READ
 * instead of once per field. A payload is a FULL tree or a DELTA against
 * the previous payload (#162): the delta names, by position, the children
 * of the previous read that did not move, and those values are reused as
 * they are -- every decoded value is immutable data, so a read that shares
 * subtrees with the read before it is still a plain value. */

/** The frame a payload leads with (`markdown_core_wire_frame`): the request
 * a document sends and the byte the payload answers with. */
export const WireFrame: { readonly full: 0; readonly delta: 1 } = Object.freeze({ full: 0, delta: 1 } as const);

/* The delta's op tags, above every kind ordinal: a tag below them IS a
 * node's kind byte. */
const OP_SPINE = 0xfe;
const OP_SAME = 0xff;

/** The two members a decoded value does not carry yet: `dump` is defined on
 * every node after the copy, out of the decoder's reach. */
type MarkupValue = Markup extends infer Node ? (Node extends Markup ? Omit<Node, "dump"> : never) : never;
type MarkupValueOf<Kind extends Markup["kind"]> = Extract<MarkupValue, { readonly kind: Kind }>;

/* The one `dump` every decoded node shares (#139): reached through the
 * prototype `WireReader.base` creates nodes on, so no node pays its own
 * closure or descriptor and `Object.keys` still excludes it. */
const MARKUP_PROTOTYPE = {
    dump(this: Markup): string {
        return TreeDumper.dump(this);
    }
};

interface DirectiveFields {
    readonly name: string;
    readonly attributes: readonly DirectiveAttribute[] | null;
    readonly label: DirectiveLabel | null;
    readonly content: readonly Markup[];
}

/** The decoded view; the `Read` wrapper (with its non-enumerable `dump`) is
 * the runtime's to seal. */
export interface DecodedRead {
    readonly semantic: Semantic;
}

/** Decodes a payload: the envelope, then the tree -- whole, or as a delta
 * against `previous`, the semantic tree of the payload before it. */
export function decodeRead(bytes: Uint8Array, previous: Semantic | null = null): DecodedRead {
    const reader = new WireReader(bytes);
    reader.header();
    return reader.read(previous);
}

/**
 * Decodes only the envelope of a payload whose read is discarded -- the
 * `Document` constructor's initial feed -- so an error still surfaces and a
 * healthy tree is not built just to be thrown away.
 */
export function decodeDiscarded(bytes: Uint8Array): void {
    new WireReader(bytes).header();
}

/** `MKC8`: the payload after the envelope is the facade's canonical wire,
 * frame byte first -- every node leads with its identity, a definition's
 * match key rides where `identifier` did, and a reference carries the
 * identity of the definition it resolved to instead of repeating that key. */
const magic = [0x4d, 0x4b, 0x43, 0x38];

class WireReader {
    #offset = 0;
    readonly #bytes: Uint8Array;
    readonly #view: DataView;
    readonly #utf8Decoder = new TextDecoder("utf-8", { fatal: false });
    /* THE SPINE CONTEXT (#162): set by `spine` just before it decodes the
     * rewritten node, and consumed by that node's first `markupList` -- the
     * one that reads its children -- which then reads OPS against these
     * previous children instead of a child list. Null everywhere else, so a
     * node written whole inside a delta decodes exactly as in a full frame.
     * Always consumed: `spine` admits only the kinds `positionalChildren`
     * knows, and every one of them reads its children through
     * `markupList`. */
    #spine: readonly Markup[] | null = null;

    constructor(bytes: Uint8Array) {
        this.#bytes = bytes;
        this.#view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    }

    /** The magic and the status: the part of every payload that says whether
     * a tree or an error follows, throwing the error itself. */
    header(): void {
        for (const [index, expected] of magic.entries()) {
            const actual = this.byte();
            if (actual !== expected) {
                throw new Error(`invalid native bridge payload at byte ${index}: expected ${expected}, got ${actual}`);
            }
        }
        const status = this.byte();
        if (status === 1) throw this.parseError();
        if (status !== 0) throw new Error("unsupported native bridge status");
    }

    read(previous: Semantic | null): DecodedRead {
        const frame = this.byte();
        if (frame === WireFrame.delta) {
            if (previous === null) throw new Error("native bridge sent a delta frame with no previous read");
            if (this.byte() !== OP_SPINE) {
                throw new Error("native bridge sent a delta frame that does not open with the document's spine");
            }
            // `spine` refuses any kind but the previous read's, and the
            // previous read is the document, so the result is one too.
            const semantic = this.spine(previous) as Semantic;
            if (!this.finished) throw new Error("native bridge returned a truncated payload");
            return { semantic };
        }
        if (frame !== WireFrame.full) throw new Error(`native bridge sent an unknown wire frame ${frame}`);
        if (this.kind() !== "document") throw new Error("native bridge returned an invalid document tree");
        const id = this.identity();
        const scope = this.scope();
        const content = this.markupList();
        if (!this.finished) throw new Error("native bridge returned a truncated payload");
        const semantic = this.markup(Object.assign(this.base(id, scope, "document"), { content })) as Semantic;
        return { semantic };
    }

    parseError(): ParseError {
        const code = errorCode(this.int());
        const message = this.requiredString();
        if (!this.finished) throw new Error("invalid native error payload");
        return new ParseError(code, message);
    }

    get finished(): boolean {
        return this.#offset === this.#bytes.length;
    }

    byte(): number {
        if (this.#offset >= this.#bytes.length) throw new Error("truncated native bridge payload");
        return this.#bytes[this.#offset++]!;
    }

    int(): number {
        if (this.#offset > this.#bytes.length - 4) throw new Error("truncated native bridge payload");
        const value = this.#view.getInt32(this.#offset, true);
        this.#offset += 4;
        return value;
    }

    uint(): number {
        if (this.#offset > this.#bytes.length - 4) throw new Error("truncated native bridge payload");
        const value = this.#view.getUint32(this.#offset, true);
        this.#offset += 4;
        return value;
    }

    long(): number {
        if (this.#offset > this.#bytes.length - 8) throw new Error("truncated native bridge payload");
        const value = this.#view.getBigInt64(this.#offset, true);
        this.#offset += 8;
        const narrowed = Number(value);
        if (!Number.isSafeInteger(narrowed)) {
            throw new Error("native value exceeds JavaScript integer precision");
        }
        return narrowed;
    }

    string(): string | null {
        const size = this.int();
        if (size === -1) return null;
        if (size < 0 || size > this.#bytes.length - this.#offset) throw new Error("invalid native bridge string");
        const end = this.#offset + size;
        const value = this.#utf8Decoder.decode(this.#bytes.subarray(this.#offset, end));
        this.#offset = end;
        return value;
    }

    requiredString(): string {
        const value = this.string();
        if (value === null) throw new Error("missing native field");
        return value;
    }

    scope(): Scope {
        return {
            start: { line: this.int(), column: this.int() },
            end: { line: this.int(), column: this.int() }
        };
    }

    identity(): Identity {
        return { block: this.uint(), ordinal: this.uint() };
    }

    kind(): NativeKind {
        return this.kindOf(this.byte());
    }

    kindOf(rawValue: number): NativeKind {
        const kind = kinds[rawValue];
        if (!kind || kind === "none") throw new Error(`native parser returned unknown node kind ${rawValue}`);
        return kind;
    }

    boolean(field: string): boolean {
        const rawValue = this.byte();
        if (rawValue === 0) return false;
        if (rawValue === 1) return true;
        throw new Error(`native parser returned invalid ${field} ${rawValue}`);
    }

    nullableBoolean(field: string): boolean | null {
        const rawValue = this.byte();
        if (rawValue === 255) return null;
        if (rawValue === 0) return false;
        if (rawValue === 1) return true;
        throw new Error(`native parser returned invalid ${field} ${rawValue}`);
    }

    count(field: string): number {
        const value = this.int();
        if (value < 0) throw new Error(`native parser returned invalid ${field} ${value}`);
        return value;
    }

    markupList(): readonly Markup[] {
        const previous = this.#spine;
        if (previous !== null) {
            this.#spine = null;
            return this.ops(previous);
        }
        const count = this.count("child count");
        const children: Markup[] = [];
        for (let index = 0; index < count; index++) {
            children.push(this.node());
        }
        return children;
    }

    node(): Markup {
        return this.nodeOfKind(this.kind());
    }

    nodeOfKind(kind: NativeKind): Markup {
        const id = this.identity();
        const scope = this.scope();
        return this.markup(this.value(kind, id, scope));
    }

    /** A SPINE op's node, `previous` being the previous read's node at the
     * same position: the fields are read afresh and the children come from
     * the ops that follow, against the previous node's children. */
    spine(previous: Markup): Markup {
        const kind = this.kind();
        if (kind !== previous.kind) throw new Error(`native parser rewrote a ${previous.kind} as a ${kind}`);
        const id = this.identity();
        if (id.block !== previous.id.block || id.ordinal !== previous.id.ordinal) {
            throw new Error("native parser rewrote a node under another identity");
        }
        const scope = this.scope();
        this.#spine = positionalChildren(previous);
        const value =
            kind === "document"
                ? Object.assign(this.base(id, scope, "document"), { content: this.markupList() })
                : this.value(kind, id, scope);
        return this.markup(value);
    }

    /** The ops that turn `previous` -- the previous read's children of the
     * node being rewritten -- into the new node's children. Two passes: the
     * ops are read into PARTS, a reused run's length or a node read afresh,
     * and counted; the children are then filled into an array of exactly
     * that length. The reused run is every closed block of the document on
     * every feed that grows it -- the reference copy the delta leaves the
     * reader (docs/STREAMING.md §6) -- and an append per reused child grew
     * the array by halves and copied it on the way, twice the copy's own
     * cost at 32,000 blocks. */
    ops(previous: readonly Markup[]): readonly Markup[] {
        const count = this.count("op count");
        const parts: (Markup | number)[] = [];
        let total = 0;
        let position = 0;
        for (let index = 0; index < count; index++) {
            const tag = this.byte();
            if (tag === OP_SAME) {
                const run = this.count("reused child count");
                if (run > previous.length - position) {
                    throw new Error("native parser reused children the previous read does not have");
                }
                parts.push(run);
                total += run;
                position += run;
            } else if (tag === OP_SPINE) {
                const node = previous[position];
                if (node === undefined)
                    throw new Error("native parser rewrote a child the previous read does not have");
                parts.push(this.spine(node));
                total += 1;
                position += 1;
            } else {
                parts.push(this.nodeOfKind(this.kindOf(tag)));
                total += 1;
                position += 1;
            }
        }
        const children: Markup[] = new Array<Markup>(total);
        let at = 0;
        position = 0;
        for (const part of parts) {
            if (typeof part === "number") {
                for (let offset = 0; offset < part; offset++) children[at++] = previous[position + offset]!;
                position += part;
            } else {
                children[at++] = part;
                position += 1;
            }
        }
        return children;
    }

    /** A decoded value already carries its `dump` through the shared
     * prototype `base` builds on (#139); nothing is defined per node. */
    markup(value: MarkupValue): Markup {
        return value as Markup;
    }

    value(kind: NativeKind, id: Identity, scope: Scope): MarkupValue {
        switch (kind) {
            case "document":
                throw new Error("a document node cannot be a child");
            case "blockQuote":
                return Object.assign(this.base(id, scope, kind), { content: this.markupList() });
            case "paragraph":
                return Object.assign(this.base(id, scope, kind), { content: this.markupList() });
            case "heading": {
                const level = this.int();
                if (!Number.isInteger(level) || level < 1 || level > 6) {
                    throw new Error(`native parser returned an invalid heading level ${level}`);
                }
                return Object.assign(this.base(id, scope, kind), { level, content: this.markupList() });
            }
            case "thematicBreak":
                return this.base(id, scope, kind);
            case "list":
                return this.list(id, scope);
            case "listItem": {
                const checked = this.nullableBoolean("list item checked state");
                return Object.assign(this.base(id, scope, kind), { checked, content: this.markupList() });
            }
            case "codeBlock":
                return Object.assign(this.base(id, scope, kind), {
                    info: this.string(),
                    language: this.string(),
                    literal: this.requiredString(),
                    fenced: this.boolean("code fenced state"),
                    closed: this.boolean("code closed state")
                });
            case "htmlBlock":
                return Object.assign(this.base(id, scope, kind), { literal: this.requiredString() });
            case "formulaBlock":
                // No `mode`: a formula block is always standalone -- the wire
                // stopped carrying the byte at Q29.
                return Object.assign(this.base(id, scope, kind), { literal: this.requiredString() });
            case "table":
                return this.table(id, scope);
            case "directiveBlock":
                return Object.assign(this.base(id, scope, kind), this.directiveFields());
            case "footnoteDefinition":
                return Object.assign(this.base(id, scope, kind), {
                    label: this.requiredString(),
                    norm: this.requiredString(),
                    content: this.markupList()
                });
            case "referenceDefinition":
                return Object.assign(this.base(id, scope, kind), {
                    label: this.requiredString(),
                    norm: this.requiredString(),
                    destination: this.requiredString(),
                    title: this.string()
                });
            case "text":
                return Object.assign(this.base(id, scope, kind), { literal: this.requiredString() });
            case "softBreak":
                return this.base(id, scope, kind);
            case "lineBreak":
                return this.base(id, scope, kind);
            case "code":
                return Object.assign(this.base(id, scope, kind), { literal: this.requiredString() });
            case "html":
                return Object.assign(this.base(id, scope, kind), { literal: this.requiredString() });
            case "formula":
                return Object.assign(this.base(id, scope, kind), {
                    mode: this.placement(),
                    literal: this.requiredString()
                });
            case "emphasis":
                return Object.assign(this.base(id, scope, kind), { content: this.markupList() });
            case "strong":
                return Object.assign(this.base(id, scope, kind), { content: this.markupList() });
            case "strikethrough":
                return Object.assign(this.base(id, scope, kind), { content: this.markupList() });
            case "link":
                return Object.assign(this.base(id, scope, kind), {
                    destination: this.requiredString(),
                    title: this.string(),
                    content: this.markupList()
                });
            case "image":
                return Object.assign(this.base(id, scope, kind), {
                    source: this.requiredString(),
                    title: this.string(),
                    content: this.markupList()
                });
            case "directive": {
                const fields = this.directiveFields();
                if (fields.content.length !== 0) throw new Error("inline directive contains block content");
                return Object.assign(this.base(id, scope, kind), {
                    name: fields.name,
                    attributes: fields.attributes,
                    label: fields.label
                });
            }
            case "footnoteReference":
                return Object.assign(this.base(id, scope, kind), {
                    label: this.requiredString(),
                    definition: this.identity()
                });
            case "linkReference":
            case "imageReference":
                return Object.assign(this.base(id, scope, kind), {
                    label: this.requiredString(),
                    form: this.referenceForm(),
                    definition: this.identity(),
                    content: this.markupList()
                });
            case "tableRow":
                return this.tableRow(id, scope);
            case "tableCell":
                return Object.assign(this.base(id, scope, "tableCell"), { content: this.markupList() });
            case "directiveLabel":
                return Object.assign(this.base(id, scope, kind), { content: this.markupList() });
        }
        return unreachable(kind);
    }

    list(id: Identity, scope: Scope): MarkupValueOf<"list"> {
        const flavor = this.listFlavor();
        const startValue = this.long();
        const start = this.boolean("list start state") ? startValue : null;
        const tight = this.boolean("list tight state");
        if (flavor === "bullet" && start !== null) {
            throw new Error("native parser returned a start value for a bullet list");
        }
        /* Through `markupList`, not a loop of its own: a list is a container
         * of blocks, so a delta may rewrite it as a SPINE whose ops address
         * its items by position (#162). */
        const items = this.markupList();
        for (const item of items) {
            if (item.kind !== "listItem") throw new Error("list contains a non-item node");
        }
        return Object.assign(this.base(id, scope, "list"), { flavor, start, tight, items: items as ListItem[] });
    }

    table(id: Identity, scope: Scope): MarkupValueOf<"table"> {
        const columnCount = this.count("table column count");
        const alignments: TableAlignment[] = [];
        for (let index = 0; index < columnCount; index++) {
            alignments.push(this.tableAlignment());
        }
        /* Through `markupList` for the same reason as a list's items. The
         * header row is the table's FIRST child on the wire -- the engine
         * opens a table with it -- which is what lets a delta address
         * `[header, ...rows]` by position. */
        const children = this.markupList();
        for (const row of children) {
            if (row.kind !== "tableRow") throw new Error("table contains a non-row node");
        }
        const rows = children as TableRow[];
        const headers = rows.filter((row) => row.isHeader);
        if (headers.length !== 1) throw new Error(`table contains ${headers.length} header rows`);
        const header = rows[0]!;
        if (!header.isHeader) throw new Error("table does not open with its header row");
        return Object.assign(this.base(id, scope, "table"), {
            alignments,
            header,
            rows: rows.slice(1)
        });
    }

    tableRow(id: Identity, scope: Scope): Omit<TableRow, "dump"> {
        const isHeader = this.boolean("table header state");
        const cellCount = this.count("table cell count");
        const cells: TableCell[] = [];
        for (let index = 0; index < cellCount; index++) {
            const cell = this.node();
            if (cell.kind !== "tableCell") throw new Error("table row contains a non-cell node");
            cells.push(cell);
        }
        return Object.assign(this.base(id, scope, "tableRow"), { isHeader, cells });
    }

    directiveFields(): DirectiveFields {
        const name = this.requiredString();
        const attributes = this.directiveAttributes();
        const children = this.markupList();
        const first = children.length > 0 ? children[0]! : null;
        const label = first !== null && first.kind === "directiveLabel" ? first : null;
        return {
            name,
            attributes,
            label,
            content: label === null ? children : children.slice(1)
        };
    }

    /** An absent attribute container and an empty one are different things, so
     * the wire carries the presence byte before the count. */
    directiveAttributes(): readonly DirectiveAttribute[] | null {
        const present = this.boolean("directive attribute presence");
        const count = this.count("directive attribute count");
        if (!present) {
            if (count !== 0) throw new Error("an absent directive attribute container cannot hold attributes");
            return null;
        }
        const attributes: DirectiveAttribute[] = [];
        for (let index = 0; index < count; index++) {
            attributes.push({ name: this.requiredString(), value: this.requiredString() });
        }
        return Object.freeze(attributes);
    }

    /** Every node starts on the shared prototype that carries `dump`
     * (#139): a prototype method is not an own enumerable property, so the
     * pinned contract (`Object.keys` excludes `dump`) holds without the
     * per-node `defineProperty` -- which allocated a descriptor and a fresh
     * closure per node and forced the slow-path definition, ~20% of the
     * whole decode. The `value()` cases assign their fields onto this one
     * object instead of spreading it into a second. (The retired own
     * property was non-writable; a prototype method is shadowable -- the
     * public promise is the enumerability, which the tests pin.) */
    base<Kind extends Markup["kind"]>(id: Identity, scope: Scope, kind: Kind): Omit<MarkupBase<Kind>, "dump"> {
        const value = Object.create(MARKUP_PROTOTYPE) as { kind: Kind; id: Identity; scope: Scope };
        value.kind = kind;
        value.id = id;
        value.scope = scope;
        return value;
    }

    placement(): PlacementMode {
        const rawValue = this.int();
        if (rawValue === 1) return "embedded";
        if (rawValue === 2) return "standalone";
        throw new Error(`native parser returned invalid placement mode ${rawValue}`);
    }

    referenceForm(): ReferenceForm {
        const rawValue = this.int();
        if (rawValue === 1) return "full";
        if (rawValue === 2) return "collapsed";
        if (rawValue === 3) return "shortcut";
        throw new Error(`native parser returned invalid reference form ${rawValue}`);
    }

    listFlavor(): ListFlavor {
        const rawValue = this.int();
        if (rawValue === 1) return "bullet";
        if (rawValue === 2) return "ordered";
        throw new Error(`native parser returned invalid list flavor ${rawValue}`);
    }

    tableAlignment(): TableAlignment {
        const values: readonly TableAlignment[] = ["none", "left", "center", "right"];
        const alignment = values[this.byte()];
        if (alignment === undefined) throw new Error("native parser returned invalid table alignment");
        return alignment;
    }
}

function unreachable(value: never): never {
    throw new Error(`unreachable native node kind ${String(value)}`);
}

/** The previous read's children of a node a SPINE rewrites, in WIRE ORDER:
 * the positions the op stream counts in. Only the block containers the
 * native side ever rewrites have one -- a list's items, a table's header
 * then its rows, a directive block's label then its content, and the
 * content of the rest -- so a spine on any other kind is refused. */
function positionalChildren(node: Markup): readonly Markup[] {
    switch (node.kind) {
        case "document":
        case "blockQuote":
        case "listItem":
        case "footnoteDefinition":
            return node.content;
        case "list":
            return node.items;
        case "table":
            return [node.header, ...node.rows];
        case "directiveBlock":
            return node.label === null ? node.content : [node.label, ...node.content];
        default:
            throw new Error(`native parser rewrote a ${node.kind}, which has no children to reuse`);
    }
}

function errorCode(rawValue: number): ParseErrorCode {
    if (rawValue === 1) return "invalidArgument";
    if (rawValue === 2) return "allocationFailed";
    return "internal";
}
