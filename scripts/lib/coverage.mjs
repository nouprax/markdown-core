/**
 * Coverage normalization and policy enforcement.
 *
 * One semantic operation, one model: every execution platform emits its
 * toolchain-native report, this module normalizes that report into per-file
 * `(lines, functions, branches)` counters keyed by a repository-relative POSIX
 * path, and one enforcer applies one policy to the result. Platform producers
 * stay thin — they build, run, and hand the raw report over; none of them
 * carries a threshold, an exemption, or a second notion of what "covered"
 * means.
 */

import fs from "node:fs";
import path from "node:path";

export const METRICS = ["lines", "functions", "branches"];

function counter() {
    return { covered: 0, total: 0 };
}

function emptyFile(relativePath) {
    const file = { path: relativePath };
    for (const metric of METRICS) file[metric] = counter();
    return file;
}

/**
 * `root` is the repository root every reported path is expressed against;
 * `base` is the directory the toolchain's own report is relative to, which is
 * the producer's working directory and not always the repository root.
 */
function toRepositoryRelative({ root, base }, filename) {
    const absolute = path.isAbsolute(filename) ? filename : path.resolve(base ?? root, filename);
    return path.relative(root, absolute).split(path.sep).join("/");
}

function merge(into, from) {
    for (const metric of METRICS) {
        into[metric].covered += from[metric].covered;
        into[metric].total += from[metric].total;
    }
}

function collect(entries) {
    const byPath = new Map();
    for (const entry of entries) {
        const existing = byPath.get(entry.path);
        if (existing) merge(existing, entry);
        else byPath.set(entry.path, entry);
    }
    return [...byPath.values()].sort((left, right) => (left.path < right.path ? -1 : left.path > right.path ? 1 : 0));
}

// A branch record is
// [lineStart, columnStart, lineEnd, columnEnd, trueCount, falseCount, fileId,
//  expandedFileId, kind], and `fileId` indexes the owning function's
// `filenames`. Both arms count, so one record contributes two branches.
const BRANCH_TRUE_COUNT = 4;
const BRANCH_FALSE_COUNT = 5;
const BRANCH_FILE_ID = 6;

/**
 * `llvm-cov export` JSON, produced by the C and Swift producers. Region
 * coverage is deliberately dropped: it is a clang-only metric, and keeping it
 * would give two of the four platforms a wider contract than the other two can
 * express.
 *
 * Line and function counters come from each file's own summary, which is
 * already attributed correctly. Branch counters are not: llvm-cov charges
 * every branch of a function to the file the function *starts* in, so code the
 * preprocessor splices into a function body — a generated table included
 * inside it, an inline helper from a header — lands on the host file instead of
 * its own. The effect is not marginal here: it charges `core/utf8.c`, a
 * 301-line source, with the 2,804 branches of the 4,327-line generated
 * case-fold table it includes, which is 1,361 uncovered branches recorded
 * against the wrong file and unreachable from any test of that file.
 *
 * So branches are recomputed from the function-level records, each charged to
 * the file its own `fileId` names. That is why the producers must export the
 * full report: `-summary-only` omits `functions` and leaves no way to recover
 * the attribution.
 */
export function fromLlvmCovExport(text, context) {
    const report = JSON.parse(text);
    const entries = [];
    const branches = new Map();

    for (const dataset of report.data ?? []) {
        if (!dataset.functions) {
            throw new Error(
                "llvm-cov report carries no `functions` records: it was exported with -summary-only, " +
                    "which attributes every branch to the file its function starts in. Export the full report."
            );
        }
        for (const file of dataset.files ?? []) {
            const entry = emptyFile(toRepositoryRelative(context, file.filename));
            for (const metric of ["lines", "functions"]) {
                const summary = file.summary?.[metric];
                if (!summary) continue;
                entry[metric] = { covered: summary.covered ?? 0, total: summary.count ?? 0 };
            }
            entries.push(entry);
        }
        for (const fn of dataset.functions) {
            for (const branch of fn.branches ?? []) {
                const filename = fn.filenames?.[branch[BRANCH_FILE_ID]] ?? fn.filenames?.[0];
                if (filename === undefined) continue;
                const key = toRepositoryRelative(context, filename);
                const tally = branches.get(key) ?? counter();
                tally.total += 2;
                if (branch[BRANCH_TRUE_COUNT] > 0) tally.covered += 1;
                if (branch[BRANCH_FALSE_COUNT] > 0) tally.covered += 1;
                branches.set(key, tally);
            }
        }
    }

    // Branch tallies are folded in after every dataset is read, and each entry
    // takes its own copy: `collect` merges same-path entries by adding their
    // counters together, so two entries sharing one tally object would count
    // the same branches twice.
    //
    // A file can also own branches without owning any function that starts in
    // it — a header of inline helpers, or a table included into a function body
    // — and it must still reach the gate as a measured file.
    const seen = new Set(entries.map((entry) => entry.path));
    for (const key of branches.keys()) {
        if (!seen.has(key)) entries.push(emptyFile(key));
    }
    const claimed = new Set();
    for (const entry of entries) {
        const tally = branches.get(entry.path);
        if (!tally || claimed.has(entry.path)) continue;
        claimed.add(entry.path);
        entry.branches = { covered: tally.covered, total: tally.total };
    }
    return collect(entries);
}

