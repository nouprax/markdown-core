package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

/** The JNI transport: the bytes the bridge hands over are the encoder's, unchanged. */
class JniTransportTest {
    @Test
    fun definitionAttributePayloadGrowthIsIndependentOfOccurrences() {
        // Exercise the private JNI entry point without adding a public payload API.
        Document.parse("")
        val parser = Class.forName("com.nouprax.markdown.core.JniParser")
        val instance = parser.getDeclaredField("INSTANCE").apply { isAccessible = true }.get(null)
        val parse = parser.getDeclaredMethod("parsePayload", ByteArray::class.java).apply { isAccessible = true }

        fun payload(
            occurrences: Int,
            value: String,
        ): ByteArray {
            val source = "[r]: /u {#$value .$value k=$value}\n\n" + "[r]\n\n".repeat(occurrences)
            return parse.invoke(instance, source.encodeToByteArray()) as ByteArray
        }
        val short = "a"
        val long = "中".repeat(1024)
        val attributeGrowth = 3 * (long.encodeToByteArray().size - short.encodeToByteArray().size)
        for (occurrences in listOf(1, 64, 4096)) {
            val bytes = payload(occurrences, long)
            assertEquals(attributeGrowth, bytes.size - payload(occurrences, short).size)
            val document = PayloadDecoder.decode(bytes)
            bytes.fill(0)
            val links = document.content.map { assertIs<Link>(assertIs<Paragraph>(it).content.single()) }
            assertEquals(occurrences, links.size)
            assertTrue(
                links.all {
                    it.anchor == long && it.attributes.records
                        .single()
                        .value == long
                },
            )
        }
    }
}
