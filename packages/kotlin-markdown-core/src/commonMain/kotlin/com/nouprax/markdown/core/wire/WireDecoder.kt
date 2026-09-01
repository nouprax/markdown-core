package com.nouprax.markdown.core

internal object WireDecoder {
    /**
     * `MKC8`: the payload after the envelope is the facade's own wire bytes,
     * frame byte first -- every node leads with its identity, a definition's
     * match key rides where `identifier` did, and a reference carries the
     * identity of the definition it resolved to instead of repeating that
     * key.
     */
    private val magic = byteArrayOf(0x4d, 0x4b, 0x43, 0x38)

    /** The frame a payload leads with (`markdown_core_wire_frame`): the
     * request a document sends and the byte the payload answers with. */
    const val FULL_FRAME: Int = 0
    const val DELTA_FRAME: Int = 1

    /**
     * Decodes a payload: the envelope, then the tree -- whole, or as a delta
     * (#162) against [previous], the semantic tree of the payload before it,
     * whose values are reused wherever the delta says nothing moved.
     */
    fun decodeRead(
        bytes: ByteArray,
        previous: Semantic? = null,
    ): Read {
        val reader = WireReader(bytes)
        reader.header()
        return reader.read(previous)
    }

    /**
     * Decodes only the header of a payload whose read is discarded -- the
     * [Document] constructor's initial feed -- so an error still surfaces and
     * a healthy tree is not built just to be thrown away.
     */
    fun decodeDiscarded(bytes: ByteArray) {
        WireReader(bytes).header()
    }

    /** The magic and the status: the part of every payload that says whether
     * a tree or an error follows, throwing the error's exception itself. */
    private fun WireReader.header() {
        magic.forEachIndexed { index, expected ->
            val actual = byte()
            require(actual == expected) {
                "invalid native bridge payload at byte $index: expected ${expected.toUByte()}, got ${actual.toUByte()}"
            }
        }
        when (byte().toInt()) {
            0 -> Unit
            1 -> throw error()
            else -> kotlin.error("unsupported native bridge status")
        }
    }
}

/** THE ROOT IS READ BY HAND, and it is the only node that is: whole in a
 * FULL frame, and as the one SPINE op a DELTA frame opens with. */
private fun WireReader.read(previous: Semantic?): Read {
    when (val frame = byte().toInt()) {
        WireDecoder.FULL_FRAME -> {
            require(kind() == WireKind.DOCUMENT) { "native bridge returned an invalid document tree" }
            val rootId = identity()
            val rootScope = scope()
            val content = markupList()
            require(finished) { "native bridge returned a truncated payload" }
            return Read(Semantic(content, rootId, rootScope))
        }

        WireDecoder.DELTA_FRAME -> {
            requireNotNull(previous) { "native bridge sent a delta frame with no previous read" }
            require(byte().toInt() and 0xff == WireReader.OP_SPINE) {
                "native bridge sent a delta frame that does not open with the document's spine"
            }
            val semantic = spine(previous)
            require(semantic is Semantic) { "native bridge returned an invalid document tree" }
            require(finished) { "native bridge returned a truncated payload" }
            return Read(semantic)
        }

        else -> {
            error("unsupported native wire frame $frame")
        }
    }
}

private fun WireReader.error(): ParseException {
    val code =
        when (int()) {
            1 -> ParseErrorCode.INVALID_ARGUMENT
            2 -> ParseErrorCode.ALLOCATION_FAILED
            else -> ParseErrorCode.INTERNAL
        }
    val message = requiredString()
    require(finished) { "invalid native error payload" }
    return ParseException(code, message)
}

internal class WireReader(
    private val bytes: ByteArray,
) {
    private var offset = 0
    val finished: Boolean get() = offset == bytes.size

    /**
     * THE SPINE CONTEXT (#162): set by `spine` just before it decodes the
     * rewritten node, and consumed by that node's first `markupList` -- the
     * one that reads its children -- which then reads OPS against these
     * previous children instead of a child list. Null everywhere else, so a
     * node written whole inside a delta decodes exactly as in a full frame.
     * A spine whose node never consumed it is a protocol error: the native
     * side rewrote a node that has no child list.
     */
    var spine: kotlin.collections.List<Markup>? = null

    companion object {
        /** The delta's op tags, above every kind ordinal: a tag below them IS
         * a node's kind byte. */
        const val OP_SPINE: Int = 0xfe
        const val OP_SAME: Int = 0xff
    }

    fun byte(): Byte {
        require(offset < bytes.size) { "truncated native bridge payload" }
        return bytes[offset++]
    }

    fun int(): Int {
        require(offset <= bytes.size - Int.SIZE_BYTES) { "truncated native bridge payload" }
        var value = 0
        repeat(4) { shift -> value = value or ((bytes[offset++].toInt() and 0xff) shl (shift * 8)) }
        return value
    }

    fun long(): Long {
        require(offset <= bytes.size - Long.SIZE_BYTES) { "truncated native bridge payload" }
        var value = 0L
        repeat(8) { shift -> value = value or ((bytes[offset++].toLong() and 0xff) shl (shift * 8)) }
        return value
    }

    fun string(): String? {
        val size = int()
        if (size == -1) return null
        require(size >= 0 && size <= bytes.size - offset) { "invalid native bridge string" }
        val end = offset + size
        return bytes.decodeToString(offset, end).also { offset = end }
    }

    fun requiredString(): String = requireNotNull(string()) { "missing native field" }

    fun scope(): Scope = Scope(Position(int(), int()), Position(int(), int()))

    fun identity(): Identity = Identity(int(), int())

    fun kind(): WireKind = WireKind.from(byte().toInt() and 0xff)

    fun rawTag(): Int = byte().toInt() and 0xff

    fun boolean(): Boolean =
        when (byte().toInt()) {
            0 -> false
            1 -> true
            else -> error("invalid native boolean")
        }

    fun nullableBoolean(): Boolean? =
        when (byte().toInt()) {
            -1 -> null
            0 -> false
            1 -> true
            else -> error("invalid native boolean")
        }
}
