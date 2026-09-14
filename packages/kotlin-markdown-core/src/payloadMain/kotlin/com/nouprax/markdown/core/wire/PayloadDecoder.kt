package com.nouprax.markdown.core

internal object JniPayloadDecoder {
    /** Current JVM/Android JNI payload format. */
    private val magic = byteArrayOf(0x4d, 0x4b, 0x4a, 0x31)

    fun decode(bytes: ByteArray): Document {
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
        return reader.document()
    }
}

private fun JniPayloadReader.error(): ParseException {
    val code =
        when (int()) {
            1 -> ParseErrorCode.INVALID_ARGUMENT
            2 -> ParseErrorCode.ALLOCATION_FAILED
            else -> ParseErrorCode.INTERNAL
        }
    val message = required()
    require(finished) { "invalid native error payload" }
    return ParseException(code, message)
}

/** Little-endian fixed-width fields and length-prefixed UTF-8 strings, read in place. */
internal class JniPayloadReader(
    private val bytes: ByteArray,
) {
    private var offset = 0
    val finished: Boolean get() = offset == bytes.size

    fun byte(): Byte {
        val at = offset
        require(at < bytes.size) { "truncated JNI payload" }
        offset = at + 1
        return bytes[at]
    }

    fun int(): Int {
        val at = offset
        require(at <= bytes.size - Int.SIZE_BYTES) { "truncated JNI payload" }
        offset = at + Int.SIZE_BYTES
        return (bytes[at].toInt() and 0xff) or
            ((bytes[at + 1].toInt() and 0xff) shl 8) or
            ((bytes[at + 2].toInt() and 0xff) shl 16) or
            ((bytes[at + 3].toInt() and 0xff) shl 24)
    }

    fun long(): Long {
        val at = offset
        require(at <= bytes.size - Long.SIZE_BYTES) { "truncated JNI payload" }
        offset = at + Long.SIZE_BYTES
        val low =
            (bytes[at].toInt() and 0xff) or
                ((bytes[at + 1].toInt() and 0xff) shl 8) or
                ((bytes[at + 2].toInt() and 0xff) shl 16) or
                ((bytes[at + 3].toInt() and 0xff) shl 24)
        val high =
            (bytes[at + 4].toInt() and 0xff) or
                ((bytes[at + 5].toInt() and 0xff) shl 8) or
                ((bytes[at + 6].toInt() and 0xff) shl 16) or
                ((bytes[at + 7].toInt() and 0xff) shl 24)
        return (low.toLong() and 0xffffffffL) or (high.toLong() shl 32)
    }

    fun string(): String? {
        val size = int()
        if (size == -1) return null
        val at = offset
        require(size >= 0 && size <= bytes.size - at) { "invalid JNI payload string" }
        if (size == 0) return ""
        val end = at + size
        offset = end
        return bytes.decodeToString(at, end)
    }

    fun required(): String = requireNotNull(string()) { "missing native field" }

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
