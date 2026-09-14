import type { Attributes } from "../markup/attributes.js";
import type { Dimensions } from "../common/constraints.js";
import type { MetadataValue } from "../markup/metadata.js";
import type { Markup } from "../markup/markup.js";
import type {
    CitationReferent,
    Destination,
    OrderedListDelimiter,
    OrderedListVariant,
    Scope
} from "../markup/values.js";
import { walk } from "./markup-walker.js";
import type { MarkupVisitor } from "./markup-visitor.js";

/** Produces the canonical debug tree for immutable Markdown markup. */
export class MarkupDumper {
    private constructor() {}

    /** Returns the canonical debug dump for `root` and its owned markup. */
    static dump(root: Markup): string {
        const state = new State();
        state.dump(root);
        return state.result();
    }
}

type OutputGroup = { readonly name: string | null; readonly count: number };
type OutputFrame = { readonly groups: readonly OutputGroup[]; index: number; remaining: number };

class State {
    // Output frames contain no markup or traversal actions.
    private readonly frames: OutputFrame[] = [];
    private readonly remainingNodes: number[] = [];
    /**
     * The connector segments of every open nesting level, and where the
     * segments above each depth end: a line copies its lead-in once and
     * extends the segments by the one its own connector decides, instead of
     * deriving every level again per line.
     */
    private prefix = "";
    private readonly prefixEnds: number[] = [0];
    private output = "";