/**
 * LCOV tracefile, produced by the ES producer through Node's built-in test
 * runner. Only the summary records are read (`LF`/`LH`, `FNF`/`FNH`,
 * `BRF`/`BRH`); per-line records are the same information at a granularity no
 * policy rule consumes.
 */
export function fromLcov(text, context) {
    const entries = [];
    let current = null;
    for (const rawLine of text.split("\n")) {
        const line = rawLine.trim();
        if (line.startsWith("SF:")) {
            current = emptyFile(toRepositoryRelative(context, line.slice(3)));
            continue;
        }
        if (!current) continue;
        if (line === "end_of_record") {
            entries.push(current);
            current = null;
            continue;
        }
        const separator = line.indexOf(":");
        if (separator < 0) continue;
        const tag = line.slice(0, separator);
        const value = Number.parseInt(line.slice(separator + 1), 10);
        if (!Number.isFinite(value)) continue;
        if (tag === "LF") current.lines.total = value;
        else if (tag === "LH") current.lines.covered = value;
        else if (tag === "FNF") current.functions.total = value;
        else if (tag === "FNH") current.functions.covered = value;
        else if (tag === "BRF") current.branches.total = value;
        else if (tag === "BRH") current.branches.covered = value;
    }
    if (current) entries.push(current);
    return collect(entries);
}

const JACOCO_COUNTER = { LINE: "lines", METHOD: "functions", BRANCH: "branches" };

export const UNRESOLVED_PREFIX = "<unresolved>/";

function indexSourceFiles(root, sourceRoots) {
    const byBaseName = new Map();
    const walk = (directory) => {
        for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
            const child = path.join(directory, entry.name);
            if (entry.isDirectory()) walk(child);
            else if (!byBaseName.has(entry.name)) byBaseName.set(entry.name, [child]);
            else byBaseName.get(entry.name).push(child);
        }
    };
    for (const sourceRoot of sourceRoots) {
        const absolute = path.resolve(root, sourceRoot);
        if (fs.existsSync(absolute)) walk(absolute);
    }
    return { root, byBaseName };
}

function resolveSourceFile(index, packagePath, baseName) {
    const candidates = index.byBaseName.get(baseName);
    if (!candidates || candidates.length === 0) return `${UNRESOLVED_PREFIX}${packagePath}/${baseName}`;
    const relative = candidates.map((candidate) => path.relative(index.root, candidate).split(path.sep).join("/"));
    if (relative.length === 1) return relative[0];
    // Several roots or several directories hold this base name; the declared
    // package breaks the tie when it happens to match a directory.
    const preferred = relative.find((candidate) => candidate.includes(`/${packagePath}/`));
    return preferred ?? relative.slice().sort()[0];
}

/**
 * JaCoCo-schema XML, produced by the Kotlin producer through Kover.
 *
 * The schema is machine-generated with a fixed element shape, so it is read
 * with a scanner over its `<package>`/`<sourcefile>`/`<counter>` elements
 * rather than a general XML parser: the repository ships no XML dependency,
 * and a parser powerful enough for arbitrary XML would be a larger surface
 * than the one document this ever reads.
 *
 * JaCoCo names a source file by declared package plus base name, never by
 * repository path. That is not a path: Kotlin lets a file's package differ
 * from its directory, and this repository's bindings use that freely — every
 * common source declares `com.nouprax.markdown.core` while living under
 * `model/`, `walker/`, or `wire/`. Joining package to name would
 * therefore resolve almost nothing, and a resolver that dropped what it could
 * not place would report a handful of files as the whole binding.
 *
 * So the roots are indexed by base name and the package path is used only to
 * disambiguate. A name that still cannot be placed is returned under an
 * `<unresolved>` path, which the gate refuses outright.
 */
