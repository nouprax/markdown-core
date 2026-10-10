import type { Markup } from "../markup/markup.js";

/** An absolute byte range of the UTF-8 source. */
export interface Place {
    readonly start: number;
    readonly end: number;
}

/**
 * A node's source ranges, in source order: its runs, the first leading from
 * `anchor`, where the source of the node before it in its relation ends, or
 * where its owner's source starts, and every other from the end of the run
 * before it.
 */
export function placesOf(node: Markup, anchor: number): Place[] {
    let at = anchor;
    return node.runs.map(({ lead, span }) => {
        const start = at + lead;
        at = start + span;
        return { start, end: at };
    });
}

/** Where `node`'s source ends: the end of its last run, measured as
 * `placesOf` measures them, without making a range. */
export function endOf(node: Markup, anchor: number): number {
    let at = anchor;
    for (const { lead, span } of node.runs) at += lead + span;
    return at;
}

/** Whether one of `node`'s source ranges holds the byte at `offset`. */
export function holds(node: Markup, anchor: number, offset: number): boolean {
    let at = anchor;
    for (const { lead, span } of node.runs) {
        const start = at + lead;
        at = start + span;
        if (start <= offset && offset < at) return true;
    }
    return false;
}
