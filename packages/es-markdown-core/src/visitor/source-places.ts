import type { Markup } from "../markup/markup.js";

/** An absolute byte range of the UTF-8 source. */
export interface Place {
    readonly start: number;
    readonly end: number;
}

/**
 * The source of `node` whose lead is from `anchor`, where the source of the
 * node before it in its relation ends, or where its owner's source starts:
 * from where its first run starts to where its last ends, or its range when
 * it has no runs.
 */
export function sourceOf(node: Markup, anchor: number): Place {
    const runs = node.runs;
    if (runs.length === 0) {
        const start = anchor + node.extent.lead;
        return { start, end: start + node.extent.span };
    }
    let at = anchor;
    let start = anchor;
    runs.forEach(({ source }, index) => {
        at += source.lead;
        if (index === 0) start = at;
        at += source.span;
    });
    return { start, end: at };
}

/**
 * A node's source ranges, in source order, from its source as the canonical
 * walk places it: its runs, with touching runs one range, or its source when
 * it has no runs.
 */
export function placesOf(node: Markup, source: Place): Place[] {
    const runs = node.runs;
    if (runs.length === 0) return [source];
    const places: Place[] = [];
    let at = source.start - runs[0]!.source.lead;
    for (const run of runs) {
        const start = at + run.source.lead;
        at = start + run.source.span;
        const last = places[places.length - 1];
        if (last !== undefined && last.end === start) places[places.length - 1] = { start: last.start, end: at };
        else places.push({ start, end: at });
    }
    return places;
}
