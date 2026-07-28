import Foundation
import MarkdownCore

#if canImport(Darwin)
    import Darwin
#endif

let warmupCount = 3
let repeatCount = 10

func benchmark(_ workload: String, source: String) throws {
    for _ in 0..<warmupCount {
        _ = try Document.parse(source)
    }

    let clock = ContinuousClock()
    var durations: [Duration] = []
    durations.reserveCapacity(repeatCount)
    for _ in 0..<repeatCount {
        durations.append(try clock.measure { _ = try Document.parse(source) })
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
        "benchmark runtime=swift boundary=native_parse_and_value_copy workload=\(workload) "
            + "bytes=\(source.utf8.count) warmup=\(warmupCount) repeats=\(repeatCount) "
            + "median_ns=\(Int64(medianNanoseconds)) peak_rss_kib=\(peakRSSKiB)"
    )
}

let unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"
try benchmark("large_document", source: String(repeating: unit, count: 2_000))
try benchmark("deep_nesting", source: String(repeating: "> ", count: 128) + "leaf\n")

func benchmarkSession(_ workload: String, unit: String, units: Int) throws {
    func replay() throws -> Document {
        let session = try MarkupSession()
        for _ in 0..<units {
            try session.append(unit)
            _ = try session.commit()
        }
        return session.document
    }

    for _ in 0..<warmupCount {
        _ = try replay()
    }

    let clock = ContinuousClock()
    var durations: [Duration] = []
    durations.reserveCapacity(repeatCount)
    for _ in 0..<repeatCount {
        durations.append(try clock.measure { _ = try replay() })
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
        "benchmark runtime=swift boundary=session_stream_and_delta_decode workload=\(workload) "
            + "bytes=\(unit.utf8.count * units) commits=\(units) warmup=\(warmupCount) repeats=\(repeatCount) "
            + "median_ns=\(Int64(medianNanoseconds)) peak_rss_kib=\(peakRSSKiB)"
    )
}

try benchmarkSession("streamed_document", unit: unit, units: 500)

func benchmarkDeepEdit(_ workload: String, depth: Int) throws {
    // A one-byte edit at the innermost leaf of a deep quote chain: the
    // rebuild ordering must stay proportional to the touched path, not turn
    // quadratic through per-entry ancestor walks.
    let session = try MarkupSession()
    try session.append(String(repeating: "> ", count: depth) + "a\n")
    _ = try session.commit()
    func replay(_ round: Int) throws {
        try session.replace((depth * 2)..<(depth * 2 + 1), with: round % 2 == 0 ? "b" : "a")
        _ = try session.commit()
    }

    for round in 0..<warmupCount {
        try replay(round)
    }
    let clock = ContinuousClock()
    var durations: [Duration] = []
    durations.reserveCapacity(repeatCount)
    for round in 0..<repeatCount {
        durations.append(try clock.measure { try replay(round) })
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
        "benchmark runtime=swift boundary=session_stream_and_delta_decode workload=\(workload) "
            + "bytes=\(depth * 2 + 2) commits=1 warmup=\(warmupCount) repeats=\(repeatCount) "
            + "median_ns=\(Int64(medianNanoseconds)) peak_rss_kib=\(peakRSSKiB)"
    )
}

try benchmarkDeepEdit("deep_path_edit", depth: 5_000)
