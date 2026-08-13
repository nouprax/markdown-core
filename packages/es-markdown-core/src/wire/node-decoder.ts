import type { Document } from "../model/document.js";
import type { Markup } from "../model/markup.js";
import type { MarkupID } from "../model/markup-id.js";
import type { TableCell, TableRow } from "../model/table.js";
import { ParseError, type ParseErrorCode } from "../parse-error.js";
import type { ListFlavor, PlacementMode, Scope, TableAlignment } from "../values.js";
import type { NativeExports } from "../runtime/native.js";
import { kinds, type NativeKind } from "./kinds.js";

/** A decoded document before `adopt` wires its mediators — node, edit,
 * close, dump — to the parse it came from. */
export type DocumentValue = Pick<Document, "kind" | "id" | "revision" | "scope" | "content">;

/**
 * One decode pass over the native tree. Every node is decoded, every time: a
 * document's projection is a function of its text and its options, not of how
 * the caller reached it.
 */
export interface DecodeContext {
    /** Series-interned identity for a raw id: the same identity is always
     * the same `MarkupID` object. */
    readonly ids: (rawValue: number) => MarkupID;
    /** Completes the decoded root into the document value. */
    readonly adopt: (value: DocumentValue) => Document;
    /** The document's id → node index, filled as the walk decodes. */
    readonly index: Map<number, Markup>;
}

const stringField = {
    literal: 1,
    formulaLiteral: 2,
    linkDestination: 3,
    linkTitle: 4,
    imageSource: 5,
    imageTitle: 6,
    footnoteLabel: 7,
    crossLinkReference: 8,
    embedReference: 9,
    errorMessage: 10,
    definitionLabel: 11,
    definitionDestination: 12,
    definitionTitle: 13,
    referenceLabel: 14
} as const;

const referenceForms = ["full", "collapsed", "shortcut"] as const;

const scratchSize = 4 * BigUint64Array.BYTES_PER_ELEMENT;

/** One explicit decode-stack entry; `values` collects the direct child
 * values until the node itself can be assembled. */
interface DecodeFrame {
    readonly node: number;
    readonly id: MarkupID;
    readonly revision: number;
    readonly scope: Scope;
    readonly pointers: readonly number[];
    readonly values: Markup[];
    index: number;
}

export class NodeDecoder {
    private scratch: number;
    private context: DecodeContext | null = null;
    private cachedView: DataView | null = null;
    private readonly utf8Decoder = new TextDecoder("utf-8", { fatal: false });

    constructor(private readonly native: NativeExports) {
        this.scratch = native.malloc(scratchSize);
        if (!this.scratch) throw new ParseError("allocationFailed", "failed to allocate WASM memory");
    }

    /** The scratch block shared with the document boundary. Allocated once
     * in the constructor and held for the module's lifetime — the decoder is
     * a singleton over one WASM instance, so there is nothing to release it
     * to. Holds four little-endian 64-bit slots. */
    get scratchPointer(): number {
        this.requireLive();
        return this.scratch;
    }

    decodeDocument(root: number, context: DecodeContext): Document {
        if (this.kind(root) !== "document") {
            throw new ParseError("internal", "parser returned an invalid document tree");
        }
        this.context = context;
        try {
            const document = context.adopt({
                kind: "document",
                id: context.ids(this.rawId(root)),
                revision: this.revisionOf(root),
                scope: this.scopeOf(root),
                content: this.decodeChildren(root)
            });
            return document;
        } finally {
            this.context = null;
        }
    }

    parseError(error: number): ParseError {
        if (!error) return new ParseError("internal", "markdown parsing failed");
        const code = errorCode(this.native.es_error_code(error));
        const message = this.readString(error, stringField.errorMessage) ?? "markdown parsing failed";
        return new ParseError(code, message);
    }

    toSafeNumber(value: bigint, field: string): number {
        const converted = Number(value);
        if (!Number.isSafeInteger(converted) || converted < 0) {
            throw new Error(`native parser returned ${field} ${value} beyond JavaScript integer precision`);
        }
        return converted;
    }

