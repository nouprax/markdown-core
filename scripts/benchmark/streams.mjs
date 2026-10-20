#!/usr/bin/env node
/**
 * The stream benchmark (docs/plans/2026-09-29-incremental-gates.md, sections
 * 5 and 9).
 *
 * It measures what an application pays per streamed chunk: each benchmark
 * workload of workloads.mjs, run through a subject by `stream_runner`, under
 * callgrind, one dump per window and one for the finalize. A session window's
 * cost is the edge into `bench_apply_step` or `bench_finalize`; a `reparse`
 * window's is the two parse stages of the `Document.parse` inside it; a
 * `binding` window's is what its MCB3 messages add to the session's steps.
 * Everything else in the process, including opening the subject, is outside
 * every window.
 *
 * The build, its provenance and identity, the measurement's isolation and the
 * callgrind invocation are run.mjs's own (`prepareBuild`, `profileRun`), so a
 * stream report and a stage report of one commit describe the same binaries
 * built the same way. `oneshot_ir` is the stage runner's measurement of the
 * workload's final text, both stages, exactly as the one-shot benchmark
 * measures a document.
 *
 * Three subjects run the workloads: `reparse`, the R column, `session`, the
 * S column, and `binding`, the B column. The session takes every step of its
 * windows, and runs the streams a gate bounds: every stream `reparse` runs at
 * every step -- each of at most 1,024 steps, and each of a shape 6.2 leaves
 * to 6.3 at the shape's smallest size, whatever its length (decision G17) --
 * where 6.3 holds its total over the steps and the finalize to 1.25 times
 * `reparse`'s on the same steps, and every stream of a shape whose cost per
 * chunk the language keeps local, whose chunk windows 6.2 holds flat. B is
 * the C side of a binding's build of each published document's values, on
 * the streams of at most 1,024 steps, reported without a gate (plan D5).
 * With `--baseline-ref`, the base revision's engine is rebuilt with this
 * run's harness and measured on the same workloads, and 6.4 holds R's p95,
 * maximum and total to 1.02 times the base's, and S's on the streams 6.3
 * measures. The report is written either way, and a violation of 6.2, 6.3
 * or 6.4 fails the run that writes the whole report.
 *
 *   node scripts/benchmark/streams.mjs [--out DIR] [--set all|corpus]
 *                                      [--workload NAME]... [--shard I/N]
 *                                      [--baseline-ref COMMIT] [--quiet]
 *   node scripts/benchmark/streams.mjs --merge DIR [--out DIR] [--quiet]
 *
 * `--set corpus` measures the grammar corpus's workloads alone; `--workload`
 * narrows either set to the named workloads. `--shard I/N` measures the
 * workloads at positions I, I + N, I + 2N, ... of the set and writes that
 * part of the report; `--merge DIR` joins the N parts found in DIR's
 * subdirectories into the report, so N machines measure one set.
 */

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { isMainThread, parentPort, Worker, workerData } from "node:worker_threads";

import { parseCallgrind } from "./callgrind.mjs";
import { PARSE_BOUNDARIES, STREAM_RUNNER, measure, prepareBuild, profileRun } from "./run.mjs";
import {
    bindingCost,
    everyStep,
    flatStream,
    groupWindows,
    neverWorse,
    parseCost,
    REGRESSION_LIMIT,
    regressions,
    stepCost,
    summary,
    WINDOWS,
    windowEnds
} from "./stream-gates.mjs";
import { STAGES } from "./stage-budget.mjs";
import { BENCHMARK_SETS, streamChunks, writeBenchmarkWorkloads } from "./workloads.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const SUBJECTS = Object.freeze(["reparse", "session", "binding"]);
/* What a window of each subject counts (gates 5.1): an append is the
 * session's streaming parse, all of it, of each `Document.parse` the
 * `reparse` subject calls, the two parse stages are the parsing, and a
 * binding's C side is the encode of each published document and the release
 * of its message. */
const WINDOW_COST = Object.freeze({
    reparse: (profile) => parseCost(profile, PARSE_BOUNDARIES),
    session: stepCost,
    binding: bindingCost
});

function fail(message) {
    console.error(`benchmark:streams: ${message}`);
    process.exit(1);
}

