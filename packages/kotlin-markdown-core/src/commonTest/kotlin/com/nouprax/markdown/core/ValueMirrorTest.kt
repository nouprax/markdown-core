package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertTrue

/**
 * The value_mirror gate: after every append in a streamed replay, the pruned
 * decode — the one [Document.append] performs, with unchanged subtrees
 * resolved from the predecessor's values through REUSE records — must equal
 * an independent FULL decode of the same projection, field by field.
 *
 * This is the only oracle that can see a reuse defect. The dump is blind to
 * some value bits (the `List.tight` class), and Kotlin `==` is (id, revision)
 * by design — exactly the pair a stale mirror value would still carry — so
 * the comparison here is content: kind, typed fields, literals, scopes, and
 * child recursion, never `==`.
 *
 * The reference rides the bridge's append path with baseline 0 on a chain of
 * its own, opened empty. A chain opened empty never mints a revision-0
 * descendant, and every later revision is strictly positive, so baseline 0
 * reuses nothing: every reference payload is a full decode, and [decodeWire]
 * proves it by refusing any REUSE record against its empty mirror.
 */
class ValueMirrorTest {
    @Test
    fun appendReplayOfTheCanonicalCorpusMatchesAFullDecodePerTick() {
        assertTrue(canonicalAstCases.isNotEmpty())
        for (testCase in canonicalAstCases) {
            replay(testCase.name, testCase.options, lineChunks(testCase.source))
        }
    }

    @Test
    fun appendReplayOfTheStreamingBurstsMatchesAFullDecodePerTick() {
        // The shared burst shapes: the corpus text and the split generator
        // are byte-identical to the Swift and ES streaming tests, and the
        // cuts land mid-word and mid-marker — the split class a per-line
        // replay is blind to.
        val splits = StreamingCorpus.Splits()
        val chunks = mutableListOf<String>()
        for (turn in StreamingCorpus.turns) {
            var offset = 0
            while (offset < turn.length) {
                val width = splits.width()
                val message = turn.substring(offset, minOf(offset + width, turn.length))
                offset += message.length
                chunks += message
                splits.draw(4L)
            }
        }
        assertTrue(chunks.size > 9)
        replay("streaming turns", ParseOptions(), chunks)
    }

    @Test
    fun aTrailingConstructAbsorbingItsTerminatorIsNeverReusedStale() {
        // The one shape that proves a revision cannot vouch for a scope: a
        // trailing construct absorbs its terminating newline into its scope
        // END without a revision bump — position is not content, so nothing
        // stamps it. The encoder must re-send the construct rather than let
        // the decoder reuse the receiver's narrower extent.
        var document = Document("# Title\n\n---")
        var streamed = "# Title\n\n---"
        for (chunk in listOf("\n", "tail\n")) {
            streamed += chunk
            val superseded = document
            document = document.append(chunk)
            superseded.close()
            Document(streamed).use { reference ->
                assertSameForest(
                    reference.content,
                    document.content,
                    "after appending ${chunk.length} trailing bytes",
                )
            }
        }
        document.close()
    }

    private fun replay(
        context: String,
        options: ParseOptions,
        chunks: kotlin.collections.List<String>,
    ) {
        var document = Document("", options)
        var reference = openReference(options)
        try {
            var mirrored = false
            chunks.forEachIndexed { index, chunk ->
                val superseded = document
                document = document.append(chunk)
                superseded.close()
                reference = reference.advance(chunk.encodeToByteArray())
                val tick = "$context, append ${index + 1}"
                assertSameForest(reference.content, document.content, tick)
                assertSameDiagnostics(reference.diagnostics, document.diagnostics, tick)
                mirrored = mirrored || document.content.any { it.revision < document.revision }
            }
            // The gate must not pass vacuously: at least one tick has to have
            // carried a subtree over from the predecessor's mirror. A block
            // whose revision is below the root's was not rebuilt this tick,
            // and on the append path that is exactly a REUSE resolution.
            assertTrue(mirrored, "$context: no append ever reused a subtree, the gate saw nothing")
        } finally {
            document.close()
            reference.release()
        }
    }
}

/** One full decode of the reference chain's head: its content, diagnostics,
 * and the native handle that produces the next one. No wrapper [Document] —
 * the reference deliberately stays on the raw bridge so its decode cannot
 * share the pruned path under test. */
private class FullDecode(
    val handle: CDocumentHandle,
    val content: kotlin.collections.List<Markup>,
    val diagnostics: kotlin.collections.List<Diagnostic>,
) {
    fun advance(chunk: ByteArray): FullDecode {
        val next =
            decodeWire(handle.append(chunk, 0u)) { successor, _, _, _, content, _, diagnostics ->
                FullDecode(successor, content, diagnostics)
            }
        // The append superseded this handle; the decode above already read
        // everything this test will ever want from it, and freeing a
        // superseded handle is an O(1) release.
        handle.release()
        return next
    }

    fun release() {
        handle.release()
    }
}

private fun openReference(options: ParseOptions): FullDecode =
    decodeWire(cOpen(ByteArray(0), options)) { handle, _, _, _, content, _, diagnostics ->
        FullDecode(handle, content, diagnostics)
    }
