/**
 * The text a `scope`'s line and column numbers are counted against.
 *
 * A scope is a pair of BOUNDARIES — it says which line-and-column range an
 * element occupies, and no substring is taken with it. Those numbers are not
 * counted against the string passed to `parse`: they are counted against the
 * NORMALIZED source, which is what this carries, and the two differ wherever
 * the input held a NUL.
 */
export class Concrete {
    /**
     * The NORMALIZED source: UTF-8 as fed, every NUL replaced by the three
     * bytes of U+FFFD, every line ending a single `\n` and every line having
     * one. Not the bytes the caller passed in.
     *
     * BYTES and not a string: the parser counts columns in bytes, and a string
     * index disagrees with it on the first character outside ASCII.
     */
    source;
    #lineStarts;
    constructor(source, lineStarts) {
        this.source = source;
        this.#lineStarts = lineStarts;
    }
    /** How many lines the normalized source has. */
    get lines() {
        return this.#lineStarts.length;
    }
    /**
     * The byte offset in `source` where `line` begins, counting lines from 1.
     * An OFFSET, not a boundary: this indexes bytes, which a `Scope` never
     * does.
     */
    offset(line) {
        if (!Number.isInteger(line) || line < 1 || line > this.#lineStarts.length) {
            throw new RangeError(`no line ${line}`);
        }
        return this.#lineStarts[line - 1];
    }
}
