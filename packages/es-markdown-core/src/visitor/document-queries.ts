import { MarkdownCoreError } from "../common/markdown-core-error.js";
import { SourceLines } from "../common/source-lines.js";
import type { Document } from "../markup/document.js";
import type { Markup } from "../markup/markup.js";
import type { Position, Scope } from "../markup/values.js";
import { traverse } from "./markup-walker.js";

/** An absolute byte range of the UTF-8 source. */
export interface Place {
    readonly start: number;
    readonly end: number;
}

/** The absolute range of `target`, a node of the tree under `root`, found by
 * one canonical walk. A node is found by reference. */
export function placeOf(root: Markup, target: Markup): Place {
    let place!: Place;
    traverse(root, 0, (node, phase, start, end) => {
        if (node === target && phase === "enter") place = { start, end };
    });
    return place;
}

/** `Document.scope`: one walk for the node's range, then a scan of the source
 * for its lines, with columns in the document's unit. The public boundary:
 * `outOfBounds` when `source` ends before the node does. */
export function scopeOf(document: Document, node: Markup, source: string): Scope {
    const place = placeOf(document, node);
    const lines = new SourceLines(source);
    if (place.end > lines.bytes.length) throw new MarkdownCoreError("outOfBounds");
    return lines.scope(place.start, place.end, document.unit);
}

/** `Document.nodeAt`: the position's byte offset, then one walk that keeps
 * the last node holding it. The public boundary: `outOfBounds` unless the line
 * and column are integers of at least 1. */
export function nodeAt(document: Document, position: Position, source: string): Markup | null {
    const { line, column } = position;
    if (!(Number.isInteger(line) && line >= 1 && Number.isInteger(column) && column >= 1)) {
        throw new MarkdownCoreError("outOfBounds");
    }
    const offset = new SourceLines(source).offset(position, document.unit);
    if (offset === null) return null;
    let found: Markup | null = null;
    traverse(document, 0, (node, phase, start, end) => {
        if (phase === "enter" && start <= offset && offset < end) found = node;
    });
    return found;
}
