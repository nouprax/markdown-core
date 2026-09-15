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
 * WHY CALLGRIND. Instruction and data-reference counts do not depend on how
 * fast the machine was or what else was running on it, so a hosted runner is
 * as good a place to measure as a quiet laptop and a 2% change is a real 2%.
 * Wall clock cannot make that claim, which is why the pipeline this replaced
 * could only ever be informational. Nothing here is a merge gate: it is the
 * measurement an optimization is argued from.
 *
 * They are not independent of the TOOLCHAIN: another compiler or C library
 * emits a different instruction stream for the same source, and it need not
 * change both engines by the same proportion, so a toolchain roll moves the
 * ratio too. The report records the resolved compiler, libc and valgrind
 * versions, and two reports whose toolchains differ are not comparable at all
 * -- not their counts and not their ratios. What holds inside ONE report is
 * that both engines met the same compiler, so the ratio there is a fact about
 * the two parsers rather than about the build.
 *
 * WHAT THE NUMBERS ARE NOT. Ir is work, not time: it does not price a cache
 * miss, a branch miss, or a dependency stall. A change that trades three
 * instructions for one random memory access will look like an improvement
 * here. Dr/Dw are reported alongside for exactly that reason.
 *
 *   node scripts/benchmark-stages.mjs [--out DIR] [--case NAME]... [--scale N]
 *                                     [--quiet]
 */