function parseArguments(argv) {
    const options = {
        out: path.join(root, "build/benchmark-streams"),
        set: "all",
        workloads: [],
        shard: { index: 0, count: 1 },
        baselineRef: null,
        merge: null,
        quiet: false
    };
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
        } else if (flag === "--set") {
            if (!BENCHMARK_SETS.includes(value)) fail(`--set is one of ${BENCHMARK_SETS.join(", ")}`);
            options.set = value;
            index++;
        } else if (flag === "--workload") {
            options.workloads.push(value);
            index++;
        } else if (flag === "--shard") {
            const match = /^(\d+)\/(\d+)$/u.exec(value);
            if (!match || Number(match[1]) >= Number(match[2])) fail("--shard is I/N with I < N");
            options.shard = { index: Number(match[1]), count: Number(match[2]) };
            index++;
        } else if (flag === "--baseline-ref") {
            if (!/^[0-9a-f]{40}$/u.test(value)) fail("--baseline-ref must be a full commit SHA");
            options.baselineRef = value;
            index++;
        } else if (flag === "--merge") {
            options.merge = path.resolve(value);
            index++;
        } else {
            fail(`unknown argument: ${flag}`);
        }
    }
    return options;
}

/**
 * One subject on one workload, in `windows` chunk windows: their costs, the
 * finalize's and the receipt. The profile of the most expensive window is
 * kept as `NAME.SUBJECT.max`; the rest are read and removed, because a long
 * stream writes thousands of them. `out` holds the run's workloads, and
 * `into` the profiles of the binaries `profile` names.
 */
function measureSubject(profile, out, into, workload, subject, final, windows) {
    const dump = path.join(into, "callgrind", `${workload.name}.${subject}.out`);
    const stdout = profileRun(
        profile,
        STREAM_RUNNER,
        [
            "--subject",
            subject,
            "--document",
            path.join(out, "workloads", workload.document),
            "--stream",
            workload.stream,
            "--tokens",
            path.join(out, "workloads", "token-sizes.txt"),
            "--windows",
            String(windows),
            "--final",
            final
        ],
        dump,
        into
    );
    const receipt = /steps=(\d+) windows=(\d+) bytes=(\d+) root_children=(\d+)/u.exec(stdout);
    if (!receipt) throw new Error(`${workload.name} produced no receipt from ${subject}`);
    const [steps, dumped, bytes, rootChildren] = receipt.slice(1).map(Number);
    /* Every chunk window, then the finalize's. */
    if (dumped !== windows + 1) throw new Error(`${workload.name} reported ${dumped} windows`);
    const costs = [];
    let highest = 0;
    for (let window = 1; window <= dumped; window++) {
        const cost = WINDOW_COST[subject](parseCallgrind(fs.readFileSync(`${dump}.${window}`, "utf8")));
        costs.push(cost);
        if (cost.Ir > costs[highest].Ir) highest = window - 1;
    }
    if (fs.existsSync(`${dump}.${dumped + 1}`)) throw new Error(`${workload.name} dumped more windows than it ran`);
    fs.renameSync(`${dump}.${highest + 1}`, `${dump}.max`);
    for (let window = 1; window <= dumped; window++) fs.rmSync(`${dump}.${window}`, { force: true });
    fs.rmSync(dump, { force: true });
    return {
        steps,
        bytes,
        rootChildren,
        windows: costs.map((cost) => cost.Ir),
        summary: {
            ir: summary(costs.map((cost) => cost.Ir)),
            dataRefs: costs.reduce((sum, cost) => sum + cost.Dr + cost.Dw, 0),
            maxWindow: highest + 1
        }
    };
}

/**
 * Both subjects of one build on one workload, in `windows` chunk windows:
 * the session where `reparse` runs every step (6.3), and on a longer stream
 * when `long` asks for it (6.2).
 */
function measureSubjects(profile, out, into, workload, final, windows, long) {
    const reparse = measureSubject(profile, out, into, workload, "reparse", final, windows);
    const session =
        everyStep(reparse) || long ? measureSubject(profile, out, into, workload, "session", final, windows) : null;
    if (session && (session.steps !== reparse.steps || session.bytes !== reparse.bytes)) {
        throw new Error(`${workload.name}: the subjects took different streams`);
    }
    return { reparse, session };
}

