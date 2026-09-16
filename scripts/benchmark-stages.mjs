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
import { compiledFlags as readCompiledFlags, discardTree, effectiveFlags, markTree } from "./lib/compile-identity.mjs";
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

function buildRunners(profile, cmark, cmarkBuildDir, gfm, gfmBuildDir, versions) {
    discardForeignTree(profile.binaryDir, profile, versions);
    run(
        "cmake",
        [
            "--preset",
            PROFILE_PRESET,
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            `-DMARKDOWN_CORE_CMARK_SOURCE_DIR=${path.join(cmark.checkout, "src")}`,
            `-DMARKDOWN_CORE_CMARK_BUILD_DIR=${cmarkBuildDir}`,
            `-DMARKDOWN_CORE_CMARK_GFM_SOURCE_DIR=${gfm.checkout}`,
            `-DMARKDOWN_CORE_CMARK_GFM_BUILD_DIR=${gfmBuildDir}`
        ],
        { env: buildEnvironment() }
    );
    run("cmake", ["--build", "--preset", PROFILE_PRESET, "--parallel"], { env: buildEnvironment() });
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
/**
 * A case generated from a numbered unit rather than from a sample file.
 *
 * Both halves of a LOGICAL ISOMORPH are built this way. The index keeps every
 * generated declaration distinct, which the pairing needs: a repeated literal
 * name would leave one side binding one identifier and the other binding
 * thousands, and those are not the same workload however alike they read.
 */
function generatedText(generated, target) {
    let text = "";
    let units = 0;
    while (Buffer.byteLength(text) < target) {
        text += generated.unit.replaceAll("{n}", String(units));
        units += 1;
    }
    return { text: text + (generated.tail ?? ""), length: units };
}

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
        text += counted.unit.replaceAll("{n}", String(index));
    }
    return { text: text + (counted.tail ?? ""), length: units };
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
    if (manifest.schemaVersion !== 2) fail(`unsupported corpus schema: ${manifest.schemaVersion}`);
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
    /* A paired case drags its other half in. Naming one alone would measure a
     * case whose comparison lives on a document the run never built, and a
     * counted case sized against a generated partner that was never generated
     * has no count to match at all. The pair is not optional context; it IS the
     * comparison. */
    const wanted = new Set(options.cases);
    for (const pair of [...(manifest.isomorphs ?? []), ...(manifest.logicalIsomorphs ?? [])]) {
        if (wanted.has(pair.case)) wanted.add(pair.isomorph);
        if (wanted.has(pair.isomorph)) wanted.add(pair.case);
    }
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
        const modes = [entry.samples, entry.chain, entry.counted, entry.generated].filter(Boolean).length;
        if (modes !== 1) {
            fail(`corpus case ${entry.name} must name exactly one of "samples", "chain", "generated" or "counted"`);
        }
        const unit = entry.samples ? documentsUnit(entry) : null;
        for (let scale = 1; scale <= options.scale; scale++) {
            const target = (entry.targetBytes ?? manifest.targetBytes) * scale;
            const built = entry.chain
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
                dialect: entry.dialect,
                gfm: entry.gfm === true,
                /* What the growth table is varying. `documents` cases add
                 * independent copies; a chain case grows one structure, and
                 * WHICH dimension is not the same question as the shape --
                 * chain-link-candidates grows a count of separately bounded
                 * failures, not a depth, so a table that called it "structure"
                 * alongside the nesting cases would invite exactly the reading
                 * the case was renamed to prevent. */
                growth: entry.chain
                    ? (entry.scales ?? "structure")
                    : entry.counted || entry.generated
                      ? "declarations"
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
    for (const [engine, definition] of Object.entries(ENGINES)) {
        const binary = path.join(profile.binaryDir, definition.runner);
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

function markdownReport(report) {
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
    /* Two ways a pair can be established, one decomposition either way. A
     * SUBSTITUTION isomorph is the same document under a change of marker, so
     * both halves are the same length. A LOGICAL isomorph is two spellings of
     * one declaration -- an explicit anchor here, a link reference definition
     * there -- which are not the same length, so the corpus generates them to an
     * equal count of declarations instead. Both give the same three numbers. */
    const declarations = [...(report.isomorphs ?? []), ...(report.logicalIsomorphs ?? [])];
    const paired = new Map(declarations.map((declaration) => [declaration.case, declaration]));
    const isIsomorph = new Set(declarations.map((declaration) => declaration.isomorph));
    const bySubstitution = new Set((report.isomorphs ?? []).map((declaration) => declaration.case));

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
            const twinCore = twin ? stageIr(twin.engines, "markdown-core") : null;
            const twinCmark = twin ? stageIr(twin.engines, "cmark") : null;
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
                          claim: declaration.claim,
                          /* How the pair was established, because it decides
                           * which invariant held it: equal bytes under a marker
                           * substitution, or an equal count of declarations in
                           * two spellings of different length. */
                          by: bySubstitution.has(item.case) ? "substitution" : "declaration",
                          /* Its own bytes, not this case's: a logical isomorph
                           * is a different length by construction, so dividing
                           * its cost by this document's size would be reading
                           * one document's Ir over another document's bytes. */
                          bytes: twin.bytes,
                          units: twin.units,
                          coreIr: twinCore,
                          cmarkIr: twinCmark,
                          /* What this grammar costs over a CommonMark grammar
                           * building the same tree, inside one parser. */
                          grammar: twinCore ? core / twinCore : null,
                          /* What this parser costs on the shape itself, where
                           * the reference did the same job. */
                          shape: twinCore && twinCmark ? twinCore / twinCmark : null
                      }
                    : null,
                /* The reference that did equivalent work, in order of how
                 * directly it did it: cmark-gfm where it implements the
                 * construct, the isomorph where the corpus wrote the same
                 * workload in a CommonMark grammar, and cmark on the case's own
                 * bytes when the two engines already agree on what to build.
                 *
                 * A dialect case with no isomorph publishes none of those. Its
                 * own bytes are exactly where the two engines disagree, so
                 * dividing by cmark on them is not a comparison at all; it is
                 * reported as a bound and ranked as one. */
                sameJob: gfmIr
                    ? core / gfmIr
                    : twinCmark
                      ? core / twinCmark
                      : item.dialect === "commonmark" && cmarkIr
                        ? core / cmarkIr
                        : null
            };
        })
        .sort((left, right) => (right.sameJob ?? right.cmarkRatio ?? 0) - (left.sameJob ?? left.cmarkRatio ?? 0));

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
        lines.push(
            "A ratio compares only where both parsers did the same job, so the cases are" +
                " grouped by which reference implements what they contain, and the groups are" +
                " never averaged together.",
            "",
            "cmark implements the CommonMark cases. cmark-gfm implements tables, task lists," +
                " bare autolinks and footnotes, and is measured only on the cases that hold" +
                " them. For a few dialect constructs neither one implements, an ISOMORPH" +
                " stands in: the same document written twice, once with the dialect marker" +
                " and once with a CommonMark marker of the same shape, so cmark builds the" +
                " same tree and the ratio is a comparison again. Nothing implements what is" +
                " left: cmark reads `:::note` as a paragraph, so its number there is the cost" +
                " of NOT having the feature. That bounds what a construct costs and does not" +
                " say it is slow.",
            "",
            ""
        );
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
            [
                "CommonMark",
                "cmark",
                ranked.filter((item) => item.dialect === "commonmark" && !item.gfm && !isIsomorph.has(item.case))
            ],
            ["GFM extensions", "cmark-gfm", ranked.filter((item) => item.gfm)],
            ["Dialect, via an isomorph", "cmark, on the isomorph", ranked.filter((item) => item.isomorph)],
            [
                "Dialect-only (no reference)",
                "cmark, as a bound",
                ranked.filter((item) => item.dialect !== "commonmark" && !item.gfm && !item.isomorph)
            ]
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

        const pairs = ranked.filter((item) => item.isomorph);
        if (pairs.length) {
            lines.push("### What the grammar costs", "");
            lines.push(
                "Each row is one workload written twice -- once in the dialect grammar and" +
                    " once in a CommonMark grammar of the same shape. The pairing is read off" +
                    " the GRAMMAR, never off either implementation: a pair designed by" +
                    " matching what machinery each engine happens to run would drive every" +
                    " ratio to 1.00x and measure nothing.",
                "",
                "Two productions pair when they have the same shape, and the corpus holds" +
                    " that constant in one of two ways:",
                "",
                "- **Substitution** -- the same document under a change of marker, so both" +
                    " halves are the same bytes and parse to the SAME TREE: same spans, same" +
                    " literals, same children, differing only in which grammar built each" +
                    " node. `` $x$ `` against `` `x` `` is this kind.",
                "- **Declaration** -- two spellings of one declaration, where the subsequent" +
                    " operation is what matches: an explicit anchor binds a name to the block" +
                    " it sits on, a link reference definition binds a name to a target, and" +
                    " either way the binding enters the document's table of names. The two" +
                    " spellings are different lengths, so the corpus generates them to an" +
                    " equal COUNT of declarations instead of to equal bytes.",
                "",
                "`scripts/audit-corpus-reach.mjs` checks both invariants against the parser" +
                    " rather than taking them on faith -- the tree for a substitution pair," +
                    " the declaration count for a logical one.",
                "",
                "That splits the ratio into two questions that have different answers:",
                "",
                "- **Grammar** is this parser on the dialect spelling over this parser on the" +
                    " CommonMark spelling. One parser, one tree, two grammars -- so a number" +
                    " above 1 is this grammar, and nothing else, and it names the file to open.",
                "- **Shape** is this parser over cmark on the CommonMark spelling, where both" +
                    " did the same job. It is what the parser costs on that shape before any" +
                    " dialect construct is involved, and no change to a dialect grammar will" +
                    " move it.",
                "",
                "Their product is the same-job ratio in the group table above. A pair that" +
                    " reads 1.0x on Grammar and 3x on Shape is not an extension problem at" +
                    " all, however large the bound against cmark on the dialect document" +
                    " looked.",
                "",
                "On a DECLARATION pair the two Ir/B columns are each over their own" +
                    " document's bytes, and those differ by construction -- so Grammar is not" +
                    " their quotient. It is the total over the total at an equal count of" +
                    " declarations, which is the only denominator the pair holds fixed.",
                ""
            );
            lines.push(
                "| Dialect case | Isomorph | Paired by | Dialect Ir/B | Isomorph Ir/B |" +
                    " cmark Ir/B | Grammar | Shape | Same-job |",
                "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"
            );
            for (const item of pairs) {
                const pair = item.isomorph;
                lines.push(
                    `| ${item.case} | ${pair.case} | ${pair.by} | ${(item.coreIr / item.bytes).toFixed(1)} |` +
                        ` ${pair.coreIr === null ? "-" : (pair.coreIr / pair.bytes).toFixed(1)} |` +
                        ` ${pair.cmarkIr === null ? "-" : (pair.cmarkIr / pair.bytes).toFixed(1)} |` +
                        ` ${pair.grammar === null ? "-" : `${pair.grammar.toFixed(2)}x`} |` +
                        ` ${pair.shape === null ? "-" : `${pair.shape.toFixed(2)}x`} |` +
                        ` ${item.sameJob === null ? "-" : `${item.sameJob.toFixed(2)}x`} |`
                );
            }
            lines.push("");
            lines.push(
                "What each pair claims to hold constant, from `corpus.json`:",
                "",
                ...pairs.map((item) => `- **${item.case}** -- ${item.isomorph.claim}`),
                ""
            );
            lines.push(
                "A construct with no row here has no isomorph, and the honest reason is that" +
                    " CommonMark has no production of the same shape. A grid table is not a" +
                    " pipe table (a grid cell holds a paragraph, a pipe cell holds inlines), a" +
                    " definition list is not a bullet list (the definition groups term and" +
                    " body under one node), and a comment is not strong emphasis (strong" +
                    " parses its body, a comment keeps it literal). A DERIVED anchor is the" +
                    " sharpest case: a heading whose identifier comes from its own text" +
                    " declares nothing, and CommonMark has no production that derives a" +
                    " binding, so `block-heading` stays a bound while the explicit anchor --" +
                    " which does declare -- pairs. Those stay bounds, and a bound is reported" +
                    " as a bound.",
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
            "Ranked by the same-job ratio where there is one, and by the bound otherwise.",
            ""
        );
        lines.push(
            "| Case | Reference | Ratio | Core Ir/B | Dominant self cost |",
            "| --- | --- | ---: | ---: | --- |"
        );
        for (const item of ranked.slice(0, 16)) {
            const hot = (item.engines["markdown-core"].hotPaths ?? [])
                .slice(0, 3)
                .map((entry) => `\`${entry.name}\` ${(entry.share * 100).toFixed(1)}%`)
                .join(", ");
            const ratio = item.sameJob ?? item.cmarkRatio;
            const reference = item.gfm
                ? "cmark-gfm"
                : item.isomorph
                  ? "cmark, isomorph"
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
                " what was scaled reports a growth ratio equal to the byte ratio; a stage" +
                " quadratic in it reports the square.",
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
        )
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

    const corpus = buildCorpus(options, manifest);
    const cases = [];
    for (const document of corpus.documents) {
        const engines = {};
        /* cmark is measured on every document as the CommonMark floor. cmark-gfm
         * is measured only where it implements the case's constructs, because a
         * reference that reads the document as paragraphs is not a second
         * opinion, and paying callgrind for one would buy a number nobody can
         * read. */
        const applicable = Object.keys(ENGINES).filter((engine) => engine !== "cmark-gfm" || document.gfm === true);
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
        cases.push({ ...document, file: path.relative(options.out, document.file), engines });
    }

    const report = {
        schemaVersion: 2,
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
        /* Which pairs were in force, recorded beside the counts: a ratio for a
         * dialect construct is only readable against the pairing that produced
         * it, and `scripts/audit-corpus-reach.mjs` is what holds the pairing to
         * being true. */
        isomorphs: manifest.isomorphs ?? [],
        /* Recorded beside them for the same reason, and separately because the
         * two kinds of pair are held to different invariants: a substitution
         * isomorph is the same bytes under a change of marker, a logical
         * isomorph is the same COUNT of declarations in two spellings that are
         * not the same length and were never meant to be. */
        logicalIsomorphs: manifest.logicalIsomorphs ?? [],
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
