#!/usr/bin/env node
/**
 * Stage benchmark: Markdown Core against cmark, one parse stage at a time.
 *
 * WHAT IS MEASURED. A parse has two paths worth optimizing separately: the
 * source bytes being read into the block buffers, and those buffers being
 * turned into an AST. Everything around them -- allocating the parser,
 * attaching the fixed dialect, discovering elements, and releasing the tree --
 * is fixed cost that no document-size argument applies to, so it is excluded
 * rather than amortized into a number that looks like parsing.
 *
 *   source_to_buffer   markdown-core  markdown_core_parse_document_with_mem
 *                                       -> S_parse_source
 *                      cmark          cmark_parser_feed
 *   buffer_to_ast      markdown-core  markdown_core_parse_document_with_mem
 *                                       -> S_finish_parse
 *                      cmark          cmark_parser_finish
 *
 * WHY cmark's FEED API. cmark splits the same two paths across two public
 * calls, so feeding the whole document and then finishing gives a boundary
 * that is the same boundary, not an approximation of one. Both engines get
 * byte-identical documents built from the same tracked corpus.
 *
 * WHY CALLGRIND. Instruction and data-reference counts are a property of the
 * program, not of the machine it ran on, so a hosted runner reports the same
 * numbers a laptop does and a 2% change is a real 2%. Wall clock cannot make
 * that claim, which is why the pipeline this replaced could only ever be
 * informational. Nothing here is a merge gate: it is the measurement an
 * optimization is argued from.
 *
 * WHAT THE NUMBERS ARE NOT. Ir is work, not time: it does not price a cache
 * miss, a branch miss, or a dependency stall. A change that trades three
 * instructions for one random memory access will look like an improvement
 * here. Dr/Dw are reported alongside for exactly that reason.
 *
 *   node scripts/benchmark-stages.mjs [--out DIR] [--case NAME]... [--scale N]
 *                                     [--skip-build] [--quiet]
 */

import { Buffer } from "node:buffer";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { callEdge, calleesOf, costRecord, foldNames, parseCallgrind } from "./lib/callgrind.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const BENCHMARKS = path.join(root, "packages/markdown-core/benchmarks");

/* A cache geometry pinned in the report rather than taken from the host, so
 * that two machines produce the same file and a diff means a code change. */
const CACHE = ["--I1=32768,8,64", "--D1=32768,8,64", "--LL=8388608,16,64"];

/* GCC clones a function when it specializes it; the clone carries the work but
 * not the plain name the stage boundary is written as. */
const CLONE_SUFFIX = /(\.(constprop|isra|part|cold|lto_priv|localalias)\.?\d*)+$/u;

const ENGINES = {
    "markdown-core": {
        runner: "packages/markdown-core/benchmarks/markdown_core_stage_runner",
        stages: {
            source_to_buffer: { caller: "markdown_core_parse_document_with_mem", callee: "S_parse_source" },
            buffer_to_ast: { caller: "markdown_core_parse_document_with_mem", callee: "S_finish_parse" }
        }
    },
    cmark: {
        runner: "packages/markdown-core/benchmarks/cmark_stage_runner",
        stages: {
            source_to_buffer: { caller: "bench_parse_document", callee: "cmark_parser_feed" },
            buffer_to_ast: { caller: "bench_parse_document", callee: "cmark_parser_finish" }
        }
    }
};

const STAGES = ["source_to_buffer", "buffer_to_ast"];

/* The harness entry both runners publish, and so the whole parse path a stage
 * is a part of: parser creation, the two stages, and releasing the tree. */
const ENGINE_ENTRY = "bench_parse_document";

function fail(message) {
    console.error(`benchmark-stages: ${message}`);
    process.exit(1);
}

function run(command, args, options = {}) {
    const result = spawnSync(command, args, { encoding: "utf8", ...options });
    if (result.error) fail(`${command} could not be run: ${result.error.message}`);
    if (result.status !== 0) {
        fail(`${command} ${args.join(" ")} exited ${result.status}\n${result.stdout ?? ""}${result.stderr ?? ""}`);
    }
    return result.stdout ?? "";
}

