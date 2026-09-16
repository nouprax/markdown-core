/**
 * WHAT THE ATTRIBUTE GRAMMAR COSTS, against a fast C implementation of the
 * same job.
 *
 * `{#lane .stage k="callgrind"}` has no reference. cmark reads it as text and
 * cmark-gfm reads it as text, so the stage benchmark can only bound it -- and
 * the bound it reports, 4.46x on `inline-span`, is mostly the inline parser
 * around the attributes rather than the attributes. The largest single self
 * cost in that case is `utf8proc_is_letter` at 23.1%.
 *
 * An HTML start tag's attribute list is the same job: a bracketed run split
 * into an identifier, a class run and key/value records, with quoting and
 * character references. lexbor implements that in C and is written for speed.
 * So the comparison here is one grammar against one grammar:
 *
 *   {#lane .stage k="callgrind"}      <x id="lane" class="stage" k="callgrind">
 *
 * Neither implementation reads the other's spelling, which is why this is a
 * separate driver rather than a fourth engine in `benchmark-stages.mjs`: there
 * the whole point is that both engines get byte-identical files. Here they
 * cannot, so what makes it a comparison instead of two numbers is that both
 * baselines must RECOVER THE SAME ATTRIBUTES -- each writes a canonical census
 * and the two are compared line for line before any count is reported.
 *
 * Both spellings are generated from ONE list of specifications, so the two
 * inputs cannot drift into describing different attributes; the census check
 * is what proves the generation and both recoveries agree.
 *
 * WHAT THE NUMBERS ARE. Instruction and data-reference counts under callgrind,
 * measured on the call edge into `bench_parse_attributes`, with both runners
 * built by one configure from the `benchmark` preset -- so the ratio is a fact
 * about the two grammars rather than about two builds. Absolute counts are a
 * property of the toolchain and comparable only within one report; see
 * `packages/markdown-core/benchmarks/README.md` for what that means and why.
 *
 *   node scripts/benchmark-attributes.mjs [--out DIR] [--lists N]
 */

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { baseName, callEdge, costRecord, foldNames, parseCallgrind } from "./lib/callgrind.mjs";
import { CACHE, measurementEnvironment, measurementRoot } from "./lib/measurement.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const PROFILE_PRESET = "benchmark";
const MEASURED = { caller: "main", callee: "bench_parse_attributes" };

const BASELINES = {
    "markdown-core": { runner: "packages/markdown-core/benchmarks/markdown_core_attribute_runner", spelling: "pandoc" },
    lexbor: { runner: "packages/markdown-core/benchmarks/lexbor_attribute_runner", spelling: "html" }
};

/**
 * The attribute lists both spellings are generated from.
 *
 * Written as data rather than as two files, because two files drift: an edit
 * to one spelling that nobody makes to the other produces two inputs that
 * describe different attributes, and the ratio between them would be a ratio
 * between different workloads. The census check downstream is what catches a
 * generator that was wrong as well.
 *
 * The shapes are the ones `elements/attributes.c` distinguishes: an anchor
 * alone, a class alone, several classes, records with quoted and bare values,
 * and the combinations -- plus values holding the characters the scanner has
 * to look at rather than skip.
 */
const SPECIFICATIONS = [
    { anchor: "lane", classes: ["stage"], records: [["k", "callgrind"]] },
    { anchor: "nested", classes: ["one", "two"], records: [] },
    { anchor: null, classes: ["only"], records: [] },
    { anchor: "bare", classes: [], records: [] },
    { anchor: null, classes: [], records: [["k", "v"]] },
    {
        anchor: "four",
        classes: ["a", "b"],
        records: [
            ["k", "v"],
            ["data-kind", "note"]
        ]
    },
    { anchor: null, classes: ["r"], records: [["k", "a value with spaces"]] },
    {
        anchor: "wide",
        classes: ["x", "y", "z", "w"],
        records: [
            ["one", "1"],
            ["two", "2"],
            ["three", "3"]
        ]
    },
    {
        anchor: null,
        classes: [],
        records: [
            ["k", "callgrind"],
            ["j", "valgrind"]
        ]
    },
    { anchor: "tail", classes: ["last"], records: [["k", "v"]] }
];

