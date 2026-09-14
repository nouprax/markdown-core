package com.nouprax.markdown.core.benchmark

import com.nouprax.markdown.core.Callout
import com.nouprax.markdown.core.Citation
import com.nouprax.markdown.core.Cite
import com.nouprax.markdown.core.Code
import com.nouprax.markdown.core.CodeBlock
import com.nouprax.markdown.core.Comment
import com.nouprax.markdown.core.CrossEmbedded
import com.nouprax.markdown.core.CrossLink
import com.nouprax.markdown.core.Definition
import com.nouprax.markdown.core.DefinitionList
import com.nouprax.markdown.core.Directive
import com.nouprax.markdown.core.DirectiveBlock
import com.nouprax.markdown.core.DirectiveLabel
import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.Embedded
import com.nouprax.markdown.core.Emphasis
import com.nouprax.markdown.core.Footnote
import com.nouprax.markdown.core.Formula
import com.nouprax.markdown.core.FormulaBlock
import com.nouprax.markdown.core.HTML
import com.nouprax.markdown.core.HTMLBlock
import com.nouprax.markdown.core.Heading
import com.nouprax.markdown.core.Insertion
import com.nouprax.markdown.core.LineBreak
import com.nouprax.markdown.core.Link
import com.nouprax.markdown.core.List
import com.nouprax.markdown.core.ListItem
import com.nouprax.markdown.core.Mark
import com.nouprax.markdown.core.MarkupVisitPhase
import com.nouprax.markdown.core.MarkupVisitor
import com.nouprax.markdown.core.Metadata
import com.nouprax.markdown.core.Paragraph
import com.nouprax.markdown.core.SoftBreak
import com.nouprax.markdown.core.Span
import com.nouprax.markdown.core.Specimen
import com.nouprax.markdown.core.Strikethrough
import com.nouprax.markdown.core.Strong
import com.nouprax.markdown.core.Subscript
import com.nouprax.markdown.core.Superscript
import com.nouprax.markdown.core.Table
import com.nouprax.markdown.core.TableCaption
import com.nouprax.markdown.core.TableCell
import com.nouprax.markdown.core.TableRow
import com.nouprax.markdown.core.Text
import com.nouprax.markdown.core.ThematicBreak
import com.nouprax.markdown.core.walk
import java.io.File
import java.security.MessageDigest

/*
 * The Kotlin/JVM timing lane: source string to value tree through the
 * public entry, on the workloads the C timing lane reads, so the two
 * measure the same bytes. Opt-in, informational, never a gate:
 * `pnpm benchmark:kotlin`, or
 * `scripts/gradle.sh :packages:kotlin-markdown-core:jvmBenchmark -PbenchmarkArgs="--repeats 50"`.
 *
 * Each case reports the parse (source in, tree out) and a walk of the tree
 * with an empty visitor, the minimum and the median of every repeat, the
 * throughput from the bytes and the time per node, and the SHA-256 of the
 * input so a measurement can prove which bytes it read.
 */

/** Every kind's callback counts and nothing else: the walk measures the traversal and the dispatch. */
private class CountingVisitor : MarkupVisitor {
    var visits = 0L

    override fun visit(
        document: Document,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        callout: Callout,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        paragraph: Paragraph,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        heading: Heading,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        list: List,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        listItem: ListItem,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        codeBlock: CodeBlock,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        table: Table,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        tableCaption: TableCaption,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        tableRow: TableRow,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        tableCell: TableCell,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        text: Text,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        softBreak: SoftBreak,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        lineBreak: LineBreak,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        code: Code,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        html: HTML,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        comment: Comment,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        crossLink: CrossLink,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        formula: Formula,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        emphasis: Emphasis,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        strong: Strong,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        strikethrough: Strikethrough,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        mark: Mark,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        insertion: Insertion,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        span: Span,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        superscript: Superscript,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        subscript: Subscript,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        definitionList: DefinitionList,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        definition: Definition,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        link: Link,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        embedded: Embedded,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        directive: Directive,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        cite: Cite,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        citation: Citation,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        footnote: Footnote,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        specimen: Specimen,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }

    override fun visit(
        metadata: Metadata,
        phase: MarkupVisitPhase,
    ) {
        visits++
    }
}

private class Case(
    val name: String,
    val generator: String,
    val parameters: String,
    val source: String,
)

private class Sample(
    val parseNs: Long,
    val walkNs: Long,
)

private const val BASELINE_UNIT = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"

private fun sha256(text: String): String =
    MessageDigest.getInstance("SHA-256").digest(text.encodeToByteArray()).joinToString("") { "%02x".format(it) }

