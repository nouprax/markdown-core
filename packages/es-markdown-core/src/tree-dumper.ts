import type { Attributes, Metadata, MetadataValue } from "./values.js";
import type { Callout } from "./model/callout.js";
import type { Citation, Cite } from "./model/cite.js";
import type { CodeBlock } from "./model/code-block.js";
import type { Code } from "./model/code.js";
import type { Comment } from "./model/comment.js";
import type { CrossLink } from "./model/cross-link.js";
import type { DirectiveBlock } from "./model/directive-block.js";
import type { DirectiveLabel } from "./model/directive-label.js";
import type { Directive } from "./model/directive.js";
import type { Document } from "./model/document.js";
import type { Emphasis } from "./model/emphasis.js";
import type { Specimen } from "./model/specimen.js";
import type { Footnote } from "./model/footnote.js";
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
import type { Mark } from "./model/mark.js";
import type { Strong } from "./model/strong.js";
import type { Table, TableCell, TableRow } from "./model/table.js";
import type { Text } from "./model/text.js";
import type { ThematicBreak } from "./model/thematic-break.js";
import type { CitationReferent, Destination, OrderedListDelimiter, OrderedListVariant, Scope } from "./values.js";
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
        visitDocument: (node: Document) => {
            // The footnotes are value lines after the content, never counted
            // by the document's own `children`.
            this.line("Document", node, [], node.content.length);
            this.nested(
                node.content.length + node.footnotes.length + node.specimens.length + (node.metadata === null ? 0 : 1),
                () => {
                    if (node.metadata !== null) this.metadata(node.metadata);
                    for (const child of node.content) this.dump(child);
                    for (const footnote of node.footnotes) this.footnote(footnote);
                    for (const specimen of node.specimens) this.specimen(specimen);
                }
            );
        },
        visitCallout: (node: Callout) => {
            this.line(
                "Callout",
                node,
                [`variant=${optionalString(node.variant)}`, `collapsed=${node.collapsed ?? "null"}`],
                node.content.length
            );
            // A non-null title is a `Title` group before the content; a null
            // one prints nothing. Neither is counted by `children`.
            this.nested(node.content.length + (node.title === null ? 0 : 1), () => {
                if (node.title !== null) {
                    this.group("Title", node.title.length);
                    this.nested(node.title.length, () => {
                        for (const child of node.title ?? []) this.dump(child);
                    });
                }
                for (const child of node.content) this.dump(child);
            });
        },
        visitParagraph: (node: Paragraph) => this.container("Paragraph", node, [], node.content),
        visitHeading: (node: Heading) => this.container("Heading", node, [`level=${node.level}`], node.content),
        visitThematicBreak: (node: ThematicBreak) => this.line("ThematicBreak", node),
        visitList: (node: List) =>
            this.container(
                "List",
                node,
                [
                    `flavor=${node.flavor}`,
                    `start=${node.start ?? "null"}`,
                    `variant=${orderedListVariant(node.variant)}`,
                    `delimiter=${orderedListDelimiter(node.delimiter)}`,
                    `tight=${node.tight}`
                ],
                node.items
            ),
        visitListItem: (node: ListItem) =>
            this.container("ListItem", node, [`marker=${optionalString(node.marker)}`], node.content),
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
            const columns = node.columns.map((column) => `${column.alignment}:${column.relative ?? "null"}`).join(",");
            this.line(
                "Table",
                node,
                [`columns=[${columns}]`],
                node.head.length + node.content.length + node.foot.length
            );
            this.nested(3, () => {
                for (const [name, rows] of [
                    ["TableHead", node.head],
                    ["TableBody", node.content],
                    ["TableFoot", node.foot]
                ] as const) {
                    this.group(name, rows.length);
                    this.nested(rows.length, () => {
                        for (const row of rows) this.dump(row);
                    });
                }
            });
        },
        visitTableRow: (node: TableRow) => this.container("TableRow", node, [], node.cells),
        visitTableCell: (node: TableCell) =>
            this.container("TableCell", node, [`rowspan=${node.rowspan}`, `colspan=${node.colspan}`], node.content),
        visitDirectiveBlock: (node: DirectiveBlock) => {
            this.line("DirectiveBlock", node, [`name=${jsonString(node.name)}`], node.content.length);
            this.nested(node.content.length + (node.label === null ? 0 : 1), () => {
                if (node.label !== null) this.dump(node.label);
                for (const child of node.content) this.dump(child);
            });
        },
        visitDirectiveLabel: (node: DirectiveLabel) => this.container("DirectiveLabel", node, [], node.content),
        visitText: (node: Text) => this.line("Text", node, [`literal=${jsonString(node.literal)}`]),
        visitSoftBreak: (node: SoftBreak) => this.line("SoftBreak", node),
        visitLineBreak: (node: LineBreak) => this.line("LineBreak", node),
        visitCode: (node: Code) => this.line("Code", node, [`literal=${jsonString(node.literal)}`]),
        visitHTML: (node: HTML) => this.line("HTML", node, [`literal=${jsonString(node.literal)}`]),
        visitCrossLink: (node: CrossLink) =>
            this.line("CrossLink", node, [
                `embedded=${String(node.embedded)}`,
                `dest=${destination(node.dest)}`,
                `label=${optionalString(node.label)}`
            ]),
        visitComment: (node: Comment) => this.line("Comment", node, [`literal=${jsonString(node.literal)}`]),
        visitFormula: (node: Formula) =>
            this.line("Formula", node, [`mode=${node.mode}`, `literal=${jsonString(node.literal)}`]),
        visitEmphasis: (node: Emphasis) => this.container("Emphasis", node, [], node.content),
        visitStrong: (node: Strong) => this.container("Strong", node, [], node.content),
        visitStrikethrough: (node: Strikethrough) => this.container("Strikethrough", node, [], node.content),
        visitMark: (node: Mark) => this.container("Mark", node, [], node.content),
        visitLink: (node: Link) =>
            this.container(
                "Link",
                node,
                [`dest=${destination(node.dest)}`, `title=${optionalString(node.title)}`],
                node.content
            ),
        visitImage: (node: Image) =>
            this.container(
                "Image",
                node,
                [
                    `dest=${destination(node.dest)}`,
                    `title=${optionalString(node.title)}`,
                    `width=${node.width ?? "null"}`,
                    `height=${node.height ?? "null"}`
                ],
                node.content
            ),
        visitDirective: (node: Directive) => {
            this.line("Directive", node, [`name=${jsonString(node.name)}`]);
            this.nested(node.label === null ? 0 : 1, () => {
                if (node.label !== null) this.dump(node.label);
            });
        },
        visitCite: (node: Cite) => {
            // Each item is a value line whose affixes are groups; the cite's
            // own `children` counts the items.
            this.line("Cite", node, [], node.citations.length);
            this.nested(node.citations.length, () => {
                for (const item of node.citations) this.citation(item);
            });
        }
    };

    dump(node: Markup): void {
        visit(node, this.visitor);
    }

    result(): string {
        return `${this.lines.join("\n")}\n`;
    }

    private citation(item: Citation): void {
        this.valueLine("Citation", item.scope, [`referent=${referent(item.referent)}`], 0);
        this.nested(2, () => {
            this.group("CitationPrefix", item.prefix.length);
            this.nested(item.prefix.length, () => {
                for (const child of item.prefix) this.dump(child);
            });
            this.group("CitationSuffix", item.suffix.length);
            this.nested(item.suffix.length, () => {
                for (const child of item.suffix) this.dump(child);
            });
        });
    }

    private metadata(value: Metadata): void {
        this.valueLine("Metadata", value.scope, [], value.content.length);
        this.nested(value.content.length, () => {
            for (const content of value.content) {
                if (content.kind === "comment") {
                    this.emit(`MetadataContent value=comment(${jsonString(content.value)}) children=0`);
                } else {
                    const record = content.record;
                    this.valueLine(
                        "MetadataRecord",
                        record.scope,
                        [`name=${jsonString(record.name)}`, `value=${metadataValue(record.value)}`],
                        0
                    );
                }
            }
        });
    }

    private footnote(value: Footnote): void {
        this.valueLine("Footnote", value.scope, [`id=${jsonString(value.id)}`], value.content.length);
        this.nested(value.content.length, () => {
            for (const child of value.content) this.dump(child);
        });
    }

    private specimen(value: Specimen): void {
        this.valueLine(
            "Specimen",
            value.scope,
            [`id=${value.id === null ? "null" : jsonString(value.id)}`, `start=${value.start ?? "null"}`],
            value.content.length
        );
        this.nested(value.content.length, () => {
            for (const child of value.content) this.dump(child);
        });
    }

    private container(kind: string, node: Markup, fields: readonly string[], children: readonly Markup[]): void {
        this.line(kind, node, fields, children.length);
        this.nested(children.length, () => {
            for (const child of children) this.dump(child);
        });
    }

    private line(kind: string, node: Markup, fields: readonly string[] = [], children = 0): void {
        this.valueLine(
            kind,
            node.scope,
            [`anchor=${optionalString(node.anchor)}`, `attributes=${attributesString(node.attributes)}`, ...fields],
            children
        );
    }

    /** A value line prints like a node line: scope, fields, `children`. */
    private valueLine(kind: string, at: Scope, fields: readonly string[], children: number): void {
        const fieldText = fields.length === 0 ? "" : ` ${fields.join(" ")}`;
        this.emit(`${kind} ${scope(at)}${fieldText} children=${children}`);
    }

    /**
     * A group line nests a node-valued list under its owner: `Kind children=N`
     * with no scope and no fields. The caller opens the list's own nesting.
     */
    private group(kind: string, children: number): void {
        this.emit(`${kind} children=${children}`);
    }

    private emit(text: string): void {
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

function scope(value: Scope): string {
    return `scope=${value.start.line}:${value.start.column}..${value.end.line}:${value.end.column}`;
}

function optionalString(value: string | null): string {
    return value === null ? "null" : jsonString(value);
}

function orderedListDelimiter(value: OrderedListDelimiter | null): string {
    if (value === null || typeof value === "string") return value ?? "null";
    return `parenthesis(closed=${value.closed})`;
}

function orderedListVariant(value: OrderedListVariant | null): string {
    if (value === null || typeof value === "string") return value ?? "null";
    return `${value.kind}(lowercased=${value.lowercased})`;
}

/** A tagged value prints its branch and its named fields with no spaces. */
function referent(value: CitationReferent): string {
    return value.kind === "bib"
        ? `bib(key=${jsonString(value.key)},mode=${value.mode})`
        : `${value.kind}(id=${jsonString(value.id)})`;
}

/** A tagged value prints its branch and its named fields with no spaces. */
function destination(value: Destination): string {
    return value.kind === "url"
        ? `url(${jsonString(value.value)})`
        : `cross(path=${jsonString(value.path)},anchor=${optionalString(value.anchor)})`;
}

function jsonString(value: string): string {
    return JSON.stringify(value);
}

function attributesString(value: Attributes): string {
    return (
        "{" +
        [
            ...value.classes.map(
                (name) => "." + (/^[!-~]+$/u.test(name) && !/["\\{}[\]()=]/u.test(name) ? name : jsonString(name))
            ),
            ...value.records.map((record) => `${record.name}=${jsonString(record.value)}`)
        ].join(" ") +
        "}"
    );
}
function metadataValue(value: MetadataValue): string {
    if (value.kind === "list")
        return "list([" + value.items.map((item) => `${item.kind}(${jsonString(item.value)})`).join(",") + "])";
    const scalar = value.value;
    if (scalar.kind === "null") return "scalar(null)";
    return `scalar(${scalar.kind}(${scalar.kind === "bool" ? String(scalar.value) : jsonString(scalar.value)}))`;
}
