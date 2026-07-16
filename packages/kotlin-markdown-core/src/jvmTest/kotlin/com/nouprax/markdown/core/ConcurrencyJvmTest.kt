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
}
