import { MarkdownCoreError } from "../common/markdown-core-error.js";
import { SourceLines } from "../common/source-lines.js";
import type { Document } from "../markup/document.js";
import type { Markup } from "../markup/markup.js";
import type { Position, Scope } from "../markup/values.js";
import { markupEquals } from "./markup-equals.js";
import { traverse } from "./markup-walker.js";
import { holds, placesOf, type Place } from "./source-places.js";

/** The source ranges of `target`, a node of `document`, found by one
 * canonical walk. A node is found by reference. */
function placesIn(document: Document, target: Markup): Place[] {
    let places!: Place[];
    traverse(document, (node, phase, anchor) => {
        if (node === target && phase === "enter") places = placesOf(node, anchor);
    });
    return places;
}

/** `Document.scope`: one walk for the node's source ranges, then a scan of
 * the source for its lines, with columns in the document's unit. The public
 * boundary: `outOfBounds` when `source` ends before the node does. */
export function scopeOf(document: Document, node: Markup, source: string): Scope[] {
    const places = placesIn(document, node);
    const lines = new SourceLines(source);
    if (places[places.length - 1]!.end > lines.bytes.length) throw new MarkdownCoreError("outOfBounds");
    return places.map(({ start, end }) => lines.scope(start, end, document.unit));
}

/** `Document.nodeAt`: the position's byte offset, then one walk that keeps
 * the last node one of whose source ranges holds it. The public boundary:
 * `outOfBounds` unless the line and column are integers of at least 1. */
export function nodeAt(document: Document, position: Position, source: string): Markup | null {
    const { line, column } = position;
    if (!(Number.isInteger(line) && line >= 1 && Number.isInteger(column) && column >= 1)) {
        throw new MarkdownCoreError("outOfBounds");
    }
    const offset = new SourceLines(source).offset(position, document.unit);
    if (offset === null) return null;
    let found: Markup | null = null;
    traverse(document, (node, phase, anchor) => {
        if (phase === "enter" && holds(node, anchor, offset)) found = node;
    });
    return found;
}

/** Every node of `document` one of whose source ranges holds the byte at
 * `offset`, in canonical walk order, found by one walk. */
export function holdersOf(document: Document, offset: number): Markup[] {
    const found: Markup[] = [];
    traverse(document, (node, phase, anchor) => {
        if (phase === "enter" && holds(node, anchor, offset)) found.push(node);
    });
    return found;
}

/** The offset of the first byte `target` holds, found by one walk: the node
 * of `document` that is `target` or, with its id, equals it. Null when it
 * holds no byte. The public boundary: `outOfBounds` when `document` holds no
 * such node. */
export function leadOf(document: Document, target: Markup): number | null {
    let held: Markup | undefined;
    let lead: number | null = null;
    traverse(document, (node, phase, anchor) => {
        if (phase !== "enter" || held !== undefined || node.id !== target.id) return;
        held = node;
        lead = placesOf(node, anchor).find(({ start, end }) => start < end)?.start ?? null;
    });
    if (held === undefined || !(held === target || markupEquals(held, target))) {
        throw new MarkdownCoreError("outOfBounds");
    }
    return lead;
}
