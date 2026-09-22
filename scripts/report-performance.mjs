#!/usr/bin/env node
/** Aggregate an existing stage artifact without running or changing its experiment. */
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { baseName, parseCallgrind } from "./lib/callgrind.mjs";
import { pairRatios, provenPair } from "./lib/corpus-pairs.mjs";
import { pairReview } from "./lib/pair-review.mjs";

export function referenceFor(entry) {
    if (entry.carries.length) return null;
    return entry.gfm ? "cmark-gfm" : entry.dialect === "commonmark" ? "cmark" : null;
}

const functionName = (name) =>
    baseName(name).replace(/(\.(constprop|isra|part|cold|lto_priv|localalias)\.?\d*)+$/u, "");
const fresh = () => ({
    documents: 0,
    source: 0,
    ast: 0,
    parse: 0,
    outside: 0,
    program: 0,
    allocator: 0,
    functions: {},
    files: {},
    edges: {}
});
const add = (object, key, value) => {
    object[key] = (object[key] ?? 0) + value;
};
const stageIr = (engine) => engine.stages.source_to_buffer.cost.Ir + engine.stages.buffer_to_ast.cost.Ir;

function accumulate(target, engine, profile) {
    target.documents++;
    target.source += engine.stages.source_to_buffer.cost.Ir;
    target.ast += engine.stages.buffer_to_ast.cost.Ir;
    target.parse += engine.parsePathIr;
    target.outside += engine.outsideStagesIr;
    assert.equal(stageIr(engine) + engine.outsideStagesIr, engine.parsePathIr);
    const ir = profile.events.indexOf("Ir");
    assert.ok(ir >= 0);
    let sum = 0,
        fileSum = 0;
    for (const [name, cost] of profile.self) {
        add(target.functions, functionName(name), cost[ir] ?? 0);
        sum += cost[ir] ?? 0;
    }
    for (const [file, cost] of profile.selfByFile) {
        const value = cost[ir] ?? 0;
        // Basenames deliberately match the historical subsystem accounting;
        // inline header costs remain with the header, not their caller.
        add(target.files, path.posix.basename(file), value);
        fileSum += value;
        if (path.posix.basename(file) === "malloc.c") target.allocator += value;
    }
    assert.equal(sum, fileSum);
    assert.equal(sum, profile.totals[ir]);
    target.program += sum;
    for (const edge of profile.edges.values()) {
        const key = `${functionName(edge.caller)} → ${functionName(edge.callee)}`;
        const row = (target.edges[key] ??= { calls: 0, ir: 0 });
        row.calls += edge.calls;
        row.ir += edge.cost[ir] ?? 0;
    }
}

export function summarize(report, readProfile) {
    const full = fresh(),
        core = fresh(),
        reference = fresh();
    const members = [],
        excluded = [];
    for (const entry of report.cases) {
        const current = readProfile(entry, "markdown-core");
        accumulate(full, entry.engines["markdown-core"], current);
        const ref = referenceFor(entry);
        if (ref) {
            members.push({ case: entry.case, scale: entry.scale, reference: ref, sha256: entry.sha256 });
            accumulate(core, entry.engines["markdown-core"], current);
            accumulate(reference, entry.engines[ref], readProfile(entry, ref));
        } else if (entry.dialect === "commonmark" || entry.gfm) {
            excluded.push({ case: entry.case, scale: entry.scale, carries: entry.carries });
        }
    }
    const atOne = new Map(report.cases.filter((c) => c.scale === 1).map((c) => [c.case, c]));
    const measuredPairs = report.pairs.filter((pair) => atOne.has(pair.case) && atOne.has(pair.isomorph));
    const pairs = measuredPairs.filter(provenPair).map((pair) => {
        const a = atOne.get(pair.case),
            b = atOne.get(pair.isomorph);
        const ref = referenceFor(b);
        assert.ok(ref);
        const values = {
            dialect: stageIr(a.engines["markdown-core"]),
            common: stageIr(b.engines["markdown-core"]),
            reference: stageIr(b.engines[ref]),
            carries: b.carries
        };
        return {
            case: pair.case,
            counterpart: pair.isomorph,
            proof: pair.contract.proof,
            reference: ref,
            a: values.dialect,
            b: values.common,
            r: values.reference,
            ...pairRatios(pair, values)
        };
    });
    const boundaries = report.pairs
        .filter((pair) => pair.contract.review)
        .flatMap((pair) => {
            const review = pairReview(pair);
            if (!review.baseline || !atOne.has(pair.case) || !atOne.has(review.baseline)) return [];
            const a = atOne.get(pair.case),
                b = atOne.get(review.baseline);
            assert.equal(a.units, b.units, `${pair.case}: boundary units`);
            const full = stageIr(a.engines["markdown-core"]),
                without = stageIr(b.engines["markdown-core"]);
            return [
                {
                    case: pair.case,
                    baseline: review.baseline,
                    units: a.units,
                    full,
                    without,
                    delta: full - without,
                    fullBytes: a.bytes,
                    withoutBytes: b.bytes
                }
            ];
        });
    return {
        schemaVersion: 1,
        corpus: report.corpus,
        pairingDigest: report.pairingDigest,
        toolchain: report.toolchain,
        members,
        excluded,
        full,
        core,
        reference,
        pairs,
        boundaries
    };
}

