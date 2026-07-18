import type { Document } from "../model/document.js";
import type { Markup } from "../model/markup.js";
import type { MarkupID } from "../model/markup-id.js";
import type { TableCell, TableRow } from "../model/table.js";
import { ParseError, type ParseErrorCode } from "../parse-error.js";
import type { ListFlavor, PlacementMode, Scope, TableAlignment } from "../values.js";
import type { NativeExports } from "../runtime/native.js";
import type { ScopeEntry } from "../session/scope-resolver.js";
import { kinds, type NativeKind } from "./kinds.js";

/** A decoded document before snapshot adoption wires its scope and dump
 * mediators to a resolver. */
export type DocumentValue = Omit<Document, "scope" | "dump">;

/**
 * One decode pass over the native committed tree. One-shot parses decode
 * everything (`touched === null`); session commits decode only the delta —
 * a node outside `touched` is taken from the mirror by identity, reusing the
 * previous snapshot's value and entire subtree.
 */
export interface DecodeContext {
    /** Session-interned identity for a raw id: the same identity is always
     * the same `MarkupID` object. */
    readonly ids: (rawValue: number) => MarkupID;
    /** Wires the decoded root to the snapshot's scope resolver. */
    readonly adopt: (value: DocumentValue) => Document;
    /** The session's id → node mirror; null for a one-shot parse, whose
     * values live only in the returned tree. */
    readonly mirror: Map<number, Markup> | null;
    /** Raw ids of this commit's `added ∪ changed ∪ bubbled`; null decodes
     * every node. */
    readonly touched: ReadonlySet<number> | null;
}

const stringField = {
    codeInfo: 1,
    codeLanguage: 2,
    codeLiteral: 3,
    literal: 4,
    formulaLiteral: 5,
    directiveName: 6,
    directiveAttributes: 7,
    linkDestination: 8,
    linkTitle: 9,
    imageSource: 10,
    imageTitle: 11,
    footnoteLabel: 12,
    errorMessage: 13
} as const;

const scratchSize = 4 * BigUint64Array.BYTES_PER_ELEMENT;

export class NodeDecoder {
    private scratch: number;
    private context: DecodeContext | null = null;
    private readonly utf8Decoder = new TextDecoder("utf-8", { fatal: false });

    constructor(private readonly native: NativeExports) {
        this.scratch = native.malloc(scratchSize);
        if (!this.scratch) throw new ParseError("allocationFailed", "failed to allocate WASM memory");
    }

    dispose(): void {
        if (!this.scratch) return;
        this.native.free(this.scratch);
        this.scratch = 0;
    }