function parseArguments(argv) {
    const options = { out: path.join(root, "build/benchmark-stages"), cases: [], scale: 2, build: true, quiet: false };
    for (let index = 0; index < argv.length; index++) {
        const flag = argv[index];
        const value = argv[index + 1];
        if (flag === "--skip-build") {
            options.build = false;
        } else if (flag === "--quiet") {
            options.quiet = true;
        } else if (!value) {
            fail(`${flag} needs a value`);
        } else if (flag === "--out") {
            options.out = path.resolve(value);
            index++;
        } else if (flag === "--case") {
            options.cases.push(value);
            index++;
        } else if (flag === "--scale") {
            options.scale = Number.parseInt(value, 10);
            index++;
        } else {
            fail(`unknown argument: ${flag}`);
        }
    }
    if (!Number.isInteger(options.scale) || options.scale < 1) fail("--scale must be a positive integer");
    return options;
}

/** The one place the profile's compiler and flags are written down. */
function profileBuild() {
    const presets = JSON.parse(fs.readFileSync(path.join(root, "CMakePresets.json"), "utf8"));
    const preset = presets.configurePresets.find((entry) => entry.name === "benchmark");
    if (!preset) fail("CMakePresets.json has no benchmark configure preset");
    const compiler = preset.cacheVariables.CMAKE_C_COMPILER;
    const flags = preset.cacheVariables.CMAKE_C_FLAGS_RELEASE;
    if (!compiler || !flags) fail("the benchmark preset must pin CMAKE_C_COMPILER and CMAKE_C_FLAGS_RELEASE");
    return { compiler, flags, binaryDir: path.join(root, "build/benchmark") };
}

/** The pinned cmark the parity oracles already use; no second version exists. */
function pinnedCmark() {
    const script = fs.readFileSync(path.join(root, "scripts/init-environment.sh"), "utf8");
    const version = /^CMARK_VERSION=(.+)$/mu.exec(script)?.[1];
    const commit = /^CMARK_COMMIT=([0-9a-f]{40})$/mu.exec(script)?.[1];
    if (!version || !commit) fail("scripts/init-environment.sh does not pin cmark");
    const checkout = path.join(root, ".tools/cmark", version);
    if (!fs.existsSync(path.join(checkout, "src/cmark.h"))) {
        fail(`the pinned cmark oracle is not installed; run: scripts/init-environment.sh --install oracle-cmark`);
    }
    return { version, commit, checkout };
}

function buildCmark(profile, cmark, out) {
    const buildDir = path.join(out, "cmark");
    run("cmake", [
        "-S",
        cmark.checkout,
        "-B",
        buildDir,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_TESTING=OFF",
        "-DBUILD_SHARED_LIBS=OFF",
        `-DCMAKE_C_COMPILER=${profile.compiler}`,
        `-DCMAKE_C_FLAGS_RELEASE=${profile.flags}`
    ]);
    run("cmake", ["--build", buildDir, "--parallel"]);
    return path.join(buildDir, "src");
}

/**
 * CMake keeps the compiler and the release flags in its cache, and a cache
 * written by an earlier preset wins over the preset that is being asked for
 * now. That is how a profile build silently comes out compiled by whatever
 * built the tree last, with the stage boundaries inlined away -- so the cache
 * is checked against the preset and discarded rather than reused.
 */
function discardMismatchedCache(profile) {
    const cache = path.join(profile.binaryDir, "CMakeCache.txt");
    if (!fs.existsSync(cache)) return;
    const text = fs.readFileSync(cache, "utf8");
    const entry = (name) => new RegExp(`^${name}:[A-Z]+=(.*)$`, "mu").exec(text)?.[1] ?? "";
    if (entry("CMAKE_C_FLAGS_RELEASE") === profile.flags && entry("CMAKE_C_COMPILER").endsWith(profile.compiler)) {
        return;
    }
    fs.rmSync(profile.binaryDir, { recursive: true, force: true });
}

function buildRunners(profile, cmark, cmarkBuildDir) {
    discardMismatchedCache(profile);
    run("cmake", [
        "--preset",
        "benchmark",
        `-DMARKDOWN_CORE_CMARK_SOURCE_DIR=${path.join(cmark.checkout, "src")}`,
        `-DMARKDOWN_CORE_CMARK_BUILD_DIR=${cmarkBuildDir}`
    ]);
    run("cmake", ["--build", "--preset", "benchmark", "--parallel"]);
}

/**
 * A stage boundary that the compiler folded into its caller is not a smaller
 * number, it is a missing one; the profile flags exist to prevent it and this
 * is where that contract is checked instead of discovered as a zero.
 */
