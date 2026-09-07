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
    fun aTitleIsDecodedBeforeTheContentAndDumpedAsAGroup() {
        // The title path of the wire: a node-valued list the payload sends
        // between the callout's metadata and its content. No parse produces
        // one until O8, so the payload is built by hand: a document holding
        // one expanded `note` callout whose title is the text `T` and whose
        // content is empty.
        val payload =
            jniPayload(
                "MKJ1",
                0.toByte(),
                1.toByte(),
                1,
                1,
                1,
                8,
                1,
                2.toByte(),
                1,
                1,
                1,
                8,
                4,
                "note",
                2,
                1.toByte(),
                1,
                14.toByte(),
                1,
                10,
                1,
                10,
                1,
                "T",
                0,
            )
        val document = JniPayloadDecoder.decodeDocument(payload)
        val callout = document.content.single() as Callout
        assertEquals("note", callout.variant)
        assertEquals(CalloutFold.EXPANDED, callout.fold)
        assertEquals("T", (callout.title!!.single() as Text).literal)
        assertEquals(emptyList(), callout.content)
        assertEquals(
            "Document scope=1:1..1:8 children=1\n" +
                "└── Callout scope=1:1..1:8 variant=\"note\" fold=expanded children=0\n" +
                "    └── Title children=1\n" +
                "        └── Text scope=1:10..1:10 literal=\"T\" children=0\n",
            document.dump(),
        )
        val events = mutableListOf<String>()
        callout.walk(
            object : WalkingVisitor by RecordingWalkingVisitor() {
                override fun visitCallout(
                    node: Callout,
                    phase: WalkPhase,
                ) {
                    events += "$phase:Callout"
                }

                override fun visitText(
                    node: Text,
                    phase: WalkPhase,
                ) {
                    events += "$phase:Text"
                }
            },
        )
        assertEquals(listOf("ENTERING:Callout", "ENTERING:Text", "EXITING:Text", "EXITING:Callout"), events)
    }

    @Test
    fun corruptedPayloadFailsInsteadOfProducingAPartialTree() {
        assertFailsWith<IllegalArgumentException> {
            JniPayloadDecoder.decodeDocument(byteArrayOf(0x4d, 0x4b, 0x4a))
        }
    }

    @Test
    fun malformedJniPayloadValuesAreRejectedBeforeTheyEnterTheAst() {
        assertFailsWith<IllegalStateException> { JniNodeKind.from(0) }
        assertFailsWith<IllegalStateException> { JniNodeKind.from(31) }
        assertEquals(JniNodeKind.COMMENT, JniNodeKind.from(30))
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
