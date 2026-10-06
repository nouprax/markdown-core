import type { Markup } from "../markup/markup.js";
import type { Run } from "../markup/values.js";

/** An absolute byte range of the UTF-8 source, or of an inline root's content. */
export interface Place {
    readonly start: number;
    readonly end: number;
}

/** A run with its places made absolute: `length` content bytes from
 * `content`, read from the source bytes [start, end). */
interface SourceRun {
    readonly content: number;
    readonly length: number;
    readonly start: number;
    readonly end: number;
}

/**
 * THE RUNS OF AN INLINE ROOT'S CONTENT, in absolute offsets: where in the
 * source each part of the content was read from. A copied run, whose span is
 * its length, reads each content byte from one source byte; any other reads
 * all of its content from all of its source. A walk holds one per root it
 * enters, so they are made absolute when a place is first asked for.
 */
export class SourceRuns {
    private absolute: readonly SourceRun[] | null = null;

    /** The runs of a node that starts at `start`. */
    constructor(
        private readonly stored: readonly Run[],
        private readonly start: number
    ) {}

    /** Each run starts `lead` past the end of the run before, or past the
     * node's start for the first. */
    private get runs(): readonly SourceRun[] {
        if (this.absolute !== null) return this.absolute;
        let content = 0;
        let at = this.start;
        this.absolute = this.stored.map(({ lead, span, length }) => {
            const run = { content, length, start: at + lead, end: at + lead + span };
            content += length;
            at = run.end;
            return run;
        });
        return this.absolute;
    }

    /**
     * The source ranges the content range [start, end) was read from, in
     * source order: each run's part of it, a copied run's byte for byte and
     * any other's whole, with touching parts joined. An empty range is one
     * empty range where its offset is read from.
     */
    places(start: number, end: number): Place[] {
        const runs = this.runs;
        if (runs.length === 0) return [];
        if (end <= start) {
            const at = this.place(start);
            return [{ start: at, end: at }];
        }
        const places: { start: number; end: number }[] = [];
        for (let index = this.at(start); index < runs.length && runs[index]!.content < end; index += 1) {
            const run = runs[index]!;
            const from = Math.max(start, run.content);
            const to = Math.min(end, run.content + run.length);
            if (from >= to) continue;
            const copied = copiedRun(run);
            const part = {
                start: copied ? run.start + (from - run.content) : run.start,
                end: copied ? run.start + (to - run.content) : run.end
            };
            const last = places[places.length - 1];
            if (last !== undefined && last.end === part.start) last.end = part.end;
            else places.push(part);
        }
        return places;
    }

    /** The index of the run content offset `offset` is in: the last that
     * starts at or before it. */
    private at(offset: number): number {
        let lower = 0;
        const runs = this.runs;
        let upper = runs.length;
        while (upper - lower > 1) {
            const middle = lower + ((upper - lower) >> 1);
            if (runs[middle]!.content <= offset) lower = middle;
            else upper = middle;
        }
        return lower;
    }

    /** Where content offset `offset` is read from: its source byte, the start
     * of the run that reads it whole, or, past the content, where the content
     * ends. */
    private place(offset: number): number {
        const run = this.runs[this.at(offset)]!;
        if (offset >= run.content + run.length) return run.end;
        return copiedRun(run) ? run.start + (offset - run.content) : run.start;
    }
}

function copiedRun(run: SourceRun): boolean {
    return run.end - run.start === run.length;
}

/**
 * A node's source ranges, in source order, from its range as the canonical
 * walk places it: the source its content range was read from when it is in
 * an inline root's content, whose runs are `content`; else its pieces, each
 * leading from the end of the one before or from the node's start; else its
 * one range.
 */
export function placesOf(node: Markup, start: number, end: number, content: SourceRuns | null): Place[] {
    if (content !== null) return content.places(start, end);
    if (node.pieces.length === 0) return [{ start, end }];
    let at = start;
    return node.pieces.map(({ lead, span }) => {
        const piece = { start: at + lead, end: at + lead + span };
        at = piece.end;
        return piece;
    });
}
