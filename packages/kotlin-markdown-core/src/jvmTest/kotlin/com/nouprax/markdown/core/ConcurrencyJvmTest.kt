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
                    tables = false,
                    strikethrough = false,
                    autolinks = false,
                    taskLists = false,
                    formulas = false,
                    directives = false,
                    crossLinks = false,
                    embeds = false,
                ),
                ParseOptions(
                    strikethrough = false,
                    formulas = false,
                ),
            )
        val combos =
            sources.flatMap { source ->
                variants.map { options -> Triple(source, options, Document(source, options).use { it.dump() }) }
            }

        val failures = ConcurrentLinkedQueue<String>()
        val start = CountDownLatch(1)
        val threads =
            (0 until 8).map { worker ->
                thread {
                    start.await()
                    repeat(25) { iteration ->
                        val (source, options, reference) = combos[(worker + iteration) % combos.size]
                        val dump = Document(source, options).use { it.dump() }
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
    fun parallelLineagesOnExecutorThreadsStayIsolated() {
        // One lineage per worker thread, interleaved edit and read, with
        // cross-thread reads after the writer thread finished — a document is
        // an immutable value, so whatever a thread produced stays readable
        // from any other.
        val source = "# Title\n\nBody with *emphasis*, `code`, and [^n].\n\n[^n]: note\n"
        val reference = Document(source).use { it.dump() }
        val retained = ConcurrentLinkedQueue<Document>()
        val failures = ConcurrentLinkedQueue<String>()
        val start = CountDownLatch(1)
        val threads =
            (0 until 8).map { worker ->
                thread {
                    start.await()
                    var document = Document("")
                    repeat(25) { iteration ->
                        document = document.edit("").document
                        var streamed = ""
                        for (line in source.split("\n").dropLast(1)) {
                            streamed += line + "\n"
                            document = document.edit(streamed).document
                        }
                        if (document.dump() != reference) {
                            failures.add("worker $worker iteration $iteration diverged")
                        }
                    }
                    retained.add(document)
                }
            }
        start.countDown()
        threads.forEach { it.join() }
        assertTrue(failures.isEmpty(), failures.joinToString("\n"))
        // Every document answers for itself from any thread, and releasing
        // one never touches another's parse.
        assertTrue(retained.all { it.scope(it).start.line == 1 })
        retained.forEach { it.close() }
        assertTrue(retained.all { it.scope(it).start.line == 1 })
    }
}