import { Buffer } from "node:buffer";
import { spawnSync } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { baseName, costRecord, edgesBetween, foldNames, nodesEnteredFrom, parseCallgrind } from "./lib/callgrind.mjs";

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
    const options = { out: path.join(root, "build/benchmark-stages"), cases: [], scale: 2, quiet: false };
    for (let index = 0; index < argv.length; index++) {
        const flag = argv[index];
        const value = argv[index + 1];
        if (flag === "--quiet") {
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

/**
 * The one place the profile's compiler and flags are written down -- and what
 * that name resolved to on this machine.
 *
 * The preset pins a compiler NAME and a flag string, which is what makes both
 * engines comparable to each other. It does not pin a toolchain: a different
 * gcc or libc emits a different instruction stream for the same source, so
 * absolute counts are a property of (commit, toolchain), not of the commit.
 * Recording the resolved versions is what lets a reader tell a code change
 * from an image roll instead of subtracting two numbers that were never
 * comparable.
 */
/**
 * What the compiler will actually generate for, not what the flags say.
 *
 * `-march=native` is resolved by the compiler against the host CPU, so two
 * machines with the same gcc, libc and flag text generate different
 * instruction streams; recording the literal string would call those reports
 * comparable. Asking the compiler closes that, and closes the same gap for
 * flags nobody passed: distribution builds differ in their default -march,
 * so the resolved target belongs in the identity however it was arrived at.
 */
function resolveTarget(compiler, flags) {
    const probe = spawnSync(compiler, [...flags.split(/\s+/u).filter(Boolean), "-Q", "--help=target"], {
        encoding: "utf8"
    });
    if (probe.status !== 0) return "unknown";
    const value = (name) => new RegExp(`^\\s+-m${name}=\\s+(\\S+)`, "mu").exec(probe.stdout ?? "")?.[1];
    const march = value("arch");
    const mtune = value("tune");
    return march || mtune ? `march=${march ?? "?"} mtune=${mtune ?? "?"}` : "unknown";
}

function toolchain(profile) {
    const first = (text) => text.split("\n")[0].trim();
    const optional = (command, args) => {
        const result = spawnSync(command, args, { encoding: "utf8" });
        return result.status === 0 ? first(result.stdout ?? "") : "unknown";
    };
    return {
        compiler: first(run(profile.compiler, ["--version"])),
        libc: optional("ldd", ["--version"]),
        valgrind: optional("valgrind", ["--version"]),
        target: resolveTarget(profile.compiler, `${process.env.CFLAGS ?? ""} ${profile.flags}`)
    };
}

/**
 * The two build trees must not contain one another.
 *
 * The cmark tree lives under the output directory and the profile tree is
 * fixed by the preset. If the output directory sits inside the profile tree,
 * discarding a foreign-stamped profile tree takes the freshly built cmark
 * archive with it and the configure that follows cannot find it. The
 * arrangement is refused rather than half-supported.
 */
function refuseOverlappingTrees(options, profile) {
    const inside = (child, parent) => {
        const relative = path.relative(parent, child);
        return relative === "" || (!relative.startsWith("..") && !path.isAbsolute(relative));
    };
    if (inside(options.out, profile.binaryDir) || inside(profile.binaryDir, options.out)) {
        fail(
            `--out ${path.relative(root, options.out)} overlaps the profile build tree ` +
                `${path.relative(root, profile.binaryDir)}; one would delete the other's build. Choose a path ` +
                "outside it."
        );
    }
}

function profileBuild() {
    const presets = JSON.parse(fs.readFileSync(path.join(root, "CMakePresets.json"), "utf8"));
    const preset = presets.configurePresets.find((entry) => entry.name === "benchmark");
    if (!preset) fail("CMakePresets.json has no benchmark configure preset");
    const compiler = preset.cacheVariables.CMAKE_C_COMPILER;
    const flags = preset.cacheVariables.CMAKE_C_FLAGS_RELEASE;
    if (!compiler || !flags) fail("the benchmark preset must pin CMAKE_C_COMPILER and CMAKE_C_FLAGS_RELEASE");
    return { compiler, flags, binaryDir: path.join(root, "build/benchmark") };
}

/**
 * The pinned cmark the parity oracles already use; no second version exists.
 *
 * The checkout is verified, not assumed. `.tools/` is a local working
 * directory: a checkout can be left on another revision or edited in place,
 * and building whatever bytes are there while the report states the pinned
 * commit would mislabel the comparison as against upstream cmark. The build
 * is refused instead, the way `init-environment.sh --check` refuses it.
 */
function pinnedCmark() {
    const script = fs.readFileSync(path.join(root, "scripts/init-environment.sh"), "utf8");
    const version = /^CMARK_VERSION=(.+)$/mu.exec(script)?.[1];
    const commit = /^CMARK_COMMIT=([0-9a-f]{40})$/mu.exec(script)?.[1];
    if (!version || !commit) fail("scripts/init-environment.sh does not pin cmark");
    const checkout = path.join(root, ".tools/cmark", version);
    const install = "scripts/init-environment.sh --install oracle-cmark";
    if (!fs.existsSync(path.join(checkout, "src/cmark.h"))) {
        fail(`the pinned cmark oracle is not installed; run: ${install}`);
    }
    const head = run("git", ["-C", checkout, "rev-parse", "HEAD"]).trim();
    if (head !== commit) {
        fail(`the cmark oracle checkout is at ${head}, but cmark ${version} is pinned to ${commit}; run: ${install}`);
    }
    const dirty = run("git", ["-C", checkout, "status", "--porcelain", "--untracked-files=no"]).trim();
    if (dirty) {
        fail(`the cmark oracle checkout has local modifications, so it is not cmark ${version}:\n${dirty}`);
    }
    return { version, commit, checkout };
}

function buildCmark(profile, cmark, out, versions) {
    const buildDir = path.join(out, "cmark");
    discardForeignTree(buildDir, profile, versions);
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
    stampTree(buildDir, profile, versions);
    return path.join(buildDir, "src");
}

/**
 * A build tree is reused only when it was produced by this exact toolchain.
 *
 * CMake's cache records the compiler NAME ("gcc") and the flags, not the
 * compiler's version, and an incremental build invalidates objects on source
 * timestamps alone. So upgrading gcc in place leaves every existing object
 * file untouched and the cache looking correct, while the report goes on to
 * state the newly resolved compiler for binaries that predate it -- and one
 * engine's tree can be rebuilt while the other's is not, which is a
 * comparison between two compilers reported as one.
 *
 * Neither the cache nor CMake can answer that, so the tree carries a stamp of
 * the toolchain and flags that produced it, and a tree stamped differently is
 * discarded rather than built on top of.
 */
const STAMP = "markdown-core-profile-stamp.txt";

function stampOf(profile, versions) {
    /* CFLAGS is in here because CMake folds it into CMAKE_C_FLAGS, which is
     * prepended to CMAKE_C_FLAGS_RELEASE on every compile line: an exported
     * `-march=native` changes the instruction stream without touching the
     * preset. process.arch is in here because the same compiler string builds
     * for more than one target. */
    return [
        profile.compiler,
        profile.flags,
        process.env.CFLAGS ?? "",
        process.arch,
        versions.compiler,
        versions.libc,
        versions.target
    ].join("\n");
}

function discardForeignTree(buildDir, profile, versions) {
    if (!fs.existsSync(buildDir)) return;
    const stamp = path.join(buildDir, STAMP);
    const current = fs.existsSync(stamp) ? fs.readFileSync(stamp, "utf8") : "";
    if (current === stampOf(profile, versions)) return;
    fs.rmSync(buildDir, { recursive: true, force: true });
}

/**
 * The flags a tree's compile lines actually carry.
 *
 * `CMAKE_C_FLAGS_RELEASE` is only half of it: CMake initializes
 * `CMAKE_C_FLAGS` from the CFLAGS environment variable and puts it FIRST on
 * every compile line, so a preset's flags and the effective flags are not the
 * same string and only the effective one describes the binary.
 */
function effectiveFlags(buildDir) {
    const cache = fs.readFileSync(path.join(buildDir, "CMakeCache.txt"), "utf8");
    const entry = (name) => new RegExp(`^${name}:[A-Z]+=(.*)$`, "mu").exec(cache)?.[1] ?? "";
    return `${entry("CMAKE_C_FLAGS")} ${entry("CMAKE_C_FLAGS_RELEASE")}`.replace(/\s+/gu, " ").trim();
}

function stampTree(buildDir, profile, versions) {
    fs.writeFileSync(path.join(buildDir, STAMP), stampOf(profile, versions));
}

function buildRunners(profile, cmark, cmarkBuildDir, versions) {
    discardForeignTree(profile.binaryDir, profile, versions);
    run("cmake", [
        "--preset",
        "benchmark",
        `-DMARKDOWN_CORE_CMARK_SOURCE_DIR=${path.join(cmark.checkout, "src")}`,
        `-DMARKDOWN_CORE_CMARK_BUILD_DIR=${cmarkBuildDir}`
    ]);
    run("cmake", ["--build", "--preset", "benchmark", "--parallel"]);
    stampTree(profile.binaryDir, profile, versions);
}

/**
 * Both binaries were produced by the toolchain the report names -- checked,
 * not assumed.
 *
 * Stamping a tree prevents the common way that goes wrong; this catches every
 * other way, because it asks the binaries instead of the build system. Each
 * compiler writes its identity into a `.comment` entry per translation unit
 * and the linker concatenates them, so a binary holding objects from two
 * compilers carries both strings.
 *
 * EVERY entry is compared, not just the ones that look like GCC's. A gcc
 * object linked with a clang object yields one "GCC: (...)" line and one
 * "Ubuntu clang version ..." line, so a check that only counted GCC spellings
 * would see a single producer and wave through a binary built by two
 * compilers -- which is exactly the mixed tree this exists to catch.
 */
function verifyBuildProvenance(profile, versions) {
    /* `gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` -> the part .comment also
     * carries, so the two spellings are compared on what they share. */
    const identity = versions.compiler.replace(/^\S+\s+/u, "").trim();
    for (const [engine, definition] of Object.entries(ENGINES)) {
        const binary = path.join(profile.binaryDir, definition.runner);
        const readelf = spawnSync("readelf", ["-p", ".comment", binary], { encoding: "utf8" });
        if (readelf.status !== 0) {
            fail(`${engine}: cannot read the build provenance of ${definition.runner}; readelf is required`);
        }
        /* `  [     0]  GCC: (Ubuntu 13.3.0-...) 13.3.0` -> the string itself. */
        const producers = [
            ...new Set([...(readelf.stdout ?? "").matchAll(/^\s*\[\s*[0-9a-f]+\]\s{2}(.+?)\s*$/gmu)].map((m) => m[1]))
        ];
        if (producers.length !== 1) {
            fail(
                `${engine}: ${definition.runner} records ${producers.length} compiler identities ` +
                    `(${producers.join(" | ") || "none"}), so the report cannot name one; ` +
                    `delete ${path.relative(root, profile.binaryDir)} and re-run`
            );
        }
        if (!producers[0].includes(identity)) {
            fail(
                `${engine}: ${definition.runner} was built by "${producers[0]}" but the report would name ` +
                    `"${identity}"; delete ${path.relative(root, profile.binaryDir)} and re-run`
            );
        }
    }
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
 * A case built by repeating whole documents.
 *
 * Each sample is normalized to end in exactly one newline and the whole unit
 * gets a blank line after it, so that repeating a unit cannot merge the last
 * block of one copy into the first block of the next -- which would make the
 * document's structure, and so its cost, a non-linear function of the repeat
 * count and quietly ruin the scaling comparison.
 */
function documentsUnit(entry) {
    return (
        entry.samples
            .map((sample) => {
                const file = path.join(BENCHMARKS, "samples", sample);
                if (!fs.existsSync(file)) fail(`corpus.json names a missing sample: ${sample}`);
                return `${fs.readFileSync(file, "utf8").replace(/\n*$/u, "")}\n`;
            })
            .join("") + "\n"
    );
}

/**
 * A case built as ONE structure whose depth or run length is the scale.
 *
 * Repeating whole documents grows the number of independent blocks and nothing
 * else -- the blank line between copies is there precisely to keep them from
 * interacting. That makes the growth table blind in the dimension adversarial
 * inputs actually attack: a container nested D deep or a delimiter run of D
 * openers stays at its original D no matter how many copies are concatenated,
 * so work quadratic in D still reports linear growth in bytes.
 *
 * A chain case has no copies. Its single structure is `unit` repeated until the
 * document reaches its size, so D doubles when the document doubles and cost
 * quadratic in D shows up as a 4x growth ratio.
 */
function chainText(chain, target) {
    const tail = chain.tail ?? "";
    const unit = Buffer.byteLength(chain.unit);
    const length = Math.max(1, Math.floor((target - Buffer.byteLength(tail)) / unit));
    return { text: chain.unit.repeat(length) + tail, length };
}

function buildCorpus(options) {
    const manifest = JSON.parse(fs.readFileSync(path.join(BENCHMARKS, "corpus.json"), "utf8"));
    if (manifest.schemaVersion !== 2) fail(`unsupported corpus schema: ${manifest.schemaVersion}`);
    const directory = path.join(options.out, "corpus");
    fs.mkdirSync(directory, { recursive: true });

    const selected = options.cases.length
        ? manifest.cases.filter((entry) => options.cases.includes(entry.name))
        : manifest.cases;
    if (!selected.length) fail(`no corpus case matched ${options.cases.join(", ")}`);

    const documents = [];
    for (const entry of selected) {
        if (Boolean(entry.samples) === Boolean(entry.chain)) {
            fail(`corpus case ${entry.name} must name exactly one of "samples" or "chain"`);
        }
        const unit = entry.chain ? null : documentsUnit(entry);
        for (let scale = 1; scale <= options.scale; scale++) {
            const target = (entry.targetBytes ?? manifest.targetBytes) * scale;
            const built = entry.chain
                ? chainText(entry.chain, target)
                : (() => {
                      const repeats = Math.max(1, Math.ceil(target / Buffer.byteLength(unit)));
                      return { text: unit.repeat(repeats), length: repeats };
                  })();
            const file = path.join(directory, `${entry.name}.x${scale}.md`);
            fs.writeFileSync(file, built.text);
            documents.push({
                case: entry.name,
                dialect: entry.dialect,
                /* What the growth table is varying: whole documents, or the
                 * depth/run length of one structure. */
                growth: entry.chain ? "structure" : "documents",
                scale,
                units: built.length,
                bytes: Buffer.byteLength(built.text),
                file
            });
        }
    }
    return { targetBytes: manifest.targetBytes, documents };
}

/** The exact bytes measured, so a report's numbers can be traced to a binary. */
function runnerIdentity(profile) {
    const identity = {};
    for (const [engine, definition] of Object.entries(ENGINES)) {
        const binary = path.join(profile.binaryDir, definition.runner);
        identity[engine] = {
            sha256: crypto.createHash("sha256").update(fs.readFileSync(binary)).digest("hex"),
            builtAt: new Date(fs.statSync(binary).mtimeMs).toISOString()
        };
    }
    return identity;
}

function measure(profile, engine, document, out) {
    const definition = ENGINES[engine];
    const dump = path.join(out, "callgrind", `${engine}.${document.case}.x${document.scale}.out`);
    fs.mkdirSync(path.dirname(dump), { recursive: true });
    const stdout = run("valgrind", [
        "--tool=callgrind",
        "--cache-sim=yes",
        "--dump-instr=no",
        /* One level of calling context. `S_parse_source` is entered twice --
         * as the document's source read, and again nested under
         * `S_finish_parse` for mapped block content -- and without this the
         * two share one node, so the source stage's callee breakdown silently
         * includes the AST stage's nested work. The totals were always read
         * from the edge and so were right; the breakdown was not. */
        "--separate-callers=1",
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
    const profileByName = foldNames(parsed, (name) => {
        const context = name.indexOf("'");
        if (context < 0) return name.replace(CLONE_SUFFIX, "");
        return name.slice(0, context).replace(CLONE_SUFFIX, "") + name.slice(context);
    });
    const stages = {};
    for (const stage of STAGES) {
        const boundary = definition.stages[stage];
        const edges = edgesBetween(profileByName, boundary.caller, boundary.callee);
        if (!edges.length) {
            fail(`${engine}: no call edge ${boundary.caller} -> ${boundary.callee} in ${path.basename(dump)}`);
        }
        const cost = [];
        let calls = 0;
        for (const edge of edges) {
            calls += edge.calls;
            edge.cost.forEach((value, index) => (cost[index] = (cost[index] ?? 0) + value));
        }
        /* Only the callee nodes this stage entered, never the same function as
         * the other stage reached it. */
        const scoped = new Set(nodesEnteredFrom(profileByName, boundary.callee, boundary.caller));
        const breakdown = new Map();
        for (const edge of profileByName.edges.values()) {
            if (!scoped.has(edge.caller)) continue;
            const name = baseName(edge.callee);
            const total = breakdown.get(name) ?? [];
            edge.cost.forEach((value, index) => (total[index] = (total[index] ?? 0) + value));
            breakdown.set(name, total);
        }
        const stageCost = costRecord(profileByName, cost);
        const callees = [...breakdown]
            .map(([callee, total]) => ({ callee, cost: costRecord(profileByName, total) }))
            .sort((left, right) => right.cost.Ir - left.cost.Ir);
        /* A part cannot exceed its whole. This held false for a release: the
         * breakdown summed a callee across both contexts it was reached from,
         * so the source stage reported a callee costing more than the stage
         * itself. The contradiction was sitting in the JSON the whole time --
         * it is checked now rather than left for a reader to notice. */
        for (const callee of callees) {
            if (callee.cost.Ir > stageCost.Ir) {
                fail(
                    `${engine}: ${stage} reports ${callee.callee} at ${callee.cost.Ir} Ir inside a stage of ` +
                        `${stageCost.Ir} Ir, so the breakdown is counting another stage's work`
                );
            }
        }
        stages[stage] = {
            entry: `${boundary.caller} -> ${boundary.callee}`,
            calls,
            cost: stageCost,
            breakdown: callees.slice(0, 8)
        };
    }
    /* What excluding setup, discovery and release actually excluded. Reading
     * it keeps the exclusion auditable: a claim that fixed cost is small is a
     * measurement, and a stage split that has quietly stopped covering the
     * parse shows up here as a growing remainder rather than not at all. */
    const whole = edgesBetween(profileByName, "main", ENGINE_ENTRY);
    if (!whole.length) fail(`${engine}: no call edge main -> ${ENGINE_ENTRY} in ${path.basename(dump)}`);
    const parsePathIr = whole.reduce((total, edge) => total + (costRecord(profileByName, edge.cost).Ir ?? 0), 0);

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
    /* Truncated, not rounded, at both ends: a split covering 99.998% of the
     * path must not be reported as covering all of it, and a "covers at least"
     * claim should err low. */
    const percent = (value) => `${(Math.floor(value * 10000) / 100).toFixed(2)}%`;
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
        `Markdown Core against cmark \`${report.cmark.version}\` (\`${report.cmark.commit.slice(0, 12)}\`)` +
            " on the same corpus. Both engines were compiled by the same toolchain with the" +
            " same flags -- verified against what each build recorded, not assumed -- so" +
            " within this report the ratio between them is a fact about the two parsers" +
            " rather than about the build.",
        "",
        "| | |",
        "| --- | --- |",
        `| Compiler | \`${report.toolchain.compiler}\` |`,
        `| C library | \`${report.toolchain.libc}\` |`,
        `| Profiler | \`${report.toolchain.valgrind}\` |`,
        `| Architecture | \`${report.toolchain.architecture}\` |`,
        `| Code generation target | \`${report.toolchain.target}\` |`,
        `| Effective C flags | \`${report.toolchain.flags}\` |`,
        "",
        "Counts do not depend on the machine's speed, its load, or what else was" +
            " running: re-running this commit on this toolchain reproduces every number" +
            " exactly. They DO depend on the toolchain -- another compiler or C library" +
            " emits a different instruction stream for the same source, and it need not" +
            " change both engines by the same proportion, so a toolchain roll moves the" +
            " ratio columns too.",
        "",
        "**Compare this report only against one whose table above is identical.**" +
            " Across differing toolchains nothing here is comparable, ratios included," +
            " and a difference cannot be read as a code change. The table carries the" +
            " EFFECTIVE compile flags rather than the preset's, because CMake folds the" +
            " CFLAGS environment variable into every compile line ahead of them, and it" +
            " carries the architecture and the compiler's RESOLVED code generation" +
            " target, because `-march=native` and a distribution's default -march both" +
            " name themselves identically on machines that generate different code.",
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
                " what was scaled reports a growth ratio equal to the byte ratio; a stage" +
                " quadratic in it reports the square.",
            "",
            "The `scaled` column says which dimension grew. `documents` cases add" +
                " independent copies, so they scale breadth and hold depth fixed." +
                " `structure` cases are a single nested container or delimiter run whose" +
                " depth is the size, which is the dimension a copied document cannot" +
                " reach.",
            "",
            "| Case | Scaled | Byte ratio | Stage | Core growth | cmark growth |",
            "| --- | --- | ---: | --- | ---: | ---: |"
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
                    `| ${entry.case} | ${entry.growth} | ${(entry.bytes / base.bytes).toFixed(2)}x |` +
                        ` ${stage} | ${ratio(core.ir, coreBase.ir)} | ${ratio(cmark.ir, cmarkBase.ir)} |`
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
    refuseOverlappingTrees(options, profile);
    const cmark = pinnedCmark();

    if (spawnSync("valgrind", ["--version"], { encoding: "utf8" }).status !== 0) {
        fail("valgrind is required; install it and re-run");
    }
    const versions = toolchain(profile);

    fs.mkdirSync(options.out, { recursive: true });
    /* Always built, never reused. Nothing in a compiled binary says which
     * source produced it, so a reuse option is a way for the report to state
     * this commit's pins over another revision's instruction counts -- and an
     * up-to-date rebuild of both engines costs about two seconds against a
     * measurement that takes minutes. There is no flag to get it wrong with. */
    const cmarkBuildDir = buildCmark(profile, cmark, options.out, versions);
    buildRunners(profile, cmark, cmarkBuildDir, versions);
    verifyStageSymbols(profile);
    verifyBuildProvenance(profile, versions);
    /* The report's central claim is that both engines met the same compiler
     * with the same flags. The two trees are configured separately, so that is
     * checked against what they each recorded rather than assumed from having
     * passed the same string to both. */
    const coreFlags = effectiveFlags(profile.binaryDir);
    const cmarkFlags = effectiveFlags(path.join(options.out, "cmark"));
    if (coreFlags !== cmarkFlags) {
        fail(`the engines were compiled with different flags:\n  markdown-core: ${coreFlags}\n  cmark: ${cmarkFlags}`);
    }
    versions.flags = coreFlags;
    versions.architecture = process.arch;
    const binaries = runnerIdentity(profile);

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
        schemaVersion: 2,
        toolchain: versions,
        /* The exact bytes measured, so a report's numbers trace to a binary. */
        binaries,
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
