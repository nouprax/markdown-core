package com.nouprax.markdown.core.benchmark

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.MarkupSession
import java.io.File
import kotlin.system.measureNanoTime

private const val SCALING_WARMUP = 2
private const val SCALING_REPEATS = 3
private const val SCALING_SAMPLE_FLOOR_NS = 100_000_000L

// The depths grow 8x. A quadratic path therefore approaches 8x normalized
// slowdown (and the removed ancestor-walk implementation measured >15x),
// while the linear child-before-parent encoder stays near 1x. A 4x ceiling
// leaves wide hosted-runner headroom without accepting the old algorithm.
private const val MAX_NORMALIZED_SLOWDOWN = 4.0
private const val SHALLOW_SCALING_DEPTH = 1_024
private const val DEEP_SCALING_DEPTH = 8_192

@Volatile
private var benchmarkSink = 0

private fun residentSetKiB(): Long {
    val procStatus = File("/proc/self/status")
    if (procStatus.isFile) {
        val line = procStatus.useLines { lines -> lines.firstOrNull { it.startsWith("VmRSS:") } }
        if (line != null) return line.split(Regex("\\s+"))[1].toLong()
    }
    return runCatching {
        ProcessBuilder("ps", "-o", "rss=", "-p", ProcessHandle.current().pid().toString())
            .start()
            .inputStream
            .bufferedReader()
            .readText()
            .trim()
            .toLong()
    }.getOrDefault(-1)
}

private fun memoryMetrics(): String {
    val runtime = Runtime.getRuntime()
    val heapUsedKiB = (runtime.totalMemory() - runtime.freeMemory()) / 1024
    val heapCommittedKiB = runtime.totalMemory() / 1024
    return "heap_used_kib=$heapUsedKiB heap_committed_kib=$heapCommittedKiB rss_kib=${residentSetKiB()}"
}

private fun consume(document: Document) {
    benchmarkSink = benchmarkSink xor document.revision.hashCode() xor document.content.size
}

private fun benchmark(
    workload: String,
    source: String,
) {
    Document.parse(source)
    val samples =
        kotlin.collections
            .List(5) { measureNanoTime { Document.parse(source) } }
            .sorted()
    println(
        "benchmark runtime=kotlin boundary=jni_parse_and_value_copy workload=$workload " +
            "workload_version=1 bytes=${source.encodeToByteArray().size} warmup=1 repeats=5 " +
            "median_ns=${samples[samples.size / 2]} ${memoryMetrics()}",
    )
}

private fun medianOperationNs(operation: () -> Long): Double {
    val samples =
        kotlin.collections
            .List(SCALING_REPEATS) {
                var elapsed = 0L
                var iterations = 0
                do {
                    elapsed += operation()
                    iterations += 1
                } while (elapsed < SCALING_SAMPLE_FLOOR_NS)
                elapsed.toDouble() / iterations
            }.sorted()
    return samples[samples.size / 2]
}

private fun deepScalingBenchmark(
    workload: String,
    boundary: String,
    operation: (String) -> Long,
) {
    val shallowSource = "> ".repeat(SHALLOW_SCALING_DEPTH) + "leaf\n"
    val deepSource = "> ".repeat(DEEP_SCALING_DEPTH) + "leaf\n"
    repeat(SCALING_WARMUP) {
        operation(shallowSource)
        operation(deepSource)
    }
    val shallowNs = medianOperationNs { operation(shallowSource) }
    val deepNs = medianOperationNs { operation(deepSource) }
    val depthGrowth = DEEP_SCALING_DEPTH.toDouble() / SHALLOW_SCALING_DEPTH
    val normalizedSlowdown = deepNs / shallowNs / depthGrowth
    println(
        "benchmark runtime=kotlin boundary=$boundary workload=$workload workload_version=1 " +
            "bytes=${deepSource.encodeToByteArray().size} depth=$DEEP_SCALING_DEPTH " +
            "warmup=$SCALING_WARMUP repeats=$SCALING_REPEATS median_ns=${deepNs.toLong()} " +
            "scale_from_depth=$SHALLOW_SCALING_DEPTH normalized_slowdown=$normalizedSlowdown " +
            memoryMetrics(),
    )
    check(normalizedSlowdown <= MAX_NORMALIZED_SLOWDOWN) {
        "$workload scaled non-linearly: depth grew ${depthGrowth}x but median time grew " +
            "${deepNs / shallowNs}x (normalized slowdown ${normalizedSlowdown}x, " +
            "maximum ${MAX_NORMALIZED_SLOWDOWN}x)"
    }
}

private fun oneShotParseNs(source: String): Long {
    lateinit var document: Document
    val elapsed = measureNanoTime { document = Document.parse(source) }
    consume(document)
    return elapsed
}

private fun firstCommitNs(source: String): Long =
    MarkupSession().use { session ->
        // Isolate the first commit and delta decode: session creation and
        // edit transfer prepare the exact committed input outside the clock.
        session.append(source)
        lateinit var document: Document
        val elapsed = measureNanoTime { document = session.commit().document }
        consume(document)
        elapsed
    }

private fun scopeMaterializationBenchmark(
    workload: String,
    source: String,
    depth: Int,
) {
    fun materialize(): Long =
        MarkupSession().use { session ->
            session.append(source)
            session.commit()
            val document = session.document
            measureNanoTime { document.scope(document) }
        }

    materialize()
    val samples =
        kotlin.collections
            .List(5) { materialize() }
            .sorted()
    println(
        "benchmark runtime=kotlin boundary=jni_session_scope_materialization workload=$workload " +
            "workload_version=1 bytes=${source.encodeToByteArray().size} depth=$depth warmup=1 repeats=5 " +
            "median_ns=${samples[samples.size / 2]} ${memoryMetrics()}",
    )
}

private fun sessionBenchmark(
    workload: String,
    chunks: kotlin.collections.List<String>,
) {
    fun stream(): Long =
        measureNanoTime {
            MarkupSession().use { session ->
                for (chunk in chunks) {
                    session.append(chunk)
                    session.commit()
                }
                // Decode cost includes one scope materialization, matching a
                // consumer that renders the final snapshot.
                session.document.scope(session.document)
            }
        }
    stream()
    val samples =
        kotlin.collections
            .List(5) { stream() }
            .sorted()
    println(
        "benchmark runtime=kotlin boundary=jni_session_stream_and_delta_decode workload=$workload " +
            "workload_version=1 bytes=${chunks.sumOf { it.encodeToByteArray().size }} " +
            "commits=${chunks.size} warmup=1 repeats=5 " +
            "median_ns=${samples[samples.size / 2]} ${memoryMetrics()}",
    )
}

fun main() {
    val unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"
    benchmark("large_document", unit.repeat(2_000))
    benchmark("deep_nesting", "> ".repeat(128) + "leaf\n")
    deepScalingBenchmark(
        "deep_one_shot_parse",
        "jni_parse_and_value_copy",
        ::oneShotParseNs,
    )
    deepScalingBenchmark(
        "deep_first_commit",
        "jni_session_first_commit_and_delta_decode",
        ::firstCommitNs,
    )
    scopeMaterializationBenchmark(
        "deep_scope_materialization",
        "> ".repeat(4_096) + "leaf\n",
        4_096,
    )
    sessionBenchmark(
        "session_stream_flat",
        buildList {
            val line = "Paragraph with **strong**, [link](https://example.com), and streaming text.\n"
            repeat(400) {
                add(line)
                add("\n")
            }
        },
    )
}
