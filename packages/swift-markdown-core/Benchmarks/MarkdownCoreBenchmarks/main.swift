import Foundation
import MarkdownCore

#if canImport(Darwin)
    import Darwin
#endif

let warmupCount = 3
let repeatCount = 10

/// The one warmup/measure/median/rusage/print harness shared by every entry
/// point. `metrics` carries the workload-specific fields between
/// `workload=` and `warmup=` (for example `bytes=` and `commits=`); the
/// printed line format is load-bearing — scripts/collect-pr-metrics.mjs
/// parses the `runtime=`/`median_ns=` fields.
func measurePreparedAndReport<Prepared>(
    boundary: String,
    workload: String,
    metrics: String,
    prepare: (Int) throws -> Prepared,
    body: (Prepared, Int) throws -> Void
) rethrows {
    for round in 0..<warmupCount {
        let prepared = try prepare(round)
        try body(prepared, round)
    }

    let clock = ContinuousClock()
    var durations: [Duration] = []
    durations.reserveCapacity(repeatCount)
    for round in 0..<repeatCount {
        let prepared = try prepare(round)
        durations.append(try clock.measure { try body(prepared, round) })
    }
    durations.sort()
    let median = durations[repeatCount / 2].components
    let medianNanoseconds = Double(median.seconds) * 1e9 + Double(median.attoseconds) / 1e9
    #if canImport(Darwin)
        var usage = rusage()
        getrusage(RUSAGE_SELF, &usage)
        let peakRSSKiB = usage.ru_maxrss / 1024
    #else
        let peakRSSKiB = -1
    #endif
    print(
        "benchmark runtime=swift boundary=\(boundary) workload=\(workload) "
            + "workload_version=1 \(metrics) warmup=\(warmupCount) repeats=\(repeatCount) "
            + "median_ns=\(Int64(medianNanoseconds)) peak_rss_kib=\(peakRSSKiB)"
    )
}

func measureAndReport(
    boundary: String,
    workload: String,
    metrics: String,
    body: (Int) throws -> Void
) rethrows {
    try measurePreparedAndReport(
        boundary: boundary,
        workload: workload,
        metrics: metrics,
        prepare: { $0 },
        body: { _, round in
            try body(round)
        }
    )
}

func benchmark(_ workload: String, source: String) throws {
    try measureAndReport(
        boundary: "native_parse_and_value_copy",
        workload: workload,
        metrics: "bytes=\(source.utf8.count)"
    ) { _ in
        _ = try Document(source)
    }
}

let unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"
try benchmark("large_document", source: String(repeating: unit, count: 2_000))
try benchmark("deep_nesting", source: String(repeating: "> ", count: 128) + "leaf\n")

// A deep document built end to end. This replaces the depth-4,096
// `deep_scope_materialization` workload, whose subject no longer exists: a
// document resolved scopes lazily against its session, so the first request
// was a measurable event. Every node carries its own extent now, so there is
// nothing to resolve at all. The cost moved into the parse boundary, so the
// workload that measures it is a deep parse, under a name that says so.
func benchmarkDeepBuild(_ workload: String, depth: Int) throws {
    let source = String(repeating: "> ", count: depth) + "leaf\n"
    try measureAndReport(
        boundary: "native_parse_and_value_copy",
        workload: workload,
        metrics: "bytes=\(source.utf8.count) depth=\(depth)"
    ) { _ in
        let document = try Document(source)
        _ = document.scope
    }
}

try benchmarkDeepBuild("deep_document_build", depth: 4_096)

func benchmarkStream(_ workload: String, unit: String, units: Int) throws {
    // The re-parsing streaming consumer: text grows by one unit per tick
    // and each tick constructs a fresh document from all text so far. A
    // document chain grows one way — append — so replacing the text IS a
    // new document; this workload prices that baseline, which is exactly
    // what the deleted whole-text edit performed.
    try measureAndReport(
        boundary: "native_parse_and_value_copy",
        workload: workload,
        metrics: "bytes=\(unit.utf8.count * units) commits=\(units)"
    ) { _ in
        var streamed = ""
        for _ in 0..<units {
            streamed += unit
            _ = try Document(streamed)
        }
    }
}

try benchmarkStream("streamed_document", unit: unit, units: 500)
