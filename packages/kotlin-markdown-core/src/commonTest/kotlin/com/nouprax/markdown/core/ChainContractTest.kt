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
            val refusal = assertFailsWith<IllegalStateException> { base.append("!") }
            assertEquals("the document has been superseded: mutate the successor", refusal.message)
        }

        // A refused mutation fails the call, never the chain: the head is
        // still live, its mutation still works, and neither tree moved an
        // inch in the meantime.
        assertEquals(expected, head.dump())
        assertEquals("# Title", lineOneOf(base))
        // The settled heading is the same value on both heads: equality is
        // the (id, revision) pair, and the append did not reach it.
        assertEquals<Markup>(base.content.first(), head.content.first())
        val appended = head.append("Tail\n")
        assertTrue(appended.dump().contains("Tail"))
        appended.close()
        head.close()
        base.close()
    }

    @Test
    fun aClosedDocumentRefusesMutationAndKeepsItsValues() {
        val closed = Document("One\n")
        closed.close()
        // Closing twice is a no-op; mutating after it is not, and the closed
        // refusal is its own message — closing is the caller's act, not a
        // mutation's.
        closed.close()
        val refusal = assertFailsWith<IllegalStateException> { closed.append("Three\n") }
        assertEquals("the document is closed", refusal.message)
        // What was already extracted stays answerable either way.
        assertEquals(
            1,
            closed.content[0]
                .scope.start.line,
        )
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
    fun streamingWithIrregularTicksOverAMultiTurnConversation() {
        // The shape of a real LLM consumer: every socket message rides the
        // real append — the streaming mutation — and an irregular render
        // tick reads what the appends produced. Three assistant turns extend
        // one document; blocks settled at a turn boundary must stay frozen
        // while later turns stream. The corpus text and the split generator
        // are the shared burst shapes, byte-identical in the Swift and ES
        // mirrors of this test.
        val splits = StreamingCorpus.Splits()

        var document = Document("")
        var streamed = ""
        var frozen = emptyList<Triple<Int, MarkupID, ULong>>()
        var messages = 0
        var ticks = 0
        var touched = 0

        fun tick() {
            ticks += 1
            // A tick no longer parses anything: the appends already did, and
            // the tick verifies their sum against a one-shot parse of every
            // byte so far.
            Document(streamed).use { reference ->
                assertEquals(reference.dump(), document.dump())
            }
            for ((index, id, revision) in frozen) {
                val node = document.content[index]
                assertEquals(id, node.id)
                assertEquals(revision, node.revision)
            }
        }

        for (turn in StreamingCorpus.turns) {
            var offset = 0
            while (offset < turn.length) {
                val width = splits.width()
                val message = turn.substring(offset, minOf(offset + width, turn.length))
                offset += message.length
                streamed += message
                messages += 1

                // Each message advances the chain; the superseded predecessor
                // is closed behind it — an O(1) release.
                val predecessorRevision = document.revision
                val superseded = document
                document = document.append(message)
                superseded.close()

                // The old delta row count, re-expressed on the tree: the
                // nodes whose revision equals the new document revision are
                // exactly the ones this append changed or minted — and a root
                // that kept its revision means the append changed nothing.
                if (document.revision != predecessorRevision) {
                    val stamped = document.revision
                    MarkupWalker.walk(document) { event, node, _ ->
                        if (event == WalkEvent.ENTERING && node.revision == stamped) {
                            touched += 1
                        }
                    }
                }
                if (splits.draw(4L) == 0L) {
                    tick()
                }
            }
            // The turn boundary always renders; everything but the still-hot
            // last block is now settled.
            tick()
            frozen =
                document.content.dropLast(1).mapIndexed { index, node ->
                    Triple(index, node.id, node.revision)
                }
        }
        assertTrue(messages > 9)
        assertTrue(ticks < messages)
        assertEquals(
            Document(StreamingCorpus.turns.joinToString(separator = "")).use { it.dump() },
            document.dump(),
        )

        // Near-O(n) pipeline: the total count of nodes stamped with a new
        // document revision stays within one per final node plus bounded
        // frontier churn per append. A full re-decode per append would be on
        // the order of messages * nodes — which is exactly the term the
        // pruned decode deletes.
        var nodes = 0
        MarkupWalker.walk(document) { event, _, _ ->
            if (event == WalkEvent.ENTERING) {
                nodes += 1
            }
        }
        assertTrue(touched < nodes + 16 * messages)
        document.close()
    }
}

private fun lineOneOf(document: Document): String {
    val heading = assertIs<Heading>(document.content.first())
    val text = assertIs<Text>(heading.content.first())
    return "#".repeat(heading.level) + " " + text.literal
}
