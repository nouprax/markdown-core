/**
 * WHAT THE ATTRIBUTE GRAMMAR COSTS, against a fast C implementation of the
 * same job.
 *
 * `{#lane .stage k="callgrind"}` has no reference. cmark reads it as text and
 * cmark-gfm reads it as text, so the stage benchmark can only bound it -- and
 * the bound it reported, 4.46x on `inline-span` when this driver was written,
 * was mostly the inline parser around the attributes rather than the
 * attributes. The largest single self cost in that case was the Unicode letter
 * test (then a range bisection), at 23.1%.
 *
 * An HTML start tag's attribute list is the same job: a bracketed run split
 * into an identifier, a class run and key/value records, with quoting and
 * character references. lexbor implements that in C and is written for speed.
 * So the comparison here is one grammar against one grammar:
 *
 *   {#lane .stage k="callgrind"}      <x id="lane" class="stage" k="callgrind">
 *
 * Neither implementation reads the other's spelling, which is why this is a
 * separate driver rather than a fourth engine in `run.mjs`: there
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
 *   node scripts/benchmark/measure-attributes.mjs [--out DIR] [--lists N]
 */

import { spawnSync } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { baseName, callEdge, costRecord, foldNames, parseCallgrind } from "./callgrind.mjs";
import { alphabets } from "./corpus.mjs";
import { compiledFlags, discardTree, effectiveFlags, markTree } from "./compile-identity.mjs";
import { buildEnvironment, CACHE, measurementEnvironment, measurementRoot } from "./measurement.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
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
 *
 * QUOTING IS PART OF THE SPECIFICATION, not of how one side renders. Both
 * grammars scan a quoted value and a bare one down different branches -- this
 * parser tracks `quoted` and `unquoted` runs separately, and lexbor has
 * distinct `attribute_value_double_quoted` and `attribute_value_unquoted`
 * tokenizer states -- so a workload that quoted everything would leave both
 * bare paths unmeasured while the comment above claimed otherwise. A value
 * that must be quoted to survive (one holding a space) says so.
 */
const SPECIFICATIONS = [
    { anchor: "lane", classes: ["stage"], records: [["k", "callgrind", "quoted"]] },
    { anchor: "nested", classes: ["one", "two"], records: [] },
    { anchor: null, classes: ["only"], records: [] },
    { anchor: "bare", classes: [], records: [] },
    { anchor: null, classes: [], records: [["k", "v", "bare"]] },
    {
        anchor: "four",
        classes: ["a", "b"],
        records: [
            ["k", "v", "bare"],
            ["data-kind", "note", "quoted"]
        ]
    },
    { anchor: null, classes: ["r"], records: [["k", "a value with spaces", "quoted"]] },
    /* Character references. Both grammars decode them in a value and both are
     * charged for it -- this parser through `houdini_unescape_ent`, lexbor
     * through its own character-reference states -- and the census compares the
     * DECODED text, so it also proves they decoded the same thing. Without a
     * value holding one, that shared branch was described in the comment above
     * and run by neither. */
    {
        anchor: null,
        classes: ["entities"],
        records: [
            ["named", "ampersand &amp; entity", "quoted"],
            ["numeric", "reference &#81; decoded", "quoted"]
        ]
    },
    {
        anchor: "wide",
        classes: ["x", "y", "z", "w"],
        records: [
            ["one", "1", "bare"],
            ["two", "2", "quoted"],
            ["three", "3", "bare"]
        ]
    },
    {
        anchor: null,
        classes: [],
        records: [
            ["k", "callgrind", "quoted"],
            ["j", "valgrind", "bare"]
        ]
    },
    { anchor: "tail", classes: ["last"], records: [["k", "v", "bare"]] }
];

/* A specification with every letter of its names and values respelled in
 * `letters`, the same alphabets the stage corpus spells its words with. Only
 * the letters change: `-`, digits, spaces and character references keep their
 * bytes, so both grammars take the same branches on both spellings and the
 * census still compares like with like. White space stays ASCII: in both
 * grammars ASCII white space separates members and the classes of a class
 * run, and a non-ASCII space separates neither. */
