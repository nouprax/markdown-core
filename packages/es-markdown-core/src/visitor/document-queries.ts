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

/** Whether `lines` hold the whole document, as the source it was parsed
 * from does. */
export function fits(document: Document, lines: SourceLines): boolean {
    return document.extent.lead + document.extent.span <= lines.bytes.length;
}

/** The absolute range of `target` in the tree under `root`, found by one
 * canonical walk; null when the node is not in it. */
export function placeOf(root: Markup, target: Markup): Place | null {
    let place: Place | null = null;
    traverse(root, 0, (node, phase, start, end) => {
        if (node === target && phase === "enter") place = { start, end };
    });
    return place;
}

/** `Document.scope`: one walk for the node's range, then a scan of the source
 * for its lines, with columns in the document's unit. */
export function scopeOf(document: Document, node: Markup, source: string): Scope | null {
    const lines = new SourceLines(source);
    const place = fits(document, lines) ? placeOf(document, node) : null;
    return place === null ? null : lines.scope(place.start, place.end, document.unit);
}

/** `Document.nodeAt`: the position's byte offset, then one walk that keeps
 * the last node holding it. */
export function nodeAt(document: Document, position: Position, source: string): Markup | null {
    const lines = new SourceLines(source);
    const offset = fits(document, lines) ? lines.offset(position, document.unit) : null;
    if (offset === null) return null;
    let found: Markup | null = null;
    traverse(document, 0, (node, phase, start, end) => {
        if (phase === "enter" && start <= offset && offset < end) found = node;
    });
    return found;
}
