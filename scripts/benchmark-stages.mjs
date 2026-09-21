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
 *   source_to_buffer   markdown-core  markdown_core_parse_document_with_setup
 *                                       -> S_parse_source
 *                      cmark          cmark_parser_feed
 *   buffer_to_ast      markdown-core  markdown_core_parse_document_with_setup
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
 * could only ever be informational. With --baseline-ref, the source stage
 * also has a per-document regression gate measured within this one run.
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
 *                                     [--quiet] [--baseline-ref COMMIT]
 */

import { Buffer } from "node:buffer";
import { spawnSync } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { baseName, costRecord, edgesBetween, foldNames, nodesEnteredFrom, parseCallgrind } from "./lib/callgrind.mjs";
import { compiledFlags as readCompiledFlags, discardTree, effectiveFlags, markTree } from "./lib/compile-identity.mjs";
import { sourceBudget, SOURCE_IR_LIMIT } from "./lib/source-budget.mjs";
import { caseClosure, splitWithCases } from "./lib/corpus-splits.mjs";
import { boundarySource, pairReview } from "./lib/pair-review.mjs";
import { pairingIdentity, pairRatios, proofWorkload, provenPair, validatePairs } from "./lib/corpus-pairs.mjs";
import {
    BUILD_FLAG_VARIABLES,
    buildEnvironment,
    CACHE,
    measurementEnvironment,
    measurementRoot
} from "./lib/measurement.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const BENCHMARKS = path.join(root, "packages/markdown-core/benchmarks");

/* The one configure preset this script builds through. Its compiler, its
 * flags and its build tree are all read back from CMakePresets.json under this
 * name, so the preset stays the single place any of them is written down. */
const PROFILE_PRESET = "benchmark";

/* A cache geometry pinned in the report rather than taken from the host, so
 * that two machines produce the same file and a diff means a code change. */
/* GCC clones a function when it specializes it; the clone carries the work but
 * not the plain name the stage boundary is written as. */
const CLONE_SUFFIX = /(\.(constprop|isra|part|cold|lto_priv|localalias)\.?\d*)+$/u;

