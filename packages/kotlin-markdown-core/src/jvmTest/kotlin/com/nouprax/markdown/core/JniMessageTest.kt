package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

class JniMessageTest {
    @Test
    fun aDefinitionCrossesTheJniBoundaryOnceHoweverOftenItIsReferenced() {
        // Exercise the private JNI entry point without adding a public message API.
        Document.parse("")
        val parser = Class.forName("com.nouprax.markdown.core.JniParser")
        val instance = parser.getDeclaredField("INSTANCE").apply { isAccessible = true }.get(null)
        val parse = parser.getDeclaredMethod("parsePayload", ByteArray::class.java).apply { isAccessible = true }

        fun message(
            occurrences: Int,
            value: String,
        ): ByteArray {
            val source = "[r]: /u {#$value .$value k=$value}\n\n" + "[r]\n\n".repeat(occurrences)
            return parse.invoke(instance, source.encodeToByteArray()) as ByteArray
        }
        val short = "a"
        val long = "中".repeat(1024)
        // The resource's anchor, class and record value: three strings, once.
        val attributeGrowth = 3 * (long.encodeToByteArray().size - short.encodeToByteArray().size)
        for (occurrences in listOf(1, 64, 4096)) {
            val bytes = message(occurrences, long)
            assertEquals(attributeGrowth, bytes.size - message(occurrences, short).size)
            val document = WireDecoder.decode(bytes)
            // The tree owns its values: nothing reads the message after decoding.
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