/**
 * One workload, measured: each subject's windows, 6.2 on the session's chunk
 * windows of a local shape, 6.3 between R and S where R runs every step, the
 * one-shot parse of the text they end with and, with a baseline, R's and S's
 * summaries on the base revision and 6.4 against them, S's on the streams
 * 6.3 measures. B is this revision's alone, on the streams of at most 1,024
 * steps. The windows come from the generator's own chunking of the document,
 * which must take the runner's steps. Runs in a worker; every path is
 * absolute.
 */
function measureWorkload(profile, baseline, out, workload) {
    const final = path.join(out, "final", `${workload.name}.md`);
    fs.mkdirSync(path.dirname(final), { recursive: true });
    const chunkEnds = streamChunks(fs.readFileSync(path.join(out, "workloads", workload.document)), workload.stream);
    const ends = windowEnds(chunkEnds.length, workload.stepwise);
    const { reparse, session } = measureSubjects(profile, out, out, workload, final, ends.length, workload.local);
    if (reparse.steps !== chunkEnds.length) {
        throw new Error(
            `${workload.name}: the runner took ${reparse.steps} chunks and the generator ${chunkEnds.length}`
        );
    }
    const compared = everyStep(reparse);
    const short = reparse.steps <= WINDOWS;
    const binding = short ? measureSubject(profile, out, out, workload, "binding", final, ends.length) : null;
    if (binding && (binding.steps !== session.steps || binding.bytes !== session.bytes)) {
        throw new Error(`${workload.name}: the subjects took different streams`);
    }
    const oneshot = measure(profile, "markdown-core", { case: workload.name, file: final }, out);
    if (oneshot.receiptBytes !== reparse.bytes || oneshot.rootChildren !== reparse.rootChildren) {
        throw new Error(`${workload.name}: the one-shot parse of the final text disagrees with the subject's`);
    }
    fs.rmSync(oneshot.dump, { force: true });
    let base = null;
    if (baseline) {
        const measured = measureSubjects(
            baseline.profile,
            out,
            baseline.directory,
            workload,
            final,
            ends.length,
            false
        );
        if (measured.reparse.steps !== reparse.steps || !measured.session !== !compared) {
            throw new Error(`${workload.name}: the base revision took a different stream`);
        }
        base = { reparse: measured.reparse.summary.ir, session: measured.session?.summary.ir ?? null };
    }
    fs.rmSync(final, { force: true });
    return {
        steps: reparse.steps,
        bytes: reparse.bytes,
        windows: reparse.windows,
        reparse: reparse.summary,
        session: session && { windows: session.windows, ...session.summary },
        binding: binding && { windows: binding.windows, ...binding.summary },
        flatness: workload.local
            ? flatStream(
                  /* Each chunk window ends where its last chunk does (5.1). */
                  ends.map((step, window) => ({ end: chunkEnds[step - 1], Ir: session.windows[window] })),
                  reparse.bytes
              )
            : [],
        neverWorse: compared ? neverWorse(session.windows, reparse.windows) : [],
        oneshot: { ir: STAGES.reduce((sum, stage) => sum + oneshot.stages[stage].cost.Ir, 0) },
        base,
        regressions: base
            ? [
                  ...regressions(reparse.summary.ir, base.reparse).map((entry) => ({ subject: "reparse", ...entry })),
                  ...(compared
                      ? regressions(session.summary.ir, base.session).map((entry) => ({ subject: "session", ...entry }))
                      : [])
              ]
            : []
    };
}

/* A worker measures the workloads its parent hands it, one at a time. */
if (!isMainThread) {
    const { profile, baseline, out } = workerData;
    parentPort.on("message", (workload) => {
        try {
            parentPort.postMessage({ name: workload.name, result: measureWorkload(profile, baseline, out, workload) });
        } catch (error) {
            parentPort.postMessage({ name: workload.name, error: error.message });
        }
    });
}

/* Every workload across one worker per available processor. The counts do
 * not depend on what else runs: that is why they are counts. */
