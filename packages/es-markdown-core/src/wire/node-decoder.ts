import type { MarkupBase } from "../model/base.js";
import type { Document } from "../model/document.js";
import type { Markup } from "../model/markup.js";
import type { TableCell, TableRow } from "../model/table.js";
import { ParseError, type ParseErrorCode } from "../parse-error.js";
import type { ListFlavor, PlacementMode, Scope, TableAlignment } from "../values.js";
import type { NativeExports } from "../runtime/native.js";
import { TreeDumper } from "../tree-dumper.js";
import { kinds, type NativeKind } from "./kinds.js";

type MarkupValue = Markup extends infer Node ? (Node extends Markup ? Omit<Node, "dump"> : never) : never;
type MarkupValueOf<Kind extends Markup["kind"]> = Extract<MarkupValue, { readonly kind: Kind }>;

interface DirectiveFields {
    readonly mode: PlacementMode;
    readonly name: string;
    readonly attributes: string | null;
    readonly label: readonly Markup[] | null;
    readonly content: readonly Markup[];
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
    footnoteID: 12,
    errorMessage: 13
} as const;

export class NodeDecoder {
    private scratch: number;
    private readonly utf8Decoder = new TextDecoder("utf-8", { fatal: false });

    constructor(private readonly native: NativeExports) {
        this.scratch = native.malloc(2 * Uint32Array.BYTES_PER_ELEMENT);
        if (!this.scratch) throw new ParseError("allocationFailed", "failed to allocate WASM memory");
    }

    dispose(): void {
        if (!this.scratch) return;
        this.native.free(this.scratch);
        this.scratch = 0;
    }

    decodeDocument(node: number): Document {
        const document = this.copyMarkup(node);
        if (document.kind !== "document") {
            throw new ParseError("internal", "parser returned an invalid document tree");
        }
        return document;
    }

    parseError(error: number): ParseError {
        if (!error) return new ParseError("internal", "markdown parsing failed");
        const code = errorCode(this.native.es_error_code(error));
        return new ParseError(code, this.readString(error, stringField.errorMessage) ?? "markdown parsing failed");
    }

    private copyMarkup(node: number): Markup {
        const value = this.copyMarkupValue(node);
        Object.defineProperty(value, "dump", {
            enumerable: false,
            value(this: Markup): string {
                return TreeDumper.dump(this);
            }
        });
        return value as Markup;
    }

