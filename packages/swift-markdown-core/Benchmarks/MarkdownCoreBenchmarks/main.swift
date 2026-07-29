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
        _ = try Document.parse(source)
    }
}

let unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"
try benchmark("large_document", source: String(repeating: unit, count: 2_000))
try benchmark("deep_nesting", source: String(repeating: "> ", count: 128) + "leaf\n")

struct ScopeBenchmarkSnapshot {
    let session: MarkupSession
    let document: Document
}

func benchmarkScopeMaterialization(_ workload: String, depth: Int) throws {
    let source = String(repeating: "> ", count: depth) + "leaf\n"
    try measurePreparedAndReport(
        boundary: "native_session_scope_materialization",
        workload: workload,
        metrics: "bytes=\(source.utf8.count) depth=\(depth)"
    ) { _ in
        let session = try MarkupSession()
        try session.append(source)
        let document = try session.commit().document
        return ScopeBenchmarkSnapshot(session: session, document: document)
    } body: { snapshot, _ in
        _ = snapshot.document.scope(of: snapshot.document)
        withExtendedLifetime(snapshot.session) {}
    }
}

try benchmarkScopeMaterialization("deep_scope_materialization", depth: 4_096)

func benchmarkSession(_ workload: String, unit: String, units: Int) throws {
    try measureAndReport(
        boundary: "session_stream_and_delta_decode",
        workload: workload,
        metrics: "bytes=\(unit.utf8.count * units) commits=\(units)"
    ) { _ in
        let session = try MarkupSession()
        for _ in 0..<units {
            try session.append(unit)
            _ = try session.commit()
        }
    }
}

try benchmarkSession("streamed_document", unit: unit, units: 500)

func benchmarkDeepEdit(_ workload: String, depth: Int) throws {
    // A one-byte edit at the innermost leaf of a deep quote chain: the
    // rebuild ordering must stay proportional to the touched path, not turn
    // quadratic through per-entry ancestor walks.
    let session = try MarkupSession()
    try session.append(String(repeating: "> ", count: depth) + "a\n")
    _ = try session.commit()
    try measureAndReport(
        boundary: "session_stream_and_delta_decode",
        workload: workload,
        metrics: "bytes=\(depth * 2 + 2) commits=1"
    ) { round in
        try session.replace((depth * 2)..<(depth * 2 + 1), with: round % 2 == 0 ? "b" : "a")
        _ = try session.commit()
    }
}

try benchmarkDeepEdit("deep_path_edit", depth: 5_000)
