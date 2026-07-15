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
    Document.parse(source)
    val samples =
        kotlin.collections
            .List(5) { measureNanoTime { Document.parse(source) } }
            .sorted()
    val runtime = Runtime.getRuntime()
    val heapUsedKiB = (runtime.totalMemory() - runtime.freeMemory()) / 1024
    val heapCommittedKiB = runtime.totalMemory() / 1024
    println(
        "benchmark runtime=kotlin boundary=jni_parse_and_value_copy workload=$workload " +
            "bytes=${source.encodeToByteArray().size} warmup=1 repeats=5 " +
            "median_ns=${samples[samples.size / 2]} heap_used_kib=$heapUsedKiB " +
            "heap_committed_kib=$heapCommittedKiB rss_kib=${residentSetKiB()}",
    )
}

fun main() {
    val unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"
    benchmark("large_document", unit.repeat(2_000))
    benchmark("deep_nesting", "> ".repeat(128) + "leaf\n")
}
