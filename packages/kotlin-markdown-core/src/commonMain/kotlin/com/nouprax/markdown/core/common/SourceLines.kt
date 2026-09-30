package com.nouprax.markdown.core

/**
 * THE LINES OF A SOURCE: its UTF-8 bytes and the offset each line begins at.
 * A line ends after LF, after CR, or after CRLF, which is one terminator.
 * Scope queries build one per call from the caller's source; a document keeps
 * none.
 */
internal class SourceLines(
    source: String,
) {
    val bytes: ByteArray = source.encodeToByteArray()
    private val starts: IntArray

    init {
        var count = 1
        for (index in bytes.indices) if (endsLine(index)) count++
        starts = IntArray(count)
        var line = 1
        for (index in bytes.indices) if (endsLine(index)) starts[line++] = index + 1
    }

    private fun endsLine(index: Int): Boolean =
        bytes[index] == LF || (bytes[index] == CR && !(index + 1 < bytes.size && bytes[index + 1] == LF))

    /**
     * A byte range of the source as editor coordinates: the start is the
     * position of its first byte, and the end the line holding its exclusive
     * end and the columns from that line's start to it.
     */
    fun scope(
        start: Int,
        end: Int,
        unit: TextUnit,
    ): Scope {
        val first = lineOf(start)
        val last = lineOf(end)
        return Scope(
            Position(first + 1, columns(starts[first], start, unit) + 1),
            Position(last + 1, columns(starts[last], end, unit)),
        )
    }

    /**
     * The offset of the scalar that starts at [position], whose line and
     * column are at least 1, or null when the source has no such line or the
     * line no scalar there.
     */
    fun offset(
        position: Position,
        unit: TextUnit,
    ): Int? {
        if (position.line > starts.size) return null
        val line = position.line - 1
        var offset = starts[line]
        val end = if (line + 1 < starts.size) starts[line + 1] else bytes.size
        // Step over the line's scalars up to the column.
        var column = 1L
        while (column < position.column) {
            if (offset >= end) return null
            var next = offset + 1
            while (next < end && isContinuation(bytes[next])) next++
            column += columns(offset, next, unit)
            offset = next
        }
        return if (offset < end) offset else null
    }

    /** The index of the line holding [offset]: the last line starting at or before it. */
    private fun lineOf(offset: Int): Int {
        var low = 0
        var high = starts.size
        while (high - low > 1) {
            val middle = low + (high - low) / 2
            if (starts[middle] <= offset) low = middle else high = middle
        }
        return low
    }

    /**
     * The columns between two offsets of one line, in [unit]. A UTF-8 byte
     * that begins a four-byte scalar is two UTF-16 units; a continuation byte
     * is none.
     */
    private fun columns(
        from: Int,
        to: Int,
        unit: TextUnit,
    ): Int {
        if (unit == TextUnit.UTF8) return to - from
        var units = 0
        for (index in from until to) {
            val byte = bytes[index].toInt() and 0xff
            units +=
                when {
                    isContinuation(bytes[index]) -> 0
                    byte >= 0xf0 -> 2
                    else -> 1
                }
        }
        return units
    }

    private fun isContinuation(byte: Byte): Boolean = byte.toInt() and 0xc0 == 0x80

    private companion object {
        const val LF: Byte = 0x0a
        const val CR: Byte = 0x0d
    }
}

/** An absolute byte range of the source: [start] inclusive, [end] exclusive. */
internal class Place(
    val start: Long,
    val end: Long,
)
