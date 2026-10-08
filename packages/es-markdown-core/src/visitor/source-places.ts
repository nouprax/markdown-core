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