function fail(message) {
    process.stderr.write(`benchmark-attributes: ${message}\n`);
    process.exit(1);
}

function parseArguments(argv) {
    const options = { out: path.join(root, "build/benchmark-attributes"), lists: 4000 };
    for (let i = 0; i < argv.length; i++) {
        const flag = argv[i];
        const value = argv[i + 1];
        if (flag === "--out") {
            if (!value) fail("--out needs a value");
            options.out = path.resolve(root, argv[++i]);
        } else if (flag === "--lists") {
            if (!value) fail("--lists needs a value");
            if (!/^[1-9]\d*$/u.test(value)) fail(`--lists takes a whole number of attribute lists, not ${value}`);
            options.lists = Number(argv[++i]);
        } else {
            fail(`unknown flag ${flag}`);
        }
    }
    return options;
}

/* The two spellings of one specification.
 *
 * `id` and `class` come first in the HTML so the tag's attribute order matches
 * the census order both baselines write, and the classes are joined by exactly
 * one space so the class run is the same bytes on both sides -- neither
 * baseline is then charged for a pass over the run that the other does not
 * make. */
function pandocSpelling(specification) {
    const parts = [];
    if (specification.anchor) parts.push(`#${specification.anchor}`);
    for (const name of specification.classes) parts.push(`.${name}`);
    for (const [name, value] of specification.records) parts.push(`${name}="${value}"`);
    return `[text]{${parts.join(" ")}}`;
}

function htmlSpelling(specification) {
    const parts = [];
    if (specification.anchor) parts.push(`id="${specification.anchor}"`);
    if (specification.classes.length) parts.push(`class="${specification.classes.join(" ")}"`);
    for (const [name, value] of specification.records) parts.push(`${name}="${value}"`);
    return `<x ${parts.join(" ")}>`;
}

function writeInputs(options) {
    const directory = path.join(options.out, "input");
    fs.mkdirSync(directory, { recursive: true });
    const inputs = {};
    for (const [spelling, render] of [
        ["pandoc", pandocSpelling],
        ["html", htmlSpelling]
    ]) {
        const lines = [];
        for (let i = 0; i < options.lists; i++) {
            lines.push(render(SPECIFICATIONS[i % SPECIFICATIONS.length]));
        }
        const file = path.join(directory, `attributes.${spelling}.txt`);
        fs.writeFileSync(file, `${lines.join("\n")}\n`);
        inputs[spelling] = file;
    }
    return inputs;
}

function run(command, args, options = {}) {
    const result = spawnSync(command, args, { encoding: "utf8", cwd: root, ...options });
    if (result.error) fail(`${command} could not run: ${result.error.message}`);
    if (result.status !== 0) {
        fail(`${command} ${args.join(" ")} exited ${result.status}\n${result.stderr ?? ""}`);
    }
    return result.stdout ?? "";
}

function presetBinaryDir(preset) {
    if (!preset.binaryDir) fail(`the ${preset.name} preset must set binaryDir`);
    const expanded = preset.binaryDir.replace(/\$\{(\w+)\}/gu, (macro, name) => (name === "sourceDir" ? root : macro));
    if (expanded.includes("${")) fail(`the ${preset.name} preset's binaryDir uses an unexpandable macro`);
    return expanded;
}

/* The pinned lexbor checkout, read from the same script that installs it so
 * this cannot measure a different one than the environment check passed on. */
function pinnedLexbor() {
    const script = fs.readFileSync(path.join(root, "scripts/init-environment.sh"), "utf8");
    const version = /^LEXBOR_VERSION=(\S+)$/mu.exec(script)?.[1];
    const commit = /^LEXBOR_COMMIT=([0-9a-f]{40})$/mu.exec(script)?.[1];
    if (!version || !commit) fail("scripts/init-environment.sh does not pin lexbor");
    const checkout = path.join(root, ".tools/lexbor", version);
    const install = "scripts/init-environment.sh --install oracle-lexbor";
    if (!fs.existsSync(path.join(checkout, "source/lexbor/html/tokenizer.h"))) {
        fail(`the pinned lexbor baseline is not installed; run: ${install}`);
    }
    const head = run("git", ["-C", checkout, "rev-parse", "HEAD"]).trim();
    if (head !== commit) {
        fail(`the lexbor checkout is at ${head}, but lexbor ${version} is pinned to ${commit}; run: ${install}`);
    }
    return { version, commit, checkout, build: path.join(checkout, "build") };
}

