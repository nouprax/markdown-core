package com.nouprax.markdown.core.benchmark

import com.nouprax.markdown.core.Document
import java.io.File
import kotlin.system.measureNanoTime

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

private fun benchmark(
    workload: String,
    source: String,
) {
    Document(source).seal()
    val samples =
        kotlin.collections
            .List(5) { measureNanoTime { Document(source).seal() } }
            .sorted()
    val runtime = Runtime.getRuntime()
    val heapUsedKiB = (runtime.totalMemory() - runtime.freeMemory()) / 1024
    val heapCommittedKiB = runtime.totalMemory() / 1024
    println(
        "benchmark runtime=kotlin boundary=jni_feed_seal_and_value_copy workload=$workload " +
            "workload_version=1 bytes=${source.encodeToByteArray().size} warmup=1 repeats=5 " +
            "median_ns=${samples[samples.size / 2]} heap_used_kib=$heapUsedKiB " +
            "heap_committed_kib=$heapCommittedKiB rss_kib=${residentSetKiB()}",
    )
}

// The streaming path: the same input fed in N chunks, N multiplying by 16
// per step. With the wire crossing as a DELTA (#162) a feed costs the open
// spine and the changed blocks, so a step reads near 1x where the per-feed
// whole decode read up to 12.7x. The cap is the construction
// bench_runner.c's BENCH_MAX_DOUBLING_RATIO uses (4.0 for a 2x step): it
// tolerates the open block re-read per feed and refuses a return to
// per-feed whole decodes; loosening it is not an option (#148).
private const val FEED_STEP_RATIO_MAX = 4.0

private fun feedLoopBenchmark(
    workload: String,
    unit: String,
    units: Int,
) {
    val chunkCounts = listOf(1, 16, 256)
    val medians = mutableListOf<Long>()
    for (chunks in chunkCounts) {
        val chunk = unit.repeat(units / chunks)
        val run = {
            val document = Document()
            repeat(chunks) { document.feed(chunk) }
            document.seal()
        }
        run()
        val samples = List(5) { measureNanoTime { run() } }.sorted()
        medians.add(samples[samples.size / 2])
        println("feed-loop step chunks=$chunks median_ms=${"%.3f".format(samples[samples.size / 2] / 1e6)}")
    }
    for (step in 1 until medians.size) {
        // The same 500 ns floor bench_runner.c uses before taking a ratio.
        val floor = maxOf(medians[step - 1], 500L)
        val ratio = medians[step].toDouble() / floor
        check(ratio <= FEED_STEP_RATIO_MAX) {
            "feed-loop scaling ratio ${"%.2f".format(ratio)} exceeds $FEED_STEP_RATIO_MAX " +
                "at ${chunkCounts[step]} chunks"
        }
    }
    val runtime = Runtime.getRuntime()
    val heapUsedKiB = (runtime.totalMemory() - runtime.freeMemory()) / 1024
    val heapCommittedKiB = runtime.totalMemory() / 1024
    println(
        "benchmark runtime=kotlin boundary=jni_feed_loop workload=$workload " +
            "workload_version=1 bytes=${unit.encodeToByteArray().size * units} warmup=1 repeats=5 " +
            "median_ns=${medians.last()} heap_used_kib=$heapUsedKiB " +
            "heap_committed_kib=$heapCommittedKiB rss_kib=${residentSetKiB()}",
    )
}

fun main() {
    val unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"
    benchmark("large_document", unit.repeat(2_000))
    benchmark("deep_nesting", "> ".repeat(128) + "leaf\n")
    // 2048 units (not the one-shot's 2000) so every chunk count divides into
    // whole repetitions of the unit and every chunk stays valid UTF-8.
    feedLoopBenchmark("large_document", unit, 2_048)
}