    rawId(node: number): number {
        return this.toSafeNumber(this.native.es_node_id(node), "node id");
    }

    revisionOf(node: number): number {
        return this.toSafeNumber(this.native.es_node_revision(node), "node revision");
    }

    /** The node's own absolute extent, read in O(1) off the node. */
    scopeOf(node: number): Scope {
        this.requireLive();
        this.native.es_node_scope(node, this.scratch);
        const view = this.dataView();
        return {
            start: {
                line: view.getInt32(this.scratch, true),
                column: view.getInt32(this.scratch + 4, true)
            },
            end: {
                line: view.getInt32(this.scratch + 8, true),
                column: view.getInt32(this.scratch + 12, true)
            }
        };
    }

    /** One decode frame: a node whose value is assembled once every direct
     * child value in `values` is ready. */
    private static frame(node: number, id: MarkupID, revision: number, scope: Scope, pointers: number[]): DecodeFrame {
        return { node, id, revision, scope, pointers, values: [], index: 0 };
    }

    /**
     * Decodes the direct children of `parent` post-order over an explicit
     * frame stack: nesting depth is input-controlled, so the decoder must
     * not grow the JavaScript call stack with it.
     */
    private decodeChildren(parent: number): Markup[] {
        const context = this.context!;
        const stack: DecodeFrame[] = [
            NodeDecoder.frame(
                parent,
                context.ids(this.rawId(parent)),
                this.revisionOf(parent),
                this.scopeOf(parent),
                this.childPointers(parent)
            )
        ];
        while (true) {
            const top = stack[stack.length - 1]!;
            if (top.index < top.pointers.length) {
                const pointer = top.pointers[top.index]!;
                top.index += 1;
                const rawId = this.rawId(pointer);
                stack.push(
                    NodeDecoder.frame(
                        pointer,
                        context.ids(rawId),
                        this.revisionOf(pointer),
                        this.scopeOf(pointer),
                        this.childPointers(pointer)
                    )
                );
                continue;
            }
            stack.pop();
            if (stack.length === 0) return top.values;
            const value = this.decodeValue(top.node, top.id, top.revision, top.scope, top.values);
            context.index.set(top.id.rawValue, value);
            stack[stack.length - 1]!.values.push(value);
        }
    }