function build(lexbor) {
    const presets = JSON.parse(fs.readFileSync(path.join(root, "CMakePresets.json"), "utf8"));
    const preset = presets.configurePresets.find((entry) => entry.name === PROFILE_PRESET);
    if (!preset) fail(`CMakePresets.json has no ${PROFILE_PRESET} configure preset`);
    const compiler = preset.cacheVariables.CMAKE_C_COMPILER;
    const flags = preset.cacheVariables.CMAKE_C_FLAGS_RELEASE;
    if (!compiler || !flags) fail(`the ${PROFILE_PRESET} preset must pin CMAKE_C_COMPILER and CMAKE_C_FLAGS_RELEASE`);
    /* One configure, both runners: the ratio is only about the two grammars if
     * the same compiler and the same options produced both binaries. */
    run("cmake", [
        "--preset",
        PROFILE_PRESET,
        `-DMARKDOWN_CORE_LEXBOR_SOURCE_DIR=${lexbor.checkout}`,
        `-DMARKDOWN_CORE_LEXBOR_BUILD_DIR=${lexbor.build}`
    ]);
    const binaryDir = presetBinaryDir(preset);
    run("cmake", [
        "--build",
        binaryDir,
        "--target",
        "markdown_core_attribute_runner",
        "--target",
        "lexbor_attribute_runner",
        "--parallel",
        String(os.cpus().length)
    ]);
    for (const [name, definition] of Object.entries(BASELINES)) {
        const binary = path.join(binaryDir, definition.runner);
        if (!fs.existsSync(binary)) fail(`the profile build produced no ${name} attribute runner at ${binary}`);
    }
    return { compiler, flags, binaryDir };
}

/**
 * The check that makes this a comparison.
 *
 * Both baselines write what they recovered in one canonical form, and the two
 * must agree line for line. A baseline that skipped a record, kept a value
 * raw, stopped at the first malformed list or never saw the input at all would
 * otherwise post a cheaper number for doing less, and nothing in a count would
 * say so.
 */
function requireSameAttributes(profile, inputs) {
    const census = {};
    for (const [name, definition] of Object.entries(BASELINES)) {
        census[name] = run(path.join(profile.binaryDir, definition.runner), [
            "--input",
            inputs[definition.spelling],
            "--census"
        ]);
    }
    const [left, right] = Object.keys(BASELINES);
    if (census[left] === census[right]) return census[left].split("\n").filter(Boolean).length;
    const leftLines = census[left].split("\n");
    const rightLines = census[right].split("\n");
    const at = leftLines.findIndex((line, index) => line !== rightLines[index]);
    fail(
        `${left} and ${right} did not recover the same attributes, first at line ${at + 1}:\n` +
            `  ${left}: ${leftLines[at] ?? "(nothing)"}\n  ${right}: ${rightLines[at] ?? "(nothing)"}`
    );
    return 0;
}

/**
 * Where each baseline's instructions went, as SELF cost.
 *
 * A ratio says one grammar is dearer; this says what it is spending it on, and
 * the two baselines' lists read against each other are the finding. Inclusive
 * cost would not: the top of that list is always the entry point, at 100%.
 *
 * Restricted to what the measured entry can reach, because callgrind collects
 * from process start and the ranking would otherwise include the loader,
 * reading the input file and printing the receipt. A leaf shared with the rest
 * of the program -- `free` being the obvious one -- is still counted whole, so
 * this narrows the claim rather than making it exact.
 */
