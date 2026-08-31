import Foundation
import MarkdownCore

#if canImport(Darwin)
    import Darwin
#endif

let warmupCount = 3
let repeatCount = 10

func benchmark(_ workload: String, source: String) throws {
    for _ in 0..<warmupCount {
        _ = try Document(markdown: source).seal()
    }

    let clock = ContinuousClock()
    var durations: [Duration] = []
    durations.reserveCapacity(repeatCount)
    for _ in 0..<repeatCount {
        durations.append(try clock.measure { _ = try Document(markdown: source).seal() })
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
        "benchmark runtime=swift boundary=native_feed_seal_and_value_copy workload=\(workload) "
            + "workload_version=1 bytes=\(source.utf8.count) warmup=\(warmupCount) repeats=\(repeatCount) "
            + "median_ns=\(Int64(medianNanoseconds)) peak_rss_kib=\(peakRSSKiB)"
    )
}

// The streaming path: the same input fed in N chunks, N multiplying by 16
// per step. Per-feed cost that is independent of the chunk count keeps each
// step under ~16x. The cap is twice that ceiling -- the same construction
// as bench_runner.c's BENCH_MAX_DOUBLING_RATIO (4.0 for a 2x step) -- so it
// tolerates today's per-feed costs while catching a super-linear blowup;
// tightening it is part of fixing the per-feed costs, loosening it is not
// an option (#148).
let feedStepRatioMax = 32.0

struct FeedLoopScalingViolation: Error, CustomStringConvertible {
    let description: String
}

func feedLoopBenchmark(_ workload: String, unit: String, units: Int) throws {
    let chunkCounts = [1, 16, 256]
    var medians: [Double] = []
    let clock = ContinuousClock()
    for chunks in chunkCounts {
        let chunk = String(repeating: unit, count: units / chunks)
        func runOnce() throws {
            let document = try Document()
            for _ in 0..<chunks {
                _ = try document.feed(chunk: chunk)
            }
            _ = try document.seal()
        }
        for _ in 0..<warmupCount {
            try runOnce()
        }
        var durations: [Duration] = []
        durations.reserveCapacity(repeatCount)
        for _ in 0..<repeatCount {
            durations.append(try clock.measure { try runOnce() })
        }
        durations.sort()
        let median = durations[repeatCount / 2].components
        let medianNanoseconds = Double(median.seconds) * 1e9 + Double(median.attoseconds) / 1e9
        medians.append(medianNanoseconds)
        print("feed-loop step chunks=\(chunks) median_ms=\(String(format: "%.3f", medianNanoseconds / 1e6))")
    }
    for step in 1..<medians.count {
        // The same 500 ns floor bench_runner.c uses before taking a ratio.
        let floor = max(medians[step - 1], 500.0)
        let ratio = medians[step] / floor
        if ratio > feedStepRatioMax {
            throw FeedLoopScalingViolation(
                description: "feed-loop scaling ratio \(String(format: "%.2f", ratio)) "
                    + "exceeds \(feedStepRatioMax) at \(chunkCounts[step]) chunks"
            )
        }
    }
    #if canImport(Darwin)
        var usage = rusage()
        getrusage(RUSAGE_SELF, &usage)
        let peakRSSKiB = usage.ru_maxrss / 1024
    #else
        let peakRSSKiB = -1
    #endif
    print(
        "benchmark runtime=swift boundary=native_feed_loop workload=\(workload) "
            + "workload_version=1 bytes=\(unit.utf8.count * units) warmup=\(warmupCount) repeats=\(repeatCount) "
            + "median_ns=\(Int64(medians[medians.count - 1])) peak_rss_kib=\(peakRSSKiB)"
    )
}

let unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"
try benchmark("large_document", source: String(repeating: unit, count: 2_000))
try benchmark("deep_nesting", source: String(repeating: "> ", count: 128) + "leaf\n")
// 2048 units (not the one-shot's 2000) so every chunk count divides into
// whole repetitions of the unit and every chunk stays valid UTF-8.
try feedLoopBenchmark("large_document", unit: unit, units: 2_048)
