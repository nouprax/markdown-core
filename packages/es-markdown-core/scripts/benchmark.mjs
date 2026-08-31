import { performance } from "node:perf_hooks";
import { Document } from "../dist/index.js";

function benchmark(workload, source) {
    new Document(source).seal();
    const timings = [];
    for (let index = 0; index < 5; index += 1) {
        const start = performance.now();
        new Document(source).seal();
        timings.push(performance.now() - start);
    }
    timings.sort((left, right) => left - right);
    const medianNanoseconds = Math.round(timings[2] * 1e6);
    console.log(
        `benchmark runtime=es boundary=wasm_feed_seal_and_value_copy workload=${workload} ` +
            `workload_version=1 bytes=${Buffer.byteLength(source)} warmup=1 repeats=5 median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

// The streaming path: the same input fed in N chunks, N multiplying by 16
// per step. Per-feed cost that is independent of the chunk count keeps each
// step under ~16x. The cap is twice that ceiling -- the same construction
// as bench_runner.c's BENCH_MAX_DOUBLING_RATIO (4.0 for a 2x step) -- so it
// tolerates today's per-feed full decode while catching a super-linear
// blowup; tightening it is part of fixing the per-feed costs, loosening it
// is not an option (#148).
const FEED_STEP_RATIO_MAX = 32;

function feedLoopBenchmark(workload, unit, units) {
    const chunkCounts = [1, 16, 256];
    const medians = [];
    for (const chunks of chunkCounts) {
        const chunk = unit.repeat(units / chunks);
        const run = () => {
            const document = new Document();
            for (let index = 0; index < chunks; index += 1) {
                document.feed(chunk);
            }
            return document.seal();
        };
        run();
        const timings = [];
        for (let index = 0; index < 5; index += 1) {
            const start = performance.now();
            run();
            timings.push(performance.now() - start);
        }
        timings.sort((left, right) => left - right);
        medians.push(timings[2]);
        console.log(`feed-loop step chunks=${chunks} median_ms=${timings[2].toFixed(3)}`);
    }
    for (let step = 1; step < medians.length; step += 1) {
        // The same 500 ns floor bench_runner.c uses before taking a ratio.
        const floor = Math.max(medians[step - 1], 0.0005);
        const ratio = medians[step] / floor;
        if (ratio > FEED_STEP_RATIO_MAX) {
            throw new Error(
                `feed-loop scaling ratio ${ratio.toFixed(2)} exceeds ${FEED_STEP_RATIO_MAX} ` +
                    `at ${chunkCounts[step]} chunks`
            );
        }
    }
    const medianNanoseconds = Math.round(medians[medians.length - 1] * 1e6);
    console.log(
        `benchmark runtime=es boundary=wasm_feed_loop workload=${workload} ` +
            `workload_version=1 bytes=${Buffer.byteLength(unit) * units} warmup=1 repeats=5 median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

const unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
benchmark("large_document", unit.repeat(2_000));
benchmark("deep_nesting", "> ".repeat(128) + "leaf\n");
// 2048 units (not the one-shot's 2000) so every chunk count divides into
// whole repetitions of the unit and every chunk stays valid UTF-8.
feedLoopBenchmark("large_document", unit, 2_048);
