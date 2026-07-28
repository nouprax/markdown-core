package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

class WalkerTraversalTest {
    @Test
    fun scopeFreeVisitorTraversesAnUnmaterializedSupersededSnapshot() {
        MarkupSession().use { session ->
            session.append("First\n\nSecond\n")
            val first = session.commit()
            session.append("\nThird\n")
            session.commit()

            // The retained snapshot never resolved scopes while it was
            // current. Its immutable tree is complete, so the structural
            // visitor overload must traverse it — and must therefore never
            // request scope materialization.
            val recording = RecordingVisitor()
            MarkupWalker.walk(first.document, recording)
            assertEquals(listOf("Document", "Paragraph", "Text", "Paragraph", "Text"), recording.visited)

            // The scopeful overload keeps the documented superseded-snapshot
            // failure.
            assertFailsWith<IllegalStateException> {
                MarkupWalker.walk(first.document) { _, _, _ -> }
            }
        }
    }

    @Test
    fun adversarialNestingWalksAndDumpsBeyondTheCallStackBudget() {
        // 3072 nested quotes overflowed the recursive walker on the default
        // JVM stack; the explicit frame stack must keep walking and the
        // delta path of an incremental commit working at 4096. Walking is
        // stack-bound but dumping is heap-bound — the canonical dump's
        // per-line prefixes make dump bytes quadratic in depth, beyond the
        // Android instrumentation heap at this depth — so full-depth
        // verification is structural and dump equality is pinned at a
        // depth whose volume every platform affords.
        val depth = 4096
        val source = "> ".repeat(depth) + "leaf\n"
        val document = Document.parse(source)

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

        MarkupSession().use { session ->
            session.append(source)
            session.commit()
            session.replace(depth * 2, depth * 2 + 4, "seed")
            val second = session.commit()
            var seedSeen = false
            var secondEvents = 0
            MarkupWalker.walk(second.document) { event, node, _ ->
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
    fun deepIncrementalCommitDumpsIdenticallyToAOneShotParse() {
        // Byte-for-byte dump equality for the deep delta path, at a depth
        // whose quadratic dump volume fits every platform's test heap.
        val depth = 512
        MarkupSession().use { session ->
            session.append("> ".repeat(depth) + "leaf\n")
            session.commit()
            session.replace(depth * 2, depth * 2 + 4, "seed")
            val second = session.commit()
            assertEquals(Document.parse("> ".repeat(depth) + "seed\n").dump(), second.document.dump())
        }
    }
}

class SnapshotMaterializationTest {
    @Test
    fun anExplicitlyMaterializedSnapshotStaysUsableAcrossCommitsAndClose() {
        val session = MarkupSession()
        val first =
            session.use {
                it.append("First\n\nSecond\n")
                val commit = it.commit()
                // The explicit contract: materialize while current, stay
                // self-contained forever after.
                commit.document.materialize()
                it.append("\nThird\n")
                it.commit()
                commit
            }
        assertEquals(
            3,
            first.document
                .scope(first.document.content[1])
                .start.line,
        )
        assertTrue(first.document.dump().contains("Paragraph"))
    }
}
