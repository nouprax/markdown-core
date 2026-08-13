package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/** Edit replay of the shared canonical AST corpus: every per-line revision
 * must dump byte-equal to a one-shot parse of the same text, and every tree
 * must keep the (id, revision) contract against a cumulative ledger — the
 * Kotlin twin of the C harness's two-snapshot double walk
 * (tests/support/edit_replay.c). */
class EditAstTest {
    @Test
    fun editsReplayTheManifestCorpusToDumpEqualityPerRevision() {
        assertTrue(canonicalAstCases.isNotEmpty())
        for (testCase in canonicalAstCases) {
            var document = Document("", testCase.options)
            var replayed = ""
            val ledger = Ledger(testCase.name)
            // The empty document seeds the ledger: every node it has is a
            // mint.
            var previous = ledger.verify(document, null)
            for (chunk in lineChunks(testCase.source)) {
                replayed += chunk
                document = document.edit(replayed)

                // Equivalence: the edited document dumps byte-equal to a
                // one-shot parse of the same text.
                Document(replayed, testCase.options).use { reference ->
                    assertEquals(reference.dump(), document.dump(), testCase.name)
                }

                previous = ledger.verify(document, previous)
            }
            assertEquals(testCase.expected, document.dump(), testCase.name)
            document.close()
        }
    }
}

/** One node as a snapshot recorded it: its own projection with the position
 * stripped (position is not content), and its children as (id, revision)
 * pairs. The subtree form of the (id, revision) promise — equal pair means an
 * equal subtree — follows from these node-local records by induction, exactly
 * as in the C harness: an unchanged parent pins its child id list, the
 * child-below-parent revision bound forces each child's revision to be an old
 * value, the two-value rule then forces it to be the child's own last
 * sighting, and the child's own record compares its projection. */
private class Sighting(
    val revision: ULong,
    val projection: String,
    val children: kotlin.collections.List<Pair<ULong, ULong>>,
)

/** The cumulative id ledger of one replay. Each verified tree must satisfy:
 * no id appears twice, ids never resurrect (at any later edit, not just the
 * next), revisions never regress, a child's revision never exceeds its
 * parent's, a minted or changed node carries the new document revision, and a
 * node that kept its last-sighting revision kept its projection and child
 * list. Absentees are retired after each walk. */
private class Ledger(
    private val context: String,
) {
    private class Entry(
        var revision: ULong,
        var alive: Boolean,
    )

    private val entries = mutableMapOf<ULong, Entry>()

    /** Walks [document] against the ledger and returns its snapshot for the
     * next round's projection comparisons. [previous] is null only for the
     * seeding walk. */
    fun verify(
        document: Document,
        previous: Map<ULong, Sighting>?,
    ): Map<ULong, Sighting> {
        val current = snapshot(document)
        // The root's revision is stamped by a change anywhere below it, so a
        // root whose revision advanced carries the new document revision —
        // and one that kept its revision means no node changed at all, in
        // which case nothing may be minted or stamped either.
        val rootEntry = entries[document.id.rawValue]
        val successorRevision =
            if (rootEntry == null || rootEntry.revision != document.revision) document.revision else null
        for ((id, sighting) in current) {
            val entry = entries[id]
            if (entry == null) {
                assertEquals(
                    successorRevision,
                    sighting.revision,
                    "$context: a minted node must carry the new document revision",
                )
                entries[id] = Entry(sighting.revision, true)
                continue
            }
            assertTrue(entry.alive, "$context: a retired id came back")
            assertTrue(sighting.revision >= entry.revision, "$context: a node's revision went backwards")
            if (sighting.revision != entry.revision) {
                assertEquals(
                    successorRevision,
                    sighting.revision,
                    "$context: a changed node must carry the new document revision",
                )
            } else {
                val before =
                    assertNotNull(
                        previous?.get(id),
                        "$context: a live ledger id is missing from the predecessor tree",
                    )
                assertEquals(
                    before.projection,
                    sighting.projection,
                    "$context: a node's projection changed without a revision bump",
                )
                assertEquals(
                    before.children,
                    sighting.children,
                    "$context: a node's child list changed without a revision bump",
                )
            }
            entry.revision = sighting.revision
        }
        // Retire the absentees: an id that left the tree never comes back.
        for ((id, entry) in entries) {
            if (entry.alive && id !in current) {
                entry.alive = false
            }
        }
        return current
    }

    /** One preorder pass recording every node, checking the per-tree rules
     * on the way: unique ids, and no child above its parent's revision. */
    private fun snapshot(document: Document): Map<ULong, Sighting> {
        // The canonical dump lists one line per node in the same preorder the
        // walker produces; stripped of the tree-drawing prefix and the scope,
        // each line is exactly the node's own position-free projection.
        val lines = document.dump().trimEnd('\n').split('\n')
        val sightings = mutableMapOf<ULong, Sighting>()
        val childLists = ArrayDeque<MutableList<Pair<ULong, ULong>>>()
        val parentRevisions = ArrayDeque<ULong>()
        val projections = ArrayDeque<String>()
        var index = 0
        MarkupWalker.walk(document) { event, node, _ ->
            when (event) {
                WalkEvent.ENTERING -> {
                    val parent = parentRevisions.lastOrNull()
                    if (parent != null) {
                        assertTrue(
                            node.revision <= parent,
                            "$context: a child's revision exceeds its parent's",
                        )
                        childLists.last() += node.id.rawValue to node.revision
                    }
                    assertNull(sightings[node.id.rawValue], "$context: one id appears twice in one tree")
                    // Reserve the slot so a duplicate deeper in the walk is
                    // still caught before its EXITING completes it.
                    sightings[node.id.rawValue] = Sighting(node.revision, "", emptyList())
                    projections.addLast(projection(lines[index]))
                    index += 1
                    parentRevisions.addLast(node.revision)
                    childLists.addLast(mutableListOf())
                }

                WalkEvent.EXITING -> {
                    parentRevisions.removeLast()
                    sightings[node.id.rawValue] =
                        Sighting(node.revision, projections.removeLast(), childLists.removeLast())
                }
            }
        }
        assertEquals(lines.size, index, context)
        return sightings
    }

    /** A dump line without the tree-drawing prefix and without the scope:
     * the node's kind and own fields, which is its projection. */
    private fun projection(line: String): String = line.substringAfter("── ").replaceFirst(scopeField, "")

    private companion object {
        val scopeField = Regex("""scope=\S+ ?""")
    }
}

private fun lineChunks(source: String): kotlin.collections.List<String> {
    val chunks = mutableListOf<String>()
    val current = StringBuilder()
    for (character in source) {
        current.append(character)
        if (character == '\n') {
            chunks += current.toString()
            current.clear()
        }
    }
    if (current.isNotEmpty()) {
        chunks += current.toString()
    }
    return chunks
}
