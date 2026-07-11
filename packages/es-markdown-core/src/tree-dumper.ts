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
import type { LineBreak } from "./model/line-break.js";
import type { Link } from "./model/link.js";
import type { List, ListItem } from "./model/list.js";
import type { Markup } from "./model/markup.js";
import type { Paragraph } from "./model/paragraph.js";
import type { SoftBreak } from "./model/soft-break.js";
import type { Strikethrough } from "./model/strikethrough.js";
import type { Strong } from "./model/strong.js";
import type { Table, TableCell, TableRow } from "./model/table.js";
import type { Text } from "./model/text.js";
import type { ThematicBreak } from "./model/thematic-break.js";
import type { Scope } from "./values.js";
import { visit, type Visitor } from "./visitor.js";
import { Walker, WalkEvent } from "./walker.js";

interface DumpRecord {
    readonly line: string;
    readonly children: number;
}

/** Produces the canonical diagnostic tree for immutable Markdown markup. */
export class TreeDumper {
    private constructor() {}

    /** Returns the canonical diagnostic dump for `root` and its descendants. */
    static dump(root: Markup): string {
        const remainingChildren: number[] = [];
        const lines: string[] = [];

        new Walker().walk(root, (event, node) => {
            if (event === WalkEvent.entering) {
                const dump = visit(node, dumpVisitor);
                if (remainingChildren.length === 0) {
                    lines.push(dump.line);
                } else {
                    const parent = remainingChildren.length - 1;
                    const prefix = remainingChildren
                        .slice(0, -1)
                        .map((remaining) => (remaining > 0 ? "│   " : "    "))
                        .join("");
                    const connector = remainingChildren[parent] === 1 ? "└── " : "├── ";
                    lines.push(prefix + connector + dump.line);
                    remainingChildren[parent] = remainingChildren[parent]! - 1;
                }
                remainingChildren.push(dump.children);
            } else {
                if (remainingChildren.pop() !== 0) throw new Error("walker exited before its children");
            }
        });
        return `${lines.join("\n")}\n`;
    }
}

const dumpVisitor: Visitor<DumpRecord> = {
    visitDocument: (node: Document) => record("Document", node, [], node.content.length),
    visitBlockQuote: (node: BlockQuote) => record("BlockQuote", node, [], node.content.length),
    visitParagraph: (node: Paragraph) => record("Paragraph", node, [], node.content.length),
    visitHeading: (node: Heading) => record("Heading", node, [`level=${node.level}`], node.content.length),
    visitThematicBreak: (node: ThematicBreak) => record("ThematicBreak", node),
    visitList: (node: List) =>
        record(
            "List",
            node,
            [`flavor=${node.flavor}`, `start=${node.start ?? "null"}`, `tight=${node.tight}`],
            node.items.length
        ),
    visitListItem: (node: ListItem) =>
        record("ListItem", node, [`checked=${node.checked ?? "null"}`], node.content.length),
    visitCodeBlock: (node: CodeBlock) =>
        record("CodeBlock", node, [
            `mode=${node.mode}`,
            `info=${optionalString(node.info)}`,
            `language=${optionalString(node.language)}`,
            `literal=${jsonString(node.literal)}`,
            `fenced=${node.fenced}`,
            `closed=${node.closed}`
        ]),
    visitHTMLBlock: (node: HTMLBlock) => record("HTMLBlock", node, [`literal=${jsonString(node.literal)}`]),
    visitFormulaBlock: (node: FormulaBlock) =>
        record("FormulaBlock", node, [`mode=${node.mode}`, `literal=${jsonString(node.literal)}`]),
    visitTable: (node: Table) =>
        record("Table", node, [`alignments=[${node.alignments.join(",")}]`], 1 + node.rows.length),
    visitTableRow: (node: TableRow) => record("TableRow", node, [`isHeader=${node.isHeader}`], node.cells.length),
    visitTableCell: (node: TableCell) => record("TableCell", node, [], node.content.length),
    visitDirectiveBlock: (node: DirectiveBlock) =>
        record(
            "DirectiveBlock",
            node,
            directiveFields(node.mode, node.name, node.attributes, node.label?.length ?? null),
            (node.label?.length ?? 0) + node.content.length
        ),
    visitFootnoteDefinition: (node: FootnoteDefinition) =>
        record("FootnoteDefinition", node, [`id=${jsonString(node.id)}`], node.content.length),
    visitText: (node: Text) => record("Text", node, [`literal=${jsonString(node.literal)}`]),
    visitSoftBreak: (node: SoftBreak) => record("SoftBreak", node),
    visitLineBreak: (node: LineBreak) => record("LineBreak", node),
    visitCode: (node: Code) => record("Code", node, [`mode=${node.mode}`, `literal=${jsonString(node.literal)}`]),
    visitHTML: (node: HTML) => record("HTML", node, [`literal=${jsonString(node.literal)}`]),
    visitFormula: (node: Formula) =>
        record("Formula", node, [`mode=${node.mode}`, `literal=${jsonString(node.literal)}`]),
    visitEmphasis: (node: Emphasis) => record("Emphasis", node, [], node.content.length),
    visitStrong: (node: Strong) => record("Strong", node, [], node.content.length),
    visitStrikethrough: (node: Strikethrough) => record("Strikethrough", node, [], node.content.length),
    visitLink: (node: Link) =>
        record(
            "Link",
            node,
            [`destination=${optionalString(node.destination)}`, `title=${optionalString(node.title)}`],
            node.content.length
        ),
    visitImage: (node: Image) =>
        record(
            "Image",
            node,
            [`source=${optionalString(node.source)}`, `title=${optionalString(node.title)}`],
            node.content.length
        ),
    visitDirective: (node: Directive) =>
        record(
            "Directive",
            node,
            directiveFields(node.mode, node.name, node.attributes, node.label?.length ?? null),
            node.label?.length ?? 0
        ),
    visitFootnoteReference: (node: FootnoteReference) =>
        record("FootnoteReference", node, [`id=${jsonString(node.id)}`])
};

function record(kind: string, node: Markup, fields: readonly string[] = [], children = 0): DumpRecord {
    const fieldText = fields.length === 0 ? "" : ` ${fields.join(" ")}`;
    return { line: `${kind} ${scope(node.scope)}${fieldText} children=${children}`, children };
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

function scope(value: Scope): string {
    return `scope=${value.start.line}:${value.start.column}..${value.end.line}:${value.end.column}`;
}

function optionalString(value: string | null): string {
    return value === null ? "null" : jsonString(value);
}

function jsonString(value: string): string {
    return JSON.stringify(value);
}
