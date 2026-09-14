@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import com.nouprax.markdown.core.internal.capi.markdown_core_kotlin_payload_encode
import com.nouprax.markdown.core.internal.capi.markdown_core_kotlin_payload_free
import kotlinx.cinterop.CPointerVar
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import platform.posix.size_tVar
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

/** The cinterop transport: the encoder's bytes, the decoder's document, nothing native between them. */
class NativePayloadTest {
    private fun encode(source: String): ByteArray =
        memScoped {
            val bytes = source.encodeToByteArray()
            val output = alloc<CPointerVar<UByteVar>>()
            val length = alloc<size_tVar>()
            val encoded =
                bytes.usePinned { pinned ->
                    markdown_core_kotlin_payload_encode(
                        if (bytes.isEmpty()) null else pinned.addressOf(0).reinterpret(),
                        bytes.size.toULong(),
                        output.ptr,
                        length.ptr,
                    )
                }
            assertTrue(encoded)
            val payload = requireNotNull(output.value)
            try {
                payload.readBytes(length.value.toInt())
            } finally {
                markdown_core_kotlin_payload_free(payload)
            }
        }

    @Test
    fun theEncoderAnswersThePayloadTheSharedDecoderReads() {
        val payload = encode("# T\n\n1) one\n2) two\n")
        assertEquals(listOf(0x4d, 0x4b, 0x4a, 0x31, 0), payload.take(5).map { it.toInt() })
        val document = PayloadDecoder.decode(payload)
        payload.fill(0)
        assertEquals(1, assertIs<Heading>(document.content[0]).level)
        val list = assertIs<List>(document.content[1])
        assertEquals(
            OrderedListDelimiter.Parenthesis(closed = false).closed,
            assertIs<OrderedListDelimiter.Parenthesis>(list.delimiter).closed,
        )
        assertEquals(
            listOf("one", "two"),
            list.items.map {
                (assertIs<Paragraph>(it.content.single()).content.single() as Text).literal
            },
        )
        assertEquals(document.dump(), Document.parse("# T\n\n1) one\n2) two\n").dump())
    }

    @Test
    fun anEmptySourceIsAnEmptyDocumentThroughTheSameTransport() {
        val document = PayloadDecoder.decode(encode(""))
        assertTrue(document.content.isEmpty())
        assertEquals(Scope(1, 1, 0, 0), document.scope)
    }
}