export function fromJacocoXml(text, context, sourceRoots) {
    const index = indexSourceFiles(context.root, sourceRoots);
    const entries = [];
    let currentPackage = "";
    const pattern =
        /<package\s+name="([^"]*)"|<sourcefile\s+name="([^"]*)"|<counter\s+type="([^"]*)"\s+missed="(\d+)"\s+covered="(\d+)"\s*\/>|<\/sourcefile>/g;
    let current = null;
    for (let match = pattern.exec(text); match !== null; match = pattern.exec(text)) {
        if (match[1] !== undefined) {
            currentPackage = match[1];
            continue;
        }
        if (match[2] !== undefined) {
            current = emptyFile(resolveSourceFile(index, currentPackage, match[2]));
            continue;
        }
        if (match[3] !== undefined) {
            // Counters also appear at class, package, and report level; only the
            // ones inside a sourcefile element are per-file.
            if (!current) continue;
            const metric = JACOCO_COUNTER[match[3]];
            if (!metric) continue;
            const missed = Number.parseInt(match[4], 10);
            const covered = Number.parseInt(match[5], 10);
            current[metric] = { covered, total: missed + covered };
            continue;
        }
        if (current) {
            entries.push(current);
            current = null;
        }
    }
    return collect(entries);
}

/**
 * Applies a platform's declared path rewrites.
 *
 * A toolchain that measures a build output rather than the reviewed source —
 * the ES binding runs its suites against the emitted JavaScript — would
 * otherwise name files the repository does not review. The rewrite is declared
 * in the policy, with a reason, so the mapping is reviewable data rather than
 * a rule hidden in a producer. Measuring the emitted module is the stricter
 * reading in any case: TypeScript's type-only constructs emit nothing, so full
 * coverage of the output is full coverage of every executable construct in the
 * input.
 */
export function applyPathRewrites(files, rules) {
    if (!rules || rules.length === 0) return files;
    return collect(
        files.map((file) => {
            for (const rule of rules) {
                if (rule.from && !file.path.startsWith(rule.from)) continue;
                if (rule.fromSuffix && !file.path.endsWith(rule.fromSuffix)) continue;
                let rewritten = rule.from ? `${rule.to}${file.path.slice(rule.from.length)}` : file.path;
                if (rule.fromSuffix) {
                    rewritten = `${rewritten.slice(0, rewritten.length - rule.fromSuffix.length)}${rule.toSuffix}`;
                }
                return { ...file, path: rewritten };
            }
            return file;
        })
    );
}

export function loadPolicy(root) {
    const policyPath = path.resolve(root, "specs/coverage/policy.json");
    const policy = JSON.parse(fs.readFileSync(policyPath, "utf8"));
    if (policy.schemaVersion !== 1) {
        throw new Error(`specs/coverage/policy.json: unsupported schemaVersion ${String(policy.schemaVersion)}`);
    }
    return { policy, policyPath };
}

function matchesPrefix(prefixes, filePath) {
    return prefixes.some((prefix) => (prefix.endsWith("/") ? filePath.startsWith(prefix) : filePath === prefix));
}

function uncovered(entry, metric) {
    return entry[metric].total - entry[metric].covered;
}

/**
 * Applies one platform's policy to one normalized report.
 *
 * What this gate measures is how much of the parser's behaviour is *pinned*:
 * the producers run only the suites that assert parse output, so a covered
 * branch is one whose behaviour a golden assertion would catch changing, and
 * an uncovered branch is behaviour the planned rewrite can alter with nothing
 * failing. `unpinned` is therefore a map of unprotected surface, not a backlog of
 * unwritten unit tests, and it shrinks by adding corpus inputs that pin the
 * behaviour — not by adding tests that merely execute the code.
 *
 * Two rules, and no third:
 *
 * 1. a measured file with no `unpinned` entry must be fully covered on every
 *    required metric — that is the 100% target, and every file added from now
 *    on meets it by construction; and
 * 2. an `unpinned` entry is an upper bound on that file's uncovered counts, so
 *    the unprotected surface can only shrink.
 *
 * Rule 2 is a bound rather than an equality on purpose. The same source
 * compiles differently across the operating systems the required jobs run on,
 * so demanding an exact match would make an unrelated platform difference look
 * like a coverage failure. Entries that have become loose are reported as
 * tightenable and rewritten by `--update-ledger`, which keeps the record
 * honest without letting it break a build that only improved.
 */
