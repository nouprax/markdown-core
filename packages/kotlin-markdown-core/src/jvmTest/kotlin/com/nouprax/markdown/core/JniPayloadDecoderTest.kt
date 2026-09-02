package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

private fun jniPayload(vararg parts: Any): ByteArray {
    val out = mutableListOf<Byte>()
    for (part in parts) {
        when (part) {
            is String -> out += part.encodeToByteArray().toList()
            is Byte -> out += part
            is Int -> repeat(4) { shift -> out += ((part shr (shift * 8)) and 0xff).toByte() }
            else -> error("unsupported payload part")
        }
    }
    return out.toByteArray()
}

class JniPayloadDecoderTest {
    @Test
    fun corruptedPayloadFailsInsteadOfProducingAPartialTree() {
        assertFailsWith<IllegalArgumentException> {
            JniPayloadDecoder.decodeDocument(byteArrayOf(0x4d, 0x4b, 0x4a))
        }
    }

    @Test
    fun malformedJniPayloadValuesAreRejectedBeforeTheyEnterTheAst() {
        assertFailsWith<IllegalStateException> { JniNodeKind.from(0) }
        assertFailsWith<IllegalStateException> { JniNodeKind.from(33) }
        assertEquals(JniNodeKind.IMAGE_REFERENCE, JniNodeKind.from(32))
        assertFailsWith<IllegalArgumentException> {
            JniPayloadDecoder.decodeDocument("MKJ1".encodeToByteArray())
        }
    }

    @Test
    fun failedAndStructurallyInvalidPayloadsAreRejected() {
        val failure =
            assertFailsWith<ParseException> {
                JniPayloadDecoder.decodeDocument(jniPayload("MKJ1", 1.toByte(), 1, 3, "bad"))
            }
        assertEquals(ParseErrorCode.INVALID_ARGUMENT, failure.code)
        assertEquals("bad", failure.message)
        assertEquals(
            ParseErrorCode.INTERNAL,
            assertFailsWith<ParseException> {
                JniPayloadDecoder.decodeDocument(jniPayload("MKJ1", 1.toByte(), 99, 1, "x"))
            }.code,
        )
        val allocationFailure =
            assertFailsWith<ParseException> {
                JniPayloadDecoder.decodeDocument(jniPayload("MKJ1", 1.toByte(), 2, 13, "out of memory"))
            }
        assertEquals(ParseErrorCode.ALLOCATION_FAILED, allocationFailure.code)
        assertEquals("out of memory", allocationFailure.message)

        assertFailsWith<IllegalStateException> {
            JniPayloadDecoder.decodeDocument(jniPayload("MKJ1", 2.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            JniPayloadDecoder.decodeDocument(jniPayload("MKJ0", 0.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            JniPayloadDecoder.decodeDocument(jniPayload("MKJ1", 0.toByte(), 3.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            JniPayloadDecoder.decodeDocument(jniPayload("MKJ1", 0.toByte(), 1.toByte(), 1, 1))
        }
        assertFailsWith<IllegalArgumentException> {
            JniPayloadDecoder.decodeDocument(jniPayload("MKJ1", 1.toByte(), 1, -2))
        }
    }
}
