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
    const selected = options.cases.length
        ? manifest.cases.filter((entry) => options.cases.includes(entry.name))
        : manifest.cases;

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
                gfm: entry.gfm === true,
                /* The AST fields this case's tree carries that cmark's has no
                 * counterpart for. A non-empty list means the two engines did
                 * NOT build the same tree, so the number against cmark is a
                 * bound and not a comparison, whatever the syntax was.
                 * `scripts/audit-corpus-reach.mjs` is what holds this to the
                 * trees rather than to anyone's memory. */
                unmatched: entry.unmatched ?? [],
                /* What the growth table is varying. `documents` cases add
                 * independent copies; a chain case grows one structure, and
                 * WHICH dimension is not the same question as the shape --
                 * chain-link-candidates grows a count of separately bounded
                 * failures, not a depth, so a table that called it "structure"
                 * alongside the nesting cases would invite exactly the reading
                 * the case was renamed to prevent. */
                growth: entry.chain ? (entry.scales ?? "structure") : "documents",
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

/* Everything reachable from the callee side of each excluded edge, so a
 * function doing that feature's work can be recognised wherever it sits under
 * the boundary rather than only at its rim. */
function reachedFrom(profile, specs) {
    const callees = new Map();
    for (const edge of profile.edges.values()) {
        const from = baseName(edge.caller);
        if (!callees.has(from)) callees.set(from, new Set());
        callees.get(from).add(baseName(edge.callee));
    }
    const seen = new Set();
    const pending = specs.map((spec) => spec.split("->")[1].trim());
    while (pending.length) {
        const name = pending.pop();
        if (seen.has(name)) continue;
        seen.add(name);
        for (const callee of callees.get(name) ?? []) pending.push(callee);
    }
    return seen;
}

/* Enumerating call edges to take a feature out of a ratio is only honest while
 * the enumeration is complete, and nothing about a list of edges says when it
 * has stopped being. This derives the candidates instead of trusting the list:
 * it finds every function whose behaviour TRACKS the field's presence across
 * the corpus, and requires each one to have been classified -- inside an
 * excluded subtree, or named in `shared` with the reason the reference does it
 * too.
 *
 * Tracking the field is NOT the same as being caused by it, and this does not
 * claim otherwise. The syntax that carries a field and the field itself occur
 * on exactly the same documents, so shared parsing lands in the candidate set
 * too: `continue_heading`, `markdown_core_heading_begin_inlines` and
 * `markdown_core_heading_claim_tail` are all found here, and all three are work
 * cmark does as well. That is why the answer to a candidate is a CLASSIFICATION
 * and not an exclusion. The set is deliberately wide, because a narrow one
 * decided by reachability would stop asking about exactly the functions worth
 * asking about: `continue_heading` is reached from block parsing, not from
 * anchor code, and so is a heading-only function nobody has noticed yet.
 *
 * Two consequences worth stating rather than discovering:
 *
 * - The candidate set is a fact about THIS corpus. Dropping `block-lheading`
 *   would leave ATX as the only heading spelling present and the set would grow
 *   `open_atx`; dropping `block-fences` would grow `markdown_core_attributes_tail`.
 *   Both are shared work, and the correct response to either is a `shared` entry
 *   naming what cmark does -- a claim a reader can check -- never an allowlist
 *   entry written to get the run moving.
 * - It cannot catch a WRONG classification. Calling anchor work "shared" hides
 *   it just as well as never finding it.
 *
 * What it does catch is the failure that actually happened here twice: work only
 * the field's presence causes that nobody had thought about. A new candidate
 * stops the run until someone says which side it is on. */
