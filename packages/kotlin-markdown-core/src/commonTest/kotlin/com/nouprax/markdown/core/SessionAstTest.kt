package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/** Session replay of the shared canonical AST corpus: every per-line commit
 * must dump byte-equal to a one-shot parse of the same prefix, and the
 * delta must be exactly the difference between consecutive mirrors. */
class SessionAstTest {
    @Test
    fun sessionsReplayTheManifestCorpusToDumpEqualityPerCommit() {
        assertTrue(canonicalAstCases.isNotEmpty())
        for (testCase in canonicalAstCases) {
            MarkupSession(testCase.options).use { session ->
                var replayed = ""
                var previous = mapOf<ULong, ULong>()
                for (chunk in lineChunks(testCase.source)) {
                    replayed += chunk
                    session.append(chunk)
                    val commit = session.commit()

                    // Equivalence: the incremental snapshot dumps byte-equal
                    // to a one-shot parse of the same prefix.
                    val reference = Document.parse(replayed, testCase.options)
                    assertEquals(reference.dump(), commit.document.dump(), testCase.name)

                    // Delta-mirror integrity: the four arrays are disjoint,
                    // every node outside the delta kept its exact revision,
                    // and removed ids are gone.
                    val delta = commit.delta
                    val touched =
                        (delta.added + delta.changed + delta.bubbled).map { it.rawValue }
                    val removed = delta.removed.map { it.rawValue }
                    assertEquals(
                        touched.size + removed.size,
                        (touched + removed).toSet().size,
                        testCase.name,
                    )
                    val touchedSet = touched.toSet()
                    val current = mutableMapOf<ULong, ULong>()
                    for (node in flatten(commit.document)) {
                        current[node.id.rawValue] = node.revision
                        if (node.id.rawValue !in touchedSet) {
                            assertEquals(previous[node.id.rawValue], node.revision, testCase.name)
                        }
                    }
                    for (rawValue in removed) {
                        assertNull(current[rawValue], testCase.name)
                    }
                    previous = current
                }
                assertEquals(testCase.expected, session.document.dump(), testCase.name)
            }
        }
    }
}

private fun flatten(document: Document): kotlin.collections.List<Markup> {
    val nodes = mutableListOf<Markup>()
    Walker.walk(document) { event, node, _ ->
        if (event == WalkEvent.ENTERING) {
            nodes += node
        }
    }
    return nodes
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
