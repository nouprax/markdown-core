import type { Position, Scope, TextUnit } from "../markup/values.js";

const utf8Encoder = new TextEncoder();

/**
 * THE LINES OF A SOURCE, over its UTF-8 bytes: the offset each line begins
 * at. A line ends after LF, after CR, or after CRLF, which is one terminator.
 * Built per query from the caller's source, and converts between byte offsets
 * and editor coordinates in either unit.
 */
export class SourceLines {
    readonly bytes: Uint8Array;
    private readonly starts: number[] = [0];

    constructor(source: string) {
        const bytes = utf8Encoder.encode(source);
        this.bytes = bytes;
        for (let index = 0; index < bytes.length; index += 1) {
            const byte = bytes[index]!;
            if (byte === 0x0a || (byte === 0x0d && bytes[index + 1] !== 0x0a)) this.starts.push(index + 1);
        }
    }

    /**
     * A byte range as editor coordinates: the start is the position of its
     * first byte, and the end the line holding its exclusive end and the
     * columns from that line's start to it.
     */
    scope(start: number, end: number, unit: TextUnit): Scope {
        const first = this.lineOf(start);
        const last = this.lineOf(end);
        return {
            start: { line: first + 1, column: this.columns(this.starts[first]!, start, unit) + 1 },
            end: { line: last + 1, column: this.columns(this.starts[last]!, end, unit) }
        };
    }

    /**
     * The offset of the byte at `position`, whose line and column are integers
     * of at least 1, stepping over its line's scalars up to the column; null
     * unless the position lands on a byte of the line at a scalar boundary.
     */
    offset(position: Position, unit: TextUnit): number | null {
        const { line, column } = position;
        if (line > this.starts.length) return null;
        let offset = this.starts[line - 1]!;
        const end = line < this.starts.length ? this.starts[line]! : this.bytes.length;
        let at = 1;
        while (at < column) {
            if (offset >= end) return null;
            let next = offset + 1;
            while (next < end && (this.bytes[next]! & 0xc0) === 0x80) next += 1;
            at += this.columns(offset, next, unit);
            offset = next;
        }
        return at === column && offset < end ? offset : null;
    }

    /** The index of the line holding `offset`: the last line starting at or before it. */
    private lineOf(offset: number): number {
        let lower = 0;
        let upper = this.starts.length;
        while (upper - lower > 1) {
            const middle = lower + ((upper - lower) >> 1);
            if (this.starts[middle]! <= offset) lower = middle;
            else upper = middle;
        }
        return lower;
    }

    /** The columns between two offsets of one line, in `unit`. A UTF-8 byte
     * that begins a four-byte scalar is two UTF-16 units; a continuation byte
     * is none. */
    private columns(from: number, to: number, unit: TextUnit): number {
        if (unit === "utf8") return to - from;
        let units = 0;
        for (let index = from; index < to; index += 1) {
            const byte = this.bytes[index]!;
            units += (byte & 0xc0) === 0x80 ? 0 : byte >= 0xf0 ? 2 : 1;
        }
        return units;
    }
}
