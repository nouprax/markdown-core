import { performance } from "node:perf_hooks";
import { Document } from "../dist/index.js";

function benchmark(workload, source) {
    Document(source).close();
    const timings = [];
    for (let index = 0; index < 5; index += 1) {
        const start = performance.now();
        const document = Document(source);
        timings.push(performance.now() - start);
        document.close();
    }
    timings.sort((left, right) => left - right);
    const medianNanoseconds = Math.round(timings[2] * 1e6);
    console.log(
        `benchmark runtime=es boundary=wasm_parse_and_value_copy workload=${workload} ` +
            `workload_version=1 bytes=${Buffer.byteLength(source)} warmup=1 repeats=5 ` +
            `median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

// A deep document built end to end. This replaces the depth-4,096
// `deep_scope_materialization` workload, whose subject no longer exists: a
// snapshot resolved scopes lazily against its session, so the first request
// was a measurable event. A node now carries its own extent, decoded with the
// rest of its fields. The cost did not disappear — it moved into the parse
// boundary — so the workload that measures it is a deep parse, under a name
// that says so.
function benchmarkDeepBuild(workload, source, depth) {
    function build() {
        const start = performance.now();
        const document = Document(source);
        const elapsed = performance.now() - start;
        document.close();
        return elapsed;
    }

    build();
    const timings = [];
    for (let index = 0; index < 5; index += 1) timings.push(build());
    timings.sort((left, right) => left - right);
    const medianNanoseconds = Math.round(timings[2] * 1e6);
    console.log(
        `benchmark runtime=es boundary=wasm_parse_and_value_copy workload=${workload} ` +
            `workload_version=1 bytes=${Buffer.byteLength(source)} depth=${depth} warmup=1 repeats=5 ` +
            `median_ns=${medianNanoseconds} peak_rss_kib=${process.resourceUsage().maxRSS} ` +
            `rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

function benchmarkStream(workload, unit, units) {
    function replay() {
        let document = Document("");
        let streamed = "";
        for (let index = 0; index < units; index += 1) {
            streamed += unit;
            const previous = document;
            document = document.edit(streamed).document;
            // The edit reads its receiver and takes nothing, so the loop is
            // what ends the predecessor. The registry is not a backstop here:
            // a native parse costs WASM linear memory, which is invisible to
            // the JavaScript collector, so nothing would make it run.
            previous.close();
        }
        document.close();
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
        `benchmark runtime=es boundary=wasm_edit_and_delta_decode workload=${workload} ` +
            `workload_version=1 bytes=${Buffer.byteLength(unit) * units} commits=${units} ` +
            `warmup=1 repeats=5 median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

function benchmarkFanOut(workload, width) {
    // One-byte edits alternating in the first paragraph of a document with
    // `width` root children: every commit reparses the whole text and
    // decodes the whole committed tree, so a narrow edit costs what a wide
    // document costs.
    const body = "a\n\n".repeat(width);
    const sources = ["a" + body.slice(1), "b" + body.slice(1)];

    function replay(from) {
        let document = from;
        for (let index = 0; index < 20; index += 1) {
            const previous = document;
            document = document.edit(sources[index % 2]).document;
            previous.close();
        }
        return document;
    }

    let document = replay(Document(sources[0]));
    const timings = [];
    for (let index = 0; index < 5; index += 1) {
        const start = performance.now();
        document = replay(document);
        timings.push((performance.now() - start) / 20);
    }
    document.close();
    timings.sort((left, right) => left - right);
    const medianNanoseconds = Math.round(timings[2] * 1e6);
    console.log(
        `benchmark runtime=es boundary=wasm_edit_and_delta_decode workload=${workload} ` +
            `workload_version=1 bytes=${width * 3} commits=1 warmup=1 repeats=5 ` +
            `median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

const unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
benchmark("large_document", unit.repeat(2_000));
benchmark("deep_nesting", "> ".repeat(128) + "leaf\n");
benchmarkDeepBuild("deep_document_build", "> ".repeat(4_096) + "leaf\n", 4_096);
benchmarkStream("streamed_document", unit, 500);
benchmarkFanOut("fan_out_narrow_edit", 10_000);
