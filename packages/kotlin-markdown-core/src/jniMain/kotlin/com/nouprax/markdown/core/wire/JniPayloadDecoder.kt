package com.nouprax.markdown.core

internal object JniPayloadDecoder {
    /** Current JVM/Android JNI payload format. */
    private val magic = byteArrayOf(0x4d, 0x4b, 0x4a, 0x31)

    fun decodeDocument(bytes: ByteArray): Document {
        val reader = JniPayloadReader(bytes)
        magic.forEachIndexed { index, expected ->
            val actual = reader.byte()
            require(actual == expected) {
                "invalid JNI payload at byte $index: expected ${expected.toUByte()}, got ${actual.toUByte()}"
            }
        }
        when (reader.byte().toInt()) {
            0 -> Unit
            1 -> throw reader.error()
            else -> error("unsupported JNI payload status")
        }
        return reader.decodeTree()
    }
}

private fun JniPayloadReader.error(): ParseException {
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

internal class JniPayloadReader(
    private val bytes: ByteArray,
) {
    private var offset = 0
    val finished: Boolean get() = offset == bytes.size

    fun byte(): Byte {
        require(offset < bytes.size) { "truncated JNI payload" }
        return bytes[offset++]
    }

    fun int(): Int {
        require(offset <= bytes.size - Int.SIZE_BYTES) { "truncated JNI payload" }
        var value = 0
        repeat(4) { shift -> value = value or ((bytes[offset++].toInt() and 0xff) shl (shift * 8)) }
        return value
    }

    fun long(): Long {
        require(offset <= bytes.size - Long.SIZE_BYTES) { "truncated JNI payload" }
        var value = 0L
        repeat(8) { shift -> value = value or ((bytes[offset++].toLong() and 0xff) shl (shift * 8)) }
        return value
    }

    fun string(): String? {
        val size = int()
        if (size == -1) return null
        require(size >= 0 && size <= bytes.size - offset) { "invalid JNI payload string" }
        val end = offset + size
        return bytes.decodeToString(offset, end).also { offset = end }
    }

    fun requiredString(): String = requireNotNull(string()) { "missing native field" }

    fun scope(): Scope = Scope(Position(int(), int()), Position(int(), int()))

    fun kind(): JniNodeKind = JniNodeKind.from(byte().toInt() and 0xff)

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
