package com.nouprax.markdown.core

/**
 * The text a [Markup.scope]'s line and column numbers are counted against.
 *
 * A scope is a pair of BOUNDARIES -- it says which line-and-column range an
 * element occupies, and no substring is taken with it. Those numbers are not
 * counted against the string you fed the [Document]: they are counted against
 * the NORMALIZED source, which is what this carries, and the two differ
 * wherever the input held a NUL.
 */
public class Concrete internal constructor(
    /**
     * The NORMALIZED source: UTF-8 as fed, every NUL replaced by the three
     * bytes of U+FFFD, every line ending a single `\n` and every line having
     * one. Not the bytes the caller passed in.
     *
     * BYTES and not a `String`: the parser counts columns in bytes, and a
     * `String` index would disagree with it on the first character outside
     * ASCII.
     */
    public val source: ByteArray,
    private val lineStarts: IntArray,
) {
    /** How many lines the normalized source has. */
    public val lines: Int get() = lineStarts.size

    /**
     * The byte offset in [source] where [line] begins, counting lines from 1.
     * An OFFSET, not a boundary: this indexes bytes, which a [Scope] never
     * does.
     */
    public fun offset(line: Int): Int {
        if (line < 1 || line > lineStarts.size) throw IndexOutOfBoundsException("no line $line")
        return lineStarts[line - 1]
    }
}