private fun median(values: kotlin.collections.List<Long>): Long = values.sorted()[values.size / 2]

private fun escape(value: String): String =
    buildString {
        for (character in value) {
            when (character) {
                '"' -> append("\\\"")
                '\\' -> append("\\\\")
                '\n' -> append("\\n")
                else -> append(character)
            }
        }
    }

fun main(arguments: Array<String>) {
    var samples = File("packages/markdown-core/benchmarks/samples")
    var copies = 200
    var repeats = 20
    var warmup = 3
    var only: String? = null
    var json: File? = null
    val files = mutableListOf<File>()
    var index = 0

    fun value(): String {
        index++
        require(index < arguments.size) { "${arguments[index - 1]} needs a value" }
        return arguments[index]
    }
    while (index < arguments.size) {
        when (val argument = arguments[index]) {
            "--samples" -> samples = File(value())
            "--copies" -> copies = value().toInt()
            "--repeats" -> repeats = value().toInt()
            "--warmup" -> warmup = value().toInt()
            "--case" -> only = value()
            "--json" -> json = File(value())
            else -> files += File(argument)
        }
        index++
    }

    // The same generators as `tests/support/bench_workloads.c`, byte for
    // byte: the baseline unit repeated, and a tracked sample repeated with a
    // blank line between copies so the repeated shape is the sample's shape.
    val cases = mutableListOf<Case>()
    cases += Case("binding_baseline", "binding_baseline", "scale=2000", BASELINE_UNIT.repeat(2000))
    cases += Case("empty_document", "empty_document", "", "")
    for (sample in (samples.listFiles() ?: emptyArray()).filter { it.name.endsWith(".md") }.sortedBy { it.name }) {
        cases += Case(sample.name, "sample", "copies=$copies", (sample.readText() + "\n\n").repeat(copies))
    }
    for (file in files) cases += Case(file.name, "file", file.path, file.readText())

    val results = mutableListOf<String>()
    for (case in cases) {
        if (only != null && case.name != only) continue
        val bytes = case.source.encodeToByteArray().size
        val visitor = CountingVisitor()
        repeat(warmup) { Document.parse(case.source).walk(visitor) }
        val measured = mutableListOf<Sample>()
        var nodes = 0L
        repeat(repeats) {
            val parseStart = System.nanoTime()
            val document = Document.parse(case.source)
            val parseEnd = System.nanoTime()
            visitor.visits = 0
            document.walk(visitor)
            val walkEnd = System.nanoTime()
            nodes = visitor.visits / 2
            measured += Sample(parseEnd - parseStart, walkEnd - parseEnd)
        }
        val minParse = measured.minOf { it.parseNs }
        val minWalk = measured.minOf { it.walkNs }
        val medianParse = median(measured.map { it.parseNs })
        val medianWalk = median(measured.map { it.walkNs })
        val mbPerSecond = if (minParse > 0) "%.3f".format(bytes / 1e6 / (minParse / 1e9)) else "0"
        val nsPerNode = if (nodes > 0) "%.3f".format(minParse.toDouble() / nodes) else "0"
        val digest = sha256(case.source)
        println(
            "benchmark case=${case.name} bytes=$bytes nodes=$nodes repeats=$repeats warmup=$warmup " +
                "min_parse_ns=$minParse median_parse_ns=$medianParse min_walk_ns=$minWalk median_walk_ns=$medianWalk " +
                "mb_per_s=$mbPerSecond ns_per_node=$nsPerNode sha256=$digest",
        )
        results +=
            """    {
      "name": "${escape(case.name)}",
      "generator": "${case.generator}",
      "parameters": "${escape(case.parameters)}",
      "bytes": $bytes,
      "inputSha256": "$digest",
      "nodes": $nodes,
      "minParseNs": $minParse,
      "medianParseNs": $medianParse,
      "minWalkNs": $minWalk,
      "medianWalkNs": $medianWalk,
      "mbPerSecond": $mbPerSecond,
      "nsPerNode": $nsPerNode,
      "samples": [${measured.joinToString(", ") { "{\"parseNs\": ${it.parseNs}, \"walkNs\": ${it.walkNs}}" }}]
    }"""
    }
    json?.writeText(
        """{
  "schema": 2,
  "runtime": "kotlin-jvm",
  "lane": "timing",
  "workload": "binding",
  "workloadVersion": 1,
  "warmup": $warmup,
  "repeats": $repeats,
  "cases": [
${results.joinToString(",\n")}
  ]
}
""",
    )
}