const number = (n) => n.toLocaleString("en-US");
const ratio = (a, b) => (b > 0 ? `${(a / b).toFixed(3)}×` : "—");
const median = (values) => {
    const sorted = [...values].sort((a, b) => a - b),
        mid = Math.floor(sorted.length / 2);
    return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
};

export function render(summary) {
    const { core, reference, full, pairs } = summary;
    const rows = [
        "# Performance census",
        "",
        `Corpus: \`${summary.corpus.digest}\`; pairing: \`${summary.pairingDigest}\`.`,
        "",
        `${full.documents} measured documents. Same-input reference cohort: **${core.documents} documents**; ` +
            "CommonMark uses cmark, GFM uses cmark-gfm, and every nonempty carries declaration is excluded. " +
            "The JSON lists every included and excluded document. Proof pairs use scale 1 and are a separate cohort.",
        "",
        "Ir is Callgrind's instruction-read count, not elapsed time. Parse-path costs come from the harness call edge. " +
            "Complete parse means the bench_parse_document lifecycle: parsing, the root receipt and document teardown; " +
            "main's input loading and process setup are outside that edge. " +
            "Program self costs below include process startup, input loading, receipts and shared libc leaves. " +
            "Allocator self is every cost line attributed to malloc.c, including harness allocation. " +
            "It must not be subtracted from parse-path Ir or described as exact parse-only allocator cost.",
        "",
        "| Accounting, same-input cohort | Core Ir | Reference Ir | Ratio |",
        "| --- | ---: | ---: | ---: |"
    ];
    for (const [label, key] of [
        ["Source", "source"],
        ["AST", "ast"],
        ["Complete parse", "parse"],
        ["Outside stages, inside parse", "outside"],
        ["Whole program self", "program"],
        ["Allocator program self", "allocator"]
    ]) {
        rows.push(
            `| ${label} | ${number(core[key])} | ${number(reference[key])} | ${ratio(core[key], reference[key])} |`
        );
    }
    rows.push(
        `| Whole program excluding allocator | ${number(core.program - core.allocator)} | ` +
            `${number(reference.program - reference.allocator)} | ${ratio(core.program - core.allocator, reference.program - reference.allocator)} |`,
        "",
        "## Full Core corpus",
        "",
        "| Source Ir | AST Ir | Complete parse Ir | Outside stages Ir |",
        "| ---: | ---: | ---: | ---: |",
        `| ${number(full.source)} | ${number(full.ast)} | ${number(full.parse)} | ${number(full.outside)} |`,
        "",
        "## Proved domains",
        "",
        "| Reference | Pairs | Median Grammar | Median Shape | Median Same-job |",
        "| --- | ---: | ---: | ---: | ---: |"
    );
    for (const ref of ["cmark", "cmark-gfm"]) {
        const group = pairs.filter((p) => p.reference === ref);
        if (!group.length) continue;
        rows.push(
            `| ${ref} | ${group.length} | ` +
                ["grammar", "shape", "sameJob"].map((k) => ratio(median(group.map((p) => p[k])), 1)).join(" | ") +
                " |"
        );
    }
    rows.push(
        "",
        "Grammar = A/B; Shape = B/R; Same-job = A/R. Grammar includes recognition and construction; " +
            "Shape includes the complete two-stage implementation on B. Factor medians need not multiply. " +
            "Boundary interventions and unproved historical substitutions are absent from this table.",
        "",
        "## Source-attributed program self",
        "",
        "Inline costs stay with their source header. This is an accounting partition, not proof of semantic equivalence between subsystem implementations.",
        "",
        "| Subsystem | Core Ir | Reference Ir | Ratio |",
        "| --- | ---: | ---: | ---: |"
    );
    const groups = {
        "Tree and structural headers": [
            "node.c",
            "node.h",
            "iterator.c",
            "iterator.h",
            "parser.h",
            "element.c",
            "element.h"
        ],
        "Block pipeline": ["blocks.c"],
        Tables: ["table.c", "table_scanners.c"],
        Inlines: ["inlines.c", "inline_internal.h"],
        Buffers: ["buffer.c", "buffer.h"],
        "Byte classification": ["markdown_core_ctype.c", "markdown_core_ctype.h", "cmark_ctype.c", "cmark_ctype.h"],
        "UTF-8": ["utf8.c", "utf8.h"],
        "Inline libc string operations": ["string_fortified.h"]
    };
    for (const [label, names] of Object.entries(groups)) {
        const a = names.reduce((n, k) => n + (core.files[k] ?? 0), 0),
            b = names.reduce((n, k) => n + (reference.files[k] ?? 0), 0);
        rows.push(`| ${label} | ${number(a)} | ${number(b)} | ${b ? ratio(a, b) : "—"} |`);
    }
    rows.push("", "## Core function self, same-input cohort", "", "| Function | Ir |", "| --- | ---: |");
    for (const [name, cost] of Object.entries(core.functions)
        .sort((a, b) => b[1] - a[1])
        .slice(0, 35))
        rows.push(`| \`${name}\` | ${number(cost)} |`);
    rows.push(
        "",
        "## All proved pairs",
        "",
        "| Case | Reference | A Ir | B Ir | R Ir | Grammar | Shape | Same-job |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"
    );
    for (const p of [...pairs].sort((a, b) => b.sameJob - a.sameJob))
        rows.push(
            `| \`${p.case}\` | ${p.reference} | ${number(p.a)} | ${number(p.b)} | ${number(p.r)} | ` +
                [p.grammar, p.shape, p.sameJob].map((n) => ratio(n, 1)).join(" | ") +
                " |"
        );
    rows.push(
        "",
        "## Reviewed boundaries",
        "",
        "These are whole-document interventions, including recognition, construction and byte changes. " +
            "Delta is neither an isolated feature price nor a Same-job quotient; it can be negative.",
        "",
        "| Original | Without | Full Ir | Without Ir | Delta Ir | Delta / unit | Full/without bytes |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: |"
    );
    for (const b of summary.boundaries)
        rows.push(
            `| ${b.case} | ${b.baseline} | ${number(b.full)} | ${number(b.without)} | ` +
                `${number(b.delta)} | ${(b.delta / b.units).toFixed(2)} | ${b.fullBytes}/${b.withoutBytes} |`
        );
    return rows.join("\n") + "\n";
}

