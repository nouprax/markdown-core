package com.nouprax.markdown.core

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withContext
import kotlin.test.Test
import kotlin.test.assertEquals

/** Distinct sessions are fully concurrent (decision #7: zero process-global
 * state). Runs on real worker threads on every platform via
 * Dispatchers.Default: JVM executors, native workers, Android host. */
class SessionConcurrencyTest {
    @Test
    fun parallelSessionsWithDisagreeingOptionsNeverInterfere() =
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
                        stripHTMLComments = false,
                        tables = false,
                        strikethrough = false,
                        autolinks = false,
                        taskLists = false,
                        formulas = false,
                        dollarFormulaDelimiters = false,
                        latexFormulaDelimiters = false,
                        directives = false,
                    ),
                )
            // Single-threaded references computed up front.
            val expected =
                sources.flatMap { source ->
                    variants.map { options -> Document.parse(source, options).dump() }
                }

            val streamed =
                withContext(Dispatchers.Default) {
                    sources
                        .flatMap { source ->
                            variants.map { options ->
                                async {
                                    MarkupSession(options).use { session ->
                                        var commit: Commit? = null
                                        repeat(24) { round ->
                                            for (character in source) {
                                                session.append(character.toString())
                                            }
                                            commit = session.commit()
                                            if (round + 1 < 24) {
                                                session.replace(0, session.length, "")
                                                session.commit()
                                            }
                                        }
                                        requireNotNull(commit).document.dump()
                                    }
                                }
                            }
                        }.awaitAll()
                }
            assertEquals(expected, streamed)
        }
}
