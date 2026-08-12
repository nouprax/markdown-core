package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/** Edit replay of the shared canonical AST corpus: every per-line revision
 * must dump byte-equal to a one-shot parse of the same text, and the delta
 * must be exactly the difference between consecutive mirrors. */
class EditAstTest {
    @Test
    fun editsReplayTheManifestCorpusToDumpEqualityPerRevision() {
        assertTrue(canonicalAstCases.isNotEmpty())
        for (testCase in canonicalAstCases) {
            var document = Document("", testCase.options)
            var replayed = ""
            var previous = mapOf<ULong, ULong>()
            for (chunk in lineChunks(testCase.source)) {
                replayed += chunk
                val commit = document.edit(replayed)
                document = commit.document

                // Equivalence: the edited document dumps byte-equal to a
                // one-shot parse of the same text.
                Document(replayed, testCase.options).use { reference ->
                    assertEquals(reference.dump(), document.dump(), testCase.name)
                }

                // Delta integrity: no id is named twice, every node the delta
                // does not name kept its exact revision, and every retired id
                // is gone from the tree.
                val delta = commit.delta
                val named = delta.diffs.map { it.markup.rawValue }
                assertEquals(named.size, named.toSet().size, testCase.name)
                val namedSet = named.toSet()
                val current = mutableMapOf<ULong, ULong>()
                for (node in flatten(document)) {
                    current[node.id.rawValue] = node.revision
                    if (node.id.rawValue !in namedSet) {
                        assertEquals(previous[node.id.rawValue], node.revision, testCase.name)
                    }
                }
                for (diff in delta.diffs.filter { it.parts.isRetired }) {
                    assertNull(current[diff.markup.rawValue], testCase.name)
                }
                assertPostorder(delta, document)
                previous = current
            }
            assertEquals(testCase.expected, document.dump(), testCase.name)
            document.close()
        }
    }
}

private fun flatten(document: Document): kotlin.collections.List<Markup> {
    val nodes = mutableListOf<Markup>()
    MarkupWalker.walk(document) { event, node, _ ->
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
