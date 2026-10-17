#!/usr/bin/env node
/**
 * The stream benchmark (docs/plans/2026-09-29-incremental-gates.md, sections
 * 5 and 9).
 *
 * It measures what an application pays per streamed chunk: each benchmark
 * workload of workloads.mjs, run through a subject by `stream_runner`, under
 * callgrind, one dump per window and one for the finalize. A window's cost is
 * the edge into `bench_apply_step` or `bench_finalize`, less parser creation;
 * everything else in the process, including opening the subject, is outside
 * every window.
 *
 * The build, its provenance and identity, the measurement's isolation and the
 * callgrind invocation are run.mjs's own (`prepareBuild`, `profileRun`), so a
 * stream report and a stage report of one commit describe the same binaries
 * built the same way. `oneshot_ir` is the stage runner's measurement of the
 * workload's final text, both stages, exactly as the one-shot benchmark
 * measures a document.
 *
 * Two subjects run every workload: `reparse`, the R column, and `session`,
 * the S column. The session takes every step of its windows, so it is
 * measured on the workloads of at most 1,024 steps, where 6.3 holds every
 * step and the finalize to 1.25 times `reparse` on the same step; a longer
 * stream is bounded per chunk by 6.2 from the step that makes its chunks
 * local (section 7).
 * With `--baseline-ref`, the base revision's engine is rebuilt with this
 * run's harness and measured on the same workloads, and 6.4 holds each
 * subject's p95, maximum and total to 1.02 times the base's. The report is
 * written either way, and a violation of 6.3 or 6.4 fails the run that writes
 * the whole report.
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
import { STREAM_RUNNER, measure, prepareBuild, profileRun } from "./run.mjs";
import {
    groupWindows,
    neverWorse,
    REGRESSION_LIMIT,
    regressions,
    summary,
    WINDOWS,
    windowCost,
    windowEnds
} from "./stream-gates.mjs";
import { STAGES } from "./stage-budget.mjs";
import { BENCHMARK_SETS, writeBenchmarkWorkloads } from "./workloads.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const SUBJECTS = Object.freeze(["reparse", "session"]);

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
 * One subject on one workload: its windows' costs and its receipt. The
 * profile of the most expensive window is kept as `NAME.SUBJECT.max`; the
 * rest are read and removed, because a long stream writes a thousand of them.
 * `out` holds the run's workloads, and `into` the profiles of the binaries
 * `profile` names.
 */
function measureSubject(profile, out, into, workload, subject, final) {
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
            "--final",
            final
        ],
        dump,
        into
    );
    const receipt = /steps=(\d+) windows=(\d+) bytes=(\d+) root_children=(\d+)/u.exec(stdout);
    if (!receipt) throw new Error(`${workload.name} produced no receipt from ${subject}`);
    const [steps, windows, bytes, rootChildren] = receipt.slice(1).map(Number);
    /* Every chunk window, then the finalize's. */
    if (windows !== windowEnds(steps).length + 1) throw new Error(`${workload.name} reported ${windows} windows`);
    const costs = [];
    let highest = 0;
    for (let window = 1; window <= windows; window++) {
        const cost = windowCost(parseCallgrind(fs.readFileSync(`${dump}.${window}`, "utf8")));
        costs.push(cost);
        if (cost.Ir > costs[highest].Ir) highest = window - 1;
    }
    if (fs.existsSync(`${dump}.${windows + 1}`)) throw new Error(`${workload.name} dumped more windows than it ran`);
    fs.renameSync(`${dump}.${highest + 1}`, `${dump}.max`);
    for (let window = 1; window <= windows; window++) fs.rmSync(`${dump}.${window}`, { force: true });
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

/** Both subjects of one build on one workload; the session only where 6.3 measures it. */
function measureSubjects(profile, out, into, workload, final) {
    const reparse = measureSubject(profile, out, into, workload, "reparse", final);
    const session = reparse.steps <= WINDOWS ? measureSubject(profile, out, into, workload, "session", final) : null;
    if (session && (session.steps !== reparse.steps || session.bytes !== reparse.bytes)) {
        throw new Error(`${workload.name}: the subjects took different streams`);
    }
    return { reparse, session };
}

/**
 * One workload, measured: each subject's windows, 6.3 between them, the
 * one-shot parse of the text they end with and, with a baseline, each
 * subject's summary on the base revision and 6.4 against it. Runs in a
 * worker; every path is absolute.
 */
