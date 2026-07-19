package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class SessionTest {
    @Test
    fun streamingKeepsFrontierIdsAndBumpsTheTrailingTextRevision() {
        MarkupSession().use { session ->
            session.append("# Title\n\nHello")
            val first = session.commit()
            val firstHeading = assertIs<Heading>(first.document.content[0])
            val firstParagraph = assertIs<Paragraph>(first.document.content[1])
            val firstText = assertIs<Text>(firstParagraph.content[0])

            session.append(" world")
            val second = session.commit()
            val secondHeading = assertIs<Heading>(second.document.content[0])
            val secondParagraph = assertIs<Paragraph>(second.document.content[1])
            val secondText = assertIs<Text>(secondParagraph.content[0])

            assertEquals("Hello world", secondText.literal)
            assertEquals(firstParagraph.id, secondParagraph.id)
            assertEquals(firstText.id, secondText.id)
            assertTrue(secondText.revision > firstText.revision)
            assertEquals(firstHeading, secondHeading)
            assertTrue(secondParagraph.id !in second.delta.added)
            assertTrue(firstText.id !in second.delta.removed)
        }
    }

    @Test
    fun aCleanBoundaryInsertAtTheTopLeavesDownstreamIdentityIntact() {
        MarkupSession().use { session ->
            session.append("First\n\nSecond\n\nThird\n")
            val before = session.commit()
            val downstreamBefore = before.document.content.map { it.id to it.revision }

            session.replace(0, 0, "# New\n\n")
            val after = session.commit()

            assertEquals(4, after.document.content.size)
            val inserted = assertIs<Heading>(after.document.content[0])
            assertTrue(inserted.id in after.delta.added)
            after.document.content.drop(1).forEachIndexed { index, node ->
                assertEquals(downstreamBefore[index].first, node.id)
                assertEquals(downstreamBefore[index].second, node.revision)
            }
            // Downstream nodes shifted by two lines: equal values, new scopes.
            val third = assertIs<Paragraph>(after.document.content[3])
            assertEquals(
                7,
                after.document
                    .scope(third)
                    .start.line,
            )
            // An unchanged value carried over from the previous snapshot has
            // the same (id, revision) and resolves against the newer
            // snapshot — at its new position.
            val thirdBefore = assertIs<Paragraph>(before.document.content[2])
            assertEquals(
                7,
                after.document
                    .scope(thirdBefore)
                    .start.line,
            )
            assertEquals(
                Document.parse("# New\n\nFirst\n\nSecond\n\nThird\n").dump(),
                after.document.dump(),
            )
        }
    }

    @Test
    fun aKindChangeIsReportedAsRemovedPlusAdded() {
        MarkupSession().use { session ->
            session.append("text\n")
            val before = session.commit()
            val paragraph = assertIs<Paragraph>(before.document.content[0])

            session.replace(0, 0, "# ")
            val after = session.commit()
            val heading = assertIs<Heading>(after.document.content[0])

            assertTrue(paragraph.id in after.delta.removed)
            assertTrue(heading.id in after.delta.added)
            assertNotEquals(paragraph.id, heading.id)
        }
    }

    @Test
    fun equalityIsLineageSaltedIdentityPlusRevision() {
        val source = "Same *content* twice.\n"
        val first = Document.parse(source)
        val second = Document.parse(source)
        // Identical content from different parses never compares equal.
        assertNotEquals<Markup>(first, second)
        assertNotEquals<Markup>(first.content[0], second.content[0])
        // Within one snapshot, identity is value equality.
        assertEquals(first.content[0], first.content[0])
        assertNotEquals(first.id.lineage, second.id.lineage)
    }

    @Test
    fun aBlankLineOnlyEditCommitsAnEmptyDeltaYetShiftsScopes() {
        MarkupSession().use { session ->
            session.append("Alpha\n\n\n\nOmega\n")
            val before = session.commit()
            val omegaBefore = assertIs<Paragraph>(before.document.content[1])
            assertEquals(
                5,
                before.document
                    .scope(omegaBefore)
                    .start.line,
            )

            // Delete two of the blank lines: no node's content changes.
            session.replace(6, 8, "")
            val after = session.commit()
            val omegaAfter = assertIs<Paragraph>(after.document.content[1])

            assertTrue(after.delta.added.isEmpty())
            assertTrue(after.delta.removed.isEmpty())
            assertTrue(after.delta.changed.isEmpty())
            assertTrue(after.delta.bubbled.isEmpty())
            assertEquals(omegaBefore, omegaAfter)
            assertEquals(
                3,
                after.document
                    .scope(omegaAfter)
                    .start.line,
            )
            assertEquals(Document.parse("Alpha\n\nOmega\n").dump(), after.document.dump())
        }
    }

    @Test
    fun materializedScopesSurviveTheSessionAndLaterCommits() {
        val session = MarkupSession()
        session.append("One\n\nTwo\n")
        val first = session.commit()
        val two = assertIs<Paragraph>(first.document.content[1])
        // Materialize while current.
        assertEquals(
            3,
            first.document
                .scope(two)
                .start.line,
        )

        session.replace(0, 0, "Zero\n\n")
        session.commit()
        // The superseded snapshot answers from its cache, at its revision.
        assertEquals(
            3,
            first.document
                .scope(two)
                .start.line,
        )

        session.close()
        assertEquals(
            3,
            first.document
                .scope(two)
                .start.line,
        )
    }

    @Test
    fun aSupersededSnapshotThatNeverMaterializedFailsInsteadOfGuessing() {
        MarkupSession().use { session ->
            session.append("One\n")
            val first = session.commit()
            session.append("\nTwo\n")
            session.commit()
            assertFailsWith<IllegalStateException> {
                first.document.scope(first.document.content[0])
            }
        }
    }

    @Test
    fun footnoteQueriesAnswerNumberingResolutionAndBackReferences() {
        MarkupSession().use { session ->
            session.append("See [^b] then [^a].\n\n[^a]: A\n\n[^b]: B\n")
            val commit = session.commit()

            val footnotes = session.footnotes()
            assertEquals(listOf("b", "a"), footnotes.map { it.label })

            val definitionA = footnotes.last()
            val infoA = requireNotNull(session.footnote(definitionA.id))
            assertEquals(2, infoA.number)
            assertEquals(definitionA.id, infoA.definition)
            assertEquals(1, infoA.referenceCount)
            assertNull(infoA.referenceOrdinal)

            val references = session.references(definitionA.id)
            assertEquals(listOf("a"), references.map { it.label })
            val referenceInfo = requireNotNull(session.footnote(references[0].id))
            assertEquals(2, referenceInfo.number)
            assertEquals(1, referenceInfo.referenceOrdinal)

            // A non-footnote id answers null.
            assertNull(session.footnote(commit.document.id))

            // An ordinal shift surfaces as changed entries with identical
            // dump content.
            session.replace(0, 0, "Lead [^a].\n\n")
            val shifted = session.commit()
            assertEquals(listOf("a", "b"), session.footnotes().map { it.label })
            assertTrue(shifted.delta.changed.isNotEmpty())
        }
    }

    @Test
    fun conflatedStreamingWithIrregularTicksOverAMultiTurnConversation() {
        // The shape of a real LLM consumer: every socket message appends
        // (nothing parses), only an irregular render tick commits, and the
        // messages between ticks conflate into that one commit. Three
        // assistant turns extend one document; blocks settled at a turn
        // boundary must stay frozen while later turns stream.
        val turns =
            listOf(
                "# Streaming\n\nThe *quick* parser holds **steady** under bursts, " +
                    "and the heading keeps its identity from the first render on.",
                "\n\n- append per message\n- commit per tick\n- settled blocks stay frozen" +
                    "\n\n```swift\nlet constant = 1\n```",
                "\n\nA table lands in one turn:\n\n| a | b |\n| - | - |\n| 1 | 2 |" +
                    "\n\nTail with a footnote[^n].\n\n[^n]: Resolved at the end.",
            )
        // One fixed generator drives message widths and tick timing, so the
        // burst shapes are irregular but reproducible — and identical in the
        // Swift and ES mirrors of this test.
        var state = 0x9E3779B97F4A7C15UL.toLong()
        fun draw(bound: Long): Long {
            state = state * 6364136223846793005L + 1442695040888963407L
            return (state ushr 33) % bound
        }

        MarkupSession().use { session ->
            var streamed = ""
            var frozen = emptyList<Triple<Int, MarkupID, ULong>>()
            var messages = 0
            var commits = 0
            fun tick() {
                val commit = session.commit()
                commits += 1
                assertEquals(Document.parse(streamed).dump(), commit.document.dump())
                for ((index, id, revision) in frozen) {
                    val node = commit.document.content[index]
                    assertEquals(id, node.id)
                    assertEquals(revision, node.revision)
                }
            }

            for (turn in turns) {
                var offset = 0
                while (offset < turn.length) {
                    val width = 2 + draw(29L).toInt()
                    val message = turn.substring(offset, minOf(offset + width, turn.length))
                    offset += message.length
                    session.append(message)
                    streamed += message
                    messages += 1
                    if (draw(4L) == 0L) {
                        tick()
                    }
                }
                // The turn boundary always renders; everything but the
                // still-hot last block is now settled.
                tick()
                frozen =
                    session.document.content.dropLast(1).mapIndexed { index, node ->
                        Triple(index, node.id, node.revision)
                    }
            }
            assertTrue(messages > 12)
            assertTrue(commits < messages)
            assertEquals(
                Document.parse(turns.joinToString(separator = "")).dump(),
                session.document.dump(),
            )
        }
    }

    @Test
    fun snapshotsAreValuesAndIdsAreStableDictionaryKeys() {
        MarkupSession().use { session ->
            assertTrue(session.document.content.isEmpty())
            assertEquals(0UL, session.revision)
            session.append("Alpha\n")
            val commit = session.commit()
            assertEquals(1UL, session.revision)
            assertEquals(6, session.length)
            val paragraph = assertIs<Paragraph>(commit.document.content[0])
            val byId = hashMapOf(paragraph.id to "paragraph")
            assertEquals(paragraph.id, session.node(paragraph.id)?.id)
            assertEquals("paragraph", byId[paragraph.id])
            assertNull(session.node(MarkupID(session.lineage + 1UL, paragraph.id.rawValue)))
        }
    }

    @Test
    fun aClosedSessionKeepsSnapshotsButRefusesNewWork() {
        val session = MarkupSession()
        session.append("Alpha\n")
        val commit = session.commit()
        commit.document.scope(commit.document.content[0])
        session.close()
        session.close()
        assertEquals("Alpha", assertIs<Text>(assertIs<Paragraph>(commit.document.content[0]).content[0]).literal)
        assertEquals(
            1,
            commit.document
                .scope(commit.document.content[0])
                .start.line,
        )
        assertFailsWith<IllegalStateException> { session.append("beta") }
        assertFailsWith<IllegalStateException> { session.commit() }
        assertFailsWith<IllegalStateException> { session.footnotes() }
    }

    @Test
    fun invalidEditRangesAreRejected() {
        MarkupSession().use { session ->
            assertFailsWith<IllegalArgumentException> { session.replace(2, 1, "x") }
            session.append("abc")
            assertFailsWith<ParseException> { session.replace(1, 9, "x") }
            // The session stays usable after a rejected edit.
            session.commit()
            assertEquals(Document.parse("abc").dump(), session.document.dump())
        }
    }
}