function hotPaths(parsed) {
    const callees = new Map();
    for (const edge of parsed.edges.values()) {
        const from = baseName(edge.caller);
        if (!callees.has(from)) callees.set(from, new Set());
        callees.get(from).add(baseName(edge.callee));
    }
    const reachable = new Set();
    const pending = [baseName(MEASURED.callee)];
    while (pending.length) {
        const name = pending.pop();
        if (reachable.has(name)) continue;
        reachable.add(name);
        for (const callee of callees.get(name) ?? []) pending.push(callee);
    }
    const totals = new Map();
    let whole = 0;
    for (const [name, cost] of parsed.self) {
        const ir = costRecord(parsed, cost).Ir ?? 0;
        if (!ir) continue;
        const fn = baseName(name);
        if (!reachable.has(fn)) continue;
        totals.set(fn, (totals.get(fn) ?? 0) + ir);
        whole += ir;
    }
    return [...totals.entries()]
        .sort((left, right) => right[1] - left[1])
        .slice(0, 6)
        .map(([name, ir]) => ({ name, ir, share: whole ? ir / whole : 0 }));
}

function measure(profile, name, inputs, out) {
    const definition = BASELINES[name];
    const dump = path.join(out, "callgrind", `${name}.out`);
    fs.mkdirSync(path.dirname(dump), { recursive: true });
    const isolated = measurementRoot(out, fail);
    const stdout = run(
        "valgrind",
        [
            "--tool=callgrind",
            "--cache-sim=yes",
            "--dump-instr=no",
            ...CACHE,
            `--callgrind-out-file=${dump}`,
            "--quiet",
            path.join(profile.binaryDir, definition.runner),
            "--input",
            inputs[definition.spelling]
        ],
        { env: measurementEnvironment(isolated), cwd: isolated }
    );
    const receipt = /lists=(\d+) values=(\d+)/u.exec(stdout);
    if (!receipt) fail(`${name} produced no receipt`);
    const parsed = foldNames(parseCallgrind(fs.readFileSync(dump, "utf8")), (entry) => entry);
    const edge = callEdge(parsed, MEASURED.caller, MEASURED.callee);
    if (!edge) fail(`${name}: no call edge ${MEASURED.caller} -> ${MEASURED.callee} in ${path.basename(dump)}`);
    const cost = costRecord(parsed, edge.cost);
    return {
        lists: Number(receipt[1]),
        values: Number(receipt[2]),
        ir: cost.Ir,
        dataReads: cost.Dr,
        dataWrites: cost.Dw,
        hotPaths: hotPaths(parsed)
    };
}

