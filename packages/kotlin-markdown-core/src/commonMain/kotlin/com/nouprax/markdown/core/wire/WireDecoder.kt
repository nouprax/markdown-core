package com.nouprax.markdown.core

internal object WireDecoder {
    /**
     * `MKC5`: an error lost its scope byte at Step 13, which deleted
     * `markdown_core_error_get_scope` -- a parse failure carries no scope.
     */
    private val magic = byteArrayOf(0x4d, 0x4b, 0x43, 0x35)

    fun decodeRead(bytes: ByteArray): Read {
        val reader = WireReader(bytes)
        reader.header()
        return reader.read()
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

/**
 * THE ROOT IS READ BY HAND, and it is the only node that is.
 *
 * A read carries the concrete view beside the tree, the wire writes that view
 * AFTER the tree, and a node is built as it is read -- so the root's own
 * fields are read here, the view after them, and the read constructed from
 * both.
 */
private fun WireReader.read(): Read {
    require(kind() == WireKind.DOCUMENT) { "native bridge returned an invalid document tree" }
    val rootScope = scope()
    val content = markupList()
    val concrete = concrete()
    require(finished) { "native bridge returned a truncated payload" }
    return Read(Semantic(content, rootScope), concrete)
}

/**
 * The source a scope's coordinates are counted against, as `write_concrete`
 * lays it out: the source length and its bytes, then the line count and one
 * offset per line.
 */
private fun WireReader.concrete(): Concrete {
    val source = bytes(int())
    val lineStarts = IntArray(count()) { int() }
    return Concrete(source, lineStarts)
}

private fun WireReader.count(): Int = int().also { require(it >= 0) { "invalid native count" } }

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

    fun bytes(size: Int): ByteArray {
        require(size >= 0 && size <= bytes.size - offset) { "invalid native byte run" }
        val end = offset + size
        return bytes.copyOfRange(offset, end).also { offset = end }
    }

    fun scope(): Scope = Scope(Position(int(), int()), Position(int(), int()))

    fun kind(): WireKind = WireKind.from(byte().toInt() and 0xff)

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
