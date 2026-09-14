#!/usr/bin/env node
// The ES timing lane: source string to value tree through the published
// entry, on the workloads the C timing lane reads, so the two measure the
// same bytes. Opt-in, informational, never a gate: `pnpm benchmark:es`.
//
//   node scripts/benchmark.mjs [--samples DIR] [--copies N] [--repeats N]
//       [--warmup N] [--case NAME] [--json FILE] [FILE...]
//
// Each case reports the parse (source in, tree out) and a walk of the tree
// with an empty visitor, the minimum and the median of every repeat, the
// throughput from the bytes and the time per node, and the SHA-256 of the
// input so a measurement can prove which bytes it read. A file argument is a
// case of its own bytes.
import { createHash } from "node:crypto";
import { existsSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const packageDirectory = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const repositoryRoot = path.resolve(packageDirectory, "..", "..");
const distribution = path.join(packageDirectory, "dist", "index.js");
if (!existsSync(distribution)) {
    console.error("build the package first: pnpm --filter @nouprax/es-markdown-core build");
    process.exit(2);
}
const { Document, walk } = await import(distribution);

const options = {
    samples: path.join(repositoryRoot, "packages", "markdown-core", "benchmarks", "samples"),
    copies: 200,
    repeats: 20,
    warmup: 3,
    case: null,
    json: null,
    files: []
};
for (let index = 2; index < process.argv.length; index++) {
    const argument = process.argv[index];
    const value = () => {
        const next = process.argv[++index];
        if (next === undefined) throw new Error(`${argument} needs a value`);
        return next;
    };
    if (argument === "--samples") options.samples = value();
    else if (argument === "--copies") options.copies = Number(value());
    else if (argument === "--repeats") options.repeats = Number(value());
    else if (argument === "--warmup") options.warmup = Number(value());
    else if (argument === "--case") options.case = value();
    else if (argument === "--json") options.json = value();
    else options.files.push(argument);
}

// The same generators as `tests/support/bench_workloads.c`, byte for byte:
// the baseline unit repeated, and a tracked sample repeated with a blank
// line between copies so the repeated shape is the sample's shape.
const BASELINE_UNIT = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
const cases = [
    {
        name: "binding_baseline",
        generator: "binding_baseline",
        parameters: "scale=2000",
        source: BASELINE_UNIT.repeat(2000)
    },
    { name: "empty_document", generator: "empty_document", parameters: "", source: "" }
];
for (const name of readdirSync(options.samples)
    .filter((entry) => entry.endsWith(".md"))
    .sort()) {
    const sample = readFileSync(path.join(options.samples, name), "utf8");
    cases.push({
        name,
        generator: "sample",
        parameters: `copies=${options.copies}`,
        source: `${sample}\n\n`.repeat(options.copies)
    });
}
for (const file of options.files) {
    cases.push({ name: path.basename(file), generator: "file", parameters: file, source: readFileSync(file, "utf8") });
}

// Every kind's callback is the same empty function: the walk measures the
// traversal and the dispatch, not a visitor.
const KINDS = [
    "document",
    "callout",
    "paragraph",
    "heading",
    "thematicBreak",
    "list",
    "listItem",
    "codeBlock",
    "htmlBlock",
    "formulaBlock",
    "table",
    "directiveBlock",
    "text",
    "softBreak",
    "lineBreak",
    "code",
    "html",
    "formula",
    "emphasis",
    "strong",
    "strikethrough",
    "link",
    "embedded",
    "directive",
    "cite",
    "tableRow",
    "tableCell",
    "directiveLabel",
    "comment",
    "crossLink",
    "mark",
    "crossEmbedded",
    "insertion",
    "span",
    "superscript",
    "subscript",
    "definitionList",
    "definition",
    "tableCaption",
    "citation",
    "footnote",
    "specimen",
    "metadata"
];
let visits = 0;
const counting = () => {
    visits++;
};
const visitor = Object.fromEntries(KINDS.map((kind) => [kind, counting]));

const sha256 = (text) => createHash("sha256").update(text, "utf8").digest("hex");
const median = (values) => {
    const sorted = [...values].sort((a, b) => a - b);
    return sorted[Math.floor(sorted.length / 2)];
};
const nanoseconds = () => Number(process.hrtime.bigint());

const results = [];
for (const item of cases) {
    if (options.case && item.name !== options.case) continue;
    const bytes = Buffer.byteLength(item.source, "utf8");
    for (let i = 0; i < options.warmup; i++) walk(Document.parse(item.source), visitor);
    const samples = [];
    let nodes = 0;
    for (let i = 0; i < options.repeats; i++) {
        const parseStart = nanoseconds();
        const document = Document.parse(item.source);
        const parseEnd = nanoseconds();
        visits = 0;
        walk(document, visitor);
        const walkEnd = nanoseconds();
        nodes = visits / 2;
        samples.push({ parseNs: parseEnd - parseStart, walkNs: walkEnd - parseEnd });
    }
    const minParseNs = Math.min(...samples.map((sample) => sample.parseNs));
    const minWalkNs = Math.min(...samples.map((sample) => sample.walkNs));
    const result = {
        name: item.name,
        generator: item.generator,
        parameters: item.parameters,
        bytes,
        inputSha256: sha256(item.source),
        nodes,
        minParseNs,
        medianParseNs: median(samples.map((sample) => sample.parseNs)),
        minWalkNs,
        medianWalkNs: median(samples.map((sample) => sample.walkNs)),
        mbPerSecond: minParseNs > 0 ? Number((bytes / 1e6 / (minParseNs / 1e9)).toFixed(3)) : 0,
        nsPerNode: nodes > 0 ? Number((minParseNs / nodes).toFixed(3)) : 0,
        samples
    };
    results.push(result);
    console.log(
        `benchmark case=${result.name} bytes=${bytes} nodes=${nodes} repeats=${options.repeats} warmup=${options.warmup} ` +
            `min_parse_ns=${result.minParseNs} median_parse_ns=${result.medianParseNs} min_walk_ns=${result.minWalkNs} ` +
            `median_walk_ns=${result.medianWalkNs} mb_per_s=${result.mbPerSecond} ns_per_node=${result.nsPerNode} ` +
            `sha256=${result.inputSha256}`
    );
}
if (options.json) {
    writeFileSync(
        options.json,
        `${JSON.stringify(
            {
                schema: 2,
                runtime: "es",
                lane: "timing",
                workload: "binding",
                workloadVersion: 1,
                warmup: options.warmup,
                repeats: options.repeats,
                cases: results
            },
            null,
            2
        )}\n`
    );
}
