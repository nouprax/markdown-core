package com.nouprax.markdown.core

/** What a region's bytes are to their owner. */
public enum class RegionRole {
    /** Syntax the owner is made of: a fence, a bullet, a heading's `#`s. */
    MARKER,

    /** Bytes the owner's meaning is made of. */
    CONTENT,

    /** Bytes the owner consumed and kept nothing of. */
    DISCARDED,
}

/**
 * One region of the concrete view: a byte range of the normalized source with
 * exactly one owner and exactly one role.
 *
 * [start] and [length] index [Concrete.source], which is BYTES and not a
 * `String`: the parser counts positions in bytes, and a `String` index
 * disagrees with it on the first character outside ASCII.
 *
 * [owner] is the path of child indices from the semantic root: an empty path is
 * the root and `[0, 2]` is the third child of the first. A pointer names a node
 * only while the native handle is alive and these values outlive it, so the
 * path is the locator rather than the node.
 */
public class Region internal constructor(
    public val start: Int,
    public val length: Int,
    public val role: RegionRole,
    public val owner: kotlin.collections.List<Int>,
)

/**
 * The concrete view of a parse: the normalized source, its line index, and
 * every region of it.
 *
 * Total, and that is the point of the pair: every byte of [source] lies in
 * exactly one region and every region has exactly one owner in the semantic
 * tree, so nothing the parser read is reachable through neither view.
 *
 * The regions are held as parallel arrays and a [Region] is built when it is
 * asked for. Measured on this repository's own design document -- one region
 * per 17 bytes of prose -- an object per region costs several times the source
 * it describes, and the arrays cost about 25 bytes each.
 */
public class Concrete internal constructor(
    /**
     * The NORMALIZED source: UTF-8 validated, NUL replaced, every line ending a
     * single `\n`. Not the bytes the caller passed in.
     */
    public val source: ByteArray,
    private val lineStarts: IntArray,
    private val regionStarts: IntArray,
    private val regionLengths: IntArray,
    private val regionRoles: ByteArray,
    private val ownerPaths: IntArray,
    private val ownerOffsets: IntArray,
) {
    /** How many lines the normalized source has. */
    public val lineCount: Int get() = lineStarts.size

    /** Where [line] begins in [source], counting lines from 1. */
    public fun lineStart(line: Int): Int {
        if (line < 1 || line > lineStarts.size) throw IndexOutOfBoundsException("no line $line")
        return lineStarts[line - 1]
    }

    /** How many regions the view has. They are in source order. */
    public val regionCount: Int get() = regionStarts.size

    /** The region at [index], counting from 0. */
    public fun region(index: Int): Region {
        if (index < 0 || index >= regionStarts.size) throw IndexOutOfBoundsException("no region $index")
        val from = ownerOffsets[index]
        val to = ownerOffsets[index + 1]
        return Region(
            regionStarts[index],
            regionLengths[index],
            ROLES[regionRoles[index].toInt()],
            immutableList(to - from) { ownerPaths[from + it] },
        )
    }

    private companion object {
        /** The native role, in the order `markdown_core_region_role` declares it. */
        val ROLES = arrayOf(RegionRole.MARKER, RegionRole.CONTENT, RegionRole.DISCARDED)
    }
}
