import { performance } from "node:perf_hooks";
import { Document, MarkupSession } from "../dist/index.js";

function benchmark(workload, source) {
    Document.parse(source);
    const timings = [];
    for (let index = 0; index < 5; index += 1) {
        const start = performance.now();
        Document.parse(source);
        timings.push(performance.now() - start);
    }
    timings.sort((left, right) => left - right);
    const medianNanoseconds = Math.round(timings[2] * 1e6);
    console.log(
        `benchmark runtime=es boundary=wasm_parse_and_value_copy workload=${workload} ` +
            `bytes=${Buffer.byteLength(source)} warmup=1 repeats=5 median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

function benchmarkSession(workload, unit, units) {
    function replay() {
        const session = new MarkupSession();
        try {
            for (let index = 0; index < units; index += 1) {
                session.append(unit);
                session.commit();
            }
            return session.document;
        } finally {
            session.close();
        }
    }

    replay();
    const timings = [];
    for (let index = 0; index < 5; index += 1) {
        const start = performance.now();
        replay();
        timings.push(performance.now() - start);
    }
    timings.sort((left, right) => left - right);
    const medianNanoseconds = Math.round(timings[2] * 1e6);
    console.log(
        `benchmark runtime=es boundary=wasm_session_stream_and_delta_decode workload=${workload} ` +
            `bytes=${Buffer.byteLength(unit) * units} commits=${units} warmup=1 repeats=5 median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

const unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
benchmark("large_document", unit.repeat(2_000));
benchmark("deep_nesting", "> ".repeat(128) + "leaf\n");
benchmarkSession("streamed_document", unit, 500);