function verifyStageSymbols(profile) {
    for (const [engine, definition] of Object.entries(ENGINES)) {
        const binary = path.join(profile.binaryDir, definition.runner);
        if (!fs.existsSync(binary)) fail(`${engine}: ${definition.runner} was not built`);
        const symbols = new Set(
            run("nm", ["-a", binary])
                .split("\n")
                .map((line) => line.trim().split(/\s+/u).pop() ?? "")
                .map((name) => name.replace(CLONE_SUFFIX, ""))
        );
        for (const boundary of Object.values(definition.stages)) {
            for (const name of [boundary.caller, boundary.callee]) {
                if (!symbols.has(name)) {
                    fail(
                        `${engine}: ${name} is absent from ${definition.runner}. The profile build must keep the ` +
                            `stage boundaries out of line; check CMAKE_C_FLAGS_RELEASE in the benchmark preset.`
                    );
                }
            }
        }
    }
}

/**
 * One document per case, built from the tracked samples.
 *
 * Each sample is normalized to end in exactly one newline and the whole unit
 * gets a blank line after it, so that repeating a unit cannot merge the last
 * block of one copy into the first block of the next -- which would make the
 * document's structure, and so its cost, a non-linear function of the repeat
 * count and quietly ruin the scaling comparison.
 */
function buildCorpus(options) {
    const manifest = JSON.parse(fs.readFileSync(path.join(BENCHMARKS, "corpus.json"), "utf8"));
    if (manifest.schemaVersion !== 1) fail(`unsupported corpus schema: ${manifest.schemaVersion}`);
    const directory = path.join(options.out, "corpus");
    fs.mkdirSync(directory, { recursive: true });

    const selected = options.cases.length
        ? manifest.cases.filter((entry) => options.cases.includes(entry.name))
        : manifest.cases;
    if (!selected.length) fail(`no corpus case matched ${options.cases.join(", ")}`);

    const documents = [];
    for (const entry of selected) {
        const unit =
            entry.samples
                .map((sample) => {
                    const file = path.join(BENCHMARKS, "samples", sample);
                    if (!fs.existsSync(file)) fail(`corpus.json names a missing sample: ${sample}`);
                    return `${fs.readFileSync(file, "utf8").replace(/\n*$/u, "")}\n`;
                })
                .join("") + "\n";
        for (let scale = 1; scale <= options.scale; scale++) {
            const target = manifest.targetBytes * scale;
            const repeats = Math.max(1, Math.ceil(target / Buffer.byteLength(unit)));
            const text = unit.repeat(repeats);
            const file = path.join(directory, `${entry.name}.x${scale}.md`);
            fs.writeFileSync(file, text);
            documents.push({
                case: entry.name,
                dialect: entry.dialect,
                scale,
                repeats,
                bytes: Buffer.byteLength(text),
                file
            });
        }
    }
    return { targetBytes: manifest.targetBytes, documents };
}

function measure(profile, engine, document, out) {
    const definition = ENGINES[engine];
    const dump = path.join(out, "callgrind", `${engine}.${document.case}.x${document.scale}.out`);
    fs.mkdirSync(path.dirname(dump), { recursive: true });
    const stdout = run("valgrind", [
        "--tool=callgrind",
        "--cache-sim=yes",
        "--dump-instr=no",
        ...CACHE,
        `--callgrind-out-file=${dump}`,
        "--quiet",
        path.join(profile.binaryDir, definition.runner),
        "--document",
        document.file
    ]);

    const receipt = /bytes=(\d+) root_children=(\d+)/u.exec(stdout);
    if (!receipt) fail(`${engine}: ${document.case} produced no receipt`);

    const parsed = parseCallgrind(fs.readFileSync(dump, "utf8"));
    const profileByName = foldNames(parsed, (name) => name.replace(CLONE_SUFFIX, ""));
    const stages = {};
    for (const stage of STAGES) {
        const boundary = definition.stages[stage];
        const edge = callEdge(profileByName, boundary.caller, boundary.callee);
        if (!edge) {
            fail(`${engine}: no call edge ${boundary.caller} -> ${boundary.callee} in ${path.basename(dump)}`);
        }
        stages[stage] = {
            entry: `${boundary.caller} -> ${boundary.callee}`,
            calls: edge.calls,
            cost: costRecord(profileByName, edge.cost),
            breakdown: calleesOf(profileByName, boundary.callee)
                .map((callee) => ({ callee: callee.callee, cost: costRecord(profileByName, callee.cost) }))
                .sort((left, right) => right.cost.Ir - left.cost.Ir)
                .slice(0, 8)
        };
    }
    /* What excluding setup, discovery and release actually excluded. Reading
     * it keeps the exclusion auditable: a claim that fixed cost is small is a
     * measurement, and a stage split that has quietly stopped covering the
     * parse shows up here as a growing remainder rather than not at all. */
    const whole = callEdge(profileByName, "main", ENGINE_ENTRY);
    if (!whole) fail(`${engine}: no call edge main -> ${ENGINE_ENTRY} in ${path.basename(dump)}`);
    const parsePathIr = costRecord(profileByName, whole.cost).Ir ?? 0;

    return {
        rootChildren: Number(receipt[2]),
        receiptBytes: Number(receipt[1]),
        parsePathIr,
        outsideStagesIr: STAGES.reduce((total, stage) => total - stages[stage].cost.Ir, parsePathIr),
        stages,
        dump
    };
}

