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
                                    // growing and clearing, so each series is
                                    // busy in the engine while the others are.
                                    var document = Document("", options)
                                    repeat(24) { round ->
                                        var streamed = ""
                                        for (character in source) {
                                            streamed += character
                                        }
                                        document = document.edit(streamed).document
                                        if (round + 1 < 24) {
                                            document = document.edit("").document
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