function requireCompleteExclusions(cases, ran, excludedReach, excludedLeaks, unmatchedFields, filtered) {
    /* The derivation is a statement about the whole corpus: "runs wherever the
     * field is and nowhere it is not" only means "caused by the field" when
     * "nowhere it is not" covers every compared document. A `--case` run does
     * not, and the inference degrades immediately -- two cases is enough to
     * indict `open_atx`, which opens an ATX heading and is exactly the shared
     * work this is meant to leave alone. So a filtered run does not get to
     * decide the question either way.
     *
     * Returning here is not the end of it. A run that cannot judge the
     * subtraction must not publish a subtracted ratio as a comparison either,
     * so `report.completenessJudged` carries the same fact to the renderer,
     * which withholds those cases from every group and prints their arithmetic
     * under a heading that says it is unchecked. */
    if (filtered) {
        console.error(
            "note: a --case run cannot judge whether an exclusion is complete, so same-job ratios " +
                "for cases holding an unmatched field are withheld from this report"
        );
        return;
    }
    for (const [field, declaration] of Object.entries(unmatchedFields)) {
        const carries = [];
        const plain = [];
        for (const item of cases) {
            if (item.scale !== 1 || !ran.has(item.case)) continue;
            /* Only where the field could change a number: a bound is not a
             * comparison, so nothing is carved out of it and nothing about it
             * needs classifying. */
            if (item.dialect !== "commonmark" && !item.gfm) continue;
            ((item.engines["markdown-core"]?.witnesses?.[field] ?? 0) > 0 ? carries : plain).push(item.case);
        }
        if (!carries.length || !plain.length) continue;
        const shared = new Set(Object.keys(declaration.shared ?? {}));
        /* "Reachable from an excluded edge" is not "its cost was removed". The
         * same helper can run under an excluded edge and again under a caller
         * the exclusion never named, and only the first is subtracted -- so a
         * candidate still taking cost from an unnamed caller inside a measured
         * stage has to be classified on its own, not waved through because some
         * other call to it was excluded. */
        const covered = (fn) =>
            shared.has(fn) ||
            carries.every((name) => excludedReach.get(name).has(fn) && !(excludedLeaks.get(name)?.[fn] > 0));
        /* Functions only the field's presence RUNS. */
        let only = new Set(Object.keys(ran.get(carries[0])));
        for (const name of carries.slice(1)) only = new Set([...only].filter((fn) => fn in ran.get(name)));
        for (const name of plain) for (const fn of Object.keys(ran.get(name))) only.delete(fn);
        /* And the ones the field's presence only makes EXPENSIVE. These are the
         * blind spot in the test above, and the one that actually bit: a
         * function like `markdown_core_block_dispose_headings` runs on every
         * document, costs 48 Ir over an empty collection and 25,442 over a full
         * one, so "does it run here" cannot see it. Comparing against the
         * HIGHEST cost the function reaches on any compared case without the
         * field keeps this free of a tuned threshold. It is the same kind of
         * evidence as the test above and carries the same caveat -- a cost
         * ordering is not causation either -- so it produces candidates to
         * classify, not a verdict. On this corpus it is what finds the four
         * functions that run unconditionally over the field's own collection:
         * the two anchor passes, the loop over them, and the disposal. */
        for (const fn of new Set(carries.flatMap((name) => Object.keys(ran.get(name))))) {
            const floor = Math.max(0, ...plain.map((name) => ran.get(name)[fn] ?? 0));
            if (carries.every((name) => (ran.get(name)[fn] ?? 0) > floor)) only.add(fn);
        }
        const unclassified = [...only].filter((fn) => !covered(fn));
        if (unclassified.length) {
            fail(
                `these functions track the presence of "${field}" across the corpus -- each one either runs ` +
                    `only on the cases carrying it, or costs more on every one of them than it ever does ` +
                    `without it -- and none has been classified: ${unclassified.sort().join(", ")}. ` +
                    `Classify each one. If the field's machinery is what does the work, add the call edge ` +
                    `to unmatchedFields.${field}.excludes with the stage it sits in. If the reference does ` +
                    `the same work, add it to unmatchedFields.${field}.shared with a reason NAMING what the ` +
                    `reference does, the way continue_heading names S_process_line -- tracking the field is ` +
                    `not the same as being caused by it, and shared parsing reaches this list too. An entry ` +
                    `written only to get this run moving is the one failure nothing downstream can catch.`
            );
        }
        for (const fn of shared) {
            /* A `shared` entry for something no case runs is a note about code
             * that has moved, kept true by nobody. */
            if (!carries.some((name) => fn in ran.get(name))) {
                fail(`unmatchedFields.${field}.shared names ${fn}, which no case carrying the field runs`);
            }
        }
    }
}

