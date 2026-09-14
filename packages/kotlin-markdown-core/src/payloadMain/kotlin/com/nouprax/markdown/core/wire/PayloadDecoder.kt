package com.nouprax.markdown.core

/**
 * Decodes the payload the C encoder in `src/native/markdown_core_kotlin_payload.c`
 * produces: the one wire every Kotlin target reads, whether the JNI bridge
 * handed it over as a `byte[]` or Kotlin/Native copied it out of C.
 */
internal object PayloadDecoder {
    /** The current payload format. */
    private val magic = byteArrayOf(0x4d, 0x4b, 0x4a, 0x31)

    fun decode(bytes: ByteArray): Document {
        val reader = PayloadReader(bytes)
        magic.forEachIndexed { index, expected ->
            val actual = reader.byte()
            require(actual == expected) {
                "invalid payload at byte $index: expected ${expected.toUByte()}, got ${actual.toUByte()}"
            }
        }
        when (reader.byte().toInt()) {
            0 -> Unit
            1 -> throw reader.error()
            else -> error("unsupported payload status")
        }
        return reader.document()
    }
}

private fun PayloadReader.error(): ParseException {
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
internal class PayloadReader(
    private val bytes: ByteArray,
) {
    private var offset = 0
    val finished: Boolean get() = offset == bytes.size

    fun byte(): Byte {
        val at = offset
        require(at < bytes.size) { "truncated payload" }
        offset = at + 1
        return bytes[at]
    }

    fun int(): Int {
        val at = offset
        require(at <= bytes.size - Int.SIZE_BYTES) { "truncated payload" }
        offset = at + Int.SIZE_BYTES
        return (bytes[at].toInt() and 0xff) or
            ((bytes[at + 1].toInt() and 0xff) shl 8) or
            ((bytes[at + 2].toInt() and 0xff) shl 16) or
            ((bytes[at + 3].toInt() and 0xff) shl 24)
    }

    fun long(): Long {
        val at = offset
        require(at <= bytes.size - Long.SIZE_BYTES) { "truncated payload" }
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
        require(size >= 0 && size <= bytes.size - at) { "invalid payload string" }
        if (size == 0) return ""
        val end = at + size
        offset = end
        return bytes.decodeToString(at, end)
    }

    fun required(): String = requireNotNull(string()) { "missing native field" }

    fun kind(): PayloadNodeKind = PayloadNodeKind.from(byte().toInt() and 0xff)

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