    /** Each callback formats its node; the walker controls traversal. */
    private readonly visitor: MarkupVisitor = {
        document: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("Document", node, [], node.content.length, [
                {
                    name: null,
                    count:
                        node.content.length +
                        node.footnotes.length +
                        node.specimens.length +
                        (node.metadata === null ? 0 : 1)
                }
            ]);
        },
        callout: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line(
                "Callout",
                node,
                [`variant=${optional(node.variant)}`, `collapsed=${node.collapsed ?? "null"}`],
                node.content.length,
                [
                    ...(node.title === null ? [] : [{ name: "Title", count: node.title.length }]),
                    { name: null, count: node.content.length }
                ]
            );
        },
        paragraph: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("Paragraph", node, [], node.content);
        },
        heading: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("Heading", node, [`level=${node.level}`], node.content);
        },
        thematicBreak: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("ThematicBreak", node);
        },
        list: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container(
                "List",
                node,
                [
                    `flavor=${node.flavor}`,
                    `start=${node.start ?? "null"}`,
                    `variant=${variant(node.variant)}`,
                    `delimiter=${delimiter(node.delimiter)}`,
                    `tight=${node.tight}`
                ],
                node.items
            );
        },
        listItem: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("ListItem", node, [`marker=${optional(node.marker)}`], node.content);
        },
        codeBlock: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("CodeBlock", node, [
                `info=${optional(node.info)}`,
                `language=${optional(node.language)}`,
                `literal=${escaped(node.literal)}`,
                `fenced=${node.fenced}`,
                `closed=${node.closed}`
            ]);
        },
        htmlBlock: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("HTMLBlock", node, [`literal=${escaped(node.literal)}`]);
        },
        formulaBlock: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("FormulaBlock", node, [`literal=${escaped(node.literal)}`]);
        },
        table: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            const columns = node.columns.map((column) => `${column.flow}:${column.relative ?? "null"}`).join(",");
            this.line(
                "Table",
                node,
                [`columns=[${columns}]`],
                node.head.length + node.content.length + node.foot.length,
                [
                    ...(node.caption === null ? [] : [{ name: null, count: 1 }]),
                    { name: "TableHead", count: node.head.length },
                    { name: "TableBody", count: node.content.length },
                    { name: "TableFoot", count: node.foot.length }
                ]
            );
        },
        tableCaption: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("TableCaption", node, [], node.content);
        },
        tableRow: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("TableRow", node, [], node.cells);
        },
        tableCell: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("TableCell", node, [`rowspan=${node.rowspan}`, `colspan=${node.colspan}`], node.content);
        },
        definitionList: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("DefinitionList", node, [], node.definitions);
        },
        definition: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("Definition", node, [`compact=${node.compact}`], node.content.length, [
                { name: "DefinitionTerm", count: node.term.length },
                ...node.content.map((body) => ({ name: "DefinitionBody", count: body.length }))
            ]);
        },
        directiveBlock: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("DirectiveBlock", node, [`name=${optional(node.name)}`], node.content.length, [
                { name: null, count: node.content.length + (node.label === null ? 0 : 1) }
            ]);
        },
        directiveLabel: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("DirectiveLabel", node, [], node.content);
        },
        text: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("Text", node, [`literal=${escaped(node.literal)}`]);
        },
        softBreak: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("SoftBreak", node);
        },
        lineBreak: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("LineBreak", node);
        },
        code: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("Code", node, [`literal=${escaped(node.literal)}`]);
        },
        html: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("HTML", node, [`literal=${escaped(node.literal)}`]);
        },
        crossLink: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("CrossLink", node, [`dest=${destination(node.dest)}`, `label=${optional(node.label)}`]);
        },
        crossEmbedded: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("CrossEmbedded", node, [
                `dest=${destination(node.dest)}`,
                `label=${optional(node.label)}`,
                `dimensions=${dimensions(node.dimensions)}`
            ]);
        },
        comment: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("Comment", node, [`literal=${escaped(node.literal)}`]);
        },
        formula: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("Formula", node, [`mode=${node.mode}`, `literal=${escaped(node.literal)}`]);
        },
        emphasis: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("Emphasis", node, [], node.content);
        },
        strong: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("Strong", node, [], node.content);
        },
        strikethrough: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("Strikethrough", node, [], node.content);
        },
        mark: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("Mark", node, [], node.content);
        },
        insertion: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("Insertion", node, [], node.content);
        },
        span: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("Span", node, [], node.content);
        },
        superscript: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("Superscript", node, [], node.content);
        },
        subscript: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container("Subscript", node, [], node.content);
        },
        link: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container(
                "Link",
                node,
                [`dest=${destination(node.dest)}`, `title=${optional(node.title)}`],
                node.content
            );
        },
        embedded: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.container(
                "Embedded",
                node,
                [
                    `dest=${destination(node.dest)}`,
                    `title=${optional(node.title)}`,
                    `dimensions=${dimensions(node.dimensions)}`
                ],
                node.content
            );
        },
        directive: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("Directive", node, [`name=${escaped(node.name)}`], 0, [
                { name: null, count: node.label === null ? 0 : 1 }
            ]);
        },
        cite: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("Cite", node, [], node.citations.length);
        },
        citation: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("Citation", node, [`referent=${referent(node.referent)}`], 0, [
                { name: "CitationPrefix", count: node.prefix.length },
                { name: "CitationSuffix", count: node.suffix.length }
            ]);
        },
        metadata: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line(
                "Metadata",
                node,
                [
                    `name=${node.name === null ? "null" : metadataValue(node.name)}`,
                    `title=${node.title === null ? "null" : metadataValue(node.title)}`,
                    `subtitle=${node.subtitle === null ? "null" : metadataValue(node.subtitle)}`,
                    `time=${node.time === null ? "null" : metadataValue(node.time)}`,
                    `date=${node.date === null ? "null" : metadataValue(node.date)}`,
                    `authors=${node.authors === null ? "null" : metadataValue(node.authors)}`,
                    `keywords=${node.keywords === null ? "null" : metadataValue(node.keywords)}`,
                    `abstract=${node.abstract === null ? "null" : metadataValue(node.abstract)}`,
                    `state=${node.state === null ? "null" : metadataValue(node.state)}`,
                    `comment=${node.comment === null ? "null" : metadataValue(node.comment)}`
                ],
                0
            );
        },
        footnote: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line("Footnote", node, [`id=${escaped(node.id)}`], node.content.length);
        },
        specimen: (node, phase) => {
            if (phase === "exit") {
                this.end();
                return;
            }
            this.start();
            this.line(
                "Specimen",
                node,
                [`id=${node.id === null ? "null" : escaped(node.id)}`, `start=${node.start ?? "null"}`],
                node.content.length
            );
        }
    };

    dump(node: Markup): void {
        walk(node, this.visitor);
    }

    result(): string {
        return this.output;
    }

    private container(kind: string, node: Markup, fields: readonly string[], children: readonly Markup[]): void {
        this.line(kind, node, fields, children.length);
    }

    private line(
        kind: string,
        node: Markup,
        fields: readonly string[] = [],
        children = 0,
        groups: readonly OutputGroup[] = [{ name: null, count: children }]
    ): void {
        this.value(
            kind,
            node.scope,
            [`anchor=${optional(node.anchor)}`, `attributes=${attributes(node.attributes)}`, ...fields],
            children
        );
        this.frames.push({ groups, index: -1, remaining: 0 });
        this.remainingNodes.push(groups.reduce((sum, group) => sum + (group.name === null ? group.count : 1), 0));
    }

    /** A value line prints like a node line: scope, fields, `children`. */
    private value(kind: string, at: Scope, fields: readonly string[], children: number): void {
        const fieldText = fields.length === 0 ? "" : ` ${fields.join(" ")}`;
        this.emit(`${kind} ${scope(at)}${fieldText} children=${children}`);
    }

    /**
     * A group line nests a node-valued list under its owner: `Kind children=N`
     * with no scope and no fields. The caller opens the list's own nesting.
     */

    private emit(text: string): void {
        const depth = this.remainingNodes.length;
        if (depth === 0) {
            this.output += `${text}\n`;
            return;
        }

        const parent = depth - 1;
        const remaining = this.remainingNodes[parent]! - 1;
        this.remainingNodes[parent] = remaining;
        const above = this.prefix.slice(0, this.prefixEnds[parent]);
        this.output += `${above}${remaining === 0 ? "└── " : "├── "}${text}\n`;
        this.prefix = above + (remaining > 0 ? "│   " : "    ");
        this.prefixEnds[depth] = this.prefix.length;
    }

    private start(): void {
        if (this.frames.length === 0) return;
        this.advance();
        const frame = this.frames[this.frames.length - 1]!;
        if (frame.remaining <= 0) throw new Error("unexpected dump child");
        frame.remaining -= 1;
    }

    private end(): void {
        this.advance();
        if (this.frames.pop()!.remaining !== 0 || this.remainingNodes.pop() !== 0) {
            throw new Error("incomplete dump output");
        }
    }

    private advance(): void {
        const frame = this.frames[this.frames.length - 1]!;
        while (frame.remaining === 0 && frame.index < frame.groups.length) {
            if (frame.index >= 0 && frame.groups[frame.index]!.name !== null) {
                if (this.remainingNodes.pop() !== 0) throw new Error("incomplete dump group");
            }
            frame.index += 1;
            if (frame.index === frame.groups.length) return;
            const group = frame.groups[frame.index]!;
            if (group.name !== null) {
                this.emit(group.name + " children=" + group.count);
                this.remainingNodes.push(group.count);
            }
            frame.remaining = group.count;
        }
    }
}

