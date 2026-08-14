package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * The chain contract through the public surface alone, the Kotlin twin of the
 * C api_test's chain section: a mutation advances the chain and supersedes
 * its receiver, history is linear, a stale mutation is a deterministic
 * refusal that disturbs nothing, and an empty append advances the chain over
 * an identical projection.
 */
class ChainContractTest {
    @Test
    fun aStaleMutationFailsDeterministicallyAndDisturbsNothing() {
        val base = Document("# Title\n\nBody\n")
        val head = base.append("More body\n")
        val expected = head.dump()

        // Every mutation of the superseded receiver refuses with the
        // engine's wording, repeatably — the refusal is deterministic, not a
        // race with anything.
        repeat(3) {
            for (refused in listOf<() -> Document>({ base.append("!") }, { base.edit("!") })) {
                val refusal = assertFailsWith<IllegalStateException>(block = refused)
                assertEquals("the document has been superseded: mutate the successor", refusal.message)
            }
        }

        // A refused mutation fails the call, never the chain: the head is
        // still live, both of its mutations still work, and neither tree
        // moved an inch in the meantime.
        assertEquals(expected, head.dump())
        assertEquals("# Title", lineOneOf(base))
        val edited = head.edit("# Title\n\nBody\nMore body\n")
        assertEquals(expected, edited.dump())
        val appended = edited.append("Tail\n")
        assertTrue(appended.dump().contains("Tail"))
        appended.close()
        edited.close()
        head.close()
        base.close()
    }

    @Test
    fun anEmptyAppendAdvancesTheChainOverAnIdenticalTree() {
        val receiver = Document("# Title\n\nBody with *emphasis*.\n")
        val successor = receiver.append("")

        // Identical projection: every node keeps its exact (id, revision)
        // pair — same chain, so the pairs are comparable — and the content
        // matches field by field.
        assertEquals(receiver.dump(), successor.dump())
        assertEquals(receiver.id, successor.id)
        assertEquals(receiver.revision, successor.revision)
        val pairs = { document: Document ->
            buildList {
                MarkupWalker.walk(document) { event, node, _ ->
                    if (event == WalkEvent.ENTERING) add(node.id to node.revision)
                }
            }
        }
        assertEquals(pairs(receiver), pairs(successor))
        assertSameForest(receiver.content, successor.content, "empty append")
        assertSameDiagnostics(receiver.diagnostics, successor.diagnostics, "empty append")

        // Yet the chain DID advance: the receiver is superseded — the
        // wrapper's view of the chain's revision moving under an unmoved
        // tree — and the successor is the head with both mutations its own.
        assertFailsWith<IllegalStateException> { receiver.append("x") }
        val extended = successor.append("\nTail\n")
        assertTrue(extended.dump().contains("Tail"))
        extended.close()
        successor.close()
        receiver.close()
    }

    @Test
    fun appendsCrossConstructBoundariesAtArbitraryCharacterCuts() {
        // Token-sized strides that ignore every construct boundary: markers
        // split from their content, the two newlines of a block boundary
        // landing in different appends. Every head must project exactly what
        // a one-shot parse of every byte so far projects.
        val text =
            "# Heading\n\n- item *emph*\n- item `code`\n\n```\nfence\n```\n\n" +
                "| a | b |\n| - | - |\n| 1 | 2 |\n"
        var document = Document("")
        var streamed = ""
        var offset = 0
        var stride = 3
        while (offset < text.length) {
            val chunk = text.substring(offset, minOf(offset + stride, text.length))
            offset += chunk.length
            stride = if (stride == 8) 3 else stride + 1
            streamed += chunk
            val superseded = document
            document = document.append(chunk)
            superseded.close()
            Document(streamed).use { reference ->
                assertEquals(reference.dump(), document.dump())
            }
        }
        val heading = assertIs<Heading>(document.content.first())
        assertEquals(1, heading.level)
        document.close()
    }

    @Test
    fun aSurrogatePairSplitAcrossAppendsReassembles() {
        val whole = "before \uD83D\uDE00 after\n"
        var streamed = Document("")
        // Split INSIDE the emoji: the high surrogate travels alone.
        val cut = whole.indexOf('\uD83D') + 1
        streamed = streamed.append(whole.substring(0, cut)).also { streamed.close() }
        streamed = streamed.append(whole.substring(cut)).also { streamed.close() }
        Document(whole).use { reference ->
            assertEquals(MarkupDumper.dump(reference), MarkupDumper.dump(streamed))
        }
        streamed.close()
    }

    @Test
    fun aHeldSurrogateIsDiscardedByEdit() {
        var head = Document("")
        head = head.append("x\uD83D").also { head.close() } // high surrogate held back
        head = head.edit("replaced\n").also { head.close() }
        Document("replaced\n").use { reference ->
            assertEquals(MarkupDumper.dump(reference), MarkupDumper.dump(head))
        }
        head.close()
    }
}

private fun lineOneOf(document: Document): String {
    val heading = assertIs<Heading>(document.content.first())
    val text = assertIs<Text>(heading.content.first())
    return "#".repeat(heading.level) + " " + text.literal
}
