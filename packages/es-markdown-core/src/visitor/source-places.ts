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
 * before it. Runs that touch are one range: they stand apart only where they
 * read their content differently.
 */
export function placesOf(node: Markup, anchor: number): Place[] {
    let at = anchor;
    const places: { start: number; end: number }[] = [];
    for (const { lead, span } of node.runs) {
        const start = at + lead;
        at = start + span;
        const last = places[places.length - 1];
        if (last && lead === 0 && span > 0) last.end = at;
        else places.push({ start, end: at });
    }
    return places;
}

/** Where `node`'s source ends: the end of its last run, measured as
 * `placesOf` measures them, without making a range. */
export function endOf(node: Markup, anchor: number): number {
    let at = anchor;
    for (const { lead, span } of node.runs) at += lead + span;
    return at;
}

/** Whether one of `node`'s source ranges, measured as `placesOf` measures
 * them, holds the byte at `offset`. */
export function holds(node: Markup, anchor: number, offset: number): boolean {
    let at = anchor;
    for (const { lead, span } of node.runs) {
        const start = at + lead;
        at = start + span;
        if (start <= offset && offset < at) return true;
    }
    return false;
}