function measure(profile, engine, document, out, unmatchedFields) {
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
    /* Every node either measured stage entered. A cost reached only outside
     * both stages is not in any ratio's numerator, so it cannot be work an
     * exclusion failed to remove. */
    const inStage = new Set();
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
        for (const node of scoped) inStage.add(node);
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

    /* The work the reference has no counterpart for, read off the call edges
     * the corpus declares for each dialect-only field. Measured on EVERY case
     * and every engine, not only the ones that declare the field: a case that
     * reaches this work without declaring it is a case whose ratio is about to
     * silently include it, and the only way to say so is to have looked. */
    const unmatched = [];
    /* Whether the case did this work AT ALL, counted separately from what the
     * work cost. The two are not the same question: `finish_document` calls
     * `markdown_core_block_finalize_heading_anchors` unconditionally, and the
     * callee still runs its prologue over an empty collection, so the edge
     * costs 53 Ir on a document with no heading in it. Reading "did this case
     * carry dialect output?" off a NON-ZERO COST would therefore answer yes for
     * all 62 cases. The witness is a call count into work that only a real
     * occurrence reaches, so it is 0 exactly when the tree carries nothing. */
    const witnesses = {};
    for (const [field, declaration] of Object.entries(unmatchedFields ?? {})) {
        if (typeof declaration.witness !== "string" || !declaration.witness.includes("->")) {
            fail(
                `corpus.json: unmatchedFields.${field} has no witness edge, so nothing can say whether a case ` +
                    `carries it -- the corpus audit checks the same thing and one of the two ran without the other`
            );
        }
        const [caller, callee] = declaration.witness.split("->").map((half) => half.trim());
        witnesses[field] = edgesBetween(profileByName, caller, callee).reduce((total, edge) => total + edge.calls, 0);
        for (const [spec, stage] of Object.entries(declaration.excludes ?? {})) {
            const [from, to] = spec.split("->").map((half) => half.trim());
            const edges = edgesBetween(profileByName, from, to);
            const ir = edges.reduce((total, edge) => total + (costRecord(profileByName, edge.cost).Ir ?? 0), 0);
            const calls = edges.reduce((total, edge) => total + edge.calls, 0);
            if (ir) unmatched.push({ field, stage, edge: spec, calls, ir });
        }
    }
    for (const entry of unmatched) {
        /* A part cannot exceed its whole, here for the same reason the
         * breakdown is checked above: an exclusion larger than the stage it
         * claims to sit in is an exclusion attributed to the wrong stage, and
         * subtracting it would manufacture a ratio out of arithmetic. */
        if (entry.ir > (stages[entry.stage]?.cost.Ir ?? 0)) {
            fail(
                `${engine}: ${document.case} excludes ${entry.edge} at ${entry.ir} Ir from a ${entry.stage} ` +
                    `of ${stages[entry.stage]?.cost.Ir ?? 0} Ir, so the exclusion is in the wrong stage`
            );
        }
    }

    const excludedReach = reachedFrom(
        profileByName,
        unmatched.map((entry) => entry.edge)
    );
    /* For each function the exclusion reaches, what it still costs through
     * callers the exclusion never named -- counted only where a measured stage
     * entered the caller, because cost outside both stages is in no numerator. */
    const declaredEdges = new Set(unmatched.map((entry) => entry.edge.replace(/\s*->\s*/u, "->")));
    const excludedLeaks = new Map();
    for (const edge of profileByName.edges.values()) {
        const callee = baseName(edge.callee);
        if (!excludedReach.has(callee)) continue;
        const caller = baseName(edge.caller);
        if (excludedReach.has(caller) || declaredEdges.has(`${caller}->${callee}`)) continue;
        if (!inStage.has(edge.caller)) continue;
        const ir = costRecord(profileByName, edge.cost).Ir ?? 0;
        if (ir) excludedLeaks.set(callee, (excludedLeaks.get(callee) ?? 0) + ir);
    }

    return {
        rootChildren: Number(receipt[2]),
        receiptBytes: Number(receipt[1]),
        parsePathIr,
        outsideStagesIr: STAGES.reduce((total, stage) => total - stages[stage].cost.Ir, parsePathIr),
        stages,
        unmatched,
        witnesses,
        /* Every function this document ran, and everything the excluded edges
         * reach. The completeness law below needs both: enumerating call edges
         * to carve a feature out of a ratio is only honest if something can say
         * when the enumeration has stopped being complete. */
        ran: [...profileByName.self.entries()].reduce((total, [name, cost]) => {
            const fn = baseName(name);
            total[fn] = (total[fn] ?? 0) + (costRecord(profileByName, cost).Ir ?? 0);
            return total;
        }, {}),
        excludedReach: [...excludedReach],
        /* The hole a base-name reachability test cannot see. `reachedFrom`
         * answers "is this function called somewhere under an excluded edge",
         * and a function can be BOTH: `markdown_core_resource_new` runs under
         * the excluded `markdown_core_prepare_heading` edge and again under
         * `markdown_core_link_commit`, which builds an ordinary CommonMark link
         * that cmark builds too. Only the first is subtracted, and that is
         * correct -- but it means "reachable from an exclusion" is not the same
         * as "its cost was removed". This records what each such function still
         * costs through callers the exclusion never named, counting only calls
         * inside a measured stage, so the completeness check can refuse to
         * treat those as already accounted for. */
        excludedLeaks: [...excludedLeaks].reduce((total, [name, ir]) => {
            total[name] = ir;
            return total;
        }, {}),
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
    /* What a case declared, and what its dump actually shows. A case that
     * reaches this work without declaring it would have the work silently
     * folded into its ratio; a case that declares it and does not reach it is
     * claiming an exemption it does not use. Both are refused here rather than
     * left for a reader to spot, and it is this check that stops the exclusion
     * being a way to make any number smaller. */
    const unmatchedOf = (item) => {
        /* Only a case whose number is a DIVISION against a reference is held to
         * this, and only such a case has anything subtracted. A case already
         * reported as a bound has no comparison to protect, and `block-metadata`
         * and `mixed-extended` do contain headings -- holding them to a
         * declaration they are not allowed to make (see `declaredUnmatched` in
         * the corpus audit) would abort every run. Keyed on the corpus's syntax
         * label rather than on the group the case lands in, because the
         * declaration is one of the things that decides the group. */
        if (item.dialect !== "commonmark" && !item.gfm) return 0;
        const declared = new Set(item.unmatched ?? []);
        const engine = item.engines["markdown-core"] ?? {};
        const seen = engine.unmatched ?? [];
        let total = 0;
        for (const [field, calls] of Object.entries(engine.witnesses ?? {})) {
            /* The witness, not the cost: the outer call runs on every document
             * and costs a few instructions over an empty collection, so a
             * non-zero cost would report every case as carrying the field. */
            const carries = calls > 0;
            if (carries && !declared.has(field)) {
                fail(
                    `${item.case} does ${calls} of the work behind "${field}", which cmark has no counterpart ` +
                        `for, and does not declare it -- so its ratio would quietly include it`
                );
            }
            if (!carries && declared.has(field)) {
                fail(`${item.case} declares unmatched "${field}" and never does the work behind it`);
            }
            if (carries) {
                total += seen.filter((entry) => entry.field === field).reduce((sum, entry) => sum + entry.ir, 0);
            }
        }
        return total;
    };
    /* A subtraction whose completeness nothing checked cannot produce a
     * same-job number: the whole point of the check is that a list of edges
     * says nothing about when it stopped covering the feature, so an unchecked
     * list leaves work of unknown size in the numerator. */
    const judged = report.completenessJudged !== false;
    /* Whether the case builds a field the reference has no counterpart for, read
     * from the witness and the declaration rather than from what the exclusion
     * happened to subtract. A stale edge list -- every edge renamed by a
     * refactor, say -- subtracts nothing while the tree still carries the
     * field, and keying the withholding on a non-zero subtraction would publish
     * the RAW anchor-inclusive ratio as a comparison in exactly that case. */
    const holdsUnmatchedField = (item) => {
        if (item.dialect !== "commonmark" && !item.gfm) return false;
        if ((item.unmatched ?? []).length) return true;
        return Object.values(item.engines["markdown-core"]?.witnesses ?? {}).some((calls) => calls > 0);
    };
    const paired = new Map((report.isomorphs ?? []).map((declaration) => [declaration.case, declaration]));
    const isIsomorph = new Set((report.isomorphs ?? []).map((declaration) => declaration.isomorph));
    const atScaleOne = new Map(report.cases.filter((item) => item.scale === 1).map((item) => [item.case, item]));
    const ranked = report.cases
        .filter((item) => item.scale === 1 && item.engines["markdown-core"])
        .map((item) => {
            const core = stageIr(item.engines, "markdown-core");
            const cmarkIr = stageIr(item.engines, "cmark");
            const gfmIr = stageIr(item.engines, "cmark-gfm");
            const excluded = unmatchedOf(item);
            /* A dialect construct cmark does not implement still gets a
             * same-job ratio, through the document that IS the same tree: what
             * this parser spent on the dialect spelling, over what cmark spent
             * building the same tree from the CommonMark spelling. */
            const declaration = paired.get(item.case);
            const twin = declaration ? atScaleOne.get(declaration.isomorph) : null;
            const twinCore = twin ? stageIr(twin.engines, "markdown-core") : null;
            const twinCmark = twin ? stageIr(twin.engines, "cmark") : null;
            if (twin && (twin.bytes !== item.bytes || twin.units !== item.units)) {
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
                /* What this case builds that the reference does not, taken off
                 * THIS side so the two numbers are over the same job again.
                 * Heading anchors are the case: the dialect derives one for
                 * every heading, so the trees differ -- but the difference is a
                 * named set of call edges with no counterpart on cmark's side,
                 * and once it is subtracted what remains is work both engines
                 * did. The heading's own inline parse stays in, because cmark
                 * runs it too. */
                unmatchedIr: excluded,
                /* Holds a field with no counterpart, on a run that could not
                 * check the subtraction covers it. Kept out of `sameJob` below
                 * so no table prints it as a comparison, and surfaced so the
                 * section on the exclusion can show the arithmetic anyway --
                 * the numbers are still what the focused experiment is for. */
                provisional: !judged && holdsUnmatchedField(item),
                /* The comparison that means something: the closest reference
                 * that implements what the document contains, or the reference
                 * on the document that is the same tree. */
                sameJob:
                    !judged && holdsUnmatchedField(item)
                        ? null
                        : gfmIr
                          ? (core - excluded) / gfmIr
                          : twinCmark
                            ? (core - excluded) / twinCmark
                            : item.dialect === "commonmark" && cmarkIr
                              ? (core - excluded) / cmarkIr
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
            "Syntax alone does not make a case comparable. This dialect derives an anchor" +
                " for every heading, so a document holding one is not the tree cmark builds," +
                " however ordinary the source looks. Those cases stay in the CommonMark group" +
                " and the work that produces the anchor comes off this side first, by named" +
                " call edge; what is left is what both engines did. The section after this one" +
                " lists every edge taken out, every function deliberately left in, and what" +
                " each case's number was before and after.",
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
        for (const [label, reference, all] of groups) {
            if (!all.length) continue;
            /* A case whose subtraction nothing checked has no number to put in
             * a median. Its RAW ratio is not a substitute: it is the number
             * this whole section exists to stop being read as a comparison. */
            const group = all.filter((item) => !item.provisional);
            const values = group.map((item) => item.sameJob ?? item.cmarkRatio).filter((value) => value !== null);
            if (!values.length) continue;
            const worst = group[0];
            const withheld = all.length - group.length;
            lines.push(
                `| ${label} | \`${reference}\` | ${group.length}${withheld ? ` (+${withheld} withheld)` : ""} |` +
                    ` ${median(values).toFixed(2)}x |` +
                    ` ${(worst.sameJob ?? worst.cmarkRatio).toFixed(2)}x \`${worst.case}\` |`
            );
        }
        lines.push("");
        if (ranked.some((item) => item.provisional)) {
            lines.push(
                "**This run could not judge whether the subtraction below is complete**, because that" +
                    " derivation needs every compared document and this run measured a subset. The" +
                    " cases holding a field cmark has no counterpart for are therefore withheld from" +
                    " the groups above rather than published: subtracted, their number would rest on" +
                    " a list nothing checked; unsubtracted, it would be the bound this section exists" +
                    " to stop being read as a comparison. Their arithmetic is still shown below, for" +
                    " the focused experiment it is for. Run the whole corpus for a same-job number.",
                ""
            );
        }

        const held = ranked.filter((item) => (item.unmatched ?? []).length);
        if (held.length) {
            lines.push("### What was taken off this side to keep it a comparison", "");
            lines.push(
                "These documents are CommonMark source, and this parser does not build" +
                    " CommonMark's tree from them: the dump carries a field cmark's node does" +
                    " not have. Dividing the two totals would price a feature cmark lacks as" +
                    " though it were this parser being slow, so the work that produces the" +
                    " field is subtracted from THIS side and the remainder is what both" +
                    " engines did.",
                "",
                "The subtraction is by call edge, not by function, and the edges are named in" +
                    " `corpus.json` with what each one does. Work the reference also performs" +
                    " stays in -- a heading's own inline parse is not excluded, because" +
                    " `cmark_parse_inlines` runs it too.",
                "",
                "Four checks keep the exclusion from being a way to make any number smaller." +
                    " OCCURRENCE: this driver fails when a case does the work behind a field" +
                    " without declaring it, or declares it and never does the work, read off a" +
                    " witness edge's CALL COUNT rather than a cost -- the outer call runs on" +
                    " every document and costs a few instructions over an empty collection." +
                    " COMPLETENESS: a list of edges cannot say when it has stopped covering the" +
                    " feature, so the candidates are derived instead -- every function whose" +
                    " behaviour tracks the field across the corpus, either by running only on the" +
                    " cases carrying it or by costing more on every one of them than it ever does" +
                    " without it -- and each must be excluded or listed below as shared. That" +
                    " derivation needs every compared document, so a `--case` run cannot make it," +
                    " and such a run withholds the same-job ratio rather than publishing one" +
                    " nothing checked." +
                    " PLACEMENT: an exclusion larger than the stage it claims to sit in" +
                    " is refused. THE TREE: `scripts/audit-corpus-reach.mjs` reads the field off" +
                    " the case's own AST dump and fails when the dump and the declaration" +
                    " disagree in either direction.",
                "",
                "Two things that check deliberately does NOT claim. Tracking the field is not the" +
                    " same as being caused by it: the syntax carrying a field occurs on exactly the" +
                    " documents the field does, so shared parsing lands in the candidate set too --" +
                    " `continue_heading` and `markdown_core_heading_claim_tail` are both found" +
                    " there, and both are work cmark performs. That is why a candidate is answered" +
                    " with a classification rather than an exclusion, and why the list below is" +
                    " printed in full. And the candidate set is a fact about THIS corpus, not a" +
                    " theorem: dropping the setext case would leave ATX as the only heading" +
                    " spelling and the set would grow `open_atx`. The correct answer to that is a" +
                    " shared entry naming what cmark does, which a reader can check.",
                "",
                "What none of the four catches is a WRONG classification: calling this parser's own" +
                    " work shared hides it just as well as excluding cmark's would. That is a" +
                    " judgement, and it is written out below so it can be argued with.",
                ""
            );
            lines.push(
                `| Case | Field | Excluded Ir | Against cmark, raw | ${
                    report.completenessJudged === false ? "After subtracting (UNCHECKED)" : "Same job"
                } |`,
                "| --- | --- | ---: | ---: | ---: |"
            );
            for (const item of held) {
                /* On a run that could not judge completeness `sameJob` is
                 * withheld, so the arithmetic is recomputed here to be shown
                 * under a heading that says what it is. Printing nothing would
                 * hide the one thing the focused run was for; printing it as
                 * "Same job" is the claim this section exists to stop. */
                const after =
                    item.sameJob ??
                    (item.provisional && item.cmarkRatio !== null
                        ? (item.coreIr - (item.unmatchedIr ?? 0)) / (item.coreIr / item.cmarkRatio)
                        : null);
                lines.push(
                    `| ${item.case} | ${item.unmatched.map((field) => `\`${field}\``).join(", ")} |` +
                        ` ${(item.unmatchedIr ?? 0).toLocaleString("en-US")} |` +
                        ` ${item.cmarkRatio === null ? "-" : `${item.cmarkRatio.toFixed(2)}x`} |` +
                        ` ${after === null ? "-" : `${item.provisional ? "" : "**"}${after.toFixed(2)}x${item.provisional ? "" : "**"}`} |`
                );
            }
            lines.push("");
            for (const [field, spec] of Object.entries(report.unmatchedFields ?? {})) {
                lines.push(`- **\`${field}\`** -- ${spec.reason}`, "", `  Taken off this side:`);
                for (const [edge, stage] of Object.entries(spec.excludes ?? {})) {
                    lines.push(`  - \`${edge}\` (${stage})`);
                }
                if (Object.keys(spec.shared ?? {}).length) {
                    lines.push("", `  Left in, because the reference does it too:`);
                    for (const [fn, reason] of Object.entries(spec.shared)) {
                        lines.push(`  - \`${fn}\` -- ${reason}`);
                    }
                }
            }
            lines.push("");
        }

        const pairs = ranked.filter((item) => item.isomorph);
        if (pairs.length) {
            lines.push("### What the marker costs", "");
            lines.push(
                "Each row is one document written twice -- once with the dialect marker and" +
                    " once with a CommonMark marker of the same shape, the same bytes under a" +
                    " single-character substitution. Both spellings parse to the SAME TREE:" +
                    " same spans, same literals, same children, differing only in which" +
                    " grammar built each node, which" +
                    " `scripts/audit-corpus-reach.mjs` checks against the parser rather than" +
                    " taking on faith.",
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
                ""
            );
            lines.push(
                "| Dialect case | Isomorph | Dialect Ir/B | Isomorph Ir/B | cmark Ir/B | Grammar | Shape | Same-job |",
                "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"
            );
            for (const item of pairs) {
                const pair = item.isomorph;
                lines.push(
                    `| ${item.case} | ${pair.case} | ${(item.coreIr / item.bytes).toFixed(1)} |` +
                        ` ${pair.coreIr === null ? "-" : (pair.coreIr / item.bytes).toFixed(1)} |` +
                        ` ${pair.cmarkIr === null ? "-" : (pair.cmarkIr / item.bytes).toFixed(1)} |` +
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
                    " none of the candidates survived the check. A grid table is not a pipe" +
                    " table (a grid cell holds a paragraph, a pipe cell holds inlines), a" +
                    " definition list is not a bullet list (the definition groups term and" +
                    " body under one node), and a comment is not strong emphasis (strong" +
                    " parses its body, a comment keeps it literal). Those stay bounds, and a" +
                    " bound is reported as a bound.",
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
            const reference = item.provisional
                ? "raw, not checked"
                : item.gfm
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
    /* Kept beside the cases rather than in them: these are inputs to the
     * completeness law below, not findings anyone reads, and stages.json is a
     * report. */
    const ran = new Map();
    const excludedReach = new Map();
    const excludedLeaks = new Map();
    for (const document of corpus.documents) {
        const engines = {};
        /* cmark is measured on every document as the CommonMark floor. cmark-gfm
         * is measured only where it implements the case's constructs, because a
         * reference that reads the document as paragraphs is not a second
         * opinion, and paying callgrind for one would buy a number nobody can
         * read. */
        const applicable = Object.keys(ENGINES).filter((engine) => engine !== "cmark-gfm" || document.gfm === true);
        for (const engine of applicable) {
            const measured = measure(profile, engine, document, options.out, manifest.unmatchedFields ?? {});
            if (measured.receiptBytes !== document.bytes) {
                fail(`${engine}: ${document.case} saw ${measured.receiptBytes} bytes, expected ${document.bytes}`);
            }
            if (engine === "markdown-core" && document.scale === 1) {
                ran.set(document.case, measured.ran);
                excludedReach.set(document.case, new Set(measured.excludedReach));
                excludedLeaks.set(document.case, measured.excludedLeaks);
            }
            engines[engine] = {
                parsePathIr: measured.parsePathIr,
                outsideStagesIr: measured.outsideStagesIr,
                rootChildren: measured.rootChildren,
                hotPaths: measured.hotPaths,
                unmatched: measured.unmatched,
                witnesses: measured.witnesses,
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
    requireCompleteExclusions(
        cases,
        ran,
        excludedReach,
        excludedLeaks,
        manifest.unmatchedFields ?? {},
        options.cases.length > 0
    );

    const report = {
        schemaVersion: 2,
        /* Whether anything established that the exclusion below is COMPLETE.
         * The derivation needs every compared document, so a `--case` run
         * cannot judge it, and a subtracted ratio nothing has checked is not a
         * same-job ratio -- it is this parser's own arithmetic. Recorded here
         * rather than inferred by the renderer so a stored report says which
         * kind of run produced it. */
        completenessJudged: options.cases.length === 0,
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
        /* Why the cases below hold out of the CommonMark group. Recorded beside
         * the counts for the same reason the pairs are: the group a case is in
         * is the whole meaning of its number. */
        unmatchedFields: manifest.unmatchedFields ?? {},
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
