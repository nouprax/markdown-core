import type { BlockQuote } from "./model/block-quote.js";
import type { CodeBlock } from "./model/code-block.js";
import type { Code } from "./model/code.js";
import type { DirectiveAttribute } from "./model/directive-attribute.js";
import type { DirectiveBlock } from "./model/directive-block.js";
import type { DirectiveLabel } from "./model/directive-label.js";
import type { Directive } from "./model/directive.js";
import type { Document } from "./model/document.js";
import type { Emphasis } from "./model/emphasis.js";
import type { FootnoteDefinition, FootnoteReference } from "./model/footnote.js";
import type { ReferenceDefinition } from "./model/reference-definition.js";
import type { ImageReference, LinkReference } from "./model/reference.js";
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
            `info=${optionalString(node.info)}`,
            `language=${optionalString(node.language)}`,
            `literal=${jsonString(node.literal)}`,
            `fenced=${node.fenced}`,
            `closed=${node.closed}`
        ]),
    visitHTMLBlock: (node: HTMLBlock) => record("HTMLBlock", node, [`literal=${jsonString(node.literal)}`]),
    visitFormulaBlock: (node: FormulaBlock) => record("FormulaBlock", node, [`literal=${jsonString(node.literal)}`]),
    visitTable: (node: Table) =>
        record("Table", node, [`alignments=[${node.alignments.join(",")}]`], 1 + node.rows.length),
    visitTableRow: (node: TableRow) => record("TableRow", node, [`isHeader=${node.isHeader}`], node.cells.length),
    visitTableCell: (node: TableCell) => record("TableCell", node, [], node.content.length),
    visitDirectiveBlock: (node: DirectiveBlock) =>
        record(
            "DirectiveBlock",
            node,
            directiveFields(node.name, node.attributes),
            (node.label === null ? 0 : 1) + node.content.length
        ),
    visitDirectiveLabel: (node: DirectiveLabel) => record("DirectiveLabel", node, [], node.content.length),
    visitFootnoteDefinition: (node: FootnoteDefinition) =>
        record("FootnoteDefinition", node, association(node), node.content.length),
    visitReferenceDefinition: (node: ReferenceDefinition) =>
        record("ReferenceDefinition", node, [
            ...association(node),
            `destination=${jsonString(node.destination)}`,
            `title=${optionalString(node.title)}`
        ]),
    visitLinkReference: (node: LinkReference) =>
        record("LinkReference", node, [...association(node), `form=${node.form}`], node.content.length),
    visitImageReference: (node: ImageReference) =>
        record("ImageReference", node, [...association(node), `form=${node.form}`], node.content.length),
    visitText: (node: Text) => record("Text", node, [`literal=${jsonString(node.literal)}`]),
    visitSoftBreak: (node: SoftBreak) => record("SoftBreak", node),
    visitLineBreak: (node: LineBreak) => record("LineBreak", node),
    visitCode: (node: Code) => record("Code", node, [`literal=${jsonString(node.literal)}`]),
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
            [`destination=${jsonString(node.destination)}`, `title=${optionalString(node.title)}`],
            node.content.length
        ),
    visitImage: (node: Image) =>
        record(
            "Image",
            node,
            [`source=${jsonString(node.source)}`, `title=${optionalString(node.title)}`],
            node.content.length
        ),
    visitDirective: (node: Directive) =>
        record("Directive", node, directiveFields(node.name, node.attributes), node.label === null ? 0 : 1),
    visitFootnoteReference: (node: FootnoteReference) => record("FootnoteReference", node, association(node))
};

/** The two fields five kinds carry identically, in contract order. */
function association(node: { readonly label: string; readonly identifier: string }): readonly string[] {
    return [`label=${jsonString(node.label)}`, `identifier=${jsonString(node.identifier)}`];
}

function record(kind: string, node: Markup, fields: readonly string[] = [], children = 0): DumpRecord {
    const fieldText = fields.length === 0 ? "" : ` ${fields.join(" ")}`;
    return { line: `${kind} ${scope(node.scope)}${fieldText} children=${children}`, children };
}

function directiveFields(name: string, attributes: readonly DirectiveAttribute[] | null): readonly string[] {
    if (attributes === null) return [`name=${jsonString(name)}`, "attributes=null"];
    const pairs = attributes.map((pair) => `${pair.name}=${jsonString(pair.value)}`).join(" ");
    return [`name=${jsonString(name)}`, `attributes=[${pairs}]`];
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