function respelled(specification, letters) {
    const respell = (text) =>
        text
            .split(/(&#?[a-z0-9]+;)/u)
            .map((part, i) => (i % 2 ? part : part.replace(/[a-z]/gu, (letter) => letters[letter.charCodeAt(0) - 97])))
            .join("");
    return {
        anchor: specification.anchor === null ? null : respell(specification.anchor),
        classes: specification.classes.map(respell),
        records: specification.records.map(([name, value, quoting]) => [respell(name), respell(value), quoting])
    };
}

/* A record's value as each spelling writes it. The quoting is the
 * specification's, so both grammars take the same branch on the same record --
 * a bare value on one side and a quoted one on the other would make the pair
 * measure two different scans and still produce a matching census, because the
 * census compares what was recovered rather than how it was written. */
function renderValue([, value, quoting]) {
    if (quoting !== "quoted" && quoting !== "bare") {
        fail(`a record must say whether its value is "quoted" or "bare", not ${quoting}`);
    }
    return quoting === "bare" ? value : `"${value}"`;
}

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
            /* Digits alone are not enough, for the reason the stage benchmark
             * spells out on `--scale`: 309 of them parse to Infinity and 254
             * lists would be written until memory ran out, and
             * 9007199254740993 comes back as ...992, quietly measuring a
             * different workload than the one asked for. The value has to
             * survive the round trip AND be a safe integer. */
            const digits = /^\d+$/u.test(value) ? value.replace(/^0+(?=\d)/u, "") : null;
            const lists = digits === null ? Number.NaN : Number(digits);
            if (!Number.isSafeInteger(lists) || String(lists) !== digits || lists < 1) {
                fail(`--lists takes a positive whole number of attribute lists, not ${value}`);
            }
            options.lists = lists;
            i++;
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
    for (const record of specification.records) parts.push(`${record[0]}=${renderValue(record)}`);
    return `[text]{${parts.join(" ")}}`;
}

function htmlSpelling(specification) {
    const parts = [];
    if (specification.anchor) parts.push(`id="${specification.anchor}"`);
    if (specification.classes.length) parts.push(`class="${specification.classes.join(" ")}"`);
    for (const record of specification.records) parts.push(`${record[0]}=${renderValue(record)}`);
    return `<x ${parts.join(" ")}>`;
}

/* Each alphabet's two spellings of the same lists. */
function writeInputs(options) {
    const directory = path.join(options.out, "input");
    fs.mkdirSync(directory, { recursive: true });
    const inputs = {};
    for (const [alphabet, letters] of Object.entries(alphabets)) {
        const specifications = SPECIFICATIONS.map((specification) => respelled(specification, letters));
        inputs[alphabet] = {};
        for (const [spelling, render] of [
            ["pandoc", pandocSpelling],
            ["html", htmlSpelling]
        ]) {
            const lines = [];
            for (let i = 0; i < options.lists; i++) {
                lines.push(render(specifications[i % specifications.length]));
            }
            const file = path.join(directory, `attributes.${alphabet}.${spelling}.txt`);
            fs.writeFileSync(file, `${lines.join("\n")}\n`);
            inputs[alphabet][spelling] = file;
        }
    }
    return inputs;
}

function run(command, args, options = {}) {
    const result = spawnSync(command, args, { encoding: "utf8", cwd: root, ...options });
    /* Callers that touch a compiler pass `env: buildEnvironment()`; see
     * `lib/measurement.mjs` for why an inherited `CPATH` is not a detail. */
    if (result.error) fail(`${command} could not run: ${result.error.message}`);
    if (result.status !== 0) {
        fail(`${command} ${args.join(" ")} exited ${result.status}\n${result.stderr ?? ""}`);
    }
    return result.stdout ?? "";
}

/* The pinned lexbor checkout, read from the same script that installs it so
 * this cannot measure a different one than the environment check passed on. */
function pinnedLexbor() {
    const script = fs.readFileSync(path.join(root, "scripts/tooling/setup-environment.sh"), "utf8");
    const version = /^LEXBOR_VERSION=(\S+)$/mu.exec(script)?.[1];
    const commit = /^LEXBOR_COMMIT=([0-9a-f]{40})$/mu.exec(script)?.[1];
    if (!version || !commit) fail("scripts/tooling/setup-environment.sh does not pin lexbor");
    const checkout = path.join(root, ".tools/lexbor", version);
    const install = "scripts/tooling/setup-environment.sh --install oracle-lexbor";
    if (!fs.existsSync(path.join(checkout, "source/lexbor/html/tokenizer.h"))) {
        fail(`the pinned lexbor baseline is not installed; run: ${install}`);
    }
    const head = run("git", ["-C", checkout, "rev-parse", "HEAD"]).trim();
    if (head !== commit) {
        fail(`the lexbor checkout is at ${head}, but lexbor ${version} is pinned to ${commit}; run: ${install}`);
    }
    /* Untracked files count as well as edited ones: a stray header in the
     * source tree is neither tracked nor ignored, and the source directory is
     * on the include path -- so the reference built is no longer the pin while
     * HEAD still reads as it. */
    const dirty = run("git", ["-C", checkout, "status", "--porcelain", "--untracked-files=all"]).trim();
    if (dirty) {
        fail(`the lexbor checkout has local modifications, so it is not lexbor ${version}:\n${dirty}`);
    }
    return { version, commit, checkout };
}

/**
 * lexbor is COMPILED HERE, from the pinned source, with the benchmark preset's
 * compiler and options.
 *
 * Linking an archive built by `scripts/tooling/setup-environment.sh` would have been
 * enough to produce a number, and the number would have been wrong in a way
 * nothing here could see: that build takes the host's default compiler and
 * `-DCMAKE_BUILD_TYPE=Release` and none of the profile's options, so the ratio
 * would have depended on how the baseline happened to be installed while the
 * report claimed both grammars met the same compiler. Only the thin runner
 * would have been rebuilt.
 *
 * The same reasoning the stage benchmark applies to cmark, for the same reason.
 * `compile_commands.json` is exported so what the compiler was really handed
 * can be read back and checked, rather than assumed from what CMake was told.
 */
function buildLexbor(profile, lexbor, out, stamp) {
    const buildDir = path.join(out, "lexbor");
    discardTree(buildDir, stamp);
    run(
        "cmake",
        [
            "-S",
            lexbor.checkout,
            "-B",
            buildDir,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DLEXBOR_BUILD_SHARED=OFF",
            /* Static for the reason the cmark oracles are: the runner links the
             * archive this just built rather than whatever a shared build left. */
            "-DLEXBOR_BUILD_STATIC=ON",
            "-DLEXBOR_BUILD_TESTS=OFF",
            "-DLEXBOR_BUILD_EXAMPLES=OFF",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            `-DCMAKE_C_COMPILER=${profile.compiler}`,
            `-DCMAKE_C_FLAGS_RELEASE=${profile.flags}`
        ],
        { env: buildEnvironment() }
    );
    run("cmake", ["--build", buildDir, "--parallel", String(os.cpus().length)], { env: buildEnvironment() });
    if (!fs.existsSync(path.join(buildDir, "liblexbor_static.a"))) {
        fail(`the profile build of lexbor produced no static archive in ${buildDir}`);
    }
    markTree(buildDir, stamp);
    return buildDir;
}

/**
 * Every pinned option, on every object of both baselines.
 *
 * Checked per translation unit rather than against their union: a pinned flag
 * missing from one unit is a hole a union papers over, and the unit that
 * matters here is the tokenizer's, not the one a summary is dominated by.
 * `compiledFlags` is the stage benchmark's reader, shared rather than
 * reimplemented -- it filters by target and drops the per-project include and
 * path noise, which a naive read of `compile_commands.json` does not.
 */
function objectIdentity(buildDir, target, profile, label) {
    const record = compiledFlags(root, buildDir, target, fail);
    const pinned = profile.flags.split(/\s+/u).filter(Boolean);
    for (const line of record.distinct) {
        const missing = pinned.filter((flag) => !line.split(" ").includes(flag));
        if (missing.length) {
            fail(`${label} has an object compiled without the pinned flags ${missing.join(" ")}:\n  ${line}`);
        }
    }
    return { units: record.units, distinct: record.distinct.length, digest: record.digest };
}

/**
 * WHAT PRODUCED THE COUNTS, resolved rather than named.
 *
 * The preset says `gcc`, and `gcc` is a name PATH resolves to whatever the
 * runner image ships this month. Recording the literal name while telling a
 * reader that matching rows make two reports comparable is the wrong way
 * round: the rows have to move when the environment does, or the rule they
 * carry is a claim nothing can check. A rolled compiler, C library or
 * profiler changes the instruction stream without touching a line of either
 * grammar.
 *
 * The compiler is asked how it was configured, with the profile's own flags,
 * and the answer is digested -- `--version` alone is a marketing string and
 * says nothing about the target or the defaults a distribution built in. The C
 * library and the profiler are recorded by their own version banners.
 *
 * NARROWER THAN THE STAGE BENCHMARK'S TABLE, and the report says so. That one
 * additionally digests the resolved code-generation target and what glibc
 * dispatches on from inside valgrind, neither of which is here. Two attribute
 * reports agreeing on these rows is a weaker statement than two stage reports
 * agreeing on theirs.
 */
function resolvedToolchain(profile) {
    const line = (command, args) => {
        const probe = spawnSync(command, args, { encoding: "utf8", env: buildEnvironment() });
        if (probe.status !== 0) fail(`${command} ${args.join(" ")} would not report its version`);
        return `${probe.stdout ?? ""}${probe.stderr ?? ""}`.trim().split("\n")[0];
    };
    /* gcc and clang both write the configuration banner to stderr and exit 0. */
    const probe = spawnSync("/bin/sh", ["-c", `${profile.compiler} ${profile.flags} -v`], {
        encoding: "utf8",
        env: buildEnvironment()
    });
    const banner = probe.status === 0 ? `${probe.stderr ?? ""}${probe.stdout ?? ""}` : "";
    if (!banner.trim()) fail(`the compiler would not report how it was configured: ${profile.compiler}`);
    return {
        compiler: line(profile.compiler, ["--version"]),
        compilerDigest: crypto.createHash("sha256").update(banner).digest("hex"),
        libc: line("ldd", ["--version"]),
        valgrind: line("valgrind", ["--version"])
    };
}

/* The compiler and options both grammars are measured under, read from the
 * same preset the stage benchmark uses so the two reports describe one build. */
function profileBuild() {
    const presets = JSON.parse(fs.readFileSync(path.join(root, "CMakePresets.json"), "utf8"));
    const preset = presets.configurePresets.find((entry) => entry.name === PROFILE_PRESET);
    if (!preset) fail(`CMakePresets.json has no ${PROFILE_PRESET} configure preset`);
    const compiler = preset.cacheVariables.CMAKE_C_COMPILER;
    const flags = preset.cacheVariables.CMAKE_C_FLAGS_RELEASE;
    if (!compiler || !flags) fail(`the ${PROFILE_PRESET} preset must pin CMAKE_C_COMPILER and CMAKE_C_FLAGS_RELEASE`);
    return { compiler, flags };
}

/**
 * What the two trees were built by, in the form a stamp compares.
 *
 * Everything the report names as having produced the counts, plus the
 * environment CMake initializes cache variables from ONCE at first configure:
 * a tree first configured under an exported `-march=native` or `-static` keeps
 * those flags for every later build, and a run without the variable set would
 * otherwise match and reuse binaries the preset never described.
 */
function toolchainStamp(profile, toolchain) {
    return [
        profile.compiler,
        profile.flags,
        process.env.CFLAGS ?? "",
        process.env.LDFLAGS ?? "",
        process.arch,
        toolchain.compiler,
        toolchain.compilerDigest,
        toolchain.libc,
        toolchain.valgrind
    ].join("\n");
}

function build(profile, toolchain, lexbor, out) {
    const stamp = toolchainStamp(profile, toolchain);
    /* One configure for the runners, and the lexbor archive they link is built
     * from source by this driver: the ratio is only about the two grammars if
     * one compiler with one set of options produced everything measured. */
    const lexborBuild = buildLexbor(profile, lexbor, out, stamp);
    /* The runners go in a tree this driver owns rather than the preset's shared
     * one. Both trees are then stamped and discarded by the same rule, and
     * neither driver can wipe the other's out from under it -- the stage
     * benchmark stamps the preset tree with a wider identity than this report
     * carries, so sharing it would have the two fight over every run. */
    const binaryDir = path.join(out, "runners");
    discardTree(binaryDir, stamp);
    run(
        "cmake",
        [
            "-B",
            binaryDir,
            "--preset",
            PROFILE_PRESET,
            /* The preset does not set it and the stage benchmark passes it too:
             * without it the tree compiles fine and exports no database, so the
             * object identity below has nothing to read. It went unnoticed locally
             * because the stage benchmark had already configured this same tree
             * WITH it, and a clean runner has no such leftover. */
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            `-DMARKDOWN_CORE_LEXBOR_SOURCE_DIR=${lexbor.checkout}`,
            `-DMARKDOWN_CORE_LEXBOR_BUILD_DIR=${lexborBuild}`
        ],
        { env: buildEnvironment() }
    );
    run(
        "cmake",
        [
            "--build",
            binaryDir,
            "--target",
            "markdown_core_attribute_runner",
            "--target",
            "lexbor_attribute_runner",
            "--parallel",
            String(os.cpus().length)
        ],
        { env: buildEnvironment() }
    );
    for (const [name, definition] of Object.entries(BASELINES)) {
        const binary = path.join(binaryDir, definition.runner);
        if (!fs.existsSync(binary)) fail(`the profile build produced no ${name} attribute runner at ${binary}`);
    }
    const objects = {
        /* The archive each runner links, named because one source can be
         * compiled several ways in one tree. */
        "markdown-core": objectIdentity(binaryDir, "libmarkdown-core-public-static", profile, "markdown-core"),
        lexbor: objectIdentity(lexborBuild, "lexbor_static", profile, "lexbor"),
        /* And the runner targets, because THE MEASURED FUNCTION IS IN THEM.
         * `bench_parse_attributes` is compiled from `markdown_core_attributes.c`
         * and `lexbor_attributes.c` into the executables, not into either
         * archive, and the edge this report reads is the edge into it -- so
         * their target-specific options, `LEXBOR_STATIC` among them, are inside
         * every count. Checking only the archives let the report say every
         * measured object was verified while the benchmark's own code was the
         * one thing left out.
         *
         * The stage benchmark is not in this position and is left alone: its
         * measured edges are internal to the parse transaction, so its runner
         * objects sit outside every stage it counts. */
        "markdown-core runner": objectIdentity(
            binaryDir,
            "markdown_core_attribute_runner",
            profile,
            "the markdown-core runner"
        ),
        "lexbor runner": objectIdentity(binaryDir, "lexbor_attribute_runner", profile, "the lexbor runner")
    };
    /* Read out of each tree's own cache rather than taken from what this
     * driver passed: CMake initializes `CMAKE_C_FLAGS` from CFLAGS and
     * `CMAKE_EXE_LINKER_FLAGS` from LDFLAGS at first configure, so an exported
     * `-march=native` or `-static` outlives the shell it was set in and
     * describes a binary the preset alone does not. */
    const effective = {
        "markdown-core": effectiveFlags(binaryDir),
        lexbor: effectiveFlags(lexborBuild)
    };
    if (effective["markdown-core"].compile !== effective.lexbor.compile) {
        fail(
            "the two trees were configured with different compile flags, so the ratio would be about the builds:\n" +
                `  markdown-core: ${effective["markdown-core"].compile}\n  lexbor: ${effective.lexbor.compile}`
        );
    }
    markTree(binaryDir, stamp);
    return { binaryDir, objects, effective };
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
function requireSameAttributes(built, inputs) {
    const census = {};
    for (const [name, definition] of Object.entries(BASELINES)) {
        census[name] = run(path.join(built.binaryDir, definition.runner), [
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

function measure(built, name, inputs, out) {
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
            path.join(built.binaryDir, definition.runner),
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
        "The lists are measured twice, in two alphabets: once with the names and values" +
            " spelled in ASCII letters and once with every letter a UTF-8 letter of two, three" +
            " or four bytes -- the stage corpus's alphabets. Syntax, digits, spaces and character" +
            " references are the same bytes in both.",
        ""
    ];
    for (const [alphabet, measurement] of Object.entries(report.alphabets)) {
        lines.push(
            `### ${alphabet === "ascii" ? "ASCII" : "UTF-8"} names and values`,
            "",
            `| Baseline | Spelling | Lists | Values | Ir | Ir/list | Dr | Dw |`,
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"
        );
        for (const [name, result] of Object.entries(measurement.baselines)) {
            lines.push(
                `| \`${name}\` | ${BASELINES[name].spelling} | ${result.lists.toLocaleString("en-US")} |` +
                    ` ${result.values.toLocaleString("en-US")} | ${result.ir.toLocaleString("en-US")} |` +
                    ` ${(result.ir / result.lists).toFixed(1)} | ${result.dataReads.toLocaleString("en-US")} |` +
                    ` ${result.dataWrites.toLocaleString("en-US")} |`
            );
        }
        const ours = measurement.baselines["markdown-core"];
        const theirs = measurement.baselines.lexbor;
        lines.push(
            "",
            `**${(ours.ir / theirs.ir).toFixed(2)}x on instructions, ` +
                `${((ours.dataReads + ours.dataWrites) / (theirs.dataReads + theirs.dataWrites)).toFixed(2)}x on data ` +
                "references**, over the same attributes recovered the same way.",
            "",
            "Self cost, restricted to what the measured entry reaches -- the instructions" +
                " each baseline spent IN a function rather than through it.",
            "",
            "| Baseline | Dominant self cost |",
            "| --- | --- |",
            ...Object.entries(measurement.baselines).map(
                ([name, result]) =>
                    `| \`${name}\` | ${(result.hotPaths ?? [])
                        .slice(0, 5)
                        .map((entry) => `\`${entry.name}\` ${(entry.share * 100).toFixed(1)}%`)
                        .join(", ")} |`
            ),
            ""
        );
    }
    lines.push(
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
        `| Compiler | \`${report.toolchain.compiler}\` (\`${report.toolchain.compilerDigest.slice(0, 16)}\`) |`,
        `| C library | \`${report.toolchain.libc}\` |`,
        `| Profiler | \`${report.toolchain.valgrind}\` |`,
        `| Effective compile flags | \`${report.toolchain.flags}\` |`,
        `| Effective link flags | \`${report.toolchain.linkFlags || "(none)"}\` |`,
        `| Measured objects | ${Object.entries(report.toolchain.objects)
            .map(
                ([name, record]) =>
                    `${name} ${record.units} (${record.distinct} compile ` +
                    `${record.distinct === 1 ? "line" : "lines"}, \`${record.digest.slice(0, 12)}\`)`
            )
            .join(", ")} |`,
        `| lexbor | ${report.lexbor.version} (\`${report.lexbor.commit.slice(0, 12)}\`) |`,
        `| Measured edge | \`${MEASURED.caller} -> ${MEASURED.callee}\` |`,
        "",
        "Absolute counts depend on the toolchain, so two reports are comparable only when" +
            " those rows match; `packages/markdown-core/benchmarks/README.md` sets out why" +
            " that is stricter than it sounds, and this table is NARROWER than the one" +
            " there: it does not digest the resolved code-generation target or what glibc" +
            " dispatches on from inside valgrind, so two attribute reports agreeing on" +
            " these rows is a weaker statement than two stage reports agreeing on theirs." +
            " **lexbor is compiled here, from the pinned" +
            " source, by that compiler with those options** -- not linked from an archive" +
            " someone's environment setup produced -- and every object of both baselines is" +
            " checked to have received the pinned flags, so the ratio is about the two" +
            " grammars rather than about two builds.",
        "",
        `Raw callgrind dumps are in \`${path.relative(root, report.artifacts)}\`.`
    );
    return lines.join("\n");
}

function main() {
    const options = parseArguments(process.argv.slice(2));
    if (spawnSync("valgrind", ["--version"], { encoding: "utf8" }).status !== 0) {
        fail("valgrind is not available; run: scripts/tooling/setup-environment.sh --install callgrind");
    }
    const lexbor = pinnedLexbor();
    fs.mkdirSync(options.out, { recursive: true });
    const inputs = writeInputs(options);
    const profile = profileBuild();
    const toolchain = resolvedToolchain(profile);
    const built = build(profile, toolchain, lexbor, options.out);
    /* Each alphabet is its own comparison: its own census, its own counts. */
    const measured = {};
    for (const alphabet of Object.keys(alphabets)) {
        const recovered = requireSameAttributes(built, inputs[alphabet]);
        const baselines = {};
        for (const name of Object.keys(BASELINES)) {
            baselines[name] = measure(built, name, inputs[alphabet], path.join(options.out, alphabet));
            if (baselines[name].lists !== recovered) {
                fail(`${name} measured ${baselines[name].lists} ${alphabet} lists but its census held ${recovered}`);
            }
        }
        measured[alphabet] = { baselines };
    }
    /* Schema 2: the same lists measured once per alphabet. */
    const report = {
        schemaVersion: 2,
        toolchain: {
            ...toolchain,
            preset: profile.compiler,
            flags: built.effective["markdown-core"].compile,
            linkFlags: built.effective["markdown-core"].link,
            objects: built.objects
        },
        lexbor: { version: lexbor.version, commit: lexbor.commit },
        artifacts: options.out,
        specifications: SPECIFICATIONS.length,
        alphabets: measured
    };
    fs.writeFileSync(path.join(options.out, "attributes.json"), `${JSON.stringify(report, null, 4)}\n`);
    const rendered = markdownReport(report);
    fs.writeFileSync(path.join(options.out, "attributes.md"), `${rendered}\n`);
    process.stdout.write(`${rendered}\n`);
}

main();
