import { performance } from "node:perf_hooks";
import { Document, MarkupSession } from "../dist/index.js";
import { relink } from "../dist/session/relink.js";

const scalingSampleFloorNs = 100_000_000n;
const maxNormalizedSlowdown = 4;
let benchmarkSink = 0;

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
            `workload_version=1 bytes=${Buffer.byteLength(source)} warmup=1 repeats=5 ` +
            `median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

function benchmarkScopeMaterialization(workload, source, depth) {
    function materialize() {
        const session = new MarkupSession();
        try {
            session.append(source);
            session.commit();
            const document = session.document;
            const start = performance.now();
            document.scope(document);
            return performance.now() - start;
        } finally {
            session.close();
        }
    }

    materialize();
    const timings = [];
    for (let index = 0; index < 5; index += 1) timings.push(materialize());
    timings.sort((left, right) => left - right);
    const medianNanoseconds = Math.round(timings[2] * 1e6);
    console.log(
        `benchmark runtime=es boundary=wasm_session_scope_materialization workload=${workload} ` +
            `workload_version=1 bytes=${Buffer.byteLength(source)} depth=${depth} warmup=1 repeats=5 ` +
            `median_ns=${medianNanoseconds} peak_rss_kib=${process.resourceUsage().maxRSS} ` +
            `rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
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
            `workload_version=1 bytes=${Buffer.byteLength(unit) * units} commits=${units} ` +
            `warmup=1 repeats=5 median_ns=${medianNanoseconds} ` +
            `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
    );
}

function benchmarkFanOut(workload, width) {
    // One-byte edits alternating in the first paragraph of a document with
    // `width` root children: the commit must stay proportional to the delta
    // (relink, no per-sibling native traffic), not to the root's width.
    function replay(session) {
        for (let index = 0; index < 20; index += 1) {
            session.replace(0, 1, index % 2 ? "a" : "b");
            session.commit();
        }
    }

    const session = new MarkupSession();
    try {
        session.append("a\n\n".repeat(width));
        session.commit();
        replay(session);
        const timings = [];
        for (let index = 0; index < 5; index += 1) {
            const start = performance.now();
            replay(session);
            timings.push(performance.now() - start);
        }
        timings.sort((left, right) => left - right);
        const medianNanoseconds = Math.round((timings[2] / 20) * 1e6);
        console.log(
            `benchmark runtime=es boundary=wasm_session_stream_and_delta_decode workload=${workload} ` +
                `workload_version=1 bytes=${width * 3} commits=1 warmup=1 repeats=5 ` +
                `median_ns=${medianNanoseconds} ` +
                `peak_rss_kib=${process.resourceUsage().maxRSS} rss_kib=${Math.round(process.memoryUsage().rss / 1024)}`
        );
    } finally {
        session.close();
    }
}

function medianOperationNs(operation) {
    const samples = [];
    for (let repeat = 0; repeat < 3; repeat += 1) {
        let elapsed = 0n;
        let iterations = 0;
        do {
            const started = process.hrtime.bigint();
            const result = operation();
            elapsed += process.hrtime.bigint() - started;
            benchmarkSink ^= result.revision ^ result.content.length;
            iterations += 1;
        } while (elapsed < scalingSampleFloorNs);
        samples.push(Number(elapsed) / iterations);
    }
    samples.sort((left, right) => left - right);
    return samples[1];
}

function benchmarkManySiblingRelink() {
    function input(width) {
        const lineage = 1n;
        const children = Array.from({ length: width }, (_, index) => ({
            kind: "paragraph",
            id: { lineage, rawValue: index + 2 },
            revision: 1,
            content: []
        }));
        const replacements = new Map(children.map((child) => [child, { ...child, revision: 2 }]));
        return {
            previous: {
                kind: "document",
                id: { lineage, rawValue: 1 },
                revision: 1,
                content: children
            },
            replacements
        };
    }

    const shallowWidth = 1_024;
    const deepWidth = 8_192;
    const shallow = input(shallowWidth);
    const deep = input(deepWidth);
    relink(shallow.previous, 2, shallow.replacements);
    relink(deep.previous, 2, deep.replacements);
    const shallowNs = medianOperationNs(() => relink(shallow.previous, 2, shallow.replacements));
    const deepNs = medianOperationNs(() => relink(deep.previous, 2, deep.replacements));
    const widthGrowth = deepWidth / shallowWidth;
    const normalizedSlowdown = deepNs / shallowNs / widthGrowth;
    console.log(
        `complexity runtime=es boundary=js_bubbled_parent_relink workload=many_sibling_relink ` +
            `width=${deepWidth} scale_from_width=${shallowWidth} median_ns=${Math.round(deepNs)} ` +
            `normalized_slowdown=${normalizedSlowdown} checksum=${benchmarkSink}`
    );
    if (normalizedSlowdown > maxNormalizedSlowdown) {
        throw new Error(
            `many_sibling_relink scaled non-linearly: width grew ${widthGrowth}x but median time grew ` +
                `${deepNs / shallowNs}x (normalized slowdown ${normalizedSlowdown}x, ` +
                `maximum ${maxNormalizedSlowdown}x)`
        );
    }
}

const unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
benchmark("large_document", unit.repeat(2_000));
benchmark("deep_nesting", "> ".repeat(128) + "leaf\n");
benchmarkScopeMaterialization("deep_scope_materialization", "> ".repeat(4_096) + "leaf\n", 4_096);
benchmarkSession("streamed_document", unit, 500);
benchmarkFanOut("fan_out_narrow_edit", 10_000);
benchmarkManySiblingRelink();
