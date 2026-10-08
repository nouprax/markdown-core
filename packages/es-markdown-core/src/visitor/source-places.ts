import type { Markup } from "../markup/markup.js";
import type { Run } from "../markup/values.js";

/** An absolute byte range of the UTF-8 source, or of an inline root's content. */
export interface Place {
    readonly start: number;
    readonly end: number;
}

/** A run with its places made absolute: `decoded` content bytes from
 * `content`, read from the source bytes [start, end). */
interface SourceRun {
    readonly content: number;
    readonly decoded: number;
    readonly start: number;
    readonly end: number;
}

/**
 * A NODE'S RUNS, in absolute offsets: the source it read and what each part
 * of its content was read from. A copied run, whose source spans as many
 * bytes as it decodes, reads each decoded byte from one source byte; any other
 * decodes all of its bytes from all of its source, and a run that decodes no bytes reads source that gives none. The
 * source between two runs is not the node's. A walk holds one per root it
 * enters, so they are made absolute when a place is first asked for.
 */
export class SourceRuns {
    private absolute: readonly SourceRun[] | null = null;

    /** The runs of a node that starts at `start`. */
    constructor(
        private readonly stored: readonly Run[],
        private readonly start: number
    ) {}

    /** Whether the runs read content: the node is an inline root. */
    static readContent(runs: readonly Run[]): boolean {
        return runs.some((run) => run.decoded > 0);
    }

    /** Each run's source starts its `lead` past the end of the run before,
     * or past the node's start for the first. */
    private get runs(): readonly SourceRun[] {
        if (this.absolute !== null) return this.absolute;
        let content = 0;
        let at = this.start;
        this.absolute = this.stored.map(({ source, decoded }) => {
            const run = { content, decoded, start: at + source.lead, end: at + source.lead + source.span };
            content += decoded;
            at = run.end;
            return run;
        });
        return this.absolute;
    }

    /**
     * The source window of the content range [start, end): from where its
     * first byte is read to where its last is. An empty range is the empty
     * window where its offset is read from.
     */
    window(start: number, end: number): Place {
        const from = this.place(start);
        return { start: from, end: end > start ? this.placeEnd(end) : from };
    }

    /**
     * The source ranges of `window` less the gaps between the runs, in source
     * order. An empty window is one empty range.
     */
    cut(window: Place): Place[] {
        if (window.end <= window.start) return [{ start: window.start, end: window.start }];
        const runs = this.runs;
        let lower = 0;
        let upper = runs.length;
        while (lower < upper) {
            const middle = lower + ((upper - lower) >> 1);
            if (runs[middle]!.end <= window.start) lower = middle + 1;
            else upper = middle;
        }
        const places: Place[] = [];
        let from = window.start;
        for (let index = lower; index + 1 < runs.length && runs[index]!.end < window.end; index += 1) {
            const gap = runs[index]!.end;
            const past = runs[index + 1]!.start;
            if (past <= gap) continue;
            if (gap > from) places.push({ start: from, end: gap });
            from = Math.max(from, past);
        }
        if (from < window.end) places.push({ start: from, end: window.end });
        return places;
    }

    /** The index of the content run content offset `offset` is in: the last
     * that starts at or before it and reads content, or the first run. */
    private at(offset: number): number {
        let lower = 0;
        const runs = this.runs;
        let upper = runs.length;
        while (upper - lower > 1) {
            const middle = lower + ((upper - lower) >> 1);
            if (runs[middle]!.content <= offset) lower = middle;
            else upper = middle;
        }
        while (lower > 0 && runs[lower]!.decoded === 0) lower -= 1;
        return lower;
    }

    /** Where content offset `offset` is read from: its source byte, the start
     * of the run that reads it whole, or, past the content, where the content
     * ends. */
    private place(offset: number): number {
        const run = this.runs[this.at(offset)]!;
        if (offset >= run.content + run.decoded) return run.end;
        return copiedRun(run) ? run.start + (offset - run.content) : run.start;
    }

    /** Where the content byte before `offset` is read to: past its source
     * byte, or the end of the run that reads it whole. */
    private placeEnd(offset: number): number {
        const run = this.runs[this.at(offset - 1)]!;
        if (offset - 1 >= run.content + run.decoded || !copiedRun(run)) return run.end;
        return run.start + (offset - run.content);
    }
}

/** Whether `run` reads each decoded byte from one source byte. */
function copiedRun(run: SourceRun): boolean {
    return run.end - run.start === run.decoded;
}

/**
 * A node's source ranges, in source order, from its range as the canonical
 * walk places it: a window less the gaps between the runs that place it.
 * In an inline root's content, whose runs are `content`, the window is the
 * source its content range was read from; else it is its range, cut by its
 * own runs.
 */
export function placesOf(node: Markup, start: number, end: number, content: SourceRuns | null): Place[] {
    if (content !== null) return content.cut(content.window(start, end));
    return new SourceRuns(node.runs, start).cut({ start, end });
}
