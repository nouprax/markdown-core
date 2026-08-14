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

// The append arm's baseline: the same stream where every tick re-parses the
// accumulated text from scratch. A document chain grows one way — append —
// and replacing the text is a new document, so a fresh parse per tick is
// exactly what a consumer without append would pay. Each tick closes the
// previous head in the loop; the registry is not a backstop here: a native
// parse costs WASM linear memory, which is invisible to the JavaScript
// collector, so nothing would make it run.
function benchmarkParseStream(workload, unit, units) {
    function replay() {
        let streamed = "";
        for (let index = 0; index < units; index += 1) {
            streamed += unit;
            Document(streamed).close();
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
        `benchmark runtime=es boundary=wasm_parse_and_value_copy workload=${workload} ` +
            `workload_version=1 bytes=${Buffer.byteLength(unit) * units} parses=${units} ` +
            `warmup=1 repeats=5 median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

// The same stream driven by the trailing mutation: each unit arrives as an
// append, and the boundary name says what the arm actually measures — the
// native append plus the revision-pruned decode that reuses every value the
// (id, revision) mirror proves. Compared against benchmarkParseStream above,
// which pays a full re-parse per tick for the same text.
function benchmarkAppendStream(workload, unit, units) {
    function replay() {
        let document = Document("");
        for (let index = 0; index < units; index += 1) {
            const previous = document;
            document = document.append(unit);
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
        `benchmark runtime=es boundary=wasm_append_and_reuse_decode workload=${workload} ` +
            `workload_version=1 bytes=${Buffer.byteLength(unit) * units} appends=${units} ` +
            `warmup=1 repeats=5 median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

function benchmarkFanOut(workload, width) {
    // One-byte changes alternating in the first paragraph of a document with
    // `width` root children. Replacing the text is a new document, so every
    // change is a fresh parse of the whole text plus a decode of the whole
    // new tree: a narrow change costs what a wide document costs.
    const body = "a\n\n".repeat(width);
    const sources = ["a" + body.slice(1), "b" + body.slice(1)];

    function replay() {
        for (let index = 0; index < 20; index += 1) {
            Document(sources[index % 2]).close();
        }
    }

    replay();
    const timings = [];
    for (let index = 0; index < 5; index += 1) {
        const start = performance.now();
        replay();
        timings.push((performance.now() - start) / 20);
    }
    timings.sort((left, right) => left - right);
    const medianNanoseconds = Math.round(timings[2] * 1e6);
    console.log(
        `benchmark runtime=es boundary=wasm_parse_and_value_copy workload=${workload} ` +
            `workload_version=1 bytes=${width * 3} parses=1 warmup=1 repeats=5 ` +
            `median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

const unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
benchmark("large_document", unit.repeat(2_000));
benchmark("deep_nesting", "> ".repeat(128) + "leaf\n");
benchmarkDeepBuild("deep_document_build", "> ".repeat(4_096) + "leaf\n", 4_096);
benchmarkParseStream("streamed_document", unit, 500);
benchmarkAppendStream("streamed_document_append", unit, 500);
benchmarkFanOut("fan_out_narrow_change", 10_000);
