import type { BlockQuote } from "./model/block-quote.js";
import type { CodeBlock } from "./model/code-block.js";
import type { Code } from "./model/code.js";
import type { DirectiveBlock } from "./model/directive-block.js";
import type { Directive } from "./model/directive.js";
import type { Document } from "./model/document.js";
import type { Emphasis } from "./model/emphasis.js";
import type { FootnoteDefinition, FootnoteReference } from "./model/footnote.js";
import type { FormulaBlock } from "./model/formula-block.js";
import type { Formula } from "./model/formula.js";
import type { Heading } from "./model/heading.js";
import type { HTMLBlock } from "./model/html-block.js";
import type { HTML } from "./model/html.js";
import type { Image } from "./model/image.js";
import type { Link } from "./model/link.js";
import type { List, ListItem } from "./model/list.js";
import type { Markup } from "./model/markup.js";
import type { Paragraph } from "./model/paragraph.js";
import type { Strikethrough } from "./model/strikethrough.js";
import type { Strong } from "./model/strong.js";
import type { Table, TableCell, TableRow } from "./model/table.js";
import type { Text } from "./model/text.js";
import type { Scope } from "./values.js";
import { visit, type MarkupVisitor } from "./markup-visitor.js";
import { MarkupWalker, WalkEvent } from "./markup-walker.js";

/** Produces the canonical diagnostic tree for immutable Markdown markup. */
export class MarkupDumper {
    private constructor() {}

    /**
     * Returns the canonical diagnostic dump for the subtree rooted at
     * `node` (the whole document by default), resolving absolute scopes
     * through the snapshot. A non-document subtree prints scopes with the
     * subtree as origin: the root's start line becomes line 1.
     * Position-free markers (0:0..0:0) print unchanged.
     */
    static dump(document: Document, node: Markup = document): string {
        const origin = document.scope(node).start.line;
        const offset = origin > 0 ? origin - 1 : 0;
        const remainingChildren: number[] = [];
        const lines: string[] = [];

        new MarkupWalker().walk(document, node, (event, current, scope) => {
            if (event === WalkEvent.entering) {
                const record = visit(current, dumpVisitor);
                const line = record.line(rebased(scope, offset));
                if (remainingChildren.length === 0) {
                    lines.push(line);
                } else {
                    const parent = remainingChildren.length - 1;
                    const prefix = remainingChildren
                        .slice(0, -1)
                        .map((remaining) => (remaining > 0 ? "│   " : "    "))
                        .join("");
                    const connector = remainingChildren[parent] === 1 ? "└── " : "├── ";
                    lines.push(prefix + connector + line);
                    remainingChildren[parent] = remainingChildren[parent]! - 1;
                }
                remainingChildren.push(record.children);
            } else {
                if (remainingChildren.pop() !== 0) throw new Error("walker exited before its children");
            }
        });
        return `${lines.join("\n")}\n`;
    }
}

interface PendingRecord {
    readonly line: (scope: string) => string;
    readonly children: number;
}