function measureAll(profile, baseline, out, workloads, quiet) {
    const results = new Map();
    const queue = [...workloads];
    const count = Math.min(os.availableParallelism(), queue.length);
    return new Promise((resolve, reject) => {
        let running = count;
        for (let index = 0; index < count; index++) {
            const worker = new Worker(fileURLToPath(import.meta.url), { workerData: { profile, baseline, out } });
            let finished = false;
            const next = () => {
                const workload = queue.shift();
                if (workload) {
                    worker.postMessage(workload);
                } else {
                    finished = true;
                    worker.terminate();
                    if (--running === 0) resolve(results);
                }
            };
            worker.on("message", (message) => {
                if (message.error) {
                    reject(new Error(`${message.name}: ${message.error}`));
                    return;
                }
                results.set(message.name, message.result);
                if (!quiet) console.error(`measured ${message.name} (${results.size}/${workloads.length})`);
                next();
            });
            worker.on("error", reject);
            /* run.mjs refuses a measurement by exiting, which ends the worker. */
            worker.on("exit", (code) => {
                if (!finished) reject(new Error(`a measurement worker exited ${code}; see the message above`));
            });
            next();
        }
    });
}

const number = (value) => value.toLocaleString("en-US");

/** A group's four figures, or blanks when it has no such column. */
const figures = (column) =>
    column ? [column.p50, column.p95, column.max, column.total].map(number) : ["", "", "", ""];

/** A group's S / R in total and on its highest step, or blanks without S. */
const ratios = (group) =>
    group.totalRatio === null ? ["", ""] : [`${group.totalRatio.toFixed(3)}×`, `${group.stepRatio.toFixed(3)}×`];

/** The 6.2 violations of a report: every point whose m(p) is above 1.25 times m(n/16). */
export function steep(report) {
    return report.results.flatMap((row) => row.flatness.map((entry) => ({ name: row.name, ...entry })));
}

/** The 6.3 violations of a report: every workload whose total is above 1.25 times `reparse`'s. */
export function violations(report) {
    return report.results.flatMap((row) => row.neverWorse.map((entry) => ({ name: row.name, ...entry })));
}

/** The 6.4 violations of a report: every figure above 1.02 times the base's. */
export function regressed(report) {
    return report.results.flatMap((row) => row.regressions.map((entry) => ({ name: row.name, ...entry })));
}

/**
 * The human report: per document source, family and size, the R and S
 * columns pooled over every window of every workload in the group, S's total
 * over R's and the highest S / R of one step, and the one-shot parse of the
 * final texts; with a baseline, 6.4 per workload; and the B column beside S.
 */
