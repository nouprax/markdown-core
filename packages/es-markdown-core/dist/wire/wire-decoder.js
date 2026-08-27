import { Concrete } from "../concrete.js";
import { ParseError } from "../parse-error.js";
import { TreeDumper } from "../tree-dumper.js";
import { kinds } from "./kinds.js";
/** Decodes a full payload: the envelope, the tree, the concrete view. */
export function decodeRead(bytes) {
    const reader = new WireReader(bytes);
    reader.header();
    return reader.read();
}
/**
 * Decodes only the envelope of a payload whose read is discarded -- the
 * `Document` constructor's initial feed -- so an error still surfaces and a
 * healthy tree is not built just to be thrown away.
 */
export function decodeDiscarded(bytes) {
    new WireReader(bytes).header();
}
/** `MKC6`: the payload after the envelope is the facade's canonical wire --
 * every node leads with its identity, a definition's match key rides where
 * `identifier` did, and a reference carries the identity of the definition it
 * resolved to instead of repeating that key. */
const magic = [0x4d, 0x4b, 0x43, 0x36];
class WireReader {
    #offset = 0;
    #bytes;
    #view;
    #utf8Decoder = new TextDecoder("utf-8", { fatal: false });
    constructor(bytes) {
        this.#bytes = bytes;
        this.#view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    }
    /** The magic and the status: the part of every payload that says whether
     * a tree or an error follows, throwing the error itself. */
    header() {
        for (const [index, expected] of magic.entries()) {
            const actual = this.byte();
            if (actual !== expected) {
                throw new Error(`invalid native bridge payload at byte ${index}: expected ${expected}, got ${actual}`);
            }
        }
        const status = this.byte();
        if (status === 1)
            throw this.parseError();
        if (status !== 0)
            throw new Error("unsupported native bridge status");
    }
    read() {
        if (this.kind() !== "document")
            throw new Error("native bridge returned an invalid document tree");
        const id = this.identity();
        const scope = this.scope();
        const content = this.markupList();
        const concrete = this.concrete();
        if (!this.finished)
            throw new Error("native bridge returned a truncated payload");
        const semantic = this.markup({ kind: "document", id, scope, content });
        return { semantic, concrete };
    }
    parseError() {
        const code = errorCode(this.int());
        const message = this.requiredString();
        if (!this.finished)
            throw new Error("invalid native error payload");
        return new ParseError(code, message);
    }
    get finished() {
        return this.#offset === this.#bytes.length;
    }
    byte() {
        if (this.#offset >= this.#bytes.length)
            throw new Error("truncated native bridge payload");
        return this.#bytes[this.#offset++];
    }
    int() {
        if (this.#offset > this.#bytes.length - 4)
            throw new Error("truncated native bridge payload");
        const value = this.#view.getInt32(this.#offset, true);
        this.#offset += 4;
        return value;
    }
    uint() {
        if (this.#offset > this.#bytes.length - 4)
            throw new Error("truncated native bridge payload");
        const value = this.#view.getUint32(this.#offset, true);
        this.#offset += 4;
        return value;
    }
    long() {
        if (this.#offset > this.#bytes.length - 8)
            throw new Error("truncated native bridge payload");
        const value = this.#view.getBigInt64(this.#offset, true);
        this.#offset += 8;
        const narrowed = Number(value);
        if (!Number.isSafeInteger(narrowed)) {
            throw new Error("native value exceeds JavaScript integer precision");
        }
        return narrowed;
    }
    string() {
        const size = this.int();
        if (size === -1)
            return null;
        if (size < 0 || size > this.#bytes.length - this.#offset)
            throw new Error("invalid native bridge string");
        const end = this.#offset + size;
        const value = this.#utf8Decoder.decode(this.#bytes.subarray(this.#offset, end));
        this.#offset = end;
        return value;
    }
    requiredString() {
        const value = this.string();
        if (value === null)
            throw new Error("missing native field");
        return value;
    }
    bytes(size) {
        if (size < 0 || size > this.#bytes.length - this.#offset)
            throw new Error("invalid native byte run");
        const end = this.#offset + size;
        const value = this.#bytes.slice(this.#offset, end);
        this.#offset = end;
        return value;
    }
    scope() {
        return {
            start: { line: this.int(), column: this.int() },
            end: { line: this.int(), column: this.int() }
        };
    }
    identity() {
        return { block: this.uint(), ordinal: this.uint() };
    }
    kind() {
        const rawValue = this.byte();
        const kind = kinds[rawValue];
        if (!kind || kind === "none")
            throw new Error(`native parser returned unknown node kind ${rawValue}`);
        return kind;
    }
    boolean(field) {
        const rawValue = this.byte();
        if (rawValue === 0)
            return false;
        if (rawValue === 1)
            return true;
        throw new Error(`native parser returned invalid ${field} ${rawValue}`);
    }
    nullableBoolean(field) {
        const rawValue = this.byte();
        if (rawValue === 255)
            return null;
        if (rawValue === 0)
            return false;
        if (rawValue === 1)
            return true;
        throw new Error(`native parser returned invalid ${field} ${rawValue}`);
    }
    count(field) {
        const value = this.int();
        if (value < 0)
            throw new Error(`native parser returned invalid ${field} ${value}`);
        return value;
    }
    markupList() {
        const count = this.count("child count");
        const children = [];
        for (let index = 0; index < count; index++) {
            children.push(this.node());
        }
        return children;
    }
    node() {
        const kind = this.kind();
        const id = this.identity();
        const scope = this.scope();
        return this.markup(this.value(kind, id, scope));
    }
    /** Seals a decoded value with its non-enumerable `dump`. */
    markup(value) {
        Object.defineProperty(value, "dump", {
            enumerable: false,
            value() {
                return TreeDumper.dump(this);
            }
        });
        return value;
    }
    value(kind, id, scope) {
        switch (kind) {
            case "document":
                throw new Error("a document node cannot be a child");
            case "blockQuote":
                return { ...this.base(id, scope, kind), content: this.markupList() };
            case "paragraph":
                return { ...this.base(id, scope, kind), content: this.markupList() };
            case "heading": {
                const level = this.int();
                if (!Number.isInteger(level) || level < 1 || level > 6) {
                    throw new Error(`native parser returned an invalid heading level ${level}`);
                }
                return { ...this.base(id, scope, kind), level, content: this.markupList() };
            }
            case "thematicBreak":
                return this.base(id, scope, kind);
            case "list":
                return this.list(id, scope);
            case "listItem": {
                const checked = this.nullableBoolean("list item checked state");
                return { ...this.base(id, scope, kind), checked, content: this.markupList() };
            }
            case "codeBlock":
                return {
                    ...this.base(id, scope, kind),
                    info: this.string(),
                    language: this.string(),
                    literal: this.requiredString(),
                    fenced: this.boolean("code fenced state"),
                    closed: this.boolean("code closed state")
                };
            case "htmlBlock":
                return { ...this.base(id, scope, kind), literal: this.requiredString() };
            case "formulaBlock":
                // No `mode`: a formula block is always standalone -- the wire
                // stopped carrying the byte at Q29.
                return { ...this.base(id, scope, kind), literal: this.requiredString() };
            case "table":
                return this.table(id, scope);
            case "directiveBlock":
                return { ...this.base(id, scope, kind), ...this.directiveFields() };
            case "footnoteDefinition":
                return {
                    ...this.base(id, scope, kind),
                    label: this.requiredString(),
                    norm: this.requiredString(),
                    content: this.markupList()
                };
            case "referenceDefinition":
                return {
                    ...this.base(id, scope, kind),
                    label: this.requiredString(),
                    norm: this.requiredString(),
                    destination: this.requiredString(),
                    title: this.string()
                };
            case "text":
                return { ...this.base(id, scope, kind), literal: this.requiredString() };
            case "softBreak":
                return this.base(id, scope, kind);
            case "lineBreak":
                return this.base(id, scope, kind);
            case "code":
                return { ...this.base(id, scope, kind), literal: this.requiredString() };
            case "html":
                return { ...this.base(id, scope, kind), literal: this.requiredString() };
            case "formula":
                return {
                    ...this.base(id, scope, kind),
                    mode: this.placement(),
                    literal: this.requiredString()
                };
            case "emphasis":
                return { ...this.base(id, scope, kind), content: this.markupList() };
            case "strong":
                return { ...this.base(id, scope, kind), content: this.markupList() };
            case "strikethrough":
                return { ...this.base(id, scope, kind), content: this.markupList() };
            case "link":
                return {
                    ...this.base(id, scope, kind),
                    destination: this.requiredString(),
                    title: this.string(),
                    content: this.markupList()
                };
            case "image":
                return {
                    ...this.base(id, scope, kind),
                    source: this.requiredString(),
                    title: this.string(),
                    content: this.markupList()
                };
            case "directive": {
                const fields = this.directiveFields();
                if (fields.content.length !== 0)
                    throw new Error("inline directive contains block content");
                return {
                    ...this.base(id, scope, kind),
                    name: fields.name,
                    attributes: fields.attributes,
                    label: fields.label
                };
            }
            case "footnoteReference":
                return {
                    ...this.base(id, scope, kind),
                    label: this.requiredString(),
                    definition: this.identity()
                };
            case "linkReference":
            case "imageReference":
                return {
                    ...this.base(id, scope, kind),
                    label: this.requiredString(),
                    form: this.referenceForm(),
                    definition: this.identity(),
                    content: this.markupList()
                };
            case "tableRow":
                return this.tableRow(id, scope);
            case "tableCell":
                return { ...this.base(id, scope, "tableCell"), content: this.markupList() };
            case "directiveLabel":
                return { ...this.base(id, scope, kind), content: this.markupList() };
        }
        return unreachable(kind);
    }
    list(id, scope) {
        const flavor = this.listFlavor();
        const startValue = this.long();
        const start = this.boolean("list start state") ? startValue : null;
        const tight = this.boolean("list tight state");
        if (flavor === "bullet" && start !== null) {
            throw new Error("native parser returned a start value for a bullet list");
        }
        const count = this.count("list item count");
        const items = [];
        for (let index = 0; index < count; index++) {
            const item = this.node();
            if (item.kind !== "listItem")
                throw new Error("list contains a non-item node");
            items.push(item);
        }
        return { ...this.base(id, scope, "list"), flavor, start, tight, items };
    }
    table(id, scope) {
        const columnCount = this.count("table column count");
        const alignments = [];
        for (let index = 0; index < columnCount; index++) {
            alignments.push(this.tableAlignment());
        }
        const rowCount = this.count("table row count");
        const rows = [];
        for (let index = 0; index < rowCount; index++) {
            const row = this.node();
            if (row.kind !== "tableRow")
                throw new Error("table contains a non-row node");
            rows.push(row);
        }
        const headers = rows.filter((row) => row.isHeader);
        if (headers.length !== 1)
            throw new Error(`table contains ${headers.length} header rows`);
        return {
            ...this.base(id, scope, "table"),
            alignments,
            header: headers[0],
            rows: rows.filter((row) => !row.isHeader)
        };
    }
    tableRow(id, scope) {
        const isHeader = this.boolean("table header state");
        const cellCount = this.count("table cell count");
        const cells = [];
        for (let index = 0; index < cellCount; index++) {
            const cell = this.node();
            if (cell.kind !== "tableCell")
                throw new Error("table row contains a non-cell node");
            cells.push(cell);
        }
        return { ...this.base(id, scope, "tableRow"), isHeader, cells };
    }
    directiveFields() {
        const name = this.requiredString();
        const attributes = this.directiveAttributes();
        const children = this.markupList();
        const first = children.length > 0 ? children[0] : null;
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
    directiveAttributes() {
        const present = this.boolean("directive attribute presence");
        const count = this.count("directive attribute count");
        if (!present) {
            if (count !== 0)
                throw new Error("an absent directive attribute container cannot hold attributes");
            return null;
        }
        const attributes = [];
        for (let index = 0; index < count; index++) {
            attributes.push({ name: this.requiredString(), value: this.requiredString() });
        }
        return Object.freeze(attributes);
    }
    /**
     * The source a scope's coordinates are counted against, as `wire_concrete`
     * lays it out: the source length and its bytes, then the line count and
     * one offset per line.
     */
    concrete() {
        const source = this.bytes(this.count("source length"));
        const lineCount = this.count("line count");
        const lineStarts = new Uint32Array(lineCount);
        for (let index = 0; index < lineCount; index++) {
            lineStarts[index] = this.uint();
        }
        return new Concrete(source, lineStarts);
    }
    base(id, scope, kind) {
        return { kind, id, scope };
    }
    placement() {
        const rawValue = this.int();
        if (rawValue === 1)
            return "embedded";
        if (rawValue === 2)
            return "standalone";
        throw new Error(`native parser returned invalid placement mode ${rawValue}`);
    }
    referenceForm() {
        const rawValue = this.int();
        if (rawValue === 1)
            return "full";
        if (rawValue === 2)
            return "collapsed";
        if (rawValue === 3)
            return "shortcut";
        throw new Error(`native parser returned invalid reference form ${rawValue}`);
    }
    listFlavor() {
        const rawValue = this.int();
        if (rawValue === 1)
            return "bullet";
        if (rawValue === 2)
            return "ordered";
        throw new Error(`native parser returned invalid list flavor ${rawValue}`);
    }
    tableAlignment() {
        const values = ["none", "left", "center", "right"];
        const alignment = values[this.byte()];
        if (alignment === undefined)
            throw new Error("native parser returned invalid table alignment");
        return alignment;
    }
}
function unreachable(value) {
    throw new Error(`unreachable native node kind ${String(value)}`);
}
function errorCode(rawValue) {
    if (rawValue === 1)
        return "invalidArgument";
    if (rawValue === 2)
        return "allocationFailed";
    return "internal";
}