    /** The scratch block shared with the session boundary; valid until
     * `dispose`. Holds `scratchSlots` little-endian 64-bit slots. */
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
                content: this.content(root)
            });
            context.mirror?.set(document.id.rawValue, document);
            return document;
        } finally {
            this.context = null;
        }
    }

    /** One walk over the committed native tree: every node's (revision,
     * absolute scope) keyed by raw id — the snapshot's scope table. */
    scopeTable(root: number): Map<number, ScopeEntry> {
        const table = new Map<number, ScopeEntry>();
        const stack = [root];
        while (stack.length > 0) {
            const node = stack.pop()!;
            table.set(this.rawId(node), { revision: this.revisionOf(node), scope: this.scope(node) });
            for (
                let child = this.native.es_node_first_child(node);
                child;
                child = this.native.es_node_next_sibling(child)
            ) {
                stack.push(child);
            }
        }
        return table;
    }

    parseError(error: number): ParseError {
        if (!error) return new ParseError("internal", "markdown parsing failed");
        const code = errorCode(this.native.es_error_code(error));
        return new ParseError(code, this.readString(error, stringField.errorMessage) ?? "markdown parsing failed");
    }

    toSafeNumber(value: bigint, field: string): number {
        const converted = Number(value);
        if (!Number.isSafeInteger(converted) || converted < 0) {
            throw new Error(`native parser returned ${field} ${value} beyond JavaScript integer precision`);
        }
        return converted;
    }

    private rawId(node: number): number {
        return this.toSafeNumber(this.native.es_node_id(node), "node id");
    }

    private revisionOf(node: number): number {
        return this.toSafeNumber(this.native.es_node_revision(node), "node revision");
    }

    private copyMarkup(node: number): Markup {
        const context = this.context!;
        const rawId = this.rawId(node);
        const revision = this.revisionOf(node);
        if (context.touched !== null && !context.touched.has(rawId)) {
            const existing = context.mirror?.get(rawId);
            if (existing === undefined || existing.revision !== revision) {
                throw new Error("the delta omitted a node the session mirror does not carry");
            }
            return existing;
        }
        const value = this.copyMarkupValue(node, context.ids(rawId), revision);
        context.mirror?.set(rawId, value);
        return value;
    }

    private copyMarkupValue(node: number, id: MarkupID, revision: number): Markup {
        const kind = this.kind(node);
        switch (kind) {
            case "document":
                throw new Error("native parser returned a nested document node");
            case "blockQuote":
                return { kind, id, revision, content: this.content(node) };
            case "paragraph":
                return { kind, id, revision, content: this.content(node) };
            case "heading": {
                const level = this.native.es_node_heading_level(node);
                if (!Number.isInteger(level) || level < 1 || level > 6) {
                    throw new Error(`native parser returned an invalid heading level ${level}`);
                }
                return { kind, id, revision, level, content: this.content(node) };
            }
            case "thematicBreak":
                return { kind, id, revision };
            case "list":
                return this.copyList(node, id, revision);
            case "listItem":
                return {
                    kind,
                    id,
                    revision,
                    checked: this.nullableBoolean(this.native.es_node_checked(node), "list item checked state"),
                    content: this.content(node)
                };
            case "codeBlock":
                return {
                    kind,
                    id,
                    revision,
                    mode: "standalone",
                    info: this.readString(node, stringField.codeInfo),
                    language: this.readString(node, stringField.codeLanguage),
                    literal: this.requiredString(node, stringField.codeLiteral),
                    fenced: this.boolean(this.native.es_node_code_flag(node, 0), "code fenced state"),
                    closed: this.boolean(this.native.es_node_code_flag(node, 1), "code closed state")
                };
            case "htmlBlock":
                return { kind, id, revision, literal: this.requiredString(node, stringField.literal) };
            case "formulaBlock": {
                const mode = this.placement(this.native.es_node_formula_mode(node));
                if (mode !== "standalone") throw new Error("native parser returned an embedded formula block");
                return { kind, id, revision, mode, literal: this.requiredString(node, stringField.formulaLiteral) };
            }
            case "table":
                return this.copyTable(node, id, revision);
            case "directiveBlock": {
                const fields = this.directiveFields(node);
                if (fields.mode !== "standalone") {
                    throw new Error("native parser returned an embedded directive block");
                }
                return { kind, id, revision, ...fields };
            }
            case "footnoteDefinition":
                return {
                    kind,
                    id,
                    revision,
                    label: this.requiredString(node, stringField.footnoteLabel),
                    content: this.content(node)
                };
            case "text":
                return { kind, id, revision, literal: this.requiredString(node, stringField.literal) };
            case "softBreak":
                return { kind, id, revision };
            case "lineBreak":
                return { kind, id, revision };
            case "code":
                return {
                    kind,
                    id,
                    revision,
                    mode: "embedded",
                    literal: this.requiredString(node, stringField.literal)
                };
            case "html":
                return { kind, id, revision, literal: this.requiredString(node, stringField.literal) };
            case "formula":
                return {
                    kind,
                    id,
                    revision,
                    mode: this.placement(this.native.es_node_formula_mode(node)),
                    literal: this.requiredString(node, stringField.formulaLiteral)
                };
            case "emphasis":
                return { kind, id, revision, content: this.content(node) };
            case "strong":
                return { kind, id, revision, content: this.content(node) };
            case "strikethrough":
                return { kind, id, revision, content: this.content(node) };
            case "link":
                return {
                    kind,
                    id,
                    revision,
                    destination: this.readString(node, stringField.linkDestination),
                    title: this.readString(node, stringField.linkTitle),
                    content: this.content(node)
                };
            case "image":
                return {
                    kind,
                    id,
                    revision,
                    source: this.readString(node, stringField.imageSource),
                    title: this.readString(node, stringField.imageTitle),
                    content: this.content(node)
                };
            case "directive": {
                const fields = this.directiveFields(node);
                if (fields.mode !== "embedded") throw new Error("native parser returned a standalone directive");
                if (fields.content.length !== 0) throw new Error("inline directive contains block content");
                return {
                    kind,
                    id,
                    revision,
                    mode: fields.mode,
                    name: fields.name,
                    attributes: fields.attributes,
                    label: fields.label
                };
            }
            case "footnoteReference":
                return { kind, id, revision, label: this.requiredString(node, stringField.footnoteLabel) };
            case "tableRow":
                return this.copyTableRow(node, id, revision);
            case "tableCell":
                return this.copyTableCell(node, id, revision);
        }
        return unreachable(kind);
    }

    private copyList(node: number, id: MarkupID, revision: number): Extract<Markup, { kind: "list" }> {
        const flavor = this.listFlavor(this.native.es_node_list_flavor(node));
        const start = this.readStart(node);
        if (flavor === "bullet" && start !== null) {
            throw new Error("native parser returned a start value for a bullet list");
        }
        const items = this.childPointers(node).map((child) => {
            const item = this.copyMarkup(child);
            if (item.kind !== "listItem") throw new Error("list contains a non-item node");
            return item;
        });
        return {
            kind: "list",
            id,
            revision,
            flavor,
            start,
            tight: this.boolean(this.native.es_node_list_tight(node), "list tight state"),
            items
        };
    }

    private copyTable(node: number, id: MarkupID, revision: number): Extract<Markup, { kind: "table" }> {
        const columnCount = this.count(this.native.es_node_table_column_count(node), "table column count");
        const alignments = Array.from({ length: columnCount }, (_, index) =>
            this.tableAlignment(this.native.es_node_table_alignment(node, index))
        );
        const rows = this.childPointers(node).map((child) => {
            const row = this.copyMarkup(child);
            if (row.kind !== "tableRow") throw new Error("table contains a non-row node");
            return row;
        });
        const headers = rows.filter((row) => row.isHeader);
        if (headers.length !== 1) throw new Error(`table contains ${headers.length} header rows`);
        return {
            kind: "table",
            id,
            revision,
            alignments,
            header: headers[0]!,
            rows: rows.filter((row) => !row.isHeader)
        };
    }

    private copyTableRow(node: number, id: MarkupID, revision: number): TableRow {
        return {
            kind: "tableRow",
            id,
            revision,
            isHeader: this.boolean(this.native.es_node_table_row_header(node), "table header state"),
            cells: this.childPointers(node).map((child) => {
                const cell = this.copyMarkup(child);
                if (cell.kind !== "tableCell") throw new Error("table row contains a non-cell node");
                return cell;
            })
        };
    }

    private copyTableCell(node: number, id: MarkupID, revision: number): TableCell {
        return { kind: "tableCell", id, revision, content: this.content(node) };
    }

    private directiveFields(node: number): {
        readonly mode: PlacementMode;
        readonly name: string;
        readonly attributes: string | null;
        readonly label: readonly Markup[] | null;
        readonly content: readonly Markup[];
    } {
        const childPointers = this.childPointers(node);
        const labelCount = this.native.es_node_directive_label_count(node);
        if (!Number.isInteger(labelCount) || labelCount < -1 || labelCount > childPointers.length) {
            throw new Error(`native parser returned an invalid directive label count ${labelCount}`);
        }
        const label = labelCount < 0 ? null : childPointers.slice(0, labelCount).map((child) => this.copyMarkup(child));
        const contentOffset = labelCount < 0 ? 0 : labelCount;
        return {
            mode: this.placement(this.native.es_node_directive_mode(node)),
            name: this.requiredString(node, stringField.directiveName),
            attributes: this.readString(node, stringField.directiveAttributes),
            label,
            content: childPointers.slice(contentOffset).map((child) => this.copyMarkup(child))
        };
    }

    private content(node: number): readonly Markup[] {
        return this.childPointers(node).map((child) => this.copyMarkup(child));
    }

    private childPointers(node: number): number[] {
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

    private scope(node: number): Scope {
        return {
            start: {
                line: this.native.es_scope_coordinate(node, 0),
                column: this.native.es_scope_coordinate(node, 1)
            },
            end: {
                line: this.native.es_scope_coordinate(node, 2),
                column: this.native.es_scope_coordinate(node, 3)
            }
        };
    }

    readString(object: number, field: number): string | null {
        this.requireLive();
        this.native.es_string(object, field, this.scratch, this.scratch + Uint32Array.BYTES_PER_ELEMENT);
        const view = this.dataView();
        const data = view.getUint32(this.scratch, true);
        const length = view.getUint32(this.scratch + Uint32Array.BYTES_PER_ELEMENT, true);
        if (!data) {
            if (length !== 0) throw new Error("native parser returned an invalid string view");
            return null;
        }
        if (length > this.native.memory.buffer.byteLength - data) {
            throw new Error("native parser returned an out-of-bounds string view");
        }
        return this.utf8Decoder.decode(new Uint8Array(this.native.memory.buffer, data, length));
    }

    private requiredString(object: number, field: number): string {
        const value = this.readString(object, field);
        if (value === null) throw new Error("native parser returned a missing string");
        return value;
    }

    private readStart(node: number): number | null {
        this.requireLive();
        if (!this.boolean(this.native.es_node_list_start_state(node, this.scratch), "list start state")) {
            return null;
        }
        const value = Number(this.dataView().getBigInt64(this.scratch, true));
        if (!Number.isSafeInteger(value)) throw new Error("native list start exceeds JavaScript integer precision");
        return value;
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

    dataView(): DataView {
        return new DataView(this.native.memory.buffer);
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