const dumpVisitor: MarkupVisitor<PendingRecord> = {
    visitDocument: (node: Document) => record("Document", [], node.content.length),
    visitBlockQuote: (node: BlockQuote) => record("BlockQuote", [], node.content.length),
    visitParagraph: (node: Paragraph) => record("Paragraph", [], node.content.length),
    visitHeading: (node: Heading) => record("Heading", [`level=${node.level}`], node.content.length),
    visitThematicBreak: () => record("ThematicBreak"),
    visitList: (node: List) =>
        record(
            "List",
            [`flavor=${node.flavor}`, `start=${node.start ?? "null"}`, `tight=${node.tight}`],
            node.items.length
        ),
    visitListItem: (node: ListItem) => record("ListItem", [`checked=${node.checked ?? "null"}`], node.content.length),
    visitCodeBlock: (node: CodeBlock) =>
        record("CodeBlock", [
            `mode=${node.mode}`,
            `info=${optionalString(node.info)}`,
            `language=${optionalString(node.language)}`,
            `literal=${jsonString(node.literal)}`,
            `fenced=${node.fenced}`,
            `closed=${node.closed}`
        ]),
    visitHTMLBlock: (node: HTMLBlock) => record("HTMLBlock", [`literal=${jsonString(node.literal)}`]),
    visitFormulaBlock: (node: FormulaBlock) =>
        record("FormulaBlock", [`mode=${node.mode}`, `literal=${jsonString(node.literal)}`]),
    visitTable: (node: Table) => record("Table", [`alignments=[${node.alignments.join(",")}]`], 1 + node.rows.length),
    visitTableRow: (node: TableRow) => record("TableRow", [`isHeader=${node.isHeader}`], node.cells.length),
    visitTableCell: (node: TableCell) => record("TableCell", [], node.content.length),
    visitDirectiveBlock: (node: DirectiveBlock) =>
        record(
            "DirectiveBlock",
            directiveFields(node.mode, node.name, node.attributes, node.label?.length ?? null),
            (node.label?.length ?? 0) + node.content.length
        ),
    visitFootnoteDefinition: (node: FootnoteDefinition) =>
        record("FootnoteDefinition", [`id=${jsonString(node.label)}`], node.content.length),
    visitText: (node: Text) => record("Text", [`literal=${jsonString(node.literal)}`]),
    visitSoftBreak: () => record("SoftBreak"),
    visitLineBreak: () => record("LineBreak"),
    visitCode: (node: Code) => record("Code", [`mode=${node.mode}`, `literal=${jsonString(node.literal)}`]),
    visitHTML: (node: HTML) => record("HTML", [`literal=${jsonString(node.literal)}`]),
    visitFormula: (node: Formula) => record("Formula", [`mode=${node.mode}`, `literal=${jsonString(node.literal)}`]),
    visitEmphasis: (node: Emphasis) => record("Emphasis", [], node.content.length),
    visitStrong: (node: Strong) => record("Strong", [], node.content.length),
    visitStrikethrough: (node: Strikethrough) => record("Strikethrough", [], node.content.length),
    visitLink: (node: Link) =>
        record(
            "Link",
            [`destination=${optionalString(node.destination)}`, `title=${optionalString(node.title)}`],
            node.content.length
        ),
    visitImage: (node: Image) =>
        record(
            "Image",
            [`source=${optionalString(node.source)}`, `title=${optionalString(node.title)}`],
            node.content.length
        ),
    visitDirective: (node: Directive) =>
        record(
            "Directive",
            directiveFields(node.mode, node.name, node.attributes, node.label?.length ?? null),
            node.label?.length ?? 0
        ),
    visitFootnoteReference: (node: FootnoteReference) => record("FootnoteReference", [`id=${jsonString(node.label)}`])
};

function record(kind: string, fields: readonly string[] = [], children = 0): PendingRecord {
    const fieldText = fields.length === 0 ? "" : ` ${fields.join(" ")}`;
    return { line: (scope) => `${kind} ${scope}${fieldText} children=${children}`, children };
}

function directiveFields(
    mode: "embedded" | "standalone",
    name: string,
    attributes: string | null,
    labelCount: number | null
): readonly string[] {
    return [
        `mode=${mode}`,
        `name=${jsonString(name)}`,
        `attributes=${optionalString(attributes)}`,
        `label=${labelCount ?? "null"}`
    ];
}

function rebased(value: Scope, offset: number): string {
    const start = value.start.line > 0 ? value.start.line - offset : value.start.line;
    const end = value.end.line > 0 ? value.end.line - offset : value.end.line;
    return `scope=${start}:${value.start.column}..${end}:${value.end.column}`;
}

function optionalString(value: string | null): string {
    return value === null ? "null" : jsonString(value);
}

function jsonString(value: string): string {
    return JSON.stringify(value);
}
