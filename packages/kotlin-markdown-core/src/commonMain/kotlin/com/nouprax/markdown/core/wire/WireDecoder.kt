package com.nouprax.markdown.core

internal object WireDecoder {
    private val magic = byteArrayOf(0x4d, 0x4b, 0x43, 0x32)

    fun decodeDocument(bytes: ByteArray): Document {
        val reader = WireReader(bytes)
        magic.forEachIndexed { index, expected ->
            val actual = reader.byte()
            require(actual == expected) {
                "invalid native bridge payload at byte $index: expected ${expected.toUByte()}, got ${actual.toUByte()}"
            }
        }
        when (reader.byte().toInt()) {
            0 -> Unit
            1 -> throw reader.error()
            else -> error("unsupported native bridge status")
        }
        val root = reader.markup()
        require(reader.finished && root is Document) { "native bridge returned an invalid document tree" }
        return root
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
    val errorScope = if (boolean()) scope() else null
    require(finished) { "invalid native error payload" }
    return ParseException(code, message, errorScope)
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