function scope(value: Scope): string {
    return `scope=${value.start.line}:${value.start.column}..${value.end.line}:${value.end.column}`;
}

function optional(value: string | null): string {
    return value === null ? "null" : escaped(value);
}

function delimiter(value: OrderedListDelimiter | null): string {
    if (value === null || typeof value === "string") return value ?? "null";
    return `parenthesis(closed=${value.closed})`;
}

function variant(value: OrderedListVariant | null): string {
    if (value === null || typeof value === "string") return value ?? "null";
    return `${value.kind}(lowercased=${value.lowercased})`;
}

/** A tagged value prints its branch and its named fields with no spaces. */
function referent(value: CitationReferent): string {
    return value.kind === "bib"
        ? `bib(key=${escaped(value.key)},mode=${value.mode})`
        : `${value.kind}(id=${escaped(value.id)})`;
}

/** A tagged value prints its branch and its named fields with no spaces. */
function destination(value: Destination): string {
    return value.kind === "url"
        ? `url(${escaped(value.value)})`
        : `cross(path=${escaped(value.path)},anchor=${optional(value.anchor)})`;
}

function escaped(value: string): string {
    return JSON.stringify(value);
}

function attributes(value: Attributes): string {
    return (
        "{" +
        [
            ...value.classes.map(
                (name) => "." + (/^[!-~]+$/u.test(name) && !/["\\{}[\]()=]/u.test(name) ? name : escaped(name))
            ),
            ...value.records.map((record) => `${record.name}=${escaped(record.value)}`)
        ].join(" ") +
        "}"
    );
}
function metadataValue(value: MetadataValue): string {
    if (value.kind === "list")
        return "list([" + value.items.map((item) => `${item.kind}(${escaped(item.value)})`).join(",") + "])";
    const scalar = value.value;
    if (scalar.kind === "null") return "scalar(null)";
    return `scalar(${scalar.kind}(${scalar.kind === "bool" ? String(scalar.value) : escaped(scalar.value)}))`;
}

function dimensions(value: Dimensions | null): string {
    return value === null ? "null" : `(width=${value.width},height=${value.height ?? "null"})`;
}