function measureWorkload(profile, baseline, out, workload) {
    const final = path.join(out, "final", `${workload.name}.md`);
    fs.mkdirSync(path.dirname(final), { recursive: true });
    const { reparse, session } = measureSubjects(profile, out, out, workload, final);
    const oneshot = measure(profile, "markdown-core", { case: workload.name, file: final }, out);
    if (oneshot.receiptBytes !== reparse.bytes || oneshot.rootChildren !== reparse.rootChildren) {
        throw new Error(`${workload.name}: the one-shot parse of the final text disagrees with the subject's`);
    }
    fs.rmSync(oneshot.dump, { force: true });
    let base = null;
    if (baseline) {
        const measured = measureSubjects(baseline.profile, out, baseline.directory, workload, final);
        if (measured.reparse.steps !== reparse.steps || !measured.session !== !session) {
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
        neverWorse: session && reparse.steps <= WINDOWS ? neverWorse(session.windows, reparse.windows) : [],
        oneshot: { ir: STAGES.reduce((sum, stage) => sum + oneshot.stages[stage].cost.Ir, 0) },
        base,
        regressions: base
            ? [
                  ...regressions(reparse.summary.ir, base.reparse).map((entry) => ({ subject: "reparse", ...entry })),
                  ...(session
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

/** The 6.3 violations of a report: every step above 1.25 times `reparse`. */
export function violations(report) {
    return report.results.flatMap((row) => row.neverWorse.map((entry) => ({ name: row.name, ...entry })));
}

/** The 6.4 violations of a report: every figure above 1.02 times the base's. */
export function regressed(report) {
    return report.results.flatMap((row) => row.regressions.map((entry) => ({ name: row.name, ...entry })));
}

/**
 * The human report: per document source, family and size, the R and S
 * columns pooled over every window of every workload in the group, the
 * highest S / R of one step, and the one-shot parse of the final texts; with
 * a baseline, 6.4 per workload.
 */
export function markdownReport(report) {
    const lines = [
        "# Stream benchmark",
        "",
        `Subjects: ${report.subjects.map((subject) => `\`${subject}\``).join(", ")}. Workloads: ${number(report.results.length)} (\`${report.workloads.version}\`, digest \`${report.workloads.digest.slice(0, 16)}\`).`,
        "Every figure is Ir per window, the edge into `bench_apply_step` or, for the finalize, `bench_finalize`, less parser creation; the one-shot column parses each workload's final text once, both stages.",
        `S is measured on the workloads of at most ${number(WINDOWS)} steps, where each step costs at most 1.25 times R's (6.3).`,
        "",
        "| Documents | Family | Size | Workloads | Windows | R p50 | R p95 | R max | R total | S p50 | S p95 | S max | S total | Highest S / R | One-shot total |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
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
                group.ratio === null ? "" : `${group.ratio.toFixed(3)}×`,
                number(group.oneshot)
            ].join(" | ")} |`
        );
    }
    const failed = violations(report);
    if (failed.length) {
        lines.push("", `6.3 fails on ${number(failed.length)} step(s):`, "");
        for (const entry of failed.slice(0, 20)) {
            lines.push(`- ${entry.name} step ${entry.step + 1}: ${entry.ratio.toFixed(3)}× reparse`);
        }
    }
    if (report.baseline) {
        const worse = regressed(report);
        const workloads = new Set(worse.map((entry) => entry.name)).size;
        lines.push(
            "",
            "## Regression gate",
            "",
            `Base: ${report.baseline.revision}, measured with this run's harness and workloads. Each subject's p95, maximum and total Ir of each workload must stay within ${((REGRESSION_LIMIT - 1) * 100).toFixed(0)}% of the base's (6.4). ${number(report.results.length - workloads)}/${number(report.results.length)} workloads passed.`
        );
        for (const entry of worse.slice(0, 20)) {
            lines.push(`- ${entry.name} ${entry.subject} ${entry.metric}: ${entry.ratio.toFixed(4)}× base`);
        }
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
            stream: workload.family
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
        schemaVersion: 3,
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
    const failed = violations(report);
    const worse = regressed(report);
    if (failed.length || worse.length) {
        fail(
            [
                failed.length
                    ? `6.3: ${failed.length} step(s) cost more than 1.25 times reparse, the first ${failed[0].name} step ${failed[0].step + 1} at ${failed[0].ratio.toFixed(3)}×`
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