    /**
     * Assembles one node's value from its native fields and its ready direct
     * child values.
     */
    decodeValue(node: number, id: MarkupID, revision: number, scope: Scope, children: readonly Markup[]): Markup {
        const kind = this.kind(node);
        switch (kind) {
            case "document":
                throw new Error("native parser returned a nested document node");
            case "blockQuote":
                return { kind, id, revision, scope, content: children };
            case "paragraph":
                return { kind, id, revision, scope, content: children };
            case "heading": {
                const level = this.native.es_node_heading_level(node);
                if (!Number.isInteger(level) || level < 1 || level > 6) {
                    throw new Error(`native parser returned an invalid heading level ${level}`);
                }
                return { kind, id, revision, scope, level, content: children };
            }
            case "thematicBreak":
                return { kind, id, revision, scope };
            case "list":
                return this.copyList(node, id, revision, scope, children);
            case "listItem":
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    checked: this.nullableBoolean(this.native.es_node_checked(node), "list item checked state"),
                    content: children
                };
            case "codeBlock":
                return this.copyCodeBlock(node, id, revision, scope);
            case "htmlBlock": {
                const literal = this.requiredString(node, stringField.literal);
                return { kind, id, revision, scope, comment: this.native.es_node_html_comment(node) !== 0, literal };
            }
            case "formulaBlock": {
                const mode = this.placement(this.native.es_node_formula_mode(node));
                if (mode !== "standalone") throw new Error("native parser returned an embedded formula block");
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    mode,
                    literal: this.requiredString(node, stringField.formulaLiteral)
                };
            }
            case "table":
                return this.copyTable(node, id, revision, scope, children);
            case "directiveBlock": {
                const fields = this.directiveFields(node, children);
                if (fields.mode !== "standalone") {
                    throw new Error("native parser returned an embedded directive block");
                }
                return { kind, id, revision, scope, ...fields };
            }
            case "footnoteDefinition":
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    label: this.requiredString(node, stringField.footnoteLabel),
                    content: children
                };
            case "text":
                return { kind, id, revision, scope, literal: this.requiredString(node, stringField.literal) };
            case "softBreak":
                return { kind, id, revision, scope };
            case "lineBreak":
                return { kind, id, revision, scope };
            case "code":
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    mode: "embedded",
                    literal: this.requiredString(node, stringField.literal)
                };
            case "html": {
                const literal = this.requiredString(node, stringField.literal);
                return { kind, id, revision, scope, comment: this.native.es_node_html_comment(node) !== 0, literal };
            }
            case "formula":
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    mode: this.placement(this.native.es_node_formula_mode(node)),
                    literal: this.requiredString(node, stringField.formulaLiteral)
                };
            case "emphasis":
                return { kind, id, revision, scope, content: children };
            case "strong":
                return { kind, id, revision, scope, content: children };
            case "strikethrough":
                return { kind, id, revision, scope, content: children };
            case "link":
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    destination: this.requiredString(node, stringField.linkDestination),
                    title: this.readString(node, stringField.linkTitle),
                    content: children
                };
            case "image":
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    source: this.requiredString(node, stringField.imageSource),
                    title: this.readString(node, stringField.imageTitle),
                    content: children
                };
            case "directive": {
                const fields = this.directiveFields(node, children);
                if (fields.mode !== "embedded") throw new Error("native parser returned a standalone directive");
                if (fields.content.length !== 0) throw new Error("inline directive contains block content");
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    mode: fields.mode,
                    name: fields.name,
                    attributes: fields.attributes,
                    label: fields.label
                };
            }
            case "footnoteReference":
                return { kind, id, revision, scope, label: this.requiredString(node, stringField.footnoteLabel) };
            case "crossLink":
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    reference: this.requiredString(node, stringField.crossLinkReference)
                };
            case "embed":
                return { kind, id, revision, scope, reference: this.requiredString(node, stringField.embedReference) };
            case "referenceDefinition":
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    label: this.requiredString(node, stringField.definitionLabel),
                    destination: this.requiredString(node, stringField.definitionDestination),
                    title: this.readString(node, stringField.definitionTitle)
                };
            case "linkReference":
            case "imageReference":
                return {
                    kind,
                    id,
                    revision,
                    scope,
                    label: this.requiredString(node, stringField.referenceLabel),
                    form: referenceForms[this.native.es_node_reference_form(node)] ?? "shortcut",
                    content: children
                };
            case "tableRow":
                return this.copyTableRow(node, id, revision, scope, children);
            case "tableCell":
                return this.copyTableCell(id, revision, scope, children);
            case "directiveLabel":
                return { kind, id, revision, scope, content: children };
        }
        return unreachable(kind);
    }

    /** One packed crossing for every code-block field; scratch layout
     * mirrors `es_node_code_properties`. */
    private copyCodeBlock(
        node: number,
        id: MarkupID,
        revision: number,
        scope: Scope
    ): Extract<Markup, { kind: "codeBlock" }> {
        this.requireLive();
        this.native.es_node_code_properties(node, this.scratch);
        const view = this.dataView();
        const literal = this.scratchString(view, 16);
        if (literal === null) throw new Error("native parser returned a missing string");
        return {
            kind: "codeBlock",
            id,
            revision,
            scope,
            mode: "standalone",
            info: this.scratchString(view, 0),
            language: this.scratchString(view, 8),
            literal,
            fenced: this.boolean(view.getInt32(this.scratch + 24, true), "code fenced state"),
            closed: this.boolean(view.getInt32(this.scratch + 28, true), "code closed state")
        };
    }

    private copyList(
        node: number,
        id: MarkupID,
        revision: number,
        scope: Scope,
        children: readonly Markup[]
    ): Extract<Markup, { kind: "list" }> {
        this.requireLive();
        this.native.es_node_list_properties(node, this.scratch);
        const view = this.dataView();
        const flavor = this.listFlavor(view.getInt32(this.scratch, true));
        const tight = this.boolean(view.getInt32(this.scratch + 4, true), "list tight state");
        let start: number | null = null;
        if (this.boolean(view.getInt32(this.scratch + 8, true), "list start state")) {
            const value = Number(view.getBigInt64(this.scratch + 16, true));
            if (!Number.isSafeInteger(value)) {
                throw new Error("native list start exceeds JavaScript integer precision");
            }
            start = value;
        }
        if (flavor === "bullet" && start !== null) {
            throw new Error("native parser returned a start value for a bullet list");
        }
        const items = children.map((item) => {
            if (item.kind !== "listItem") throw new Error("list contains a non-item node");
            return item;
        });
        return { kind: "list", id, revision, scope, flavor, start, tight, items };
    }

    private copyTable(
        node: number,
        id: MarkupID,
        revision: number,
        scope: Scope,
        children: readonly Markup[]
    ): Extract<Markup, { kind: "table" }> {
        const columnCount = this.count(this.native.es_node_table_column_count(node), "table column count");
        const alignments = Array.from({ length: columnCount }, (_, index) =>
            this.tableAlignment(this.native.es_node_table_alignment(node, index))
        );
        const rows = children.map((row) => {
            if (row.kind !== "tableRow") throw new Error("table contains a non-row node");
            return row;
        });
        const headers = rows.filter((row) => row.isHeader);
        if (headers.length !== 1) throw new Error(`table contains ${headers.length} header rows`);
        return {
            kind: "table",
            id,
            revision,
            scope,
            alignments,
            header: headers[0]!,
            rows: rows.filter((row) => !row.isHeader)
        };
    }

    private copyTableRow(
        node: number,
        id: MarkupID,
        revision: number,
        scope: Scope,
        children: readonly Markup[]
    ): TableRow {
        return {
            kind: "tableRow",
            id,
            revision,
            scope,
            isHeader: this.boolean(this.native.es_node_table_row_header(node), "table header state"),
            cells: children.map((cell) => {
                if (cell.kind !== "tableCell") throw new Error("table row contains a non-cell node");
                return cell;
            })
        };
    }

    private copyTableCell(id: MarkupID, revision: number, scope: Scope, children: readonly Markup[]): TableCell {
        return { kind: "tableCell", id, revision, scope, content: children };
    }

    private directiveFields(
        node: number,
        children: readonly Markup[]
    ): {
        readonly mode: PlacementMode;
        readonly name: string;
        readonly attributes: Readonly<Record<string, string>> | null;
        readonly label: Extract<Markup, { kind: "directiveLabel" }> | null;
        readonly content: readonly Markup[];
    } {
        // One packed crossing for mode, name, presence and count; scratch
        // layout mirrors `es_node_directive_properties`. Everything is read
        // out before the pair loop, which reuses the same scratch block.
        this.requireLive();
        this.native.es_node_directive_properties(node, this.scratch);
        const view = this.dataView();
        const mode = this.placement(view.getInt32(this.scratch, true));
        const present = view.getUint32(this.scratch + 4, true) !== 0;
        const name = this.scratchString(view, 8);
        if (name === null) throw new Error("native parser returned a missing string");
        const count = this.count(view.getUint32(this.scratch + 16, true), "directive attribute count");
        const attributes = present ? this.directiveAttributes(node, count) : null;
        const firstChild = children[0];
        const label = firstChild?.kind === "directiveLabel" ? firstChild : null;
        const content = label === null ? children : children.slice(1);
        if (content.some((child) => child.kind === "directiveLabel")) {
            throw new Error("native parser returned a misplaced directive label");
        }
        return { mode, name, attributes, label, content };
    }

    /** The attribute pairs, one crossing each, in the source order the engine
     * holds them. The grammar has no duplicate name to represent, so a plain
     * object IS the value rather than a rendering of it. */
    private directiveAttributes(node: number, count: number): Record<string, string> {
        const attributes: Record<string, string> = {};
        for (let index = 0; index < count; index += 1) {
            if (!this.native.es_node_directive_attribute_at(node, index, this.scratch)) {
                throw new Error("native parser returned fewer directive attributes than it counted");
            }
            const view = this.dataView();
            const key = this.scratchString(view, 0);
            const value = this.scratchString(view, 8);
            if (key === null || value === null) {
                throw new Error("native parser returned a missing string");
            }
            attributes[key] = value;
        }
        return attributes;
    }

    childPointers(node: number): number[] {
        const result: number[] = [];
        for (
            let child = this.native.es_node_first_child(node);
            child;
            child = this.native.es_node_next_sibling(child)
        ) {
            result.push(child);
        }
        return result;
    }

    private kind(node: number): NativeKind {
        const rawValue = this.native.es_node_kind(node);
        const kind = kinds[rawValue];
        if (!kind || kind === "none") throw new Error(`native parser returned unknown node kind ${rawValue}`);
        return kind;
    }

    readString(object: number, field: number): string | null {
        this.requireLive();
        this.native.es_string(object, field, this.scratch, this.scratch + Uint32Array.BYTES_PER_ELEMENT);
        return this.scratchString(this.dataView(), 0);
    }

    private requiredString(object: number, field: number): string {
        const value = this.readString(object, field);
        if (value === null) throw new Error("native parser returned a missing string");
        return value;
    }

    /** Decodes the (data, length) string view stored at scratch `offset`;
     * null for the null view. */
    private scratchString(view: DataView, offset: number): string | null {
        const data = view.getUint32(this.scratch + offset, true);
        const length = view.getUint32(this.scratch + offset + Uint32Array.BYTES_PER_ELEMENT, true);
        if (!data) {
            if (length !== 0) throw new Error("native parser returned an invalid string view");
            return null;
        }
        if (length > this.native.memory.buffer.byteLength - data) {
            throw new Error("native parser returned an out-of-bounds string view");
        }
        return this.utf8Decoder.decode(new Uint8Array(this.native.memory.buffer, data, length));
    }

    private placement(rawValue: number): PlacementMode {
        if (rawValue === 1) return "embedded";
        if (rawValue === 2) return "standalone";
        throw new Error(`native parser returned invalid placement mode ${rawValue}`);
    }

    private listFlavor(rawValue: number): ListFlavor {
        if (rawValue === 1) return "bullet";
        if (rawValue === 2) return "ordered";
        throw new Error(`native parser returned invalid list flavor ${rawValue}`);
    }

    private tableAlignment(rawValue: number): TableAlignment {
        const values: readonly TableAlignment[] = ["none", "left", "center", "right"];
        const alignment = values[rawValue];
        if (alignment === undefined) throw new Error(`native parser returned invalid table alignment ${rawValue}`);
        return alignment;
    }

    private boolean(rawValue: number, field: string): boolean {
        if (rawValue === 0) return false;
        if (rawValue === 1) return true;
        throw new Error(`native parser returned invalid ${field} ${rawValue}`);
    }

    private nullableBoolean(rawValue: number, field: string): boolean | null {
        if (rawValue === -1) return null;
        return this.boolean(rawValue, field);
    }

    private count(rawValue: number, field: string): number {
        if (!Number.isSafeInteger(rawValue) || rawValue < 0) {
            throw new Error(`native parser returned invalid ${field} ${rawValue}`);
        }
        return rawValue;
    }

    /** A DataView over the current wasm memory. Cached per underlying
     * buffer: growth replaces `memory.buffer` (detaching the old one), so
     * revalidating against buffer identity on every access stays safe while
     * dropping the per-scalar-read allocation. */
    dataView(): DataView {
        const buffer = this.native.memory.buffer;
        if (this.cachedView === null || this.cachedView.buffer !== buffer) {
            this.cachedView = new DataView(buffer);
        }
        return this.cachedView;
    }

    private requireLive(): void {
        if (!this.scratch) throw new Error("native decoder has been disposed");
    }
}

function unreachable(value: never): never {
    throw new Error(`unreachable native node kind ${String(value)}`);
}

function errorCode(rawValue: number): ParseErrorCode {
    if (rawValue === 1) return "invalidArgument";
    if (rawValue === 2) return "allocationFailed";
    return "internal";
}