export function summarizeArtifact(input, baseline = false) {
    const readReport = (directory) => JSON.parse(fs.readFileSync(path.join(directory, "stages.json"), "utf8"));
    const current = readReport(input);
    const directory = baseline ? path.join(input, "baseline") : input;
    const report = baseline ? readReport(directory) : current;
    if (baseline) {
        // The producer remeasures Core and shares this job's reference profiles.
        // Bind those two reports before reading any shared evidence; never search
        // parent directories or substitute a profile just because it exists.
        assert.ok(current.sourceBudget?.baseline, "the artifact has no recorded baseline");
        assert.equal(report.revision, current.sourceBudget.baseline, "baseline revision");
        for (const key of ["corpus", "pairingDigest", "pairs", "cmark", "cmarkGfm", "profile"])
            assert.deepEqual(report[key], current[key], `baseline ${key}`);
        const { compiled: beforeCompiled, ...beforeRuntime } = report.toolchain;
        const { compiled: afterCompiled, ...afterRuntime } = current.toolchain;
        assert.deepEqual(beforeRuntime, afterRuntime, "baseline runtime");
        for (const engine of ["cmark", "cmark-gfm", "cmark-gfm-extensions"]) {
            assert.deepEqual(report.binaries[engine], current.binaries[engine], `${engine} binary`);
            assert.deepEqual(beforeCompiled.objects[engine], afterCompiled.objects[engine], `${engine} objects`);
        }
        assert.equal(report.cases.length, current.cases.length, "baseline case count");
        const omit = (object, keys) =>
            Object.fromEntries(Object.entries(object).filter(([key]) => !keys.includes(key)));
        for (let i = 0; i < report.cases.length; i++) {
            const before = report.cases[i],
                after = current.cases[i];
            assert.deepEqual(
                omit(before, ["file", "engines"]),
                omit(after, ["file", "engines"]),
                "baseline document identity"
            );
            assert.deepEqual(
                omit(before.engines, ["markdown-core"]),
                omit(after.engines, ["markdown-core"]),
                "shared reference measurements"
            );
        }
    }
    return summarize(report, (entry, engine) =>
        parseCallgrind(
            fs.readFileSync(
                path.join(
                    engine === "markdown-core" ? directory : input,
                    "callgrind",
                    `${engine}.${entry.case}.x${entry.scale}.out`
                ),
                "utf8"
            )
        )
    );
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const input = process.argv[2],
        output = process.argv[3];
    if (
        !input ||
        !output ||
        (process.argv.length !== 4 && !(process.argv.length === 5 && process.argv[4] === "--baseline"))
    )
        throw new Error("usage: report-performance.mjs ARTIFACT_DIR OUTPUT_PREFIX [--baseline]");
    const summary = summarizeArtifact(input, process.argv[4] === "--baseline");
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.writeFileSync(`${output}.json`, JSON.stringify(summary, null, 2) + "\n");
    fs.writeFileSync(`${output}.md`, render(summary));
    console.log(
        `Reported ${summary.full.documents} documents, ${summary.members.length} same-input documents, ${summary.pairs.length} proved pairs`
    );
}