function derive(document, stage) {
    const ir = stage.cost.Ir ?? 0;
    const data = (stage.cost.Dr ?? 0) + (stage.cost.Dw ?? 0);
    return {
        ir,
        dataRefs: data,
        irPerByte: ir / document.bytes,
        dataRefsPerByte: data / document.bytes,
        bytesPerMegaIr: ir > 0 ? document.bytes / (ir / 1e6) : 0
    };
}

/** The share of each engine's parse path the two stages actually cover. */
function coverage(report) {
    const shares = [];
    for (const entry of report.cases) {
        for (const engine of Object.values(entry.engines)) {
            if (engine.parsePathIr > 0) {
                shares.push((engine.parsePathIr - engine.outsideStagesIr) / engine.parsePathIr);
            }
        }
    }
    if (!shares.length) return "an unknown share of";
    /* Two decimals: a stage split covering 99.98% of the path must not be
     * reported as covering all of it. */
    const percent = (value) => `${(value * 100).toFixed(2)}%`;
    return `${percent(Math.min(...shares))} to ${percent(Math.max(...shares))}`;
}

function ratio(head, base) {
    if (!base) return "n/a";
    return `${(head / base).toFixed(2)}x`;
}

function markdownReport(report) {
    const lines = [];
    lines.push("## Parse stage comparison", "");
    lines.push(
        `Markdown Core against cmark \`${report.cmark.version}\` on the same corpus, ` +
            `measured with \`${report.valgrind}\`.`,
        "",
        `Both engines are compiled by \`${report.profile.compiler}\` with ` +
            `\`${report.profile.flags}\`. Counts are deterministic: the same commit ` +
            "reports the same numbers on any machine.",
        ""
    );

    lines.push("### Cost per input byte", "");
    lines.push(
        "| Case | Dialect | Bytes | Stage | Core Ir/B | cmark Ir/B | Ir ratio |" +
            " Core refs/B | cmark refs/B | Refs ratio |",
        "| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |"
    );
    for (const entry of report.cases.filter((item) => item.scale === 1)) {
        for (const stage of STAGES) {
            const core = entry.engines["markdown-core"]?.stages[stage];
            const cmark = entry.engines.cmark?.stages[stage];
            if (!core || !cmark) continue;
            lines.push(
                `| ${entry.case} | ${entry.dialect} | ${entry.bytes.toLocaleString("en-US")} | ${stage} |` +
                    ` ${core.irPerByte.toFixed(1)} | ${cmark.irPerByte.toFixed(1)} |` +
                    ` ${ratio(core.ir, cmark.ir)} |` +
                    ` ${core.dataRefsPerByte.toFixed(1)} | ${cmark.dataRefsPerByte.toFixed(1)} |` +
                    ` ${ratio(core.dataRefs, cmark.dataRefs)} |`
            );
        }
    }
    lines.push("");

    /* The same facts as Ir/B, inverted into the unit an optimization target is
     * usually stated in. Only the mixed cases: a per-sample throughput table
     * is the first table again with the numbers turned upside down. */
    const mixed = report.cases.filter((item) => item.scale === 1 && item.case.startsWith("mixed-"));
    if (mixed.length) {
        lines.push(
            "### Throughput",
            "",
            "Input bytes per million instructions, over the concatenated corpus.",
            "",
            "| Case | Stage | Core B/MIr | cmark B/MIr |",
            "| --- | --- | ---: | ---: |"
        );
        for (const entry of mixed) {
            for (const stage of STAGES) {
                const core = entry.engines["markdown-core"]?.stages[stage];
                const cmark = entry.engines.cmark?.stages[stage];
                if (!core || !cmark) continue;
                lines.push(
                    `| ${entry.case} | ${stage} | ${Math.round(core.bytesPerMegaIr).toLocaleString("en-US")} |` +
                        ` ${Math.round(cmark.bytesPerMegaIr).toLocaleString("en-US")} |`
                );
            }
        }
        lines.push("");
    }

    const scaled = report.cases.filter((item) => item.scale > 1);
    if (scaled.length) {
        lines.push(
            "### Growth against input size",
            "",
            "Each case is measured again at a larger size. A stage whose cost is linear in" +
                " the input reports a growth ratio equal to the byte ratio.",
            "",
            "| Case | Byte ratio | Stage | Core growth | cmark growth |",
            "| --- | ---: | --- | ---: | ---: |"
        );
        for (const entry of scaled) {
            const base = report.cases.find((item) => item.case === entry.case && item.scale === 1);
            if (!base) continue;
            for (const stage of STAGES) {
                const core = entry.engines["markdown-core"]?.stages[stage];
                const coreBase = base.engines["markdown-core"]?.stages[stage];
                const cmark = entry.engines.cmark?.stages[stage];
                const cmarkBase = base.engines.cmark?.stages[stage];
                if (!core || !coreBase || !cmark || !cmarkBase) continue;
                lines.push(
                    `| ${entry.case} | ${(entry.bytes / base.bytes).toFixed(2)}x | ${stage} |` +
                        ` ${ratio(core.ir, coreBase.ir)} | ${ratio(cmark.ir, cmarkBase.ir)} |`
                );
            }
        }
        lines.push("");
    }

    lines.push(
        `The two stages cover ${coverage(report)} of each engine's parse path across the` +
            " corpus. The rest is parser allocation, dialect attachment and element" +
            " discovery -- setup that no document-size argument applies to -- plus releasing" +
            " the finished tree, which is not parsing either. Amortizing any of it into the" +
            " stages would produce a number that looks like parsing and isn't.",
        "",
        "Ir counts executed instructions and Dr/Dw count data references. Neither prices a" +
            " cache miss, a branch miss or a stall, so a change that trades instructions for" +
            " random memory access improves these numbers without improving the parser.",
        "",
        `Per-stage call breakdowns and the raw callgrind dumps are in \`${report.artifacts}\`.`,
        ""
    );
    return lines.join("\n");
}

