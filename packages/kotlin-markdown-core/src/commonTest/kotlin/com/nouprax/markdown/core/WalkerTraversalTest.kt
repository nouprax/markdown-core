package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class WalkerTraversalTest {
    @Test
    fun bothVisitorOverloadsTraverseASupersededDocument() {
        val first = Document("First\n\nSecond\n")
        first.edit("First\n\nSecond\n\nThird\n").document.close()

        // Editing left the predecessor's parse alone and the successor's is
        // now released. Both overloads still traverse the predecessor,
        // because its tree and its scopes were copied out at parse time.
        val recording = RecordingVisitor()
        MarkupWalker.walk(first, recording)
        assertEquals(listOf("Document", "Paragraph", "Text", "Paragraph", "Text"), recording.visited)

        var scopeful = 0
        MarkupWalker.walk(first) { event, _, scope ->
            if (event == WalkEvent.ENTERING) {
                scopeful += 1
                assertTrue(scope.start.line >= 1)
            }
        }
        assertEquals(5, scopeful)
    }

    @Test
    fun adversarialNestingWalksAndDumpsBeyondTheCallStackBudget() {
        // 3072 nested quotes overflowed the recursive walker on the default
        // JVM stack; the explicit frame stack must keep walking and the
        // delta path of an edit working at 4096. Walking is
        // stack-bound but dumping is heap-bound — the canonical dump's
        // per-line prefixes make dump bytes quadratic in depth, beyond the
        // Android instrumentation heap at this depth — so full-depth
        // verification is structural and dump equality is pinned at a
        // depth whose volume every platform affords.
        val depth = 4096
        val source = "> ".repeat(depth) + "leaf\n"
        val document = Document(source)

        var quoteEnters = 0
        var events = 0
        MarkupWalker.walk(document) { event, node, _ ->
            events += 1
            if (event == WalkEvent.ENTERING && node is BlockQuote) {
                quoteEnters += 1
            }
        }
        assertEquals(depth, quoteEnters)
        // Every node enters exactly once and exits exactly once: the
        // document, the quote chain, and the innermost paragraph and text.
        assertEquals(2 * (depth + 3), events)

        val structural = RecordingVisitor()
        MarkupWalker.walk(document, structural)
        assertEquals(depth + 3, structural.visited.size)

        document.edit("> ".repeat(depth) + "seed\n").document.use { second ->
            var seedSeen = false
            var secondEvents = 0
            MarkupWalker.walk(second) { event, node, _ ->
                secondEvents += 1
                if (event == WalkEvent.ENTERING && node is Text) {
                    seedSeen = node.literal == "seed"
                }
            }
            assertEquals(2 * (depth + 3), secondEvents)
            assertTrue(seedSeen)
        }
    }

    @Test
    fun aDeepEditDumpsIdenticallyToAOneShotParse() {
        // Byte-for-byte dump equality for the deep delta path, at a depth
        // whose quadratic dump volume fits every platform's test heap.
        val depth = 512
        Document("> ".repeat(depth) + "leaf\n").edit("> ".repeat(depth) + "seed\n").document.use { second ->
            assertEquals(Document("> ".repeat(depth) + "seed\n").use { it.dump() }, second.dump())
        }
    }
}

class ScopeOwnershipTest {
    @Test
    fun aDocumentAnswersScopesAfterEveryOtherDocumentIsGone() {
        val retained: Document
        run {
            val first = Document("First\n\nSecond\n")
            first.edit("First\n\nSecond\n\nThird\n").document.close()
            retained = first
        }
        // There is nothing to materialize: a node carries its own scope from
        // parse time, so it answers whatever happened to its series since.
        assertEquals(
            3,
            retained.content[1]
                .scope.start.line,
        )
        assertTrue(retained.dump().contains("Paragraph"))
    }
}
