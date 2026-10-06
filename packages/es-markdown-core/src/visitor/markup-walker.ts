import type { Markup } from "../markup/markup.js";
import type { MarkupVisitPhase, MarkupVisitor } from "./markup-visitor.js";
import { SourceRuns } from "./source-places.js";

/** Walks markup depth first with an explicit stack, reporting both phases to the visitor. */
export function walk(root: Markup, visitor: MarkupVisitor): void {
    traverse(root, (node, phase) => dispatch(visitor, node, phase));
}

/** The walk the dumper drives: the document's walk, which places every node
 * in the content it is in, with only `target`'s tree reported to `visitor`;
 * `place` hears each of those nodes' range and content before the visitor
 * enters it. */
export function walkTree(
    document: Markup,
    target: Markup,
    visitor: MarkupVisitor,
    place: (node: Markup, start: number, end: number, content: SourceRuns | null) => void
): void {
    let inside = false;
    traverse(document, (node, phase, start, end, content) => {
        if (!inside && node !== target) return;
        inside = !(node === target && phase === "exit");
        if (phase === "enter") place(node, start, end, content);
        dispatch(visitor, node, phase);
    });
}

/** One visit of the canonical walk: the node's phase, its absolute range, and
 * the runs of the inline root content that range is in, or null when it is in
 * the source. */
export type Visit = (
    node: Markup,
    phase: MarkupVisitPhase,
    start: number,
    end: number,
    content: SourceRuns | null
) => void;

interface Frame {
    readonly node: Markup;
    readonly start: number;
    readonly end: number;
    /** The runs of the content the node is in, or null in the source. */
    readonly within: SourceRuns | null;
    readonly relations: readonly (readonly Markup[])[];
    relation: number;
    index: number;
    /** The offset the next node's lead is relative to. */
    anchor: number;
    /** The runs of the content the relation in hand is in, or null in the
     * source. */
    content: SourceRuns | null;
}

/**
 * The canonical walk: every node enters, its relations follow in canonical
 * order with each relation in stored order, then it exits. The work stack
 * holds one frame per level, so depth is data, not call stack. Each node's
 * absolute range follows from its extent: the first node of a relation leads
 * from its owner's start, every later one from the end of the node before it.
 * A node whose runs read content is an inline root: its first relation is its content,
 * which starts at 0 and is read from the source through its runs, and every
 * node below that relation is placed in that content; its later relations are
 * back in the coordinates it is in. Roots never nest. Places are absolute
 * when `root` is a document.
 */
export function traverse(root: Markup, each: Visit): void {
    const frames: Frame[] = [];
    const enter = (node: Markup, start: number, within: SourceRuns | null): number => {
        const end = start + node.extent.span;
        each(node, "enter", start, end, within);
        const content = SourceRuns.readContent(node.runs) ? new SourceRuns(node.runs, start) : null;
        frames.push({
            node,
            start,
            end,
            within,
            relations: relations[node.kind](node as never),
            relation: 0,
            index: 0,
            anchor: content === null ? start : 0,
            content: content ?? within
        });
        return end;
    };
    enter(root, root.extent.lead, null);
    while (frames.length > 0) {
        const frame = frames[frames.length - 1]!;
        const relation = frame.relations[frame.relation];
        if (relation === undefined) {
            frames.pop();
            each(frame.node, "exit", frame.start, frame.end, frame.within);
        } else if (frame.index < relation.length) {
            const node = relation[frame.index]!;
            frame.index += 1;
            frame.anchor = enter(node, frame.anchor + node.extent.lead, frame.content);
        } else {
            frame.relation += 1;
            frame.index = 0;
            frame.anchor = frame.start;
            frame.content = frame.within;
        }
    }
}

/** Calls the visitor's method for the node's kind. The mapped union keeps
 * the kind and its node correlated during indexed dispatch. */
function dispatch<Kind extends Markup["kind"]>(
    visitor: MarkupVisitor,
    node: { [Key in Kind]: Extract<Markup, { kind: Key }> }[Kind],
    phase: MarkupVisitPhase
): void {
    visitor[node.kind](node, phase);
}

const none: readonly (readonly Markup[])[] = [];

/**
 * Every kind's owned Markup relations in canonical order, each one chain of
 * extents. An absent optional field is no relation. Every kind must specify
 * its relations.
 */
const relations: {
    [Kind in Markup["kind"]]: (node: Extract<Markup, { kind: Kind }>) => readonly (readonly Markup[])[];
} = {
    document: (node) => (node.metadata === null ? [node.content] : [[node.metadata], node.content]),
    callout: (node) => (node.title === null ? [node.content] : [node.title, node.content]),
    paragraph: (node) => [node.content],
    heading: (node) => [node.content],
    list: (node) => [node.items],
    listItem: (node) => [node.content],
    table: (node) =>
        node.caption === null
            ? [node.head, node.content, node.foot]
            : [[node.caption], node.head, node.content, node.foot],
    tableCaption: (node) => [node.content],
    tableRow: (node) => [node.cells],
    tableCell: (node) => [node.content],
    directiveBlock: (node) => (node.label === null ? [node.content] : [[node.label], node.content]),
    directiveLabel: (node) => [node.content],
    emphasis: (node) => [node.content],
    strong: (node) => [node.content],
    strikethrough: (node) => [node.content],
    mark: (node) => [node.content],
    insertion: (node) => [node.content],
    span: (node) => [node.content],
    superscript: (node) => [node.content],
    subscript: (node) => [node.content],
    link: (node) => [node.content],
    embedded: (node) => [node.content],
    directive: (node) => (node.label === null ? none : [[node.label]]),
    cite: (node) => [node.citations],
    definitionList: (node) => [node.definitions],
    definition: (node) => [node.term, ...node.content],
    citation: (node) =>
        node.referent.kind === "footnote" && node.referent.target.kind === "note"
            ? [[node.referent.target.footnote], node.prefix, node.suffix]
            : [node.prefix, node.suffix],
    footnote: (node) => [node.content],
    specimen: (node) => [node.content],
    thematicBreak: () => none,
    codeBlock: () => none,
    htmlBlock: () => none,
    formulaBlock: () => none,
    text: () => none,
    softBreak: () => none,
    lineBreak: () => none,
    code: () => none,
    html: () => none,
    crossLink: () => none,
    crossEmbedded: () => none,
    comment: () => none,
    formula: () => none,
    metadata: () => none,
    reference: () => none
};
