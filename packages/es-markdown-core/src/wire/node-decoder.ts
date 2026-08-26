import type { MarkupBase } from "../model/base.js";
import type { DirectiveAttribute } from "../model/directive-attribute.js";
import type { DirectiveLabel } from "../model/directive-label.js";
import type { Semantic } from "../model/semantic.js";
import type { Markup } from "../model/markup.js";
import type { TableCell, TableRow } from "../model/table.js";
import { ParseError, type ParseErrorCode } from "../parse-error.js";
import type { ListFlavor, PlacementMode, ReferenceForm, Scope, TableAlignment } from "../values.js";
import type { NativeExports } from "../runtime/native.js";
import { TreeDumper } from "../tree-dumper.js";
import { kinds, type NativeKind } from "./kinds.js";

/* THE THREE MEMBERS A DECODED VALUE DOES NOT CARRY. `dump` is defined on every
 * node after the copy, and `concrete` and `ownerOf` on the ROOT once the
 * concrete view has been read -- which is after the tree, and out of the
 * decoder's reach. */
type MarkupValue = Markup extends infer Node
    ? Node extends Markup
        ? Omit<Node, "dump" | "concrete" | "ownerOf">
        : never
    : never;
type MarkupValueOf<Kind extends Markup["kind"]> = Extract<MarkupValue, { readonly kind: Kind }>;

interface DirectiveFields {
    readonly name: string;
    readonly attributes: readonly DirectiveAttribute[] | null;
    readonly label: DirectiveLabel | null;
    readonly content: readonly Markup[];
}

const stringField = {
    codeInfo: 1,
    codeLanguage: 2,
    codeLiteral: 3,
    literal: 4,
    formulaLiteral: 5,
    directiveName: 6,
    directiveAttributeName: 7,
    directiveAttributeValue: 8,
    linkDestination: 9,
    linkTitle: 10,
    imageSource: 11,
    imageTitle: 12,
    errorMessage: 14,
    definitionDestination: 15,
    definitionTitle: 16,
    associationLabel: 17,
    associationIdentifier: 18
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

    /** The root of the semantic tree; the concrete view travels beside it in the `Read`. */
    decodeSemantic(node: number): Semantic {
        const semantic = this.copyMarkup(node);
        if (semantic.kind !== "document") {
            throw new ParseError("internal", "parser returned an invalid document tree");
        }
        return semantic;
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
                    info: this.readString(node, stringField.codeInfo),
                    language: this.readString(node, stringField.codeLanguage),
                    literal: this.requiredString(node, stringField.codeLiteral),
                    fenced: this.boolean(this.native.es_node_code_flag(node, 0), "code fenced state"),
                    closed: this.boolean(this.native.es_node_code_flag(node, 1), "code closed state")
                };
            case "htmlBlock":
                return { ...this.base(node, kind), literal: this.requiredString(node, stringField.literal) };
            case "formulaBlock":
                // No `mode`: a formula block is always standalone -- the engine
                // refuses any other value for this kind -- so the assertion that
                // used to guard the constant guarded nothing (Q29).
                return {
                    ...this.base(node, kind),
                    literal: this.requiredString(node, stringField.formulaLiteral)
                };
            case "table":
                return this.copyTable(node);
            case "directiveBlock":
                return { ...this.base(node, kind), ...this.directiveFields(node) };
            case "footnoteDefinition":
                return {
                    ...this.base(node, kind),
                    ...this.association(node),
                    content: this.content(node)
                };
            case "referenceDefinition":
                return {
                    ...this.base(node, kind),
                    ...this.association(node),
                    destination: this.requiredString(node, stringField.definitionDestination),
                    title: this.readString(node, stringField.definitionTitle)
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
                    destination: this.requiredString(node, stringField.linkDestination),
                    title: this.readString(node, stringField.linkTitle),
                    content: this.content(node)
                };
            case "image":
                return {
                    ...this.base(node, kind),
                    source: this.requiredString(node, stringField.imageSource),
                    title: this.readString(node, stringField.imageTitle),
                    content: this.content(node)
                };
            case "directive": {
                const fields = this.directiveFields(node);
                if (fields.content.length !== 0) throw new Error("inline directive contains block content");
                return {
                    ...this.base(node, kind),
                    name: fields.name,
                    attributes: fields.attributes,
                    label: fields.label
                };
            }
            case "footnoteReference":
                return { ...this.base(node, kind), ...this.association(node) };
            case "linkReference":
            case "imageReference":
                return {
                    ...this.base(node, kind),
                    ...this.association(node),
                    form: this.referenceForm(this.native.es_node_reference_form(node)),
                    content: this.content(node)
                };
            case "tableRow":
                return this.copyTableRow(node);
            case "tableCell":
                return this.copyTableCell(node);
            case "directiveLabel":
                return { ...this.base(node, kind), content: this.content(node) };
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
        // The label is the first child when it is there at all. Until Step 7
        // the C facade spliced the node out and named its count instead, and
        // this had to slice the run out of a flat child list.
        const first = childPointers.length > 0 ? this.copyMarkup(childPointers[0]!) : null;
        const label = first !== null && first.kind === "directiveLabel" ? first : null;
        return {
            name: this.requiredString(node, stringField.directiveName),
            attributes: this.directiveAttributes(node),
            label,
            content: childPointers.slice(label === null ? 0 : 1).map((child) => this.copyMarkup(child))
        };
    }

    private directiveAttributes(node: number): readonly DirectiveAttribute[] | null {
        const count = this.native.es_node_directive_attribute_count(node);
        if (!Number.isInteger(count) || count < -1) {
            throw new Error(`native parser returned an invalid directive attribute count ${String(count)}`);
        }
        if (count < 0) return null;
        const attributes: DirectiveAttribute[] = [];
        for (let index = 0; index < count; index++) {
            this.native.es_set_attribute_index(index);
            attributes.push({
                name: this.requiredString(node, stringField.directiveAttributeName),
                value: this.requiredString(node, stringField.directiveAttributeValue)
            });
        }
        return Object.freeze(attributes);
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
        // PRESENCE IS THE RETURN VALUE. It used to be read off the pointer,
        // which gave one channel for the two answers requirement 14 separates.
        // A raw wasm export answers with 0 or 1, not with a JS boolean.
        const present =
            this.native.es_string(object, field, this.scratch, this.scratch + Uint32Array.BYTES_PER_ELEMENT) !== 0;
        const memory = this.dataView();
        const data = memory.getUint32(this.scratch, true);
        const length = memory.getUint32(this.scratch + Uint32Array.BYTES_PER_ELEMENT, true);
        if (!present) {
            if (data !== 0 || length !== 0) throw new Error("native parser returned an invalid string");
            return null;
        }
        if (!data) {
            if (length !== 0) throw new Error("native parser returned an invalid string");
            return "";
        }
        if (length > this.native.memory.buffer.byteLength - data) {
            throw new Error("native parser returned an out-of-bounds string");
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

    /** The two halves of an association, which five kinds carry identically. */
    private association(node: number): { readonly label: string; readonly identifier: string } {
        return {
            label: this.requiredString(node, stringField.associationLabel),
            identifier: this.requiredString(node, stringField.associationIdentifier)
        };
    }

    private referenceForm(rawValue: number): ReferenceForm {
        if (rawValue === 1) return "full";
        if (rawValue === 2) return "collapsed";
        if (rawValue === 3) return "shortcut";
        throw new Error(`native parser returned invalid reference form ${rawValue}`);
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
