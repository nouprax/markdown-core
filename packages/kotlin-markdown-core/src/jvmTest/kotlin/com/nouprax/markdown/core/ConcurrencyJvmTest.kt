package com.nouprax.markdown.core

import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CountDownLatch
import kotlin.concurrent.thread
import kotlin.test.Test
import kotlin.test.assertTrue

class ConcurrencyJvmTest {
    @Test
    fun simultaneousParsesWithDisagreeingOptionsNeverInterfere() {
        // The engine holds no process-global state: JNI parses that disagree
        // about extension special characters ('~', '$', ':') must never
        // observe each other. Dumps are compared against single-threaded
        // references computed up front.
        val sources =
            listOf(
                "# Heading\n\nPlain *emphasis* and **strong** text with `code`.\n",
                "| a | b |\n| --- | :-: |\n| 1 | 2 |\n\n~~struck~~ and *a~b*c~ mix.\n",
                "Formula \$x^2\$ inline and *a\$b*c\$ flanking.\n\n\$\$\nx = y\n\$\$\n",
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
                ParseOptions(
                    strikethrough = false,
                    formulas = false,
                    dollarFormulaDelimiters = false,
                    latexFormulaDelimiters = false,
                ),
            )
        val combos =
            sources.flatMap { source ->
                variants.map { options -> Triple(source, options, Document.parse(source, options).dump()) }
            }

        val failures = ConcurrentLinkedQueue<String>()
        val start = CountDownLatch(1)
        val threads =
            (0 until 8).map { worker ->
                thread {
                    start.await()
                    repeat(25) { iteration ->
                        val (source, options, reference) = combos[(worker + iteration) % combos.size]
                        val dump = Document.parse(source, options).dump()
                        if (dump != reference) {
                            failures.add("worker $worker iteration $iteration produced a divergent dump")
                        }
                    }
                }
            }
        start.countDown()
        threads.forEach { it.join() }
        assertTrue(failures.isEmpty(), failures.joinToString("\n"))
    }

    @Test
    fun parallelSessionsOnExecutorThreadsStayIsolated() {
        // One session per worker thread, interleaved edit/commit/read, with
        // cross-thread snapshot reads after the writer thread finished — the
        // session contract keeps snapshots readable from any thread between
        // mutating calls.
        val source = "# Title\n\nBody with *emphasis*, `code`, and [^n].\n\n[^n]: note\n"
        val reference = Document.parse(source).dump()
        val snapshots = ConcurrentLinkedQueue<Document>()
        val failures = ConcurrentLinkedQueue<String>()
        val start = CountDownLatch(1)
        val threads =
            (0 until 8).map { worker ->
                thread {
                    start.await()
                    MarkupSession().use { session ->
                        repeat(25) { iteration ->
                            session.replace(0, session.length, "")
                            session.commit()
                            for (line in source.split("\n").dropLast(1)) {
                                session.append(line + "\n")
                                session.commit()
                            }
                            if (session.document.dump() != reference) {
                                failures.add("worker $worker iteration $iteration diverged")
                            }
                        }
                        snapshots.add(session.document)
                    }
                }
            }
        start.countDown()
        threads.forEach { it.join() }
        assertTrue(failures.isEmpty(), failures.joinToString("\n"))
        // Snapshots materialized their scopes while current; they answer
        // after their sessions closed, from any thread.
        assertTrue(snapshots.all { it.scope(it).start.line == 1 })
    }
}