    private copyMarkupValue(node: number): MarkupValue {
        const kind = this.kind(node);
        switch (kind) {
            case "document":
                return { ...this.base(node, kind), content: this.content(node) };
            case "blockQuote":
                return { ...this.base(node, kind), content: this.content(node) };
            case "paragraph":
                return { ...this.base(node, kind), content: this.content(node) };
            case "heading": {
                const level = this.native.es_node_heading_level(node);
                if (!Number.isInteger(level) || level < 1 || level > 6) {
                    throw new Error(`native parser returned an invalid heading level ${level}`);
                }
                return { ...this.base(node, kind), level, content: this.content(node) };
            }
            case "thematicBreak":
                return this.base(node, kind);
            case "list":
                return this.copyList(node);
            case "listItem":
                return {
                    ...this.base(node, kind),
                    checked: this.nullableBoolean(this.native.es_node_checked(node), "list item checked state"),
                    content: this.content(node)
                };
            case "codeBlock":
                return {
                    ...this.base(node, kind),
                    mode: "standalone",
                    info: this.readString(node, stringField.codeInfo),
                    language: this.readString(node, stringField.codeLanguage),
                    literal: this.requiredString(node, stringField.codeLiteral),
                    fenced: this.boolean(this.native.es_node_code_flag(node, 0), "code fenced state"),
                    closed: this.boolean(this.native.es_node_code_flag(node, 1), "code closed state")
                };
            case "htmlBlock":
                return { ...this.base(node, kind), literal: this.requiredString(node, stringField.literal) };
            case "formulaBlock": {
                const mode = this.placement(this.native.es_node_formula_mode(node));
                if (mode !== "standalone") throw new Error("native parser returned an embedded formula block");
                return {
                    ...this.base(node, kind),
                    mode,
                    literal: this.requiredString(node, stringField.formulaLiteral)
                };
            }
            case "table":
                return this.copyTable(node);
            case "directiveBlock": {
                const fields = this.directiveFields(node);
                if (fields.mode !== "standalone") {
                    throw new Error("native parser returned an embedded directive block");
                }
                return { ...this.base(node, kind), ...fields };
            }
            case "footnoteDefinition":
                return {
                    ...this.base(node, kind),
                    id: this.requiredString(node, stringField.footnoteID),
                    content: this.content(node)
                };
            case "text":
                return { ...this.base(node, kind), literal: this.requiredString(node, stringField.literal) };
            case "softBreak":
                return this.base(node, kind);
            case "lineBreak":
                return this.base(node, kind);
            case "code":
                return {
                    ...this.base(node, kind),
                    mode: "embedded",
                    literal: this.requiredString(node, stringField.literal)
                };
            case "html":
                return { ...this.base(node, kind), literal: this.requiredString(node, stringField.literal) };
            case "formula":
                return {
                    ...this.base(node, kind),
                    mode: this.placement(this.native.es_node_formula_mode(node)),
                    literal: this.requiredString(node, stringField.formulaLiteral)
                };
            case "emphasis":
                return { ...this.base(node, kind), content: this.content(node) };
            case "strong":
                return { ...this.base(node, kind), content: this.content(node) };
            case "strikethrough":
                return { ...this.base(node, kind), content: this.content(node) };
            case "link":
                return {
                    ...this.base(node, kind),
                    destination: this.readString(node, stringField.linkDestination),
                    title: this.readString(node, stringField.linkTitle),
                    content: this.content(node)
                };
            case "image":
                return {
                    ...this.base(node, kind),
                    source: this.readString(node, stringField.imageSource),
                    title: this.readString(node, stringField.imageTitle),
                    content: this.content(node)
                };
            case "directive": {
                const fields = this.directiveFields(node);
                if (fields.mode !== "embedded") throw new Error("native parser returned a standalone directive");
                if (fields.content.length !== 0) throw new Error("inline directive contains block content");
                return {
                    ...this.base(node, kind),
                    mode: fields.mode,
                    name: fields.name,
                    attributes: fields.attributes,
                    label: fields.label
                };
            }
            case "footnoteReference":
                return { ...this.base(node, kind), id: this.requiredString(node, stringField.footnoteID) };
            case "tableRow":
                return this.copyTableRow(node);
            case "tableCell":
                return this.copyTableCell(node);
        }
        return unreachable(kind);
    }

    private copyList(node: number): MarkupValueOf<"list"> {
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
            ...this.base(node, "list"),
            flavor,
            start,
            tight: this.boolean(this.native.es_node_list_tight(node), "list tight state"),
            items
        };
    }

    private copyTable(node: number): MarkupValueOf<"table"> {
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
            ...this.base(node, "table"),
            alignments,
            header: headers[0]!,
            rows: rows.filter((row) => !row.isHeader)
        };
    }

    private copyTableRow(node: number): Omit<TableRow, "dump"> {
        if (this.kind(node) !== "tableRow") throw new Error("table contains a non-row node");
        return {
            ...this.base(node, "tableRow"),
            isHeader: this.boolean(this.native.es_node_table_row_header(node), "table header state"),
            cells: this.childPointers(node).map((child) => {
                const cell = this.copyMarkup(child);
                if (cell.kind !== "tableCell") throw new Error("table row contains a non-cell node");
                return cell;
            })
        };
    }

    private copyTableCell(node: number): Omit<TableCell, "dump"> {
        if (this.kind(node) !== "tableCell") throw new Error("table row contains a non-cell node");
        return { ...this.base(node, "tableCell"), content: this.content(node) };
    }

    private directiveFields(node: number): DirectiveFields {
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

    private base<Kind extends Markup["kind"]>(node: number, kind: Kind): Omit<MarkupBase<Kind>, "dump"> {
        return { kind, scope: this.scope(node) };
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

    private readString(object: number, field: number): string | null {
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

    private dataView(): DataView {
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
