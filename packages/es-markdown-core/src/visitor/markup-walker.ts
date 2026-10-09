import type { Markup } from "../markup/markup.js";
import type { MarkupVisitPhase, MarkupVisitor } from "./markup-visitor.js";
import { placesOf, type Place } from "./source-places.js";

/** Walks markup depth first with an explicit stack, reporting both phases to the visitor. */
export function walk(root: Markup, visitor: MarkupVisitor): void {
    traverse(root, (node, phase) => dispatch(visitor, node, phase));
}

/** The walk the dumper drives: the document's walk, which places every node
 * in the source, with only `target`'s tree reported to `visitor`; `place`
 * hears each of those nodes' source ranges before the visitor enters it. */
export function walkTree(
    document: Markup,
    target: Markup,
    visitor: MarkupVisitor,
    place: (places: readonly Place[]) => void
): void {
    let inside = false;
    traverse(document, (node, phase, places) => {
        if (!inside && node !== target) return;
        inside = !(node === target && phase === "exit");
        if (phase === "enter") place(places);
        dispatch(visitor, node, phase);
    });
}

/** One visit of the canonical walk: the node's phase and its absolute
 * source ranges, its runs, in source order. */
export type Visit = (node: Markup, phase: MarkupVisitPhase, places: readonly Place[]) => void;

interface Frame {
    readonly node: Markup;
    readonly places: readonly Place[];
    /** Where its source starts, from its first run. */
    readonly start: number;
    readonly relations: readonly Relation[];
    relation: number;
    index: number;
    /** The offset the next node's first lead is relative to. */
    anchor: number;
}

/**
 * The canonical walk: every node enters, its relations follow in canonical
 * order with each relation in stored order, then it exits. The work stack
 * holds one frame per level, so depth is data, not call stack. Each node's
 * absolute source ranges are its runs, and its source runs from where its
 * first run starts to where its last ends: the first node of a relation leads
 * from the start of its owner's source, every later one from the end of the
 * source of the node before it. Places are
 * absolute when `root` is a document.
 */
export function traverse(root: Markup, each: Visit): void {
    const frames: Frame[] = [];
    const enter = (node: Markup, anchor: number): number => {
        const places = placesOf(node, anchor);
        const start = places[0]!.start;
        each(node, "enter", places);
        frames.push({
            node,
            places,
            start,
            relations: relations[node.kind](node as never),
            relation: 0,
            index: 0,
            anchor: start
        });
        return places[places.length - 1]!.end;
    };
    enter(root, 0);
    while (frames.length > 0) {
        const frame = frames[frames.length - 1]!;
        const relation = frame.relations[frame.relation];
        const nodes = relation === undefined ? undefined : chain(relation);
        if (nodes === undefined) {
            frames.pop();
            each(frame.node, "exit", frame.places);
        } else if (frame.index < nodes.length) {
            const node = nodes[frame.index]!;
            frame.index += 1;
            frame.anchor = enter(node, frame.anchor);
        } else {
            frame.relation += 1;
            frame.index = 0;
            const next = frame.relations[frame.relation];
            if (next === undefined || !("continues" in next)) {
                frame.anchor = frame.start;
            }
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

/** A chain that continues the relation of the chain before it: its first
 * node leads from where the last node of that chain ends. A table's body
 * and foot rows continue its head rows. */
interface Continuing {
    readonly continues: readonly Markup[];
}

/** One chain of extents of a node's relations. */
type Relation = readonly Markup[] | Continuing;

function chain(relation: Relation): readonly Markup[] {
    return "continues" in relation ? relation.continues : relation;
}

const none: readonly Relation[] = [];

/**
 * Every kind's owned Markup relations in canonical order, each one chain of
 * extents. An absent optional field is no relation. Every kind must specify
 * its relations.
 */
const relations: {
    [Kind in Markup["kind"]]: (node: Extract<Markup, { kind: Kind }>) => readonly Relation[];
} = {
    document: (node) => (node.metadata === null ? [node.content] : [[node.metadata], node.content]),
    callout: (node) => (node.title === null ? [node.content] : [node.title, node.content]),
    paragraph: (node) => [node.content],
    heading: (node) => [node.content],
    list: (node) => [node.items],
    listItem: (node) => [node.content],
    // A table's rows, in head, content and foot order, are one relation.
    table: (node) =>
        node.caption === null
            ? [node.head, { continues: node.content }, { continues: node.foot }]
            : [[node.caption], node.head, { continues: node.content }, { continues: node.foot }],
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