export function markdownReport(report) {
    const lines = [
        "# Stream benchmark",
        "",
        `Subjects: ${report.subjects.map((subject) => `\`${subject}\``).join(", ")}. Workloads: ${number(report.results.length)} (\`${report.workloads.version}\`, digest \`${report.workloads.digest.slice(0, 16)}\`).`,
        "Every figure is Ir per window of chunks and for the finalize: S is the edge into `bench_apply_step` or `bench_finalize`, and R the two parse stages of each `Document.parse`; the one-shot column parses each workload's final text once, both stages.",
        `S is measured on the workloads whose R runs every step, those of at most ${number(WINDOWS)} steps and those of the shapes 6.2 leaves to 6.3 at the shape's smallest size, where each workload's S total is at most 1.25 times its R total (6.3), and on every stream of a shape whose cost per chunk the language keeps local, where the highest chunk window up to a quarter of the stream and up to its end costs at most 1.25 times the highest up to its first sixteenth (6.2). S / R is over the first.`,
        "",
        "| Documents | Family | Size | Workloads | Windows | R p50 | R p95 | R max | R total | S p50 | S p95 | S max | S total | S / R total | Highest step S / R | One-shot total |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
    ];
    const groups = groupWindows(report.results, (row) => JSON.stringify([row.source, row.family, row.size]));
    for (const group of groups) {
        const [source, family, size] = JSON.parse(group.key);
        lines.push(
            `| ${[
                source,
                family,
                size ? number(size) : "",
                number(group.workloads),
                number(group.windows),
                ...figures(group.reparse),
                ...figures(group.session),
                ...ratios(group),
                number(group.oneshot)
            ].join(" | ")} |`
        );
    }
    const uneven = steep(report);
    if (uneven.length) {
        lines.push("", `6.2 fails at ${number(uneven.length)} point(s):`, "");
        for (const entry of uneven.slice(0, 20)) {
            lines.push(`- ${entry.name} byte ${number(entry.point)}: ${entry.ratio.toFixed(3)}× the first sixteenth`);
        }
    }
    const failed = violations(report);
    if (failed.length) {
        lines.push("", `6.3 fails on ${number(failed.length)} workload(s):`, "");
        for (const entry of failed.slice(0, 20)) {
            lines.push(`- ${entry.name}: ${entry.ratio.toFixed(3)}× reparse in total`);
        }
    }
    if (report.baseline) {
        const worse = regressed(report);
        const workloads = new Set(worse.map((entry) => entry.name)).size;
        lines.push(
            "",
            "## Regression gate",
            "",
            `Base: ${report.baseline.revision}, measured with this run's harness and workloads. R's and S's p95, maximum and total Ir of each workload must stay within ${((REGRESSION_LIMIT - 1) * 100).toFixed(0)}% of the base's (6.4). ${number(report.results.length - workloads)}/${number(report.results.length)} workloads passed.`
        );
        for (const entry of worse.slice(0, 20)) {
            lines.push(`- ${entry.name} ${entry.subject} ${entry.metric}: ${entry.ratio.toFixed(4)}× base`);
        }
    }
    lines.push(
        "",
        "## Bindings' C side",
        "",
        "B is what a binding's C side adds to each S window: the MCB3 encode of each document the window publishes and the release of its message, which Kotlin and ECMAScript run per append, and whose facade reads Swift's builder makes from Swift. Building the platform values is not counted, and B has no gate (plan D5).",
        "",
        "| Documents | Family | Size | B p50 | B p95 | B max | B total | B / S total |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"
    );
    for (const group of groups.filter((entry) => entry.binding)) {
        const [source, family, size] = JSON.parse(group.key);
        lines.push(
            `| ${[
                source,
                family,
                size ? number(size) : "",
                ...figures(group.binding),
                `${group.bindingRatio.toFixed(3)}×`
            ].join(" | ")} |`
        );
    }
    return lines.join("\n");
}

/** The fields every part of one measurement shares, and the report carries. */
const IDENTITY = ["schemaVersion", "subjects", "toolchain", "binaries", "profile", "workloads", "baseline"];

/** Write the report and its human rendering under `out`. */
function writeReport(report, out, quiet) {
    fs.mkdirSync(out, { recursive: true });
    const json = path.join(out, "streams.json");
    fs.writeFileSync(json, `${JSON.stringify(report)}\n`);
    if (report.shard) {
        console.error(`wrote part ${report.shard.index}/${report.shard.count}: ${path.relative(root, json)}`);
        return;
    }
    const markdown = path.join(out, "streams.md");
    const rendered = markdownReport(report);
    fs.writeFileSync(markdown, `${rendered}\n`);
    if (!quiet) process.stdout.write(`${rendered}\n`);
    console.error(`wrote ${path.relative(root, json)} and ${path.relative(root, markdown)}`);
}

/**
 * Join the parts of one sharded measurement: every part measured the same
 * set with the same binaries, and together they hold shards 0 to N - 1 once
 * each. Workload k of the set is the (k / N)th result of part k mod N.
 */
export function mergeParts(parts) {
    if (!parts.length) throw new Error("no parts to merge");
    const count = parts[0].shard?.count;
    const byIndex = new Map();
    for (const part of parts) {
        for (const field of IDENTITY) {
            if (JSON.stringify(part[field]) !== JSON.stringify(parts[0][field])) {
                throw new Error(`the parts differ in ${field}`);
            }
        }
        if (!part.shard || part.shard.count !== count || byIndex.has(part.shard.index)) {
            throw new Error("the parts are not one set of shards");
        }
        byIndex.set(part.shard.index, part.results);
    }
    if (byIndex.size !== count) throw new Error(`${byIndex.size} of ${count} parts are present`);
    const results = [];
    for (let position = 0; position < parts[0].workloads.count; position++) {
        const row = byIndex.get(position % count)[Math.floor(position / count)];
        if (!row) throw new Error(`no part holds workload ${position}`);
        results.push(row);
    }
    if (results.length !== [...byIndex.values()].reduce((sum, rows) => sum + rows.length, 0)) {
        throw new Error("the parts hold more results than the set has workloads");
    }
    const report = Object.fromEntries(IDENTITY.map((field) => [field, parts[0][field]]));
    return { ...report, results };
}

