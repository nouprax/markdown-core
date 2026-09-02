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
import type { ReferenceDefinition } from "./model/reference-definition.js";
import type { ImageReference, LinkReference } from "./model/reference.js";
import type { SoftBreak } from "./model/soft-break.js";
import type { Strikethrough } from "./model/strikethrough.js";
import type { Strong } from "./model/strong.js";
import type { Table, TableCell, TableRow } from "./model/table.js";
import type { Text } from "./model/text.js";
import type { ThematicBreak } from "./model/thematic-break.js";
import type { Scope } from "./values.js";
import { visit, type Visitor } from "./visitor.js";

/** Produces the canonical debug tree for immutable Markdown markup. */
export class TreeDumper {
    private constructor() {}

    /** Returns the canonical debug dump for `root` and its owned markup. */
    static dump(root: Markup): string {
        const state = new DumpState();
        state.dump(root);
        return state.result();
    }
}

class DumpState {
    private readonly remainingNodes: number[] = [];
    private readonly lines: string[] = [];

    /** Each callback emits exactly its node and chooses its children/fields. */
    private readonly visitor: Visitor<void> = {
        visitDocument: (node: Document) => this.container("Document", node, [], node.content),
        visitBlockQuote: (node: BlockQuote) => this.container("BlockQuote", node, [], node.content),
        visitParagraph: (node: Paragraph) => this.container("Paragraph", node, [], node.content),
        visitHeading: (node: Heading) => this.container("Heading", node, [`level=${node.level}`], node.content),
        visitThematicBreak: (node: ThematicBreak) => this.line("ThematicBreak", node),
        visitList: (node: List) =>
            this.container(
                "List",
                node,
                [`flavor=${node.flavor}`, `start=${node.start ?? "null"}`, `tight=${node.tight}`],
                node.items
            ),
        visitListItem: (node: ListItem) =>
            this.container("ListItem", node, [`checked=${node.checked ?? "null"}`], node.content),
        visitCodeBlock: (node: CodeBlock) =>
            this.line("CodeBlock", node, [
                `info=${optionalString(node.info)}`,
                `language=${optionalString(node.language)}`,
                `literal=${jsonString(node.literal)}`,
                `fenced=${node.fenced}`,
                `closed=${node.closed}`
            ]),
        visitHTMLBlock: (node: HTMLBlock) => this.line("HTMLBlock", node, [`literal=${jsonString(node.literal)}`]),
        visitFormulaBlock: (node: FormulaBlock) =>
            this.line("FormulaBlock", node, [`literal=${jsonString(node.literal)}`]),
        visitTable: (node: Table) => {
            const children = [node.header, ...node.rows];
            this.container("Table", node, [`alignments=[${node.alignments.join(",")}]`], children);
        },
        visitTableRow: (node: TableRow) => this.container("TableRow", node, [`isHeader=${node.isHeader}`], node.cells),
        visitTableCell: (node: TableCell) => this.container("TableCell", node, [], node.content),
        visitDirectiveBlock: (node: DirectiveBlock) => {
            this.line("DirectiveBlock", node, directiveFields(node.name, node.attributes), node.content.length);
            this.nested(node.content.length + (node.label === null ? 0 : 1), () => {
                if (node.label !== null) this.dump(node.label);
                for (const child of node.content) this.dump(child);
            });
        },
        visitDirectiveLabel: (node: DirectiveLabel) => this.container("DirectiveLabel", node, [], node.content),
        visitFootnoteDefinition: (node: FootnoteDefinition) =>
            this.container("FootnoteDefinition", node, association(node), node.content),
        visitReferenceDefinition: (node: ReferenceDefinition) =>
            this.line("ReferenceDefinition", node, [
                ...association(node),
                `destination=${jsonString(node.destination)}`,
                `title=${optionalString(node.title)}`
            ]),
        visitLinkReference: (node: LinkReference) =>
            this.container("LinkReference", node, [...association(node), `form=${node.form}`], node.content),
        visitImageReference: (node: ImageReference) =>
            this.container("ImageReference", node, [...association(node), `form=${node.form}`], node.content),
        visitText: (node: Text) => this.line("Text", node, [`literal=${jsonString(node.literal)}`]),
        visitSoftBreak: (node: SoftBreak) => this.line("SoftBreak", node),
        visitLineBreak: (node: LineBreak) => this.line("LineBreak", node),
        visitCode: (node: Code) => this.line("Code", node, [`literal=${jsonString(node.literal)}`]),
        visitHTML: (node: HTML) => this.line("HTML", node, [`literal=${jsonString(node.literal)}`]),
        visitFormula: (node: Formula) =>
            this.line("Formula", node, [`mode=${node.mode}`, `literal=${jsonString(node.literal)}`]),
        visitEmphasis: (node: Emphasis) => this.container("Emphasis", node, [], node.content),
        visitStrong: (node: Strong) => this.container("Strong", node, [], node.content),
        visitStrikethrough: (node: Strikethrough) => this.container("Strikethrough", node, [], node.content),
        visitLink: (node: Link) =>
            this.container(
                "Link",
                node,
                [`destination=${jsonString(node.destination)}`, `title=${optionalString(node.title)}`],
                node.content
            ),
        visitImage: (node: Image) =>
            this.container(
                "Image",
                node,
                [`source=${jsonString(node.source)}`, `title=${optionalString(node.title)}`],
                node.content
            ),
        visitDirective: (node: Directive) => {
            this.line("Directive", node, directiveFields(node.name, node.attributes));
            this.nested(node.label === null ? 0 : 1, () => {
                if (node.label !== null) this.dump(node.label);
            });
        },
        visitFootnoteReference: (node: FootnoteReference) => this.line("FootnoteReference", node, association(node))
    };

    dump(node: Markup): void {
        visit(node, this.visitor);
    }

    result(): string {
        return `${this.lines.join("\n")}\n`;
    }

    private container(kind: string, node: Markup, fields: readonly string[], children: readonly Markup[]): void {
        this.line(kind, node, fields, children.length);
        this.nested(children.length, () => {
            for (const child of children) this.dump(child);
        });
    }

    private line(kind: string, node: Markup, fields: readonly string[] = [], children = 0): void {
        const fieldText = fields.length === 0 ? "" : ` ${fields.join(" ")}`;
        const text = `${kind} ${scope(node.scope)}${fieldText} children=${children}`;
        if (this.remainingNodes.length === 0) {
            this.lines.push(text);
            return;
        }

        const parent = this.remainingNodes.length - 1;
        const prefix = this.remainingNodes
            .slice(0, -1)
            .map((remaining) => (remaining > 0 ? "│   " : "    "))
            .join("");
        const connector = this.remainingNodes[parent] === 1 ? "└── " : "├── ";
        this.lines.push(prefix + connector + text);
        this.remainingNodes[parent] = this.remainingNodes[parent]! - 1;
    }

    private nested(count: number, body: () => void): void {
        this.remainingNodes.push(count);
        body();
        if (this.remainingNodes.pop() !== 0) throw new Error("node dumper did not emit every owned node");
    }
}

function association(node: { readonly label: string; readonly identifier: string }): readonly string[] {
    return [`label=${jsonString(node.label)}`, `identifier=${jsonString(node.identifier)}`];
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