function markdownReport(report) {
    const lines = [
        "# Attribute grammar against lexbor",
        "",
        '`{#lane .stage k="callgrind"}` has no reference implementation. cmark reads it' +
            " as text and so does cmark-gfm, so the stage benchmark can only bound it --" +
            " and that bound is mostly the inline parser around the attributes rather than" +
            " the attributes.",
        "",
        "An HTML start tag's attribute list is the same job: a bracketed run split into an" +
            " identifier, a class run and key/value records, with quoting and character" +
            " references. This measures that grammar against lexbor's, one grammar against" +
            " one grammar, on the same attributes in two spellings:",
        "",
        "```",
        `${pandocSpelling(SPECIFICATIONS[0])}`,
        `${htmlSpelling(SPECIFICATIONS[0])}`,
        "```",
        "",
        "Both inputs are generated from one list of specifications, and **both baselines" +
            " are required to recover the same attributes**: each writes a canonical census" +
            " and the two are compared line for line before any count below is reported. A" +
            " baseline that skipped a record or stopped early fails the run rather than" +
            " posting a cheaper number for doing less.",
        "",
        `| Baseline | Spelling | Lists | Values | Ir | Ir/list | Dr | Dw |`,
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"
    ];
    for (const [name, result] of Object.entries(report.baselines)) {
        lines.push(
            `| \`${name}\` | ${BASELINES[name].spelling} | ${result.lists.toLocaleString("en-US")} |` +
                ` ${result.values.toLocaleString("en-US")} | ${result.ir.toLocaleString("en-US")} |` +
                ` ${(result.ir / result.lists).toFixed(1)} | ${result.dataReads.toLocaleString("en-US")} |` +
                ` ${result.dataWrites.toLocaleString("en-US")} |`
        );
    }
    const ours = report.baselines["markdown-core"];
    const theirs = report.baselines.lexbor;
    lines.push(
        "",
        `**${(ours.ir / theirs.ir).toFixed(2)}x on instructions, ` +
            `${((ours.dataReads + ours.dataWrites) / (theirs.dataReads + theirs.dataWrites)).toFixed(2)}x on data ` +
            "references**, over the same attributes recovered the same way.",
        "",
        "### Where the cost is",
        "",
        "Self cost, restricted to what the measured entry reaches -- the instructions" +
            " each baseline spent IN a function rather than through it.",
        "",
        "| Baseline | Dominant self cost |",
        "| --- | --- |",
        ...Object.entries(report.baselines).map(
            ([name, result]) =>
                `| \`${name}\` | ${(result.hotPaths ?? [])
                    .slice(0, 5)
                    .map((entry) => `\`${entry.name}\` ${(entry.share * 100).toFixed(1)}%`)
                    .join(", ")} |`
        ),
        "",
        "### What the ratio is and is not",
        "",
        "It is a comparison: both baselines were given the same attributes, both recovered" +
            " them, and the census proving that is checked on every run.",
        "",
        "It is not a like-for-like implementation comparison in every respect. lexbor's" +
            " tokenizer is a state machine over a whole document and reaches an attribute" +
            " list already inside a tag; this parser recognizes a candidate first and can" +
            " be asked about a run that turns out not to be one. Both costs are in the" +
            " numbers above, because both are what each implementation does to get from" +
            " bytes to attributes.",
        "",
        "The counts are not time. Ir does not price a cache miss, a branch miss or a stall.",
        "",
        `| | |`,
        "| --- | --- |",
        `| Compiler | \`${report.toolchain.compiler}\` |`,
        `| Profile flags | \`${report.toolchain.flags}\` |`,
        `| lexbor | ${report.lexbor.version} (\`${report.lexbor.commit.slice(0, 12)}\`) |`,
        `| Measured edge | \`${MEASURED.caller} -> ${MEASURED.callee}\` |`,
        "",
        "Absolute counts depend on the toolchain, so two reports are comparable only when" +
            " those rows match; `packages/markdown-core/benchmarks/README.md` sets out why" +
            " that is stricter than it sounds. Both binaries here come from one configure," +
            " so the ratio is about the two grammars rather than about two builds.",
        "",
        `Raw callgrind dumps are in \`${path.relative(root, report.artifacts)}\`.`
    );
    return lines.join("\n");
}

function main() {
    const options = parseArguments(process.argv.slice(2));
    if (spawnSync("valgrind", ["--version"], { encoding: "utf8" }).status !== 0) {
        fail("valgrind is not available; run: scripts/init-environment.sh --install callgrind");
    }
    const lexbor = pinnedLexbor();
    fs.mkdirSync(options.out, { recursive: true });
    const inputs = writeInputs(options);
    const profile = build(lexbor);
    const recovered = requireSameAttributes(profile, inputs);
    const baselines = {};
    for (const name of Object.keys(BASELINES)) {
        baselines[name] = measure(profile, name, inputs, options.out);
        if (baselines[name].lists !== recovered) {
            fail(`${name} measured ${baselines[name].lists} lists but its census held ${recovered}`);
        }
    }
    const report = {
        schemaVersion: 1,
        toolchain: { compiler: profile.compiler, flags: profile.flags },
        lexbor: { version: lexbor.version, commit: lexbor.commit },
        artifacts: options.out,
        specifications: SPECIFICATIONS.length,
        baselines
    };
    fs.writeFileSync(path.join(options.out, "attributes.json"), `${JSON.stringify(report, null, 4)}\n`);
    const rendered = markdownReport(report);
    fs.writeFileSync(path.join(options.out, "attributes.md"), `${rendered}\n`);
    process.stdout.write(`${rendered}\n`);
}

main();