async function measureSet(options) {
    const { profile, versions, binaries, baseline } = prepareBuild({
        out: options.out,
        baselineRef: options.baselineRef
    });
    const index = writeBenchmarkWorkloads(path.join(options.out, "workloads"), options.set, options.workloads);
    const { index: shard, count } = options.shard;
    const selected = index.workloads.filter((_, position) => position % count === shard);
    fs.rmSync(path.join(options.out, "callgrind"), { recursive: true, force: true });
    fs.rmSync(path.join(options.out, "final"), { recursive: true, force: true });
    if (baseline) fs.rmSync(path.join(baseline.directory, "callgrind"), { recursive: true, force: true });
    const measured = await measureAll(
        profile,
        baseline && { profile: baseline.profile, directory: baseline.directory },
        options.out,
        selected.map((workload) => ({
            name: workload.name,
            document: workload.document.path,
            stream: workload.family,
            local: workload.local,
            stepwise: workload.stepwise
        })),
        options.quiet
    );
    /* The binaries a report measures, by content: parts built on different
     * machines agree on these and nothing else. */
    const measuredBinaries = (identity) =>
        Object.fromEntries(
            ["markdown-core", "markdown-core streams"].map((name) => [name, { sha256: identity[name].sha256 }])
        );
    const report = {
        schemaVersion: 6,
        subjects: SUBJECTS,
        toolchain: versions,
        binaries: measuredBinaries(binaries),
        profile: { compiler: profile.compiler, flags: profile.flags },
        workloads: { version: index.version, set: index.set, digest: index.digest, count: index.workloads.length },
        baseline: baseline && { revision: baseline.revision, binaries: measuredBinaries(baseline.binaries) },
        results: selected.map((workload) => ({
            name: workload.name,
            document: workload.document.name,
            source: workload.document.shape ?? "grammar corpus",
            alphabet: workload.document.alphabet,
            size: workload.document.shape ? workload.document.size : null,
            family: workload.family,
            local: workload.local,
            ...measured.get(workload.name)
        }))
    };
    return count === 1 ? report : { ...report, shard: options.shard };
}

async function main() {
    const options = parseArguments(process.argv.slice(2));
    let report;
    if (options.merge) {
        const parts = fs
            .readdirSync(options.merge, { withFileTypes: true })
            .filter((entry) => entry.isDirectory())
            .map((entry) => JSON.parse(fs.readFileSync(path.join(options.merge, entry.name, "streams.json"), "utf8")));
        report = mergeParts(parts);
    } else {
        report = await measureSet(options);
    }
    writeReport(report, options.out, options.quiet);
    /* A part is judged with the others, once they are joined. */
    if (report.shard) return;
    const uneven = steep(report);
    const failed = violations(report);
    const worse = regressed(report);
    if (uneven.length || failed.length || worse.length) {
        fail(
            [
                uneven.length
                    ? `6.2: ${uneven.length} point(s) cost more than 1.25 times the first sixteenth, the first ${uneven[0].name} at byte ${uneven[0].point} at ${uneven[0].ratio.toFixed(3)}×`
                    : null,
                failed.length
                    ? `6.3: ${failed.length} workload(s) cost more than 1.25 times reparse in total, the first ${failed[0].name} at ${failed[0].ratio.toFixed(3)}×`
                    : null,
                worse.length
                    ? `6.4: ${worse.length} figure(s) exceed ${REGRESSION_LIMIT} times the base, the first ${worse[0].name} ${worse[0].subject} ${worse[0].metric} at ${worse[0].ratio.toFixed(4)}×`
                    : null
            ]
                .filter(Boolean)
                .join("; ")
        );
    }
}

if (isMainThread && import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    main().catch((error) => fail(error.message));
}