export function enforce({ policy, platform, files }) {
    const configuration = policy.platforms?.[platform];
    if (!configuration) {
        throw new Error(`specs/coverage/policy.json declares no platform "${platform}"`);
    }
    const declaredMetrics = policy.requiredMetrics ?? METRICS;
    const unsupported = new Map((configuration.unsupportedMetrics ?? []).map((entry) => [entry.metric, entry]));
    const required = declaredMetrics.filter((metric) => !unsupported.has(metric));
    const include = configuration.include ?? [];
    const exempt = new Map((configuration.exempt ?? []).map((entry) => [entry.path, entry]));
    const unpinned = configuration.unpinned ?? {};

    const measured = [];
    const skipped = [];
    for (const file of files) {
        if (!matchesPrefix(include, file.path)) continue;
        if (exempt.has(file.path)) {
            skipped.push(file.path);
            continue;
        }
        // A header with no emitted code carries no counters on any metric and
        // states nothing about test quality either way.
        if (required.every((metric) => file[metric].total === 0)) continue;
        measured.push(file);
    }

    const failures = [];
    const tightenable = [];
    const seenLedgerEntries = new Set();

    for (const file of measured) {
        const allowance = unpinned[file.path];
        if (!allowance) {
            const gaps = required.filter((metric) => uncovered(file, metric) > 0);
            if (gaps.length) {
                failures.push({
                    kind: "below-target",
                    path: file.path,
                    detail: gaps.map((metric) => `${metric} ${file[metric].covered}/${file[metric].total}`).join(", ")
                });
            }
            continue;
        }
        seenLedgerEntries.add(file.path);
        const exceeded = required.filter((metric) => uncovered(file, metric) > (allowance[metric] ?? 0));
        if (exceeded.length) {
            failures.push({
                kind: "regressed",
                path: file.path,
                detail: exceeded
                    .map(
                        (metric) => `${metric} uncovered ${uncovered(file, metric)} > allowed ${allowance[metric] ?? 0}`
                    )
                    .join(", ")
            });
            continue;
        }
        if (required.some((metric) => uncovered(file, metric) < (allowance[metric] ?? 0))) {
            tightenable.push(file);
        }
    }

    for (const ledgerPath of Object.keys(unpinned)) {
        if (!seenLedgerEntries.has(ledgerPath)) {
            tightenable.push(emptyFile(ledgerPath));
        }
    }

    const totals = {};
    for (const metric of declaredMetrics) {
        totals[metric] = counter();
        for (const file of measured) {
            totals[metric].covered += file[metric].covered;
            totals[metric].total += file[metric].total;
        }
    }

    // A file that vanishes from the report satisfies every rule above by not
    // being there: an instrumenter that silently drops classes it cannot read,
    // or a filter that stops matching, would read as an improvement. The floor
    // is recorded alongside the ledger and is the only thing that notices.
    const floor = configuration.minimumMeasuredFiles;
    if (typeof floor === "number" && measured.length < floor) {
        failures.push({
            kind: "lost-files",
            path: `(platform ${platform})`,
            detail:
                `the report measures ${String(measured.length)} file(s) but the policy records a floor of ` +
                `${String(floor)}. Files left the report rather than reaching the target; find out which, ` +
                "and lower the floor deliberately if the removal is intended."
        });
    }

    // A metric whose counters are all zero passes every per-file rule above
    // while proving nothing, which is the one way this gate could report a
    // clean sheet for an unmeasured dimension. Either the platform declared
    // that its toolchain cannot produce the metric, or the absence of data is
    // itself the failure.
    for (const metric of required) {
        if (totals[metric].total === 0) {
            failures.push({
                kind: "unmeasured",
                path: `(platform ${platform})`,
                detail:
                    `${metric} coverage has no counters at all. The toolchain stopped emitting it, or the ` +
                    `report was produced without it. Declare it in unsupportedMetrics with a reason, or fix the producer.`
            });
        }
    }
    for (const [metric, entry] of unsupported) {
        if (totals[metric]?.total > 0) {
            failures.push({
                kind: "unexpectedly-measured",
                path: `(platform ${platform})`,
                detail:
                    `${metric} is declared unsupported (${entry.reason ?? "no reason recorded"}) but the report ` +
                    `carries ${String(totals[metric].total)} counters. Remove the declaration and gate on it.`
            });
        }
    }

    return { measured, skipped, failures, tightenable, totals, required, declaredMetrics, unsupported };
}

/**
 * Rewrites one platform's ledger from a measured report: every file still
 * short of the target keeps an entry holding its exact uncovered counts, and
 * every file that reached the target loses its entry outright. The ledger
 * therefore never records surface that is already pinned.
 */
export function rewriteLedger({ policy, policyPath, platform, measured, required }) {
    const configuration = policy.platforms[platform];
    const unpinned = {};
    for (const file of [...measured].sort((left, right) => (left.path < right.path ? -1 : 1))) {
        const entry = {};
        let short = false;
        for (const metric of required) {
            const gap = uncovered(file, metric);
            entry[metric] = gap;
            if (gap > 0) short = true;
        }
        if (short) unpinned[file.path] = entry;
    }
    configuration.unpinned = unpinned;
    configuration.minimumMeasuredFiles = measured.length;
    fs.writeFileSync(policyPath, `${JSON.stringify(policy, null, 4)}\n`);
    return Object.keys(unpinned).length;
}

export function formatPercent(entry) {
    if (entry.total === 0) return "n/a";
    return `${((100 * entry.covered) / entry.total).toFixed(2)}%`;
}
