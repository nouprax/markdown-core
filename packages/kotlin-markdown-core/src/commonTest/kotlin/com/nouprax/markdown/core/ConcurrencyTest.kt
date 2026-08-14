package com.nouprax.markdown.core

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withContext
import kotlin.test.Test
import kotlin.test.assertEquals

/** Distinct documents are fully concurrent (decision #7: zero process-global
 * state). Runs on real worker threads on every platform via
 * Dispatchers.Default: JVM executors, native workers, Android host. */
class ConcurrencyTest {
    @Test
    fun parallelSeriesWithDisagreeingOptionsNeverInterfere() =
        runTest {
            val sources =
                listOf(
                    "# Heading\n\nPlain *emphasis* and **strong** text with `code`.\n",
                    "| a | b |\n| --- | :-: |\n| 1 | 2 |\n\n~~struck~~ and *a~b*c~ mix.\n",
                    "Formula \$x^2\$ inline and *a\$b*c\$ flanking.\n\nSee [^n].\n\n[^n]: note\n",
                    ":::note[Label]{id=1 title=\"T\"}\ncontent *here*\n:::\n\nInline :dir[text]{k=v} tail.\n",
                )
            val variants =
                listOf(
                    ParseOptions(),
                    ParseOptions(
                        smartPunctuation = false,
                        footnotes = false,
                        tables = false,
                        strikethrough = false,
                        autolinks = false,
                        taskLists = false,
                        formulas = false,
                        directives = false,
                        crossLinks = false,
                        embeds = false,
                    ),
                )
            // Single-threaded references computed up front.
            val expected =
                sources.flatMap { source ->
                    variants.map { options -> Document(source, options).use { it.dump() } }
                }

            val streamed =
                withContext(Dispatchers.Default) {
                    sources
                        .flatMap { source ->
                            variants.map { options ->
                                async {
                                    // The same text arrived at by 24 rounds of
                                    // growing and clearing, so each chain is
                                    // busy in the engine while the others are.
                                    // A mutation supersedes its receiver, so
                                    // the loop follows the chain and closes
                                    // every predecessor behind itself — an
                                    // O(1) release each, and the old
                                    // leak-tolerance reason is gone. Clearing
                                    // is a fresh parse: whole-text replacement
                                    // is exactly a new document.
                                    fun advance(
                                        previous: Document,
                                        next: Document.() -> Document,
                                    ): Document = previous.next().also { previous.close() }

                                    var document = Document("", options)
                                    repeat(24) { round ->
                                        // Growing rides the real append, in
                                        // small uneven slices that ignore
                                        // construct boundaries.
                                        var offset = 0
                                        while (offset < source.length) {
                                            val width = minOf(5 + (offset + round) % 7, source.length - offset)
                                            val chunk = source.substring(offset, offset + width)
                                            offset += width
                                            document = advance(document) { append(chunk) }
                                        }
                                        if (round + 1 < 24) {
                                            document.close()
                                            document = Document("", options)
                                        }
                                    }
                                    document.use { it.dump() }
                                }
                            }
                        }.awaitAll()
                }
            assertEquals(expected, streamed)
        }
}