function main() {
    const options = parseArguments(process.argv.slice(2));
    const profile = profileBuild();
    const cmark = pinnedCmark();

    const valgrind = /valgrind-[0-9.]+/u.exec(run("valgrind", ["--version"]))?.[0];
    if (!valgrind) fail("valgrind is required; install it and re-run");

    fs.mkdirSync(options.out, { recursive: true });
    if (options.build) {
        const cmarkBuildDir = buildCmark(profile, cmark, options.out);
        buildRunners(profile, cmark, cmarkBuildDir);
    }
    verifyStageSymbols(profile);

    const corpus = buildCorpus(options);
    const cases = [];
    for (const document of corpus.documents) {
        const engines = {};
        for (const engine of Object.keys(ENGINES)) {
            const measured = measure(profile, engine, document, options.out);
            if (measured.receiptBytes !== document.bytes) {
                fail(`${engine}: ${document.case} saw ${measured.receiptBytes} bytes, expected ${document.bytes}`);
            }
            engines[engine] = {
                parsePathIr: measured.parsePathIr,
                outsideStagesIr: measured.outsideStagesIr,
                rootChildren: measured.rootChildren,
                stages: Object.fromEntries(
                    STAGES.map((stage) => [
                        stage,
                        { ...derive(document, measured.stages[stage]), ...measured.stages[stage] }
                    ])
                )
            };
        }
        if (!options.quiet) console.error(`measured ${document.case} x${document.scale}`);
        cases.push({ ...document, file: path.relative(options.out, document.file), engines });
    }

    const report = {
        schemaVersion: 1,
        valgrind,
        profile: { compiler: profile.compiler, flags: profile.flags },
        cmark: { version: cmark.version, commit: cmark.commit },
        corpus: { targetBytes: corpus.targetBytes, cases: corpus.documents.length },
        artifacts: path.relative(root, options.out),
        cases
    };
    const json = path.join(options.out, "stages.json");
    const markdown = path.join(options.out, "stages.md");
    fs.writeFileSync(json, `${JSON.stringify(report, null, 4)}\n`);
    const rendered = markdownReport(report);
    fs.writeFileSync(markdown, `${rendered}\n`);
    process.stdout.write(`${rendered}\n`);
    console.error(`wrote ${path.relative(root, json)} and ${path.relative(root, markdown)}`);
}

main();
