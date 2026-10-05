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
     * column are at least 1, or null when the source has no such line or no
     * scalar starts at that column, as inside a surrogate pair.
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
        return if (column == position.column.toLong() && offset < end) offset else null
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

/**
 * A NODE'S SOURCE RANGES, absolute byte ranges of the source in source order,
 * each [start] inclusive and [end] exclusive. A walk refills one for each node
 * it is asked about.
 */
internal class SourcePlaces {
    private var starts = LongArray(4)
    private var ends = LongArray(4)

    var count: Int = 0
        private set

    fun start(index: Int): Long = starts[index]

    fun end(index: Int): Long = ends[index]

    fun clear() {
        count = 0
    }

    fun add(
        start: Long,
        end: Long,
    ) {
        if (count == starts.size) {
            starts = starts.copyOf(count * 2)
            ends = ends.copyOf(count * 2)
        }
        starts[count] = start
        ends[count++] = end
    }

    /** Extends the last range to [end]. */
    fun extend(end: Long) {
        ends[count - 1] = end
    }
}

/**
 * THE RUNS OF AN INLINE ROOT'S CONTENT in absolute offsets: for each run, the
 * content offset it starts at, its content length, and the source range it
 * reads. A run whose source is as long as its content reads each content byte
 * from one source byte; any other reads all of its content from all of its
 * source. Roots never nest, so one table serves every root of a walk in turn.
 */
internal class SourceRuns {
    private var contents = LongArray(0)
    private var sizes = LongArray(0)
    private var starts = LongArray(0)
    private var ends = LongArray(0)
    private var count = 0

    /** Reads [runs], the runs of a node that starts at [anchor]. */
    fun read(
        runs: kotlin.collections.List<Run>,
        anchor: Long,
    ) {
        if (runs.size > starts.size) {
            contents = LongArray(runs.size)
            sizes = LongArray(runs.size)
            starts = LongArray(runs.size)
            ends = LongArray(runs.size)
        }
        var content = 0L
        var at = anchor
        for (index in runs.indices) {
            val run = runs[index]
            val start = at + run.lead
            contents[index] = content
            sizes[index] = run.length.toLong()
            starts[index] = start
            ends[index] = start + run.span.toLong()
            content += sizes[index]
            at = ends[index]
        }
        count = runs.size
    }

    private fun copied(index: Int): Boolean = ends[index] - starts[index] == sizes[index]

    /** The run content offset [offset] is in: the last that starts at or before it. */
    private fun runAt(offset: Long): Int {
        var lo = 0
        var hi = count
        while (hi - lo > 1) {
            val middle = lo + (hi - lo) / 2
            if (contents[middle] <= offset) lo = middle else hi = middle
        }
        return lo
    }

    /**
     * Where content offset [offset] is read from: its source byte, the start
     * of the run that reads it whole, or, past the content, where the content
     * ends.
     */
    private fun place(offset: Long): Long {
        val run = runAt(offset)
        if (offset >= contents[run] + sizes[run]) return ends[run]
        return if (copied(run)) starts[run] + (offset - contents[run]) else starts[run]
    }

    /**
     * Adds to [places] the source the content range [start, end) was read
     * from, the parts that touch joined; an empty range is read from one
     * empty range, where its offset is read from.
     */
    fun places(
        start: Long,
        end: Long,
        places: SourcePlaces,
    ) {
        if (count == 0) return
        if (end <= start) {
            val at = place(start)
            places.add(at, at)
            return
        }
        val first = places.count
        var index = runAt(start)
        while (index < count && contents[index] < end) {
            val from = maxOf(start, contents[index])
            val to = minOf(end, contents[index] + sizes[index])
            if (from < to) {
                val partStart = if (copied(index)) starts[index] + (from - contents[index]) else starts[index]
                val partEnd = if (copied(index)) starts[index] + (to - contents[index]) else ends[index]
                if (places.count > first && places.end(places.count - 1) == partStart) {
                    places.extend(partEnd)
                } else {
                    places.add(partStart, partEnd)
                }
            }
            index++
        }
    }
}