const ENGINES = {
    "markdown-core": {
        runner: "packages/markdown-core/benchmarks/markdown_core_stage_runner",
        stages: {
            source_to_buffer: { caller: "markdown_core_parse_document_with_setup", callee: "S_parse_source" },
            buffer_to_ast: { caller: "markdown_core_parse_document_with_setup", callee: "S_finish_parse" }
        }
    },
    cmark: {
        runner: "packages/markdown-core/benchmarks/cmark_stage_runner",
        stages: {
            source_to_buffer: { caller: "bench_parse_document", callee: "cmark_parser_feed" },
            buffer_to_ast: { caller: "bench_parse_document", callee: "cmark_parser_finish" }
        }
    },
    /* Same stage split, same API, same codebase -- and it implements tables,
     * strikethrough, bare autolinks, task lists and footnotes, so for those
     * constructs a ratio against it compares two parsers doing one job. */
    "cmark-gfm": {
        runner: "packages/markdown-core/benchmarks/cmark_gfm_stage_runner",
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

/* The remainder of a split, ALONE: the attribute runner `benchmark-attributes.mjs`
 * measures against lexbor, built in the same tree by the same flags as the
 * parser the split measures the remainder inside, read on the edge that
 * driver reads. The edge covers the scan, the decode and the release of each
 * list, so it is compared with the split's WHOLE-PATH marginal, which covers
 * release too, and not with the stages. */
const ATTRIBUTE_RUNNER = {
    runner: "packages/markdown-core/benchmarks/markdown_core_attribute_runner",
    target: "markdown_core_attribute_runner",
    caller: "main",
    callee: "bench_parse_attributes"
};
/* Every binary a number in the report is read from, by the name the identity
 * table gives it. */
const MEASURED_BINARIES = {
    ...Object.fromEntries(Object.entries(ENGINES).map(([engine, definition]) => [engine, definition.runner])),
    "attribute runner": ATTRIBUTE_RUNNER.runner
};
/* How many copies of the remainder the alone measurement decodes: enough that
 * the runner's own setup and the receipt are noise against the lists. */
const ALONE_LISTS = 4096;

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
    const options = {
        out: path.join(root, "build/benchmark-stages"),
        cases: [],
        scale: 2,
        quiet: false,
        corpusOnly: false
    };
    for (let index = 0; index < argv.length; index++) {
        const flag = argv[index];
        const value = argv[index + 1];
        if (flag === "--quiet") {
            options.quiet = true;
        } else if (flag === "--corpus-only") {
            options.corpusOnly = true;
        } else if (!value) {
            fail(`${flag} needs a value`);
        } else if (flag === "--out") {
            options.out = path.resolve(value);
            index++;
        } else if (flag === "--baseline-ref") {
            if (!/^[0-9a-f]{40}$/u.test(value)) fail("--baseline-ref must be a full commit SHA");
            options.baselineRef = value;
            index++;
        } else if (flag === "--case") {
            options.cases.push(value);
            index++;
        } else if (flag === "--scale") {
            /* Digits and nothing else, naming a number JavaScript can hold
             * exactly.
             *
             * Number.parseInt reads the leading digits of "2x", "1.5" and
             * "1e3" and discards the rest, so those would quietly measure a
             * different experiment than the one asked for. Digits alone are
             * not enough either: 309 of them parse to Infinity and the range
             * check below is happy with it, and 9007199254740993 comes back as
             * ...992. Both must hold -- the value has to survive the round
             * trip AND be a safe integer, since 10^20 survives the round trip
             * and is neither exact nor a number this can count up to. */
            const digits = /^\d+$/u.test(value) ? value.replace(/^0+(?=\d)/u, "") : null;
            const scale = digits === null ? Number.NaN : Number(digits);
            if (!Number.isSafeInteger(scale) || String(scale) !== digits) {
                fail(`--scale must be a positive integer, not ${value}`);
            }
            options.scale = scale;
            index++;
        } else {
            fail(`unknown argument: ${flag}`);
        }
    }
    if (options.corpusOnly && options.baselineRef) fail("--baseline-ref requires measurement");
    if (options.scale < 1) fail("--scale must be a positive integer");
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
 *
 * The model names are not the resolved target, only its label. `native` on
 * this host resolves to `sapphirerapids` with 81 feature switches enabled,
 * and `-march=native -mno-avx512f` resolves to the SAME march and mtune with
 * 68 -- so a VM that masks a feature, or a host whose cache sizes differ,
 * produces a different instruction stream under an identical pair. The cache
 * sizes are not in `--help=target` at all; `native` writes them into the
 * tuning params, where they steer unrolling and prefetching.
 *
 * So the identity carries a digest of the compiler's COMPLETE answer -- every
 * target switch and every param -- and the model names serve as the label a
 * reader can actually read. The digest moves whenever code generation could,
 * which is the direction that has to be safe: calling two differing streams
 * comparable is the failure, and a spurious rebuild is not.
 */

/**
 * Asked twice and required to agree.
 *
 * A digest that moves between two identical probes makes every report
 * incomparable with every other and every build tree foreign to the next run
 * -- and does both silently, because the counts stay perfectly plausible. One
 * varying line is enough to do it, so the property is checked rather than
 * assumed.
 */
function agreed(what, produce) {
    const first = produce();
    if (first !== produce()) {
        fail(`${what} answered differently to two identical probes, so no report could be compared to another`);
    }
    return first;
}

function resolveTarget(compiler, flags) {
    /* Through a shell, because a shell is what splits these flags when the
     * build runs them: CMake stores the string verbatim and the generated
     * compile line is interpreted, so `-isystem "/opt/a b/include"` is one
     * argument there. Splitting on whitespace here instead made the probe see
     * `"/opt/a` and `b/include"`, and gcc reject them, for a flag set that
     * compiles perfectly. The string reaches a shell either way, so asking
     * this question adds no exposure the build does not already have. */
    const ask = (option) => {
        const probe = spawnSync("/bin/sh", ["-c", `${compiler} ${flags} -Q ${option}`], {
            encoding: "utf8",
            env: buildEnvironment()
        });
        return probe.status === 0 ? (probe.stdout ?? "") : null;
    };
    /* Whitespace in this output is tabs and padding for the terminal, not
     * content: normalizing it keeps the digest a fact about the compiler's
     * configuration rather than about its column alignment.
     *
     * `-o <file>` is dropped with it. Under --help=common the compiler reports
     * the temporary assembler file of THIS invocation there, a fresh name
     * every time -- what it was handed, not how it is configured. */
    const settings = (text) =>
        text
            .split("\n")
            .map((line) => line.trim().replace(/\s+/gu, " "))
            .filter((line) => line && !line.startsWith("-o <file>"))
            .join("\n");
    const answer = () => {
        const target = ask("--help=target");
        const params = ask("--help=params");
        /* The common options too, because that is where a configure-time
         * default shows its effect: a GCC built --enable-default-pie reports
         * `-fPIE [enabled]` here and names it under --help=target only as a
         * side effect on an unrelated row. */
        const common = ask("--help=common");
        /* Refused rather than recorded as unknown. Two hosts that both failed
         * to answer would record the same "unknown" and compare as equal,
         * which is the one outcome the identity exists to prevent -- and it
         * would be reached silently, by any probe failure at all rather than
         * just this one. An unanswerable target is a broken measurement, not a
         * vague one. */
        if (target === null || params === null || common === null) {
            fail(`the compiler would not report its resolved target: ${compiler} ${flags}`);
        }
        return `${settings(target)}\n${settings(params)}\n${settings(common)}`;
    };
    const text = agreed(`the compiler's resolved target (${compiler})`, answer);
    const target = ask("--help=target");
    const value = (name) => new RegExp(`^\\s+-m${name}=\\s+(\\S+)`, "mu").exec(target ?? "")?.[1];
    const march = value("arch");
    const mtune = value("tune");
    if (!march && !mtune) fail(`the compiler reported no -march or -mtune: ${compiler} ${flags}`);
    return {
        summary: `march=${march ?? "?"} mtune=${mtune ?? "?"}`,
        digest: crypto.createHash("sha256").update(text).digest("hex")
    };
}

/**
 * What the compiler was built to be, beside what it was asked to do.
 *
 * The version line names a release, not a build of it. Two GCCs that print the
 * same line can carry different configure-time defaults and different built-in
 * specs, and those reach the object file without appearing on any compile line
 * this report records. The banner carries the `Configured with:` line, the
 * specs in use and the thread model, so the digest covers the compiler rather
 * than its release number.
 *
 * Refused rather than recorded as unknown, for the reason every other row is:
 * two hosts that could not answer would record the same nothing and compare as
 * equal.
 */
/* The programs the driver says it will exec. A wrapper one layer down sits
 * here rather than at the name PATH resolved. */
const COMPILER_PROGRAMS = ["cc1", "collect2", "as", "ld"];

/**
 * The compiler that ran, by its bytes.
 *
 * `gcc` is a name, and what PATH resolves it to can be a wrapper that answers
 * every probe here exactly as the real driver would and adds an option only
 * when asked to compile. The banner, the resolved target, the recorded compile
 * lines and the objects' own provenance would all agree while the objects
 * differed -- and `compiledFlags` drops the compiler token itself, so the
 * wrapper appears nowhere in the table at all.
 *
 * The driver and the programs it reports for the stages below it are digested
 * by content, keyed by the name asked for so that the same toolchain installed
 * at two prefixes compares equal. These are the programs a non-LTO compile and
 * link use; LTO brings its own (lto1, lto-wrapper, the plugin) and is not
 * digested because it cannot reach a report -- it inlines the stage boundaries
 * and verifyStageSymbols refuses the run. That coupling is worth knowing if
 * that check is ever loosened.
 *
 * Asked with every flag the build hands the driver, compile and link both.
 * `-B` selects these programs and rides in either variable, so a probe that
 * saw only the compile flags would keep hashing the default `ld` while
 * `LDFLAGS=-B/tmp/tools` linked the runners with another one.
 *
 * A bare name means the driver will search PATH, so PATH is asked. A relative
 * path -- what `-B./tools` produces -- is refused instead: it resolves against
 * whatever directory the compiler runs in, and the two engines are configured
 * in different build trees, so there is no single program for the identity to
 * name. Recording the string would be recording a constant that two different
 * assemblers both satisfy.
 */
function compilerBinaries(compiler, flags) {
    const ask = (command) => {
        const probe = spawnSync("/bin/sh", ["-c", command], { encoding: "utf8", env: buildEnvironment() });
        return probe.status === 0 ? (probe.stdout ?? "").trim() : "";
    };
    const driver = ask(`command -v ${compiler}`);
    if (!driver) fail(`${compiler} could not be resolved to a program, so this report cannot say what compiled it`);
    const named = [["", driver]];
    for (const program of COMPILER_PROGRAMS) {
        const reported = ask(`${compiler} ${flags} -print-prog-name=${program}`);
        /* A bare name is a PATH lookup the driver has deferred; anything with a
         * separator is a path it has already decided on. */
        named.push([program, reported.includes(path.sep) ? reported : ask(`command -v ${reported}`) || reported]);
    }
    const digest = crypto.createHash("sha256");
    for (const [program, file] of named) {
        if (!file || !path.isAbsolute(file)) {
            fail(
                `${compiler} would use ${file || "nothing"} for ${program || "itself"}, which is not one program ` +
                    "this report can identify: a relative path resolves against the directory the compiler runs " +
                    "in, and the two engines are configured in different build trees"
            );
        }
        if (!fs.existsSync(file)) {
            fail(`${compiler} would use ${file} for ${program || "itself"}, and there is no such file`);
        }
        digest.update(`${program}\u0000${crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex")}\n`);
    }
    return digest.digest("hex");
}

/**
 * The profiler that will run, by its bytes.
 *
 * `valgrind` is a name PATH resolves, and the measurement environment keeps
 * that PATH deliberately. A wrapper there can pass `--version` through
 * unchanged and add an option only for `--tool=callgrind` -- and the options
 * that decide what gets counted are precisely the ones this driver never
 * passes, so they conflict with nothing and simply win. `--collect-atstart=no`
 * alone takes the summary to zero.
 *
 * The launcher is digested, and so is the tool binary it says it will launch
 * for each tool this driver uses: `-d` reports the choice, which is the same
 * question `-print-prog-name` asks the compiler. Asked in the measurement's own
 * environment and directory, because that is where the answer has to hold.
 */
function profilerBinaries(isolated) {
    const environment = measurementEnvironment(isolated);
    const resolved = spawnSync("/bin/sh", ["-c", "command -v valgrind"], {
        encoding: "utf8",
        env: environment,
        cwd: isolated
    });
    const launcher = resolved.status === 0 ? (resolved.stdout ?? "").trim() : "";
    if (!launcher) fail("valgrind could not be resolved to a program, so this report cannot say what measured it");
    const files = [launcher];
    for (const tool of ["callgrind", "none"]) {
        const probe = spawnSync("valgrind", ["-d", `--tool=${tool}`, "/bin/true"], {
            encoding: "utf8",
            env: environment,
            cwd: isolated
        });
        const launched = /launcher launching (\S+)/u.exec(probe.stderr ?? "")?.[1];
        if (!launched) fail(`valgrind would not say which ${tool} tool it launches, so this report cannot identify it`);
        files.push(launched);
    }
    const digest = crypto.createHash("sha256");
    for (const file of files) {
        if (!path.isAbsolute(file) || !fs.existsSync(file)) {
            fail(`valgrind would use ${file}, which is not one program this report can identify`);
        }
        digest.update(
            `${path.basename(file)}\u0000${crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex")}\n`
        );
    }
    return digest.digest("hex");
}

function compilerConfiguration(compiler, flags) {
    const banner = agreed(`the compiler's configuration (${compiler})`, () => compilerBanner(compiler, flags));
    return crypto.createHash("sha256").update(banner).digest("hex");
}

function compilerBanner(compiler, flags) {
    const probe = spawnSync("/bin/sh", ["-c", `${compiler} ${flags} -v`], {
        encoding: "utf8",
        env: buildEnvironment()
    });
    /* gcc and clang both write the banner to stderr and exit 0. */
    const banner = probe.status === 0 ? `${probe.stderr ?? ""}${probe.stdout ?? ""}` : "";
    if (!banner.trim()) {
        fail(`the compiler would not report how it was configured: ${compiler} ${flags}`);
    }
    return banner
        .split("\n")
        .map((line) => line.trim().replace(/\s+/gu, " "))
        .filter(Boolean)
        .join("\n");
}

function toolchain(profile) {
    const first = (text) => text.split("\n")[0].trim();
    /* Every row here is refused rather than recorded as unknown, for the same
     * reason the resolved target is: two hosts that could not answer would
     * record the same "unknown" and compare as equal. The libc is asked for
     * two ways because the first is not present everywhere, and the second
     * answers wherever the first is missing on a glibc host. */
    const required = (what, attempts) => {
        for (const [command, args] of attempts) {
            const result = spawnSync(command, args, { encoding: "utf8" });
            if (result.status === 0) return first(result.stdout ?? "");
        }
        fail(`the ${what} version could not be determined, so this report could not say what it measured`);
    };
    const flags = `${process.env.CFLAGS ?? ""} ${profile.flags}`;
    const resolved = resolveTarget(profile.compiler, flags);
    return {
        compiler: first(run(profile.compiler, ["--version"])),
        compilerDigest: compilerConfiguration(profile.compiler, flags),
        compilerBinaries: agreed(`the compiler's programs (${profile.compiler})`, () =>
            compilerBinaries(profile.compiler, `${flags} ${process.env.LDFLAGS ?? ""}`)
        ),
        libc: required("C library", [
            ["ldd", ["--version"]],
            ["getconf", ["GNU_LIBC_VERSION"]]
        ]),
        valgrind: required("valgrind", [["valgrind", ["--version"]]]),
        valgrindDigest: agreed("the profiler's programs", () =>
            profilerBinaries(measurementRoot(path.dirname(profile.binaryDir)))
        ),
        target: resolved.summary,
        targetDigest: resolved.digest
    };
}

/**
 * Where a path actually lands, as far as it exists today.
 *
 * `path.resolve` and `path.relative` compare spelling, and a symlink is a
 * path that lands somewhere its spelling does not say. Neither directory has
 * to exist when this is asked, so the deepest ancestor that does exist is
 * canonicalised and the rest appended: what does not exist cannot be a link.
 */
function realPath(target) {
    const pending = [];
    let existing = target;
    for (;;) {
        try {
            return path.join(fs.realpathSync(existing), ...pending);
        } catch {
            const parent = path.dirname(existing);
            if (parent === existing) {
                return target;
            }
            pending.unshift(path.basename(existing));
            existing = parent;
        }
    }
}

/**
 * The two build trees must not contain one another.
 *
 * The cmark tree lives under the output directory and the profile tree is
 * fixed by the preset. If the output directory sits inside the profile tree,
 * discarding a foreign-stamped profile tree takes the freshly built cmark
 * archive with it and the configure that follows cannot find it. The
 * arrangement is refused rather than half-supported.
 *
 * Both sides are canonicalised first. Comparing what the caller typed would
 * accept `--out` through a symlink into the profile tree, and the failure that
 * follows names a missing cmark library rather than the arrangement that
 * removed it.
 */
function refuseOverlappingTrees(options, profile) {
    const out = realPath(options.out);
    const binaryDir = realPath(profile.binaryDir);
    const inside = (child, parent) => {
        const relative = path.relative(parent, child);
        return relative === "" || (!relative.startsWith("..") && !path.isAbsolute(relative));
    };
    if (inside(out, binaryDir) || inside(binaryDir, out)) {
        const shown = (typed, real) =>
            typed === real
                ? path.relative(root, typed)
                : `${path.relative(root, typed)} (${path.relative(root, real)})`;
        fail(
            `--out ${shown(options.out, out)} overlaps the profile build tree ` +
                `${shown(profile.binaryDir, binaryDir)}; one would delete the other's build. Choose a path ` +
                "outside it."
        );
    }
}

/**
 * The build tree the preset names, with CMake's macros expanded.
 *
 * `cmake --preset` puts the tree wherever `binaryDir` says, while the cleanup,
 * the overlap check, the stamp, the symbol and provenance checks and every
 * runner path here address it directly. A second copy of the path would let
 * the preset move the tree out from under all of them at once and leave this
 * script reading a stale one, so the preset is the only place it is written.
 *
 * Only the macros whose value is knowable from here are expanded. A path is
 * used by eleven call sites, so one that is silently wrong is worse than none:
 * anything left unexpanded -- `$env{}`, a macro CMake adds later -- is refused
 * rather than passed through.
 */
function presetBinaryDir(preset) {
    if (!preset.binaryDir) fail(`the ${preset.name} preset must set binaryDir`);
    const macros = {
        sourceDir: root,
        sourceParentDir: path.dirname(root),
        sourceDirName: path.basename(root),
        presetName: preset.name
    };
    const expanded = preset.binaryDir.replace(/\$\{(\w+)\}/gu, (macro, name) =>
        Object.hasOwn(macros, name) ? macros[name] : macro
    );
    if (expanded.includes("${") || expanded.includes("$env{") || expanded.includes("$penv{")) {
        fail(`the ${preset.name} preset's binaryDir uses a macro this script cannot expand: ${preset.binaryDir}`);
    }
    return path.resolve(root, expanded);
}

function profileBuild() {
    const presets = JSON.parse(fs.readFileSync(path.join(root, "CMakePresets.json"), "utf8"));
    const preset = presets.configurePresets.find((entry) => entry.name === PROFILE_PRESET);
    if (!preset) fail(`CMakePresets.json has no ${PROFILE_PRESET} configure preset`);
    const compiler = preset.cacheVariables.CMAKE_C_COMPILER;
    const flags = preset.cacheVariables.CMAKE_C_FLAGS_RELEASE;
    if (!compiler || !flags) {
        fail(`the ${PROFILE_PRESET} preset must pin CMAKE_C_COMPILER and CMAKE_C_FLAGS_RELEASE`);
    }
    return { compiler, flags, binaryDir: presetBinaryDir(preset) };
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
    /* Untracked files count. A stray `src/config.h` is not tracked and is not
     * ignored by cmark's .gitignore, and the source directory is on the
     * include path ahead of the build directory -- so it shadows the generated
     * header and the build is no longer the pinned commit, while a check that
     * asked only about tracked files reports it as one. */
    const dirty = run("git", ["-C", checkout, "status", "--porcelain", "--untracked-files=all"]).trim();
    if (dirty) {
        fail(`the cmark oracle checkout has local modifications, so it is not cmark ${version}:\n${dirty}`);
    }
    return { version, commit, checkout };
}

/* The pinned GFM oracle, located exactly as cmark is: the pin lives in
 * init-environment.sh and this reads it rather than keeping a second copy. */
function pinnedCmarkGfm() {
    const script = fs.readFileSync(path.join(root, "scripts/init-environment.sh"), "utf8");
    const version = /^CMARK_GFM_VERSION=(.+)$/mu.exec(script)?.[1];
    const commit = /^CMARK_GFM_COMMIT=([0-9a-f]{40})$/mu.exec(script)?.[1];
    if (!version || !commit) fail("scripts/init-environment.sh does not pin cmark-gfm");
    const checkout = path.join(root, ".tools/cmark-gfm", version);
    const install = "scripts/init-environment.sh --install oracle-cmark-gfm";
    if (!fs.existsSync(path.join(checkout, "src/cmark-gfm.h"))) {
        fail(`the pinned cmark-gfm oracle is not installed; run: ${install}`);
    }
    const head = run("git", ["-C", checkout, "rev-parse", "HEAD"]).trim();
    if (head !== commit) {
        fail(
            `the cmark-gfm oracle checkout is at ${head}, but cmark-gfm ${version} is pinned to ${commit}; ` +
                `run: ${install}`
        );
    }
    /* Untracked files count, for the reason set out in `pinnedCmark` above:
     * this is the same source tree with the same include ordering, so an
     * edited or stray file is built as the reference while HEAD still reads
     * as the pin. */
    const dirty = run("git", ["-C", checkout, "status", "--porcelain", "--untracked-files=all"]).trim();
    if (dirty) {
        fail(`the cmark-gfm oracle checkout has local modifications, so it is not cmark-gfm ${version}:\n${dirty}`);
    }
    return { version, commit, checkout };
}

/* Rebuilt with the profile's compiler and flags rather than read from whatever
 * init-environment produced, for the same reason cmark is: the report's central
 * claim is that every engine met the same compiler. */
function buildCmarkGfm(profile, gfm, out, versions) {
    const buildDir = path.join(out, "cmark-gfm");
    discardForeignTree(buildDir, profile, versions);
    run(
        "cmake",
        [
            "-S",
            gfm.checkout,
            "-B",
            buildDir,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMARK_TESTS=OFF",
            "-DCMARK_SHARED=OFF",
            "-DBUILD_TESTING=OFF",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            `-DCMAKE_C_COMPILER=${profile.compiler}`,
            `-DCMAKE_C_FLAGS_RELEASE=${profile.flags}`
        ],
        { env: buildEnvironment() }
    );
    run("cmake", ["--build", buildDir, "--parallel"], { env: buildEnvironment() });
    stampTree(buildDir, profile, versions);
    return buildDir;
}

function buildCmark(profile, cmark, out, versions) {
    const buildDir = path.join(out, "cmark");
    discardForeignTree(buildDir, profile, versions);
    run(
        "cmake",
        [
            "-S",
            cmark.checkout,
            "-B",
            buildDir,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_TESTING=OFF",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            `-DCMAKE_C_COMPILER=${profile.compiler}`,
            `-DCMAKE_C_FLAGS_RELEASE=${profile.flags}`
        ],
        { env: buildEnvironment() }
    );
    run("cmake", ["--build", buildDir, "--parallel"], { env: buildEnvironment() });
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
function stampOf(profile, versions) {
    /* CFLAGS and LDFLAGS are in here because CMake initializes cache variables
     * from both -- CMAKE_C_FLAGS ahead of CMAKE_C_FLAGS_RELEASE on every
     * compile line, CMAKE_EXE_LINKER_FLAGS on every link line -- and CMake
     * initializes them ONCE, at first configure. So a tree first configured
     * under an exported `-march=native` or `-static` keeps those flags in its
     * cache for every later build, and a run without the variable set would
     * otherwise match the stamp and reuse binaries the preset never described.
     * process.arch is in here because the same compiler string builds for more
     * than one target. */
    return [
        profile.compiler,
        profile.flags,
        process.env.CFLAGS ?? "",
        process.env.LDFLAGS ?? "",
        process.arch,
        versions.compiler,
        versions.compilerDigest,
        versions.compilerBinaries,
        versions.libc,
        versions.target,
        versions.targetDigest
    ].join("\n");
}

const discardForeignTree = (buildDir, profile, versions) => discardTree(buildDir, stampOf(profile, versions));

const stampTree = (buildDir, profile, versions) => markTree(buildDir, stampOf(profile, versions));

function buildRunners(profile, cmark, cmarkBuildDir, gfm, gfmBuildDir, versions, sourceRoot = root) {
    discardForeignTree(profile.binaryDir, profile, versions);
    run(
        "cmake",
        [
            "--preset",
            PROFILE_PRESET,
            "-B",
            profile.binaryDir,
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            `-DMARKDOWN_CORE_CMARK_SOURCE_DIR=${path.join(cmark.checkout, "src")}`,
            `-DMARKDOWN_CORE_CMARK_BUILD_DIR=${cmarkBuildDir}`,
            `-DMARKDOWN_CORE_CMARK_GFM_SOURCE_DIR=${gfm.checkout}`,
            `-DMARKDOWN_CORE_CMARK_GFM_BUILD_DIR=${gfmBuildDir}`
        ],
        { env: buildEnvironment(), cwd: sourceRoot }
    );
    run("cmake", ["--build", profile.binaryDir, "--parallel"], { env: buildEnvironment(), cwd: sourceRoot });
    stampTree(profile.binaryDir, profile, versions);
}

/* Rebuild the requested base with this run's harness, preset and references.
 * Only engine source comes from the base. The current corpus is generated once
 * and passed byte-for-byte to both binaries, even when a PR changes its corpus. */
function buildBaseline(options, profile, cmark, cmarkBuildDir, gfm, gfmBuildDir, versions) {
    if (!options.baselineRef) return null;
    const revision = run("git", ["rev-parse", "--verify", `${options.baselineRef}^{commit}`]).trim();
    const directory = path.join(options.out, "baseline");
    fs.rmSync(directory, { recursive: true, force: true });
    fs.mkdirSync(directory, { recursive: true });
    const source = path.join(directory, "source");
    fs.mkdirSync(source);
    fs.mkdirSync(path.join(directory, "corpus"));
    const archive = path.join(directory, "source.tar");
    run("git", ["archive", "--format=tar", `--output=${archive}`, revision]);
    run("tar", ["-xf", archive, "-C", source]);
    fs.unlinkSync(archive);
    fs.cpSync(BENCHMARKS, path.join(source, "packages/markdown-core/benchmarks"), { recursive: true });
    fs.copyFileSync(path.join(root, "CMakePresets.json"), path.join(source, "CMakePresets.json"));
    const built = { ...profile, binaryDir: path.join(directory, "build") };
    buildRunners(built, cmark, cmarkBuildDir, gfm, gfmBuildDir, versions, source);
    verifyStageSymbols(built);
    verifyBuildProvenance(built, versions);
    const compiled = readCompiledFlags(source, built.binaryDir, "libmarkdown-core-public-static", fail);
    const current = readCompiledFlags(root, profile.binaryDir, "libmarkdown-core-public-static", fail);
    if (
        JSON.stringify(compiled) !== JSON.stringify(current) ||
        JSON.stringify(effectiveFlags(built.binaryDir)) !== JSON.stringify(effectiveFlags(profile.binaryDir))
    ) {
        fail("baseline and current core use different effective build flags");
    }
    const libraries = loadedLibraries(
        path.join(built.binaryDir, ENGINES["markdown-core"].runner),
        measurementRoot(directory, fail)
    );
    if (JSON.stringify(libraries) !== JSON.stringify(versions.libraries))
        fail("baseline uses different runtime libraries");
    return { revision, profile: built, directory, binaries: runnerIdentity(built), cases: [] };
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
    for (const [engine, runner] of Object.entries(MEASURED_BINARIES)) {
        const binary = path.join(profile.binaryDir, runner);
        const readelf = spawnSync("readelf", ["-p", ".comment", binary], { encoding: "utf8" });
        if (readelf.status !== 0) {
            fail(`${engine}: cannot read the build provenance of ${runner}; readelf is required`);
        }
        /* `  [     0]  GCC: (Ubuntu 13.3.0-...) 13.3.0` -> the string itself. */
        const producers = [
            ...new Set([...(readelf.stdout ?? "").matchAll(/^\s*\[\s*[0-9a-f]+\]\s{2}(.+?)\s*$/gmu)].map((m) => m[1]))
        ];
        if (producers.length !== 1) {
            fail(
                `${engine}: ${runner} records ${producers.length} compiler identities ` +
                    `(${producers.join(" | ") || "none"}), so the report cannot name one; ` +
                    `delete ${path.relative(root, profile.binaryDir)} and re-run`
            );
        }
        if (!producers[0].includes(identity)) {
            fail(
                `${engine}: ${runner} was built by "${producers[0]}" but the report would name ` +
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
    const symbolsOf = (engine, runner) => {
        const binary = path.join(profile.binaryDir, runner);
        if (!fs.existsSync(binary)) fail(`${engine}: ${runner} was not built`);
        return new Set(
            run("nm", ["-a", binary])
                .split("\n")
                .map((line) => line.trim().split(/\s+/u).pop() ?? "")
                .map((name) => name.replace(CLONE_SUFFIX, ""))
        );
    };
    for (const [engine, definition] of Object.entries(ENGINES)) {
        const symbols = symbolsOf(engine, definition.runner);
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
    /* The edge the remainder alone is read on, held the same way: a boundary
     * folded into its caller is a missing number, not a smaller one. */
    const symbols = symbolsOf("attribute runner", ATTRIBUTE_RUNNER.runner);
    for (const name of [ATTRIBUTE_RUNNER.caller, ATTRIBUTE_RUNNER.callee]) {
        if (!symbols.has(name)) {
            fail(
                `attribute runner: ${name} is absent from ${ATTRIBUTE_RUNNER.runner}, so the remainder alone cannot be read`
            );
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
/**
 * A case generated from a numbered unit rather than from a sample file.
 *
 * Both halves of a LOGICAL ISOMORPH are built this way. The index keeps every
 * generated declaration distinct, which the pairing needs: a repeated literal
 * name would leave one side binding one identifier and the other binding
 * thousands, and those are not the same workload however alike they read.
 *
 * `{n}` is the index as written; `{n:K}` is the same index zero-padded to K
 * digits. A grid table establishes its columns by the positions of the `+`
 * characters in its border line and requires every row line to close its cells
 * at exactly those columns, so an index that gains a digit at ten, at a hundred
 * and at a thousand would silently stop the construct being recognised part way
 * through the document -- the case would still generate, and would measure a
 * paragraph. Any case whose construct is column-aligned uses the padded form.
 */
function unitText(template, index) {
    return template
        .replaceAll(/\{n:(\d+)\}/gu, (_, width) => {
            const written = String(index);
            /* `padStart` never truncates, so an index past the declared width
             * silently writes one character too many -- and a column-aligned
             * construct stops closing at its border the moment that happens.
             * The case still generates, the counts on the two sides still
             * agree because both halves are built from the same index, and what
             * gets measured is a paragraph. The reach audit cannot see it
             * either: it reads the x1 documents, and the overflow arrives at a
             * larger scale. So it is refused here, where the width is known. */
            if (written.length > Number(width)) {
                fail(
                    `a generated unit writes index ${written} into {n:${width}}, which is ${width} digits wide. ` +
                        `The placeholder exists to keep a column-aligned construct closing at its border, and an ` +
                        `index that outgrows it breaks the construct silently. Widen the placeholder and the ` +
                        `columns that depend on it`
                );
            }
            return written.padStart(Number(width), "0");
        })
        .replaceAll("{n}", String(index));
}

function generatedText(generated, target) {
    let text = "";
    let units = 0;
    /* The running total is carried, not recomputed. Measuring the whole
     * accumulated document once per unit is quadratic in the document and, since
     * `--scale N` builds every size up to N, cubic in the scale: on a 256 KiB
     * target that is 395 ms of generation against 1.6 ms here, for byte-identical
     * output. Each unit's own length is what the target is counted in. */
    let bytes = 0;
    while (bytes < target) {
        const unit = unitText(generated.unit, units);
        text += unit;
        bytes += Buffer.byteLength(unit);
        units += 1;
    }
    return { text: (generated.head ?? "") + text + (generated.tail ?? ""), length: units };
}

/**
 * A `head` exists for the one production that is recognised ONCE per document.
 * A properties envelope may only open a document, so its workload cannot scale
 * by repeating the construct -- it scales by the MEMBER LINES inside a single
 * envelope, which means the opening delimiter has to be emitted before the
 * repeated unit and the closing one after. Every other mode repeats a whole
 * construct and needs no head.
 */

/**
 * The other half of a logical isomorph, generated to the SAME COUNT.
 *
 * Not to the same byte length. Two spellings of one construct are paired
 * because they express the same thing, and when one spelling needs more bytes
 * than the other, sizing both to a byte target gives them different numbers of
 * the construct -- a comparison of one document's size with another's, wearing
 * the name of a comparison of two grammars. The count comes from what the
 * partner's generator actually emitted, so nothing has to be counted back out
 * of the text by pattern.
 */
function countedText(counted, units) {
    let text = "";
    for (let index = 0; index < units; index++) {
        text += unitText(counted.unit, index);
    }
    return { text: (counted.head ?? "") + text + (counted.tail ?? ""), length: units };
}

function chainText(chain, target) {
    const tail = chain.tail ?? "";
    const unit = Buffer.byteLength(chain.unit);
    const length = Math.max(1, Math.floor((target - Buffer.byteLength(tail)) / unit));
    return { text: chain.unit.repeat(length) + tail, length };
}

/**
 * A flag string split the way the build will split it.
 *
 * CMake stores these verbatim and the compile line it generates is
 * interpreted, so a shell decides where one flag ends -- not whitespace.
 * `\'@/tmp/flags with space.rsp\'` is three whitespace-separated words and one
 * shell argument, and it is the argument the compiler reads. Splitting it here
 * any other way asks a different question than the build answers, which is the
 * same lesson the resolved-target probe records one screen up.
 *
 * The string reaches a shell either way, so asking adds no exposure the build
 * does not already have. A string the shell cannot split is refused: the build
 * would not be able to run it either.
 */
function flagArguments(name) {
    const value = process.env[name] ?? "";
    if (!value.trim()) {
        return [];
    }
    const probe = spawnSync("/bin/sh", ["-c", `printf '%s\\0' ${value}`], {
        encoding: "utf8",
        env: buildEnvironment()
    });
    if (probe.status !== 0) {
        fail(`${name} cannot be split into arguments by the shell that will run them: ${value}`);
    }
    return (probe.stdout ?? "").split("\0").filter(Boolean);
}

/* dyld's placeholders, which are not files to read but positions to resolve
 * against at load time: `-Wl,-rpath,@loader_path/../lib` is an ordinary macOS
 * link flag and the only @ in linker arguments that does not name a file. */
const LOAD_PATH_PLACEHOLDERS = ["@executable_path", "@loader_path", "@rpath"];

/**
 * The response files one shell argument names.
 *
 * The compiler's own spelling is a leading @. The linker's and the assembler's
 * arrive through `-Wl,` and `-Wa,`, where the argument is a comma-separated
 * list and the @ begins a piece of it rather than the whole -- and `ld` and
 * `as` read those files exactly as the driver reads its own.
 */
function responseFiles(argument) {
    return argument
        .split(",")
        .filter((piece) => piece.startsWith("@") && !LOAD_PATH_PLACEHOLDERS.some((at) => piece.startsWith(at)));
}

/**
 * A response file is a flag whose content is kept somewhere else.
 *
 * `gcc @flags.rsp` compiles with whatever that file says, and every place this
 * report looks sees the path instead of the contents: the recorded compile
 * lines carry `@flags.rsp`, the resolved target never shows a define at all,
 * and the compiler's banner does not echo its own arguments. Editing the file
 * between two runs changes the objects while every digest here stays
 * identical -- two reports declared comparable that measured different
 * binaries, which is the one thing the identity exists to prevent.
 *
 * Refused rather than expanded. Expansion is not one line: response files
 * nest, carry their own quoting rules, and resolve their paths against the
 * directory the compiler ran in, so an expander that is subtly wrong rebuilds
 * this same hole behind a digest that now looks thorough. Inlining the flags
 * is the caller's one-line fix, and the preset's own flags are pinned.
 *
 * This is about options, not about every file an option can reach. `-I` and
 * `-include` name files the report does not digest either, but they say so on
 * the compile line and the identity has never claimed to cover the sources a
 * build reads. A response file is the case where the recorded flags themselves
 * are not the flags used, and that is what makes it a lie rather than a limit.
 */
function refuseResponseFiles() {
    for (const name of BUILD_FLAG_VARIABLES) {
        const file = flagArguments(name).flatMap(responseFiles).at(0);
        if (file) {
            fail(
                `${name} names the response file ${file}, whose contents reach the build but no line of the ` +
                    `report -- editing it would change the measurement and nothing would say so; inline its flags`
            );
        }
    }
}

function corpusManifest() {
    const manifest = JSON.parse(fs.readFileSync(path.join(BENCHMARKS, "corpus.json"), "utf8"));
    if (manifest.schemaVersion !== 3) fail(`unsupported corpus schema: ${manifest.schemaVersion}`);
    validatePairs(manifest);
    return manifest;
}

/**
 * Every requested name has to exist, not just one of them.
 *
 * A run filtered to `--case mixed-commonmark --case chain-braket-open` would
 * otherwise measure the first, drop the typo silently, and report an
 * experiment the caller did not ask for -- with the adversarial case they
 * wanted absent.
 *
 * Asked of the manifest before anything is installed or built, because that is
 * all it takes to answer. A typo told to go install an oracle, or told nothing
 * until two builds have run, is a correction the caller has to wait for and
 * then read past the wrong error to find.
 */
function refuseUnknownCases(options, manifest) {
    const named = new Set(manifest.cases.map((entry) => entry.name));
    const unknown = options.cases.filter((name) => !named.has(name));
    if (unknown.length) {
        fail(`no corpus case is named ${unknown.join(", ")}; the manifest has ${[...named].sort().join(", ")}`);
    }
}

function buildCorpus(options, manifest) {
    const directory = path.join(options.out, "corpus");
    fs.mkdirSync(directory, { recursive: true });
    /* A named case drags in what it is DEFINED AGAINST -- its pair, a split
     * `with` its `without`, a counted case its generated match -- to a
     * fixpoint. The rule is `caseClosure` in `lib/corpus-splits.mjs`, where
     * the reach audit reads the same declarations, and its tests hold the
     * direction: a pair half drags its other half, a `with` drags its
     * `without`, and a `without` drags no `with`. */
    const wanted = caseClosure(manifest, options.cases);
    const selected = options.cases.length ? manifest.cases.filter((entry) => wanted.has(entry.name)) : manifest.cases;

    const documents = [];
    /* What each generated case actually emitted, so its partner is built to the
     * same count rather than to a guess at one. */
    const emitted = new Map();
    const partnerUnits = (match, scale) => {
        const key = `${match}|${scale}`;
        if (!emitted.has(key)) {
            fail(`a counted case pairs with ${match}, which must be a "generated" case declared before it`);
        }
        return emitted.get(key);
    };
    for (const entry of selected) {
        const modes = [entry.samples, entry.chain, entry.counted, entry.generated, entry.boundary].filter(
            Boolean
        ).length;
        if (modes !== 1) {
            fail(
                `corpus case ${entry.name} must name exactly one of "samples", "chain", "generated", "counted" or "boundary"`
            );
        }
        const unit = entry.samples ? documentsUnit(entry) : null;
        for (let scale = 1; scale <= options.scale; scale++) {
            const target = (entry.targetBytes ?? manifest.targetBytes) * scale;
            const built = entry.boundary
                ? (() => {
                      const source = documents.find((doc) => doc.case === entry.boundary.match && doc.scale === scale);
                      if (!source) fail(`${entry.name}: boundary source must be generated first`);
                      return {
                          text: boundarySource(entry.boundary.cut, fs.readFileSync(source.file, "utf8")),
                          length: source.units
                      };
                  })()
                : entry.chain
                  ? chainText(entry.chain, target)
                  : entry.generated
                    ? generatedText(entry.generated, target)
                    : entry.counted
                      ? countedText(entry.counted, partnerUnits(entry.counted.match, scale))
                      : (() => {
                            const repeats = Math.max(1, Math.ceil(target / Buffer.byteLength(unit)));
                            return { text: unit.repeat(repeats), length: repeats };
                        })();
            if (entry.generated) emitted.set(`${entry.name}|${scale}`, built.length);
            const file = path.join(directory, `${entry.name}.x${scale}.md`);
            fs.writeFileSync(file, built.text);
            documents.push({
                case: entry.name,
                ...(entry.boundary ? { boundary: entry.boundary } : {}),
                dialect: entry.dialect,
                gfm: entry.gfm === true,
                /* The fields this case's tree carries that no reference builds.
                 * `dialect` asserts what the SYNTAX is; this says what the
                 * OUTPUT is, and reading the first as though it were the second
                 * is what published a feature's price as a parsing ratio. */
                carries: entry.carries ?? [],
                /* What the growth table is varying. `documents` cases add
                 * independent copies; a chain case grows one structure, and
                 * WHICH dimension is not the same question as the shape --
                 * chain-link-candidates grows a count of separately bounded
                 * failures, not a depth, so a table that called it "structure"
                 * alongside the nesting cases would invite exactly the reading
                 * the case was renamed to prevent. */
                growth: entry.chain
                    ? (entry.scales ?? "structure")
                    : (entry.generated ?? entry.counted)
                      ? /* Named by the case, because a generated case scales
                         * whatever its unit holds and that is not one thing:
                         * the anchor pair scales declarations, the span pair
                         * scales spans and links. A single label for the mode
                         * would describe the anchor pair and misdescribe the
                         * rest, in the one column that exists to say which
                         * dimension grew. */
                        ((entry.generated ?? entry.counted).scales ?? "constructs")
                      : "documents",
                scale,
                units: built.length,
                bytes: Buffer.byteLength(built.text),
                /* The bytes actually parsed, not just how many there were. A
                 * byte count does not distinguish two documents of one size. */
                sha256: crypto.createHash("sha256").update(built.text).digest("hex"),
                file
            });
        }
    }
    for (const pair of manifest.pairs.filter(provenPair)) {
        for (const dialect of documents.filter((entry) => entry.case === pair.case)) {
            const common = documents.find((entry) => entry.case === pair.isomorph && entry.scale === dialect.scale);
            if (!common) fail(`${pair.case}: proved pair is missing its other half`);
            proofWorkload(pair, fs.readFileSync(dialect.file, "utf8"), fs.readFileSync(common.file, "utf8"));
        }
    }
    return { targetBytes: manifest.targetBytes, digest: corpusDigest(documents), documents };
}

/**
 * One digest naming the whole workload a report measured.
 *
 * The toolchain table says what built the binaries; this says what they were
 * given. Both have to match before two reports can be compared, because an
 * edited `corpus.json`, an edited sample, or a change to how documents are
 * generated moves every count without touching either parser -- and a report
 * that recorded only byte counts cannot tell that apart from an optimization.
 *
 * Case name and scale are folded in beside the content, so a `--case`-filtered
 * run does not present itself as comparable to a full one.
 */
function corpusDigest(documents) {
    const digest = crypto.createHash("sha256");
    for (const document of documents) {
        digest.update(`${document.case}\u0000${document.scale}\u0000${document.bytes}\u0000${document.sha256}\n`);
    }
    return digest.digest("hex");
}

/** The exact bytes measured, so a report's numbers can be traced to a binary. */
function runnerIdentity(profile) {
    const identity = {};
    for (const [engine, runner] of Object.entries(MEASURED_BINARIES)) {
        const binary = path.join(profile.binaryDir, runner);
        identity[engine] = {
            sha256: crypto.createHash("sha256").update(fs.readFileSync(binary)).digest("hex"),
            builtAt: new Date(fs.statSync(binary).mtimeMs).toISOString()
        };
    }
    return identity;
}

/* The measured child's environment and the profiler's own configuration are
 * both built rather than inherited, by `lib/measurement.mjs` -- which is where
 * the reasoning lives, because the attribute benchmark measures under the same
 * isolation and a second copy of it is a copy that drifts. The numbers that
 * make it load-bearing are there too: `MALLOC_PERTURB_=42` alone moves one
 * case's summary by 47.7%. */

/**
 * What glibc will dispatch on, seen from inside the measurement.
 *
 * The C library picks an implementation per routine at load time from the CPU
 * it detects -- `__memcpy_avx_unaligned_erms` and `__strlen_avx2` here, plain
 * SSE2 variants on a host without AVX2 -- and those instructions are inside the
 * stage costs, because the parsers call them constantly and in different
 * proportions. So two hosts with the same compiler, C library, valgrind and
 * compiler target can still produce different counts AND a different ratio,
 * which the identity would otherwise declare comparable.
 *
 * What it dispatches on is not the raw host: valgrind masks CPUID, and on this
 * machine glibc sees max_cpuid 0xd under it against 0x1f native. So the
 * question has to be asked through valgrind, of the loader that will run the
 * measured binary, which is what this does.
 *
 * Recorded rather than pinned to a fixed capability set. Constraining dispatch
 * would need GLIBC_TUNABLES in the measurement environment -- the one variable
 * whose removal is load-bearing two functions up -- and would measure a libc
 * nobody runs. And recorded as the CPU FEATURES rather than as the routines
 * that were selected: the feature set depends only on the host, valgrind and
 * glibc, while the set of routines a parse happens to call is a property of the
 * code, which would make every commit incomparable with the one before it.
 */
/**
 * The C library the measured binaries actually load, by its bytes.
 *
 * `ldd --version` names a release. A distribution patch, a local rebuild or a
 * different build of the same release keeps that line and changes the
 * instructions inside memcpy and strlen -- which run inside the stage costs,
 * and which the two engines call in different proportions. The dispatch digest
 * does not cover this either: it records what glibc dispatches ON, the CPU
 * features, not which implementation those features selected.
 *
 * So the objects are digested by content, keyed by soname rather than by path
 * so that the same library installed in two places compares equal. The vdso is
 * skipped: the kernel provides it and there is no file to read.
 *
 * Asked in the measurement's own environment, because `ldd` resolves what the
 * caller's LD_PRELOAD and LD_LIBRARY_PATH say rather than what the measured
 * child will load -- and the measured child is given neither. Inheriting them
 * would fingerprint libraries that never ran, and would do it differently on
 * two hosts whose measurements were identical.
 */
function loadedLibraries(runner, isolated) {
    const listing = run("ldd", [runner], { env: measurementEnvironment(isolated), cwd: isolated });
    const resolved = [...listing.matchAll(/=>\s*(\/\S+)|^\s*(\/\S+)/gmu)]
        .map((match) => match[1] ?? match[2])
        .filter((file) => fs.existsSync(file));
    if (!resolved.length) {
        fail(`the shared libraries of ${path.relative(root, runner)} could not be identified`);
    }
    const objects = resolved
        .map(
            (file) =>
                `${path.basename(file)}\u0000${crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex")}`
        )
        .sort();
    return {
        summary: resolved
            .map((file) => path.basename(file))
            .sort()
            .join(" "),
        digest: crypto.createHash("sha256").update(objects.join("\n")).digest("hex")
    };
}

function dispatchIdentity(profile, root) {
    const runner = path.join(profile.binaryDir, ENGINES["markdown-core"].runner);
    const loader = /(\/\S*ld-linux\S*\.so\S*)/u.exec(run("ldd", [runner]))?.[1];
    if (!loader) fail(`the dynamic loader for ${path.relative(root, runner)} could not be identified`);
    /* Same isolation as the measurement itself, working directory included:
     * valgrind reads `./.valgrindrc` as well as `~/.valgrindrc`, so a probe run
     * from the repository would take options the measured child does not -- and
     * a callgrind-only option there fails outright under `--tool=none`, which
     * would turn a stray file in someone's checkout into a refused benchmark. */
    const isolated = measurementRoot(path.dirname(profile.binaryDir));
    const probe = spawnSync("valgrind", ["--tool=none", "--quiet", loader, "--list-diagnostics"], {
        encoding: "utf8",
        env: measurementEnvironment(isolated),
        cwd: isolated
    });
    const features = (probe.stdout ?? "")
        .split("\n")
        .map((line) => line.trim())
        .filter((line) => line.includes("cpu_features"))
        .sort();
    if (probe.status !== 0 || !features.length) {
        fail(
            `${loader} would not report the CPU features glibc dispatches on, so this report cannot say ` +
                "which C library implementations it measured (glibc 2.33 or newer provides --list-diagnostics)"
        );
    }
    return crypto.createHash("sha256").update(features.join("\n")).digest("hex");
}

function measure(profile, engine, document, out) {
    const definition = ENGINES[engine];
    const dump = path.join(out, "callgrind", `${engine}.${document.case}.x${document.scale}.out`);
    fs.mkdirSync(path.dirname(dump), { recursive: true });
    const root = measurementRoot(out, fail);
    const stdout = run(
        "valgrind",
        [
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
        ],
        /* Every path above is absolute, so the child can be run from the
         * directory that exists to hold no configuration. */
        { env: measurementEnvironment(root), cwd: root }
    );

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
        hotPaths: hotPaths(profileByName),
        dump
    };
}

/**
 * The remainder of a split with no host around it.
 *
 * The reference for a remainder measured in place is the same grammar
 * decoding the same bytes alone: what a host adds to that is the composition,
 * the seam between the host's scan and the list's, which is the number a
 * split exists to print. Alone is read through the attribute runner the lexbor
 * comparison uses, from the same build tree as the stage runner, on the edge
 * `scripts/benchmark-attributes.mjs` reads -- so the two drivers' numbers for
 * the list alone are one measurement, not two that happen to agree.
 */
function measureAlone(profile, split, index, out) {
    const input = path.join(out, "corpus", `split-${index}.alone.txt`);
    fs.writeFileSync(input, `${split.bytes}\n`.repeat(ALONE_LISTS));
    const dump = path.join(out, "callgrind", `attribute-runner.split-${index}.alone.out`);
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
            path.join(profile.binaryDir, ATTRIBUTE_RUNNER.runner),
            "--input",
            input
        ],
        { env: measurementEnvironment(isolated), cwd: isolated }
    );
    const receipt = /lists=(\d+) values=(\d+)/u.exec(stdout);
    if (!receipt) fail(`the attribute runner produced no receipt for the remainder of split ${index} alone`);
    const lists = Number(receipt[1]);
    /* One list per copy, or the bytes are not one list and the number per
     * list divides by the wrong count. */
    if (lists !== ALONE_LISTS) {
        fail(
            `the attribute runner recovered ${lists} lists from ${ALONE_LISTS} copies of ${JSON.stringify(split.bytes)}, ` +
                `so the remainder is not one attribute list`
        );
    }
    const parsed = foldNames(parseCallgrind(fs.readFileSync(dump, "utf8")), (name) => name.replace(CLONE_SUFFIX, ""));
    const edges = edgesBetween(parsed, ATTRIBUTE_RUNNER.caller, ATTRIBUTE_RUNNER.callee);
    if (!edges.length) {
        fail(
            `attribute runner: no call edge ${ATTRIBUTE_RUNNER.caller} -> ${ATTRIBUTE_RUNNER.callee} in ${path.basename(dump)}`
        );
    }
    const ir = edges.reduce((total, edge) => total + (costRecord(parsed, edge.cost).Ir ?? 0), 0);
    return {
        runner: ATTRIBUTE_RUNNER.runner,
        entry: `${ATTRIBUTE_RUNNER.caller} -> ${ATTRIBUTE_RUNNER.callee}`,
        lists,
        values: Number(receipt[2]),
        ir,
        perList: ir / lists,
        input: path.relative(out, input),
        dump: path.relative(out, dump)
    };
}

/**
 * Every split's rows, computed once and recorded in `stages.json` beside the
 * cases, so the markdown prints what the JSON holds and a number tracked
 * across runs is read from data rather than parsed back out of prose.
 *
 * A row is the remainder's cost IN one host, read as the difference between
 * two whole documents that differ by the remainder's bytes and nothing else
 * (`scripts/audit-corpus-reach.mjs` holds them to that), per list: over the
 * two stages, and over the whole parse path, which includes releasing what
 * the list built and the stages do not. Beside it: which stage the difference
 * fell in, read off the measurement rather than asserted from the grammar; the
 * whole document's cost with the list over without; the same marginal at the
 * next size over this one, which should not move, because a list costs what
 * it costs however many there are; and, once the remainder alone is measured,
 * the whole-path marginal over the alone cost -- what the host adds to
 * decoding the list, which is the composition.
 */
function measureSplits(manifest, cases, profile, out) {
    const engineOf = (document) => document.engines["markdown-core"];
    const coreIr = (document) => STAGES.reduce((sum, stage) => sum + engineOf(document).stages[stage].ir, 0);
    const at = (name, scale) =>
        cases.find((item) => item.case === name && item.scale === scale && item.engines["markdown-core"]);
    return (manifest.splits ?? []).map((split, index) => {
        const hosts = (split.hosts ?? []).map((host) => {
            const without = at(host.without, 1);
            const carrier = at(host.with, 1);
            /* Only where both halves were measured: a `--case` run that named
             * the `without` alone has that document's own comparison to report
             * and no split, and naming the `with` alone cannot happen, because
             * the closure drags the `without` in. */
            if (!without || !carrier) return { ...host, measured: null };
            if (carrier.units !== without.units) {
                fail(
                    `${host.with} and ${host.without} are the two halves of a split but carry ${carrier.units} ` +
                        `and ${without.units} units. The difference between them is the remainder only ` +
                        `while both documents hold the same number of everything else`
                );
            }
            const lists = carrier.units * host.each;
            const perList = (coreIr(carrier) - coreIr(without)) / lists;
            const byStage = Object.fromEntries(
                STAGES.map((stage) => [
                    stage,
                    (engineOf(carrier).stages[stage].ir - engineOf(without).stages[stage].ir) / lists
                ])
            );
            const landing = STAGES.reduce((best, stage) => (byStage[stage] > byStage[best] ? stage : best));
            const without2 = at(host.without, 2);
            const carrier2 = at(host.with, 2);
            const growth =
                without2 && carrier2 && carrier2.units === without2.units && perList > 0
                    ? (coreIr(carrier2) - coreIr(without2)) / (carrier2.units * host.each) / perList
                    : null;
            return {
                ...host,
                measured: {
                    lists,
                    perList,
                    wholePerList: (engineOf(carrier).parsePathIr - engineOf(without).parsePathIr) / lists,
                    byStage,
                    landing,
                    ratio: coreIr(carrier) / coreIr(without),
                    growth,
                    inPlaceOverAlone: null
                }
            };
        });
        if (!hosts.some((host) => host.measured)) return { ...split, alone: null, hosts };
        const alone = measureAlone(profile, split, index, out);
        return {
            ...split,
            alone,
            hosts: hosts.map((host) =>
                host.measured
                    ? {
                          ...host,
                          measured: { ...host.measured, inPlaceOverAlone: host.measured.wholePerList / alone.perList }
                      }
                    : host
            )
        };
    });
}

/* The functions this document spent the most instructions IN, as opposed to
 * through.
 *
 * The per-stage breakdown beside this one is the stage entry's immediate
 * callees, which is one level deep: on a grid table it reads `S_process_line
 * 99.7%` and names no grammar at all. A ratio can say a case is expensive; only
 * this can say what is expensive about it, which is the step between noticing a
 * number and knowing what to change.
 *
 * Self cost, not inclusive: an inclusive ranking puts the drivers on top --
 * every line goes through `S_process_line` -- and buries the work. */
function hotPaths(profile) {
    /* Callgrind collects from process start, so `profile.self` holds the whole
     * executable: the loader, reading the file, freeing the source buffer,
     * printing the receipt. Ranking that and printing it beside a parse cost is
     * a claim about the parse made from a measurement of the program -- the
     * same mistake as counting the serializer. Measured it is under 1% here,
     * which is exactly why it would have gone unnoticed.
     *
     * So the ranking is restricted to what the parse entry can reach. A leaf
     * shared with the rest of the program, `free` being the obvious one, is
     * still counted whole; this narrows the claim rather than making it exact. */
    const callees = new Map();
    for (const edge of profile.edges.values()) {
        const from = baseName(edge.caller);
        if (!callees.has(from)) callees.set(from, new Set());
        callees.get(from).add(baseName(edge.callee));
    }
    const reachable = new Set();
    const pending = [baseName(ENGINE_ENTRY)];
    while (pending.length) {
        const name = pending.pop();
        if (reachable.has(name)) continue;
        reachable.add(name);
        for (const callee of callees.get(name) ?? []) pending.push(callee);
    }
    const totals = new Map();
    let whole = 0;
    for (const [name, cost] of profile.self) {
        const ir = costRecord(profile, cost).Ir ?? 0;
        if (!ir) continue;
        const fn = baseName(name);
        if (!reachable.has(fn)) continue;
        totals.set(fn, (totals.get(fn) ?? 0) + ir);
        whole += ir;
    }
    return [...totals.entries()]
        .sort((left, right) => right[1] - left[1])
        .slice(0, 8)
        .map(([name, ir]) => ({ name, ir, share: whole ? ir / whole : 0 }));
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

export function markdownReport(report) {
    if (report.schemaVersion !== 4 || !Array.isArray(report.pairs)) {
        throw new Error("report schema 4 with explicit pairing contracts required");
    }
    const lines = [];
    lines.push("## Parse stage comparison", "");
    lines.push(
        `Markdown Core against cmark \`${report.cmark.version}\` (\`${report.cmark.commit.slice(0, 12)}\`)` +
            ` and cmark-gfm \`${report.cmarkGfm.version}\` (\`${report.cmarkGfm.commit.slice(0, 12)}\`)` +
            " on the same corpus, by the same compiler, in one run, with the profile flags" +
            " this driver pins present on all of them -- checked against every measured" +
            " object's real compile line rather than assumed from what was passed to CMake.",
        "",
        "Their compile lines are NOT identical. Each project sets its own language level," +
            " warning set and target properties, and CMake derives options from those that" +
            " appear in no cache variable, so the table splits what both engines got from" +
            " what only one of them did. The split is the thing to read: warning flags" +
            " cannot reach code generation, but a define can -- a `*_STATIC_DEFINE` is a" +
            " visibility switch -- and so can anything else that turns up in those two" +
            " rows. A ratio is a fact about the two parsers only as far as those rows are" +
            " inert, which is a judgement this report leaves to whoever reads it rather" +
            " than making on their behalf. The full lines are in `stages.json`.",
        "",
        "| | |",
        "| --- | --- |",
        `| Compiler | \`${report.toolchain.compiler}\` (\`${report.toolchain.compilerDigest.slice(0, 16)}\`) |`,
        `| Compiler programs | \`${report.toolchain.compilerBinaries.slice(0, 16)}\` |`,
        `| C library | \`${report.toolchain.libc}\` |`,
        `| C library objects | \`${report.toolchain.libraries.summary}\` (\`${report.toolchain.libraries.digest.slice(0, 16)}\`) |`,
        `| Profiler | \`${report.toolchain.valgrind}\` (\`${report.toolchain.valgrindDigest.slice(0, 16)}\`) |`,
        `| Architecture | \`${report.toolchain.architecture}\` |`,
        `| Code generation target | \`${report.toolchain.target}\` (\`${report.toolchain.targetDigest.slice(0, 16)}\`) |`,
        `| Shared cache C flags | \`${report.toolchain.flags}\` |`,
        `| Shared link flags | \`${report.toolchain.linkFlags || "(none)"}\` |`,
        `| Measured objects | ${Object.entries(report.toolchain.compiled.objects)
            .map(
                ([engine, record]) =>
                    `${engine} ${record.units} (${record.distinct.length} compile ${
                        record.distinct.length === 1 ? "line" : "lines"
                    }, \`${record.digest.slice(0, 12)}\`)`
            )
            .join(", ")} |`,
        `| Reference pins | cmark \`${report.cmark.commit.slice(0, 12)}\`,` +
            ` cmark-gfm \`${report.cmarkGfm.commit.slice(0, 12)}\` |`,
        `| Compile options both engines got | \`${report.toolchain.compiled.shared}\` |`,
        `| Markdown Core only | \`${report.toolchain.compiled["markdown-core only"] || "(nothing)"}\` |`,
        `| cmark only | \`${report.toolchain.compiled["cmark only"] || "(nothing)"}\` |`,
        `| cmark-gfm only | \`${report.toolchain.compiled["cmark-gfm only"] || "(nothing)"}\` |`,
        `| C library dispatch | \`${report.toolchain.dispatch.slice(0, 16)}\` |`,
        `| Corpus | \`${report.corpus.digest.slice(0, 16)}\` (${report.corpus.cases} documents) |`,
        `| Pairing contracts | \`${report.pairingDigest.slice(0, 16)}\` |`,
        "",
        "The measurement runs in an environment built rather than inherited: a path," +
            " a home, a temporary directory and the C locale, and nothing else. An" +
            " exported LD_PRELOAD, GLIBC_TUNABLES or MALLOC_PERTURB_ would otherwise" +
            " change what the stages execute while this table stayed identical, and the" +
            " last of those is worth nearly 48% on one case. Naming variables to remove" +
            " would leave the next one admitted; an unnamed variable is absent here." +
            " The profiler's own configuration is isolated the same way: valgrind reads" +
            " `~/.valgrindrc` and `./.valgrindrc` before its command line, so every" +
            " option not passed below is otherwise the caller's to set, and the child" +
            " runs from an empty directory that serves as both.",
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
            " EFFECTIVE compile and link flags rather than the preset's, because CMake" +
            " folds the CFLAGS and LDFLAGS environment variables into those lines" +
            " alongside them, and it carries the architecture and the compiler's" +
            " RESOLVED code generation target, because `-march=native` and a" +
            " distribution's default -march both name themselves identically on" +
            " machines that generate different code. The target row's digest covers" +
            " the compiler's complete answer -- every feature switch, tuning param and" +
            " common code generation option, cache sizes included -- because the march" +
            " and mtune names are its label and not its content: one pair of names" +
            " covers host CPUs that differ in which features they expose.",
        "",
        "The compiler row carries a digest of its own, over how the compiler was" +
            " built rather than what it was asked to do. A version line names a" +
            " release, not a build of it: two compilers printing the same line can" +
            " carry different configure-time defaults and different built-in specs," +
            " and those reach the object file without appearing on any compile line" +
            " recorded here. A GCC built --enable-default-pie is the plain case -- it" +
            " compiles position-independent by default and says so nowhere else. Every" +
            " probe behind these two digests is asked twice and must agree, because a" +
            " digest that moved between two runs would make every report incomparable" +
            " while its counts stayed perfectly plausible.",
        "",
        "The last row is the workload rather than the build: one digest over every" +
            " document measured, content and all. An edited corpus manifest, an edited" +
            " sample, or a change to how documents are generated moves every count" +
            " while the parsers stand still, and a byte count cannot tell two different" +
            " documents of one size apart. Each case carries its own document digest in" +
            " `stages.json`, so a corpus that moved can be narrowed to which cases" +
            " moved.",
        ""
    );

    /* A ratio is only a comparison where both engines did the same job. */
    const stageIr = (engines, engine) =>
        engines[engine] ? STAGES.reduce((sum, stage) => sum + engines[engine].stages[stage].ir, 0) : null;
    /* The pairing, and which cases exist only to be the other half of one. An
     * isomorph is a CommonMark document written to match a dialect document,
     * not a construct anyone writes, so it belongs in the pair table and not in
     * the CommonMark median it would otherwise move. */
    const declarations = report.pairs;
    const paired = new Map(declarations.map((declaration) => [declaration.case, declaration]));
    const isIsomorph = new Set(declarations.map((declaration) => declaration.isomorph));
    const bySubstitution = new Set(declarations.filter((pair) => pair.substitution).map((pair) => pair.case));
    /* And which cases exist only to be the `with` half of a split. Such a
     * document is a host that already has a comparison, carrying a remainder
     * that has none, so it is neither a comparison nor a bound: its number is
     * the difference against its `without`, in "The remainder inside its
     * hosts" below. Ranking it would put a document written to carry an
     * unpairable production into the bound table as if that were its
     * measurement, and into the median of a group it was never part of. */
    const splits = report.splits ?? [];
    const isSplitWith = splitWithCases(report);
    /* What each ranked case IS to the report, decided once. The groups, the
     * bound prose and the hot-path table all used to test the same flags in
     * their own order, and the order is the meaning: a paired case is a pair
     * whatever its `gfm` flag says, a split's `with` half is in no group, and
     * a twin whose dialect half was not measured is in no group either. */
    const roleOf = (item) => {
        if (item.isomorph)
            return item.isomorph.proven ? "pair" : item.isomorph.contract.review ? "reviewed" : "candidate";
        if (item.boundary) return "boundary-base";
        if (isSplitWith.has(item.case)) return "split-with";
        if (item.gfm) return item.carries.length ? "unranked" : isIsomorph.has(item.case) ? "twin" : "gfm";
        if (item.dialect === "commonmark" && !item.carries.length) {
            return isIsomorph.has(item.case) ? "twin" : "commonmark";
        }
        return "bound";
    };

    const atScaleOne = new Map(report.cases.filter((item) => item.scale === 1).map((item) => [item.case, item]));
    const ranked = report.cases
        .filter((item) => item.scale === 1 && item.engines["markdown-core"])
        .map((item) => {
            const core = stageIr(item.engines, "markdown-core");
            const cmarkIr = stageIr(item.engines, "cmark");
            const gfmIr = stageIr(item.engines, "cmark-gfm");
            /* A dialect construct cmark does not implement still gets a
             * same-job ratio, through the document that IS the same tree: what
             * this parser spent on the dialect spelling, over what cmark spent
             * building the same tree from the CommonMark spelling. */
            const declaration = paired.get(item.case);
            const twin = declaration ? atScaleOne.get(declaration.isomorph) : null;
            /* The isomorph's OWN reference, not always cmark. A dialect
             * construct can pair with a GFM production -- a task marker with a
             * GFM task list item, a specimen with a GFM footnote definition --
             * and the engine that implements the isomorph is the one that did
             * the same job on it. Reading cmark there would divide by an engine
             * that parsed the paired document as ordinary prose. */
            const twinReference = twin?.gfm ? "cmark-gfm" : "cmark";
            const twinCore = twin ? stageIr(twin.engines, "markdown-core") : null;
            const twinCmark = twin ? stageIr(twin.engines, twinReference) : null;
            if (twin && twin.units !== item.units) {
                fail(
                    `${item.case} and ${declaration.isomorph} are paired but carry ${item.units} and ` +
                        `${twin.units} of the construct. A pair compares two spellings of one thing only ` +
                        `while both documents hold the same number of it`
                );
            }
            if (twin && bySubstitution.has(item.case) && twin.bytes !== item.bytes) {
                /* The substitution is character for character, so the two
                 * documents are the same length and the corpus repeats each of
                 * them the same number of times. Different totals mean the pair
                 * is no longer measuring one workload twice, and comparing the
                 * sums would divide one document's cost by another's. */
                fail(
                    `${item.case} and ${declaration.isomorph} are paired but were measured at ` +
                        `${item.bytes}/${twin.bytes} bytes over ${item.units}/${twin.units} copies`
                );
            }
            const comparison = twin
                ? pairRatios(declaration, {
                      dialect: core,
                      common: twinCore,
                      reference: twinCmark,
                      carries: twin.carries
                  })
                : null;
            return {
                ...item,
                coreIr: core,
                cmarkRatio: cmarkIr ? core / cmarkIr : null,
                gfmRatio: gfmIr ? core / gfmIr : null,
                /* Only where the other half was actually measured: a
                 * `--case`-filtered run that named one side of a pair has no
                 * comparison to report, and falls back to the bound rather than
                 * printing a pair row of dashes. */
                isomorph: twin
                    ? {
                          case: declaration.isomorph,
                          contract: declaration.contract,
                          ...comparison,
                          /* How the pair was established, because it decides
                           * which invariant held it: equal bytes under a marker
                           * substitution, or an equal count of declarations in
                           * two spellings of different length. */
                          by: declaration.substitution ? "substitution" : declaration.counts ? "count" : "domain",
                          reference: twinReference,
                          /* Its own bytes, not this case's: a logical isomorph
                           * is a different length by construction, so dividing
                           * its cost by this document's size would be reading
                           * one document's Ir over another document's bytes. */
                          bytes: twin.bytes,
                          units: twin.units,
                          coreIr: twinCore,
                          cmarkIr: twinCmark,
                          /* The fields the ISOMORPH's tree carries that no
                           * reference builds. They decide which of the three
                           * numbers below survives, and the corpus records the
                           * derivation under `unpairable`: the three are
                           * grammar = core/twinCore, shape = twinCore/twinCmark
                           * and sameJob = core/twinCmark, and twinCore CANCELS
                           * out of the last one. So a twin that costs this
                           * parser something the reference never spent -- an
                           * ATX heading, where this dialect derives an anchor
                           * and cmark does not -- inflates the denominator of
                           * Grammar and the numerator of Shape while leaving
                           * Same-job clean. Those two are suppressed rather
                           * than printed low and high. */
                          contaminates: twin.carries,
                          /* What this grammar costs over a CommonMark grammar
                           * building the same tree, inside one parser. */
                          grammar: comparison.grammar,
                          /* What this parser costs on the shape itself, where
                           * the reference did the same job. */
                          shape: comparison.shape
                      }
                    : null,
                // A candidate never falls back to its syntax flag for a same-job claim.
                sameJob: declaration
                    ? (comparison?.sameJob ?? null)
                    : item.carries.length
                      ? null
                      : gfmIr
                        ? core / gfmIr
                        : item.dialect === "commonmark" && cmarkIr
                          ? core / cmarkIr
                          : null
            };
        })
        .sort(
            (left, right) =>
                (right.sameJob ?? right.isomorph?.quotient ?? right.cmarkRatio ?? 0) -
                (left.sameJob ?? left.isomorph?.quotient ?? left.cmarkRatio ?? 0)
        );

    if (ranked.length) {
        const median = (values) => {
            const sorted = values.slice().sort((left, right) => left - right);
            if (!sorted.length) return 0;
            const middle = Math.floor(sorted.length / 2);
            /* An even group has two middle values and neither one of them is the
             * median; the CommonMark group has an even count, so taking the
             * upper published a number that was not the median of anything. */
            return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
        };
        lines.push("### Ratio against the reference", "");
        const paired = ranked.filter((item) => roleOf(item) === "pair");
        const candidates = ranked.filter((item) => roleOf(item) === "candidate");
        const reviewedWorkloads = ranked.filter((item) => roleOf(item) === "reviewed");
        const diagnostics = [...candidates, ...reviewedWorkloads];
        const bounded = ranked.filter((item) => roleOf(item) === "bound");
        lines.push(
            "Equivalent-work ratios require a domain, reversible source transformation and a structural proof.",
            "CommonMark and GFM cases use their own references. Only pairs with a registered proof enter the",
            "formal-pair summary. Candidate substitutions and count witnesses remain diagnostic measurements;",
            "they do not establish grammar isomorphism and are not averaged into equivalent-work results.",
            "",
            `This run contains ${paired.length} proved-domain pair(s) and ${candidates.length} candidate pair(s) and ${reviewedWorkloads.length} reviewed workload(s).`,
            ""
        );
        if (bounded.length) {
            lines.push(
                "",
                `Nothing implements what is left. cmark reads the document in` +
                    ` \`${bounded[0].case}\` as ordinary prose, so its number there is the cost of` +
                    " NOT having the feature. That" +
                    " bounds what a construct costs and does not say it is slow."
            );
        }
        lines.push("", "");
        /* Read off this run. Written as prose it froze at the numbers of the
         * run that wrote it, so the sentence making the case for the
         * distinction went on asserting them while the tables below reported
         * something else. */
        const pipe = ranked.find((item) => item.case === "block-table-pipe");
        if (pipe && pipe.cmarkRatio !== null && pipe.gfmRatio !== null) {
            lines.push(
                `The pipe-table case is what the distinction is worth: **${pipe.cmarkRatio.toFixed(2)}x against` +
                    ` cmark, ${pipe.gfmRatio.toFixed(2)}x against cmark-gfm**. The first number is almost` +
                    " entirely this parser building a table while the reference reads paragraphs.",
                ""
            );
        }
        lines.push("| Group | Reference | Cases | Median | Worst |", "| --- | --- | ---: | ---: | --- |");
        const groups = [
            ["CommonMark", "cmark", ranked.filter((item) => roleOf(item) === "commonmark")],
            [
                /* A PAIRED case belongs to its pair's group whatever its own
                 * `gfm` flag says, and the flag is tested after the pair rather
                 * than before it. `pair-tcaption-dialect` is a pipe table, so it
                 * carries the flag, and its same-job denominator is cmark on the
                 * CommonMark half -- classifying it by the flag counted it twice
                 * and let a cmark-derived ratio into the cmark-gfm median. */
                "GFM extensions",
                "cmark-gfm",
                ranked.filter((item) => roleOf(item) === "gfm")
            ],
            ...["cmark", "cmark-gfm"].map((reference) => [
                "Dialect, proved domain",
                reference,
                paired.filter((item) => item.isomorph.reference === reference)
            ]),
            ["Dialect-only (no reference)", "cmark, as a bound", bounded]
        ];
        for (const [label, reference, group] of groups) {
            if (!group.length) continue;
            const values = group.map((item) => item.sameJob ?? item.cmarkRatio).filter((value) => value !== null);
            if (!values.length) continue;
            const worst = group[0];
            lines.push(
                `| ${label} | \`${reference}\` | ${group.length} | ${median(values).toFixed(2)}x |` +
                    ` ${(worst.sameJob ?? worst.cmarkRatio).toFixed(2)}x \`${worst.case}\` |`
            );
        }
        lines.push("");

        if (diagnostics.length) {
            lines.push(
                reviewedWorkloads.length
                    ? "### Reviewed pair diagnostics and remaining candidates"
                    : "### Candidate pair diagnostics (equivalence unproved)",
                "",
                "These quotients retain the measured data without claiming equivalent work. A/B and B/R are",
                "suppressed when B carries an unmatched field. A/R remains an arithmetic quotient, not Same-job.",
                "Reviewed rows name a reconstruction or a measured boundary; pending rows remain explicitly unproved.",
                "",
                "| Dialect case | Paired input | A Ir | B Ir | R Ir | A/B | B/R | A/R | Review / obligation |",
                "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |"
            );
            for (const item of diagnostics) {
                const pair = item.isomorph;
                const q = (value) => (value === null ? "-" : `${value.toFixed(2)}x`);
                lines.push(
                    `| ${item.case} | ${pair.case} | ${item.coreIr} | ${pair.coreIr} | ${pair.cmarkIr} |` +
                        ` ${q(pair.grammar)} | ${q(pair.shape)} | ${q(pair.quotient)} | ${pair.contract.review ? pairReview({ case: item.case, isomorph: pair.case, contract: pair.contract }).reason : pair.contract.pending} |`
                );
            }
            lines.push("");
        }

        const reviewed = declarations
            .filter((pair) => pair.contract.review)
            .map((pair) => ({ pair, review: pairReview(pair) }));
        const boundaryRows = reviewed.filter(
            ({ pair, review }) => review.baseline && atScaleOne.has(pair.case) && atScaleOne.has(review.baseline)
        );
        if (boundaryRows.length) {
            lines.push(
                "### Reviewed corpus boundaries",
                "",
                "These are controlled whole-document interventions. Delta = Core(full) - Core(without),",
                "including recognition/construction interaction and byte-length changes. It may be negative;",
                "it is neither an isolated feature price nor a Same-job quotient. Reconstructed proof domains",
                "are measured separately and their costs must not be subtracted from the original corpus.",
                "",
                "| Original | Boundary baseline | Full Ir | Without Ir | Delta Ir | Delta / original unit | Full/without bytes |",
                "| --- | --- | ---: | ---: | ---: | ---: | ---: |"
            );
            for (const { pair, review } of boundaryRows) {
                const full = atScaleOne.get(pair.case);
                const base = atScaleOne.get(review.baseline);
                if (full.units !== base.units) throw new Error(`${pair.case}: boundary unit count mismatch`);
                const a = stageIr(full.engines, "markdown-core");
                const b = stageIr(base.engines, "markdown-core");
                if (a === null || b === null) continue;
                lines.push(
                    `| ${pair.case} | ${review.baseline} | ${a} | ${b} | ${a - b} | ${((a - b) / full.units).toFixed(2)} | ${full.bytes}/${base.bytes} |`
                );
            }
            lines.push("");
        }

        const pairs = paired;
        if (pairs.length) {
            lines.push(
                "### Proved-domain comparisons",
                "",
                "The proof applies to its declared sublanguage, not the entire dialect. The independent domain",
                "recognizer checks the generated bytes; the pair audit compares complete ordered trees from",
                "Core on both spellings and from the reference. Kind renaming is explicit, never universal erasure.",
                "",
                "Let A = Core(dialect), B = Core(paired), R = reference(paired), using total stage Ir.",
                "Grammar = A/B, Shape = B/R and Same-job = A/R. These factors share B: lowering B alone",
                "raises Grammar while lowering Shape and leaving A unchanged. Grammar includes recognition",
                "and construction, not just lexical scanning. Read both absolute costs and the product.",
                "",
                "| Dialect case | Paired input | Proof | Reference | A Ir | B Ir | R Ir | Grammar | Shape | Same-job |",
                "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"
            );
            for (const item of pairs) {
                const pair = item.isomorph;
                lines.push(
                    `| ${item.case} | ${pair.case} | ${pair.contract.proof} | ${pair.reference} |` +
                        ` ${item.coreIr} | ${pair.coreIr} | ${pair.cmarkIr} |` +
                        ` ${ratio(item.coreIr, pair.coreIr)} | ${ratio(pair.coreIr, pair.cmarkIr)} | ${ratio(item.coreIr, pair.cmarkIr)} |`
                );
            }
            lines.push("");
        }

        /* THE REMAINDER INSIDE ITS HOSTS. A production proved unpairable is
         * not always a construct of its own: an attribute list adds fields to
         * the node its host built and no node itself, so there is no document
         * that IS the list to pair or to bound -- the bound on `inline-span`
         * was mostly the inline parser around it. The corpus splits it: the
         * host without the list is a document with a comparison of its own,
         * the list without a host is the attribute runner's job (against
         * lexbor in `scripts/benchmark-attributes.mjs`, and alone here), and
         * this prints the list IN PLACE, as the difference between two whole
         * documents that differ by exactly the remainder's bytes.
         *
         * What makes the rows one comparison is that the remainder is the
         * same bytes on every row and one grammar decodes them; the reference
         * for a row is that grammar decoding the same bytes ALONE, and what a
         * host adds to that is the composition -- the seam between the host's
         * scan and the list's, which measuring the host alone (its pair) and
         * the list alone (lexbor) each miss, and which a corpus split into a
         * paired part and a bound part would never print. The numbers are
         * computed in `measureSplits` and recorded in stages.json; this only
         * renders them.
         *
         * Not the subtraction the README rejects. That drew a boundary INSIDE
         * one measurement, through a call graph that does not carry it; this
         * boundary is in the corpus, every instruction of both documents is
         * counted, and `scripts/audit-corpus-reach.mjs` holds the two trees
         * equal modulo the fields the remainder populates. */
        const count = (value) => Math.round(value).toLocaleString("en-US");
        for (const split of splits) {
            const rows = split.hosts.filter((host) => host.measured);
            if (!rows.length) continue;
            const alone = split.alone;
            const complete = rows.length === split.hosts.length;
            const scaledRows = rows.some((host) => host.measured.growth !== null);
            lines.push(
                "### The remainder inside its hosts",
                "",
                `A production proved unpairable is not always a construct of its own. **${split.remainder}**` +
                    " adds fields to the node its host built and no node itself, so there is no document" +
                    " that IS the remainder to pair or to bound. The corpus splits it three ways: the host" +
                    " without it is a document with a comparison of its own in the tables above, the" +
                    " remainder without a host is the attribute runner's job -- against lexbor in" +
                    " `scripts/benchmark-attributes.mjs`, and alone below -- and this table measures it" +
                    ` IN PLACE: the same bytes, \`${split.bytes}\`, on every node of ${rows.length} host` +
                    `${rows.length === 1 ? "" : "s"}${complete ? "" : ` (of ${split.hosts.length}; this run named a subset)`},` +
                    " as the difference between two whole documents that `scripts/audit-corpus-reach.mjs`" +
                    ` holds to the same tree modulo the ${split.varies.map((field) => `\`${field}\``).join(", ")}` +
                    ` field${split.varies.length === 1 ? "" : "s"} it populates.`,
                "",
                `**Alone: ${count(alone.perList)} Ir per list** -- the same bytes decoded by the same grammar` +
                    ` with no host around them, through \`${path.basename(alone.runner)}\` from the same build` +
                    ` tree on ${alone.lists.toLocaleString("en-US")} copies, read on the edge` +
                    ` \`${alone.entry}\`, which covers the scan, the decode and the release of each list.`,
                "",
                "`Ir per list` is the marginal over the two stages and `Whole path` the same marginal over" +
                    " the whole parse path, which includes releasing what the list built; a change that" +
                    " defers the list's work past a stage boundary widens the gap between the two." +
                    " `In place / alone` is the whole-path marginal over the alone cost: what the host" +
                    " adds to decoding the list, which is the composition, and the number a corpus split" +
                    " into a paired part and a bound part would never print -- a host that gets cheaper" +
                    " on its own while this rises has moved cost into the seam rather than removed it." +
                    " `Lands in` is the stage the difference fell in, read off the measurement: which" +
                    " parser this implementation reads the list with, not which the grammar assigns it" +
                    " to. `With/without` is the whole document's stage cost with the list over without." +
                    (scaledRows
                        ? " `x2 / x1` is the marginal per list at the next size over this one, which" +
                          " should not move: a list costs what it costs however many there are."
                        : "") +
                    " Each row names the site production the grammar gives its host, and the spread" +
                    " between rows is that production first and this implementation's seam second." +
                    " A cost both halves pay cancels here and shows in the host's own ratio above; and a" +
                    " row is the remainder's cost at the corpus's unit shape -- per-extent work lands in" +
                    " it in proportion to extent length over lists per extent -- so rows are compared" +
                    " across runs at one corpus digest, not read as verdicts.",
                "",
                "| Host | Site | Without | With | Lists | Ir per list | Whole path | In place / alone |" +
                    ` Lands in | With/without |${scaledRows ? " x2 / x1 |" : ""}`,
                `| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- | ---: |${scaledRows ? " ---: |" : ""}`
            );
            for (const host of rows) {
                const m = host.measured;
                lines.push(
                    `| ${host.host} | ${host.site} | ${host.without} | ${host.with} |` +
                        ` ${m.lists.toLocaleString("en-US")} | ${count(m.perList)} | ${count(m.wholePerList)} |` +
                        ` ${m.inPlaceOverAlone.toFixed(2)}x | \`${m.landing}\` | ${m.ratio.toFixed(2)}x |` +
                        (scaledRows ? ` ${m.growth === null ? "-" : `${m.growth.toFixed(3)}x`} |` : "")
                );
            }
            lines.push("");
            /* The spread is a statement about the whole host set, so a run
             * that named a subset does not print one: the dearest of three
             * hosts is not the dearest host. */
            if (complete && rows.length > 1) {
                const most = rows.reduce((best, host) =>
                    host.measured.wholePerList > best.measured.wholePerList ? host : best
                );
                const least = rows.reduce((best, host) =>
                    host.measured.wholePerList < best.measured.wholePerList ? host : best
                );
                lines.push(
                    `Spread, over the whole path: the dearest host (${most.host}, ${count(most.measured.wholePerList)} Ir` +
                        ` per list, ${most.measured.inPlaceOverAlone.toFixed(2)}x alone) over the cheapest` +
                        ` (${least.host}, ${count(least.measured.wholePerList)}, ${least.measured.inPlaceOverAlone.toFixed(2)}x)` +
                        ` is **${(most.measured.wholePerList / least.measured.wholePerList).toFixed(2)}x**. The spread` +
                        " is evidence, not a threshold: it names the host to open when it moves, and it is" +
                        " comparable only against a report whose identity table above is identical.",
                    ""
                );
            }
            lines.push(
                "What the split claims to hold constant, from `corpus.json`:",
                "",
                `- **${split.remainder}** -- ${split.claim}`,
                ""
            );
        }

        lines.push("### Where the cost is", "");
        lines.push(
            "The ratio says which case to look at. This says what to look at inside it:" +
                " the functions the parse spent the most instructions IN, not through." +
                " The per-stage breakdown below is one level deep and names the drivers" +
                " -- on a grid table it reads `S_process_line`, which every line goes" +
                " through -- so it cannot answer that question.",
            "",
            "Ranked by the displayed quotient. Candidate quotients and bounds are diagnostics, not equivalent-work ratios.",
            ""
        );
        lines.push(
            "| Case | Reference | Ratio | Core Ir/B | Dominant self cost |",
            "| --- | --- | ---: | ---: | --- |"
        );
        for (const item of ranked
            .filter((entry) => !["split-with", "boundary-base"].includes(roleOf(entry)))
            .slice(0, 16)) {
            const hot = (item.engines["markdown-core"].hotPaths ?? [])
                .slice(0, 3)
                .map((entry) => `\`${entry.name}\` ${(entry.share * 100).toFixed(1)}%`)
                .join(", ");
            const ratio = item.sameJob ?? item.isomorph?.quotient ?? item.cmarkRatio;
            /* Same precedence as the group table: the reference that produced
             * the ratio, not the flag on the case. */
            const reference = item.isomorph
                ? `${item.isomorph.reference}, ${item.isomorph.proven ? "proved domain" : "candidate quotient"}`
                : item.gfm
                  ? "cmark-gfm"
                  : item.carries.length
                    ? `(bound; tree carries ${item.carries.join(", ")})`
                    : item.dialect === "commonmark"
                      ? "cmark"
                      : "(bound)";
            lines.push(
                `| ${item.case} | ${reference} | ${ratio === null ? "-" : `${ratio.toFixed(2)}x`} |` +
                    ` ${(item.coreIr / item.bytes).toFixed(1)} | ${hot || "(not recorded)"} |`
            );
        }
        lines.push("");
    }

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
                " what was scaled reports a growth ratio equal to the BASELINE column; a" +
                " stage quadratic in it reports the square. The baseline is the dimension" +
                " the `scaled` column names, which is not always bytes: a generated or" +
                " counted case is judged against its CONSTRUCT count, because `{n}` gains a" +
                " digit as the document grows and doubling the byte target multiplies the" +
                " count by 1.98 rather than 2 -- reading that against bytes would report" +
                " perfectly linear per-construct work as a percent sublinear.",
            "",
            "The `scaled` column names the dimension that grew, taken from the corpus" +
                " rather than from the case's shape: a chain case can grow a nesting depth," +
                " a live stack depth, or a count of separately bounded failures, and those" +
                " are not interchangeable readings of a linear result. `documents` cases add" +
                " independent copies, so they scale breadth and hold depth fixed." +
                " `structure` cases are a single nested container or delimiter run whose" +
                " depth is the size, which is the dimension a copied document cannot" +
                " reach.",
            "",
            "| Case | Scaled | Baseline | Stage | Core growth | cmark growth |",
            "| --- | --- | ---: | --- | ---: | ---: |"
        );
        for (const entry of scaled) {
            const base = report.cases.find((item) => item.case === entry.case && item.scale === 1);
            if (!base) continue;
            /* The baseline is the dimension the `scaled` column NAMES, which for a
             * generated or counted case is the construct count and not the byte
             * count. Those are not the same number: `{n}` gains a digit as the
             * document grows, so doubling the byte target multiplies the count by
             * 1.98 rather than 2, and judging per-construct linear work against a
             * 2.00x byte ratio reports it as 0.8% to 1.6% sublinear -- an artefact
             * of the index, in the one table that exists to find real
             * sublinearity. `units` is the named dimension in every mode -- copies
             * for a samples case, depth for a chain case, constructs for these --
             * and for the first two it is exactly proportional to bytes, so
             * reading it always is both uniform and correct. */
            const byUnits = Boolean(entry.units && base.units);
            const denominator = byUnits ? entry.units / base.units : entry.bytes / base.bytes;
            const label = byUnits ? `${denominator.toFixed(3)}x units` : `${denominator.toFixed(2)}x bytes`;
            for (const stage of STAGES) {
                const core = entry.engines["markdown-core"]?.stages[stage];
                const coreBase = base.engines["markdown-core"]?.stages[stage];
                const cmark = entry.engines.cmark?.stages[stage];
                const cmarkBase = base.engines.cmark?.stages[stage];
                if (!core || !coreBase) continue;
                /* A split's `with` half is read by no reference, and its growth
                 * is still a question -- the list's cost per list should not
                 * depend on how many there are -- so the row stands with a dash
                 * where the reference column would be. */
                lines.push(
                    `| ${entry.case} | ${entry.growth} | ${label} |` +
                        ` ${stage} | ${ratio(core.ir, coreBase.ir)} | ${cmark && cmarkBase ? ratio(cmark.ir, cmarkBase.ir) : "-"} |`
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
    /* What the arguments alone decide is settled before anything is installed,
     * configured or built: a mistyped case name is the caller's to fix either
     * way, and it costs them nothing to hear it now. */
    const manifest = corpusManifest();
    refuseUnknownCases(options, manifest);
    /* The corpus is a function of the manifest and the tracked samples alone.
     * Writing it needs no compiler, no valgrind and no reference engine, so
     * whoever only wants the documents -- the corpus-reach audit does -- can
     * have them without paying for a measurement they will not read. */
    if (options.corpusOnly) {
        fs.mkdirSync(options.out, { recursive: true });
        const only = buildCorpus(options, manifest);
        /* What each document was generated to HOLD, written beside it. The
         * corpus-reach audit counts constructs in the parser's dump and has no
         * other way to learn how many the generator emitted, so it could check
         * that two sides agree with each other and never that either agrees
         * with what was asked for. Both sides recognising the same SUBSET of
         * their units, or a unit template quietly emitting two of the construct
         * instead of one, passed. This is the generator's own arithmetic,
         * published rather than re-derived. */
        fs.writeFileSync(
            path.join(options.out, "units.json"),
            `${JSON.stringify(
                Object.fromEntries(
                    only.documents
                        .filter((document) => document.scale === 1)
                        .map((document) => [document.case, document.units])
                ),
                null,
                4
            )}\n`
        );
        if (!options.quiet) {
            process.stdout.write(
                `wrote ${only.documents.length} documents to ${path.relative(root, path.join(options.out, "corpus"))} ` +
                    `(digest ${only.digest.slice(0, 16)})\n`
            );
        }
        return;
    }
    refuseResponseFiles();
    const profile = profileBuild();
    refuseOverlappingTrees(options, profile);
    const cmark = pinnedCmark();
    const gfm = pinnedCmarkGfm();

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
    const gfmBuildDir = buildCmarkGfm(profile, gfm, options.out, versions);
    buildRunners(profile, cmark, cmarkBuildDir, gfm, gfmBuildDir, versions);
    verifyStageSymbols(profile);
    verifyBuildProvenance(profile, versions);
    /* The report's central claim is that both engines met the same compiler
     * with the same flags. The two trees are configured separately, so that is
     * checked against what they each recorded rather than assumed from having
     * passed the same string to both. */
    const coreFlags = effectiveFlags(profile.binaryDir);
    const cmarkFlags = effectiveFlags(path.join(options.out, "cmark"));
    if (coreFlags.link !== cmarkFlags.link) {
        fail(
            `the engines were linked with different flags:\n` +
                `  markdown-core: ${coreFlags.link}\n  cmark: ${cmarkFlags.link}`
        );
    }
    /* The real compile lines, not the cache variables both trees share. Each
     * project adds its own language level, warning set and target properties,
     * so these are NOT identical and the report says what each one is rather
     * than claiming they match. What must match is the profile: the flags this
     * driver pins are the reason the two engines are comparable at all, and a
     * build that dropped one of them is measuring something else. */
    const compiled = {
        /* The archive the runner links, per benchmarks/CMakeLists.txt. */
        "markdown-core": readCompiledFlags(root, profile.binaryDir, "libmarkdown-core-public-static", fail),
        cmark: readCompiledFlags(root, path.join(options.out, "cmark"), "cmark", fail),
        /* Both cmark-gfm archives, because the runner links both and a GFM
         * ratio is against whatever they were compiled as. Omitting them left
         * every GFM number resting on objects no pinned-flag check looked at
         * and no identity table named. */
        "cmark-gfm": readCompiledFlags(root, path.join(options.out, "cmark-gfm"), "libcmark-gfm_static", fail),
        "cmark-gfm-extensions": readCompiledFlags(
            root,
            path.join(options.out, "cmark-gfm"),
            "libcmark-gfm-extensions_static",
            fail
        ),
        /* The attribute runner's own objects, because the remainder alone is
         * read on an edge INTO the runner -- `bench_parse_attributes` is
         * compiled into the executable, not into the archive -- so its
         * translation units are measured ones and must carry the pinned flags
         * like every other. The stage runners are not in that position: their
         * edges are internal to the parse transaction. */
        "attribute runner": readCompiledFlags(root, profile.binaryDir, ATTRIBUTE_RUNNER.target, fail)
    };
    /* Checked against EVERY measured object rather than their union: a pinned
     * flag missing from one translation unit is a hole a union would paper. */
    for (const [engine, record] of Object.entries(compiled)) {
        for (const line of record.distinct) {
            const missing = profile.flags
                .split(/\s+/u)
                .filter(Boolean)
                .filter((flag) => !line.split(" ").includes(flag));
            if (missing.length) {
                fail(`${engine} has an object compiled without the pinned flags ${missing.join(" ")}:\n  ${line}`);
            }
        }
    }
    versions.flags = coreFlags.compile;
    versions.linkFlags = coreFlags.link;
    versions.dispatch = dispatchIdentity(profile, root);
    versions.libraries = loadedLibraries(
        path.join(profile.binaryDir, ENGINES["markdown-core"].runner),
        measurementRoot(path.dirname(profile.binaryDir))
    );
    /* Split rather than left as two long lines for a reader to diff by eye: what
     * both engines got, and what only one of them did. Order is not meaning
     * here, so this compares as sets. */
    const tokens = (engine) => compiled[engine].flags.split(" ").filter(Boolean);
    const only = (engine, other) => tokens(engine).filter((flag) => !tokens(other).includes(flag));
    versions.compiled = {
        "markdown-core": compiled["markdown-core"].flags,
        cmark: compiled.cmark.flags,
        "cmark-gfm": compiled["cmark-gfm"].flags,
        "cmark-gfm-extensions": compiled["cmark-gfm-extensions"].flags,
        objects: Object.fromEntries(
            Object.entries(compiled).map(([engine, record]) => [
                engine,
                { units: record.units, distinct: record.distinct, digest: record.digest }
            ])
        ),
        shared: tokens("markdown-core")
            .filter((flag) => tokens("cmark").includes(flag))
            .join(" "),
        "markdown-core only": only("markdown-core", "cmark").join(" "),
        "cmark only": only("cmark", "markdown-core").join(" "),
        /* The GFM reference gets the same split against this parser, because a
         * GFM ratio rests on its objects exactly as a CommonMark one rests on
         * cmark's. Its two archives are unioned first: they are one reference,
         * and the runner links both. */
        "cmark-gfm only": [...new Set([...tokens("cmark-gfm"), ...tokens("cmark-gfm-extensions")])]
            .filter((flag) => !tokens("markdown-core").includes(flag))
            .join(" ")
    };
    versions.architecture = process.arch;
    const binaries = runnerIdentity(profile);

    const baseline = buildBaseline(options, profile, cmark, cmarkBuildDir, gfm, gfmBuildDir, versions);
    const corpus = buildCorpus(options, manifest);
    const splitWith = splitWithCases(manifest);
    const cases = [];
    for (const document of corpus.documents) {
        const engines = {};
        /* cmark is measured on every document as the CommonMark floor. cmark-gfm
         * is measured only where it implements the case's constructs, because a
         * reference that reads the document as paragraphs is not a second
         * opinion, and paying callgrind for one would buy a number nobody can
         * read. A split's `with` half gets no reference at all, by the same
         * rule: its bytes hold a production no reference decodes, its number is
         * the difference against its `without`, and a cmark reading of it
         * would print a per-byte ratio for a document that is not a comparison. */
        const applicable = splitWith.has(document.case)
            ? ["markdown-core"]
            : Object.keys(ENGINES).filter((engine) => engine !== "cmark-gfm" || document.gfm === true);
        for (const engine of applicable) {
            const measured = measure(profile, engine, document, options.out);
            if (measured.receiptBytes !== document.bytes) {
                fail(`${engine}: ${document.case} saw ${measured.receiptBytes} bytes, expected ${document.bytes}`);
            }
            engines[engine] = {
                parsePathIr: measured.parsePathIr,
                outsideStagesIr: measured.outsideStagesIr,
                rootChildren: measured.rootChildren,
                hotPaths: measured.hotPaths,
                stages: Object.fromEntries(
                    STAGES.map((stage) => [
                        stage,
                        { ...derive(document, measured.stages[stage]), ...measured.stages[stage] }
                    ])
                )
            };
        }
        if (!options.quiet) console.error(`measured ${document.case} x${document.scale}`);
        const entry = { ...document, file: path.relative(options.out, document.file), engines };
        cases.push(entry);
        if (baseline) {
            const measured = measure(baseline.profile, "markdown-core", document, baseline.directory);
            if (measured.receiptBytes !== document.bytes) fail(`baseline received different bytes: ${document.case}`);
            baseline.cases.push({
                ...entry,
                file: path.relative(baseline.directory, document.file),
                engines: {
                    ...engines,
                    "markdown-core": {
                        parsePathIr: measured.parsePathIr,
                        outsideStagesIr: measured.outsideStagesIr,
                        rootChildren: measured.rootChildren,
                        hotPaths: measured.hotPaths,
                        stages: Object.fromEntries(
                            STAGES.map((stage) => [
                                stage,
                                { ...derive(document, measured.stages[stage]), ...measured.stages[stage] }
                            ])
                        )
                    }
                }
            });
        }
    }

    const report = {
        schemaVersion: 4,
        toolchain: versions,
        /* The exact bytes measured, so a report's numbers trace to a binary. */
        binaries,
        profile: { compiler: profile.compiler, flags: profile.flags },
        cmark: { version: cmark.version, commit: cmark.commit },
        /* The GFM pin is recorded beside cmark's because the GFM ratios rest on
         * it: a pin that moved changes those numbers with nothing else in the
         * report saying so. */
        cmarkGfm: { version: gfm.version, commit: gfm.commit },
        corpus: { targetBytes: corpus.targetBytes, cases: corpus.documents.length, digest: corpus.digest },
        pairingDigest: pairingIdentity(
            manifest.pairs,
            ["benchmark-isomorphism.md", "benchmark-pair-review.md"].map((name) =>
                fs.readFileSync(path.join(root, "docs/architecture", name), "utf8")
            ),
            ["corpus-pairs.mjs", "pair-productions.mjs", "pair-review.mjs", "upstream-cmark.mjs"].map((name) =>
                fs.readFileSync(path.join(root, "scripts/lib", name), "utf8")
            )
        ),
        // Record the exact contracts beside the raw measurements.
        pairs: manifest.pairs,
        /* And the splits, which are neither: a remainder proved unpairable,
         * measured inside every host that admits it as the difference between
         * two whole documents -- with each host's numbers, and the remainder
         * alone, recorded beside the declaration they came from. */
        splits: measureSplits(manifest, cases, profile, options.out),
        artifacts: path.relative(root, options.out),
        cases
    };
    if (baseline) {
        const previous = {
            ...report,
            revision: baseline.revision,
            binaries: {
                ...binaries,
                "markdown-core": baseline.binaries["markdown-core"],
                "attribute runner": baseline.binaries["attribute runner"]
            },
            cases: baseline.cases,
            splits: measureSplits(manifest, baseline.cases, baseline.profile, baseline.directory),
            artifacts: path.relative(root, baseline.directory)
        };
        fs.writeFileSync(path.join(baseline.directory, "stages.json"), `${JSON.stringify(previous, null, 4)}\n`);
        fs.writeFileSync(path.join(baseline.directory, "stages.md"), `${markdownReport(previous)}\n`);
        report.sourceBudget = {
            baseline: baseline.revision,
            limit: SOURCE_IR_LIMIT,
            rows: sourceBudget(cases, baseline.cases)
        };
    }
    const json = path.join(options.out, "stages.json");
    const markdown = path.join(options.out, "stages.md");
    fs.writeFileSync(json, `${JSON.stringify(report, null, 4)}\n`);
    let rendered = markdownReport(report);
    if (report.sourceBudget) {
        const failed = report.sourceBudget.rows.filter((row) => !row.passed);
        rendered += `\n\n## Source-stage regression gate\n\nBase: ${report.sourceBudget.baseline}. Both revisions use this run's corpus, harness, toolchain and runtime libraries. Each document/scale must stay within ${((SOURCE_IR_LIMIT - 1) * 100).toFixed(0)}% of its baseline source_to_buffer Ir. ${report.sourceBudget.rows.length - failed.length}/${report.sourceBudget.rows.length} passed. AST improvements do not offset source regressions.\n`;
        if (failed.length)
            rendered +=
                "\n| Case | Scale | Base Ir | Current Ir | Ratio |\n| --- | ---: | ---: | ---: | ---: |\n" +
                failed
                    .map(
                        (row) =>
                            `| ${row.case} | ${row.scale} | ${row.before} | ${row.after} | ${row.ratio.toFixed(3)}x |`
                    )
                    .join("\n");
    }
    fs.writeFileSync(markdown, `${rendered}\n`);
    process.stdout.write(`${rendered}\n`);
    console.error(`wrote ${path.relative(root, json)} and ${path.relative(root, markdown)}`);
    if (report.sourceBudget?.rows.some((row) => !row.passed))
        fail("source_to_buffer instruction budget exceeded; see stages.md");
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main();
