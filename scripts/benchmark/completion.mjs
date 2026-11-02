#!/usr/bin/env node
/**
 * The completion latency report (docs/plans/2026-10-23-lsp-completion-engine.md,
 * section 7, and step 4c).
 *
 * It measures what the language engine pays per completion request: the
 * stream benchmark's edit scripts (workloads.mjs) on every grammar corpus
 * document and every shape at 16, 64 and 256 KiB, each run by
 * `completion_runner` through the engine as a client typing it would, under
 * callgrind, one dump per measured request. A request is the didChange of one
 * edit and the completion at the end of its text; its cost is the edge into
 * `bench_request`, split into the edit (`markdown_core_lsp_update`) and the
 * completion (`markdown_core_lsp_complete`), which until the providers of
 * section 5 land locates the document's session at the cursor and answers no
 * item. Opening the server and the document are in no request.
 *
 * A script is measured at most `REQUESTS` times, at evenly spaced edits; the
 * edits between reach the server as changes of their own, outside every
 * request. On a shape larger than 16 KiB, whose edits each take tens to
 * hundreds of millions of instructions under callgrind, a script is cut to
 * its first `LARGE_EDITS` edits and measured at `LARGE_REQUESTS` of them. A script whose edit splits a `\r\n` is skipped, since no protocol
 * position addresses that place; the report counts them.
 *
 * The figures are callgrind Ir, as the parser's gates count, grouped by
 * document size next to the plan's proposed budget, which is wall time on a
 * device and so is not compared with them here. Nothing is gated (D5): the
 * run fails only when a measurement does.
 *
 *   node scripts/benchmark/completion.mjs [--out DIR] [--workload NAME]... [--quiet]
 */

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath, pathToFileURL } from "node:url";
import { isMainThread, parentPort, Worker, workerData } from "node:worker_threads";

import { baseName, CLONE_SUFFIX, costRecord, parseCallgrind } from "./callgrind.mjs";
import { COMPLETION_RUNNER, prepareBuild, profileRun } from "./run.mjs";
import { summary } from "./stream-gates.mjs";
import { benchmarkWorkloads, writeBenchmarkWorkloads } from "./workloads.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
export const REQUEST_ENTRY = "bench_request";
/* The two halves of a request, as `bench_request` calls them. */
export const REQUEST_PARTS = Object.freeze({
    edit: "markdown_core_lsp_update",
    complete: "markdown_core_lsp_complete"
});
export const REQUESTS = 16;
export const LARGE_REQUESTS = 4;
export const LARGE_EDITS = 16;
/* The largest shape size measured: the plan's budget stops at 256 KiB. */
export const LARGEST = 262144;
/* The plan's proposed budget per request, p95 of wall time on a device
 * (section 7), by the size it holds documents up to. */
export const BUDGET = Object.freeze([
    { size: 16384, milliseconds: 1 },
    { size: 262144, milliseconds: 10 }
]);
const SCHEMA_VERSION = 1;

function fail(message) {
    console.error(`benchmark:completion: ${message}`);
    process.exit(1);
}

function parseArguments(argv) {
    const options = { out: path.join(root, "build/benchmark-completion"), workloads: [], quiet: false };
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
        } else if (flag === "--workload") {
            options.workloads.push(value);
            index++;
        } else {
            fail(`unknown argument: ${flag}`);
        }
    }
    return options;
}

/**
 * One request's cost from its dump: the edges into `bench_request`, and the
 * edges out of it into each part. A dump that made no request is a runner
 * that measured nothing, and is refused rather than counted as free.
 */
export function requestCost(profile) {
    const edges = [...profile.edges.values()];
    const sum = (selected) => {
        const cost = [];
        for (const edge of selected) edge.cost.forEach((value, index) => (cost[index] = (cost[index] ?? 0) + value));
        return costRecord(profile, cost).Ir ?? 0;
    };
    const entered = edges.filter((edge) => baseName(edge.callee).replace(CLONE_SUFFIX, "") === REQUEST_ENTRY);
    const calls = entered.reduce((total, edge) => total + edge.calls, 0);
    if (calls !== 1) throw new Error(`a request dump holds ${calls} calls to ${REQUEST_ENTRY}`);
    const part = (callee) =>
        sum(
            edges.filter(
                (edge) =>
                    baseName(edge.caller).replace(CLONE_SUFFIX, "") === REQUEST_ENTRY &&
                    baseName(edge.callee) === callee
            )
        );
    const Ir = sum(entered);
    if (!Number.isSafeInteger(Ir) || Ir <= 0)
        throw new Error("a request's instruction count is not a positive integer");
    return { Ir, edit: part(REQUEST_PARTS.edit), complete: part(REQUEST_PARTS.complete) };
}

/** The requests `--requests most` measures in a script of `edits` edits. */
export const requestsOf = (edits, most) => Math.min(edits, most);

/**
 * How a script on `document` is measured: at most `most` requests over its
 * first `edits` edits, every edit when `edits` is null.
 */
export function measureOf(document) {
    return (document.size ?? 0) > BUDGET[0].size
        ? { most: LARGE_REQUESTS, edits: LARGE_EDITS }
        : { most: REQUESTS, edits: null };
}

/** The edits the runner sends of a script of `edits` edits under `measure`. */
export const editsOf = (edits, measure) => (measure.edits === null ? edits : Math.min(edits, measure.edits));

/**
 * One workload: each script's requests, in the file's order. The runner's
 * receipts must name the generator's scripts and edit counts. The profile of
 * the most expensive request is kept as `NAME.max`. Runs in a worker; every
 * path is absolute.
 */
function measureWorkload(profile, out, workload) {
    const dump = path.join(out, "callgrind", `${workload.name}.out`);
    const stdout = profileRun(
        profile,
        COMPLETION_RUNNER,
        [
            "--document",
            path.join(out, "workloads", workload.document),
            "--scripts",
            path.join(out, "workloads", workload.scripts),
            "--requests",
            String(workload.measure.most),
            ...(workload.measure.edits === null ? [] : ["--edits", String(workload.measure.edits)])
        ],
        dump,
        out
    );
    const receipts = [
        ...stdout.matchAll(/^completion-runner script=(\S+) (?:edits=(\d+) requests=(\d+) bytes=(\d+)|skipped)$/gmu)
    ].map((match) => ({
        name: match[1],
        skipped: match[2] === undefined,
        edits: Number(match[2] ?? 0),
        requests: Number(match[3] ?? 0),
        bytes: Number(match[4] ?? 0)
    }));
    if (
        receipts.length !== workload.expected.length ||
        receipts.some(
            (receipt, index) =>
                receipt.name !== workload.expected[index].name ||
                (!receipt.skipped &&
                    (receipt.edits !== editsOf(workload.expected[index].edits, workload.measure) ||
                        receipt.requests !== requestsOf(receipt.edits, workload.measure.most)))
        )
    ) {
        throw new Error(`${workload.name}: the runner ran other scripts than the generator wrote`);
    }
    let request = 0;
    let highest = { Ir: -1, request: 0 };
    const scripts = receipts.map((receipt, index) => {
        const costs = [];
        for (let step = 0; step < receipt.requests; step++) {
            request++;
            const cost = requestCost(parseCallgrind(fs.readFileSync(`${dump}.${request}`, "utf8")));
            costs.push(cost);
            if (cost.Ir > highest.Ir) highest = { Ir: cost.Ir, request };
        }
        return {
            name: receipt.name,
            family: workload.expected[index].family,
            skipped: receipt.skipped,
            edits: receipt.edits,
            bytes: receipt.bytes,
            requests: costs.map((cost) => cost.Ir),
            edit: costs.map((cost) => cost.edit),
            complete: costs.map((cost) => cost.complete)
        };
    });
    if (fs.existsSync(`${dump}.${request + 1}`)) throw new Error(`${workload.name} dumped more requests than it ran`);
    if (request) fs.renameSync(`${dump}.${highest.request}`, `${dump}.max`);
    for (let index = 1; index <= request; index++) fs.rmSync(`${dump}.${index}`, { force: true });
    fs.rmSync(dump, { force: true });
    return { scripts };
}

/* A worker measures the workloads its parent hands it, one at a time. */
if (!isMainThread) {
    const { profile, out } = workerData;
    parentPort.on("message", (workload) => {
        try {
            parentPort.postMessage({ name: workload.name, result: measureWorkload(profile, out, workload) });
        } catch (error) {
            parentPort.postMessage({ name: workload.name, error: error.message });
        }
    });
}

/* Every workload across one worker per available processor. */
function measureAll(profile, out, workloads, quiet) {
    const results = new Map();
    const queue = [...workloads];
    const count = Math.min(os.availableParallelism(), queue.length);
    return new Promise((resolve, reject) => {
        let running = count;
        for (let index = 0; index < count; index++) {
            const worker = new Worker(fileURLToPath(import.meta.url), { workerData: { profile, out } });
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

/** The size group of a workload's document: the grammar corpus, or a shape at its size. */
export const groupOf = (row) => (row.size === null ? "grammar" : `shapes-${row.size}`);

/** The budget the plan proposes for a group's documents (section 7). */
export function budgetOf(size) {
    return BUDGET.find((entry) => (size ?? 0) <= entry.size)?.milliseconds ?? null;
}

/**
 * The report's groups: for each document size, its workloads, scripts,
 * skipped scripts and requests, and p50, p95 and maximum of a request, of
 * its edit and of its completion, each over the group's requests.
 */
export function groupRequests(rows) {
    const groups = new Map();
    for (const row of rows) {
        const key = groupOf(row);
        if (!groups.has(key)) {
            groups.set(key, {
                key,
                size: row.size,
                budget: budgetOf(row.size),
                workloads: 0,
                scripts: 0,
                skipped: 0,
                requests: [],
                edit: [],
                complete: []
            });
        }
        const group = groups.get(key);
        group.workloads++;
        for (const script of row.scripts) {
            group.scripts++;
            if (script.skipped) group.skipped++;
            group.requests.push(...script.requests);
            group.edit.push(...script.edit);
            group.complete.push(...script.complete);
        }
    }
    return [...groups.values()]
        .sort((left, right) => (left.size ?? 0) - (right.size ?? 0))
        .map((group) => ({
            key: group.key,
            size: group.size,
            budget: group.budget,
            workloads: group.workloads,
            scripts: group.scripts,
            skipped: group.skipped,
            requests: group.requests.length,
            ir: summary(group.requests),
            edit: summary(group.edit),
            complete: summary(group.complete)
        }));
}

/** The `most` costliest requests of the report, with where they ran. */
export function costliest(rows, most = 10) {
    const all = [];
    for (const row of rows) {
        for (const script of row.scripts) {
            script.requests.forEach((Ir, index) =>
                all.push({
                    workload: row.name,
                    script: script.name,
                    family: script.family,
                    request: index + 1,
                    bytes: script.bytes,
                    Ir,
                    edit: script.edit[index],
                    complete: script.complete[index]
                })
            );
        }
    }
    return all.sort((left, right) => right.Ir - left.Ir || left.workload.localeCompare(right.workload)).slice(0, most);
}

const number = (value) => value.toLocaleString("en-US");
const groupName = (group) => (group.size === null ? "Grammar corpus" : `Shapes, ${number(group.size / 1024)} KiB`);

export function markdownReport(report) {
    const lines = [
        "# Completion latency",
        "",
        `Commit: \`${report.commit}\`. ${number(report.workloads.count)} edit workloads · \`${report.workloads.digest.slice(0, 16)}\`. Every figure is callgrind Ir per completion request: the didChange of one edit and the completion at the end of its text, at most ${number(report.requests.most)} requests a script at evenly spaced edits; on a shape larger than 16 KiB, ${number(report.requests.large)} over the script's first ${number(report.requests.edits)} edits. Opening the server and the document is in no request. The budget is the plan's proposal for p95 wall time on a device (section 7), not compared with these counts; nothing is gated (D5).`,
        "",
        "| Documents | Budget p95 | Workloads | Scripts | Skipped | Requests | p50 | p95 | Max | Edit p95 | Completion p95 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ...report.groups.map(
            (group) =>
                `| ${[
                    groupName(group),
                    group.budget === null ? "" : `${group.budget} ms`,
                    number(group.workloads),
                    number(group.scripts),
                    number(group.skipped),
                    number(group.requests),
                    number(group.ir.p50),
                    number(group.ir.p95),
                    number(group.ir.max),
                    number(group.edit.p95),
                    number(group.complete.p95)
                ].join(" | ")} |`
        ),
        "",
        "A skipped script edits between the `\\r` and `\\n` of a line break, a place no protocol position names.",
        "",
        "## Costliest requests",
        "",
        "| Workload | Script | Request | Bytes | Ir | Edit | Completion |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
        ...report.costliest.map(
            (entry) =>
                `| ${[
                    entry.workload,
                    entry.script,
                    number(entry.request),
                    number(entry.bytes),
                    number(entry.Ir),
                    number(entry.edit),
                    number(entry.complete)
                ].join(" | ")} |`
        ),
        "",
        "## Identity",
        "",
        `Compiler: ${report.toolchain.compiler}. Valgrind: ${report.toolchain.valgrind ?? "unknown"}. Runner: \`${report.binaries["markdown-core completion"].sha256}\`.`
    ];
    return `${lines.join("\n")}\n`;
}

/* The runner must keep its measured entry out of line, as the stream
 * runner's windows are checked (run.mjs, verifyStageSymbols). */
function verifyEntry(profile) {
    const binary = path.join(profile.binaryDir, COMPLETION_RUNNER);
    const listed = spawnSync("nm", ["-a", binary], { encoding: "utf8" });
    if (listed.status !== 0) fail(`cannot list the symbols of ${COMPLETION_RUNNER}; nm is required`);
    const names = new Set(
        listed.stdout.split("\n").map((line) => (line.trim().split(/\s+/u).pop() ?? "").replace(CLONE_SUFFIX, ""))
    );
    for (const name of [REQUEST_ENTRY, ...Object.values(REQUEST_PARTS)]) {
        if (!names.has(name))
            fail(`${name} is absent from ${COMPLETION_RUNNER}; the profile build must keep it out of line`);
    }
}

/** The edit workloads the report measures: every grammar corpus document and every shape up to `LARGEST`. */
export function completionWorkloads() {
    return benchmarkWorkloads("all").edits.filter((workload) => (workload.document.size ?? 0) <= LARGEST);
}

async function measureSet(options) {
    const { profile, versions, binaries } = prepareBuild({
        out: options.out,
        baselineRef: null,
        runners: { "markdown-core completion": COMPLETION_RUNNER }
    });
    verifyEntry(profile);
    const names = options.workloads.length ? options.workloads : completionWorkloads().map((workload) => workload.name);
    const index = writeBenchmarkWorkloads(path.join(options.out, "workloads"), "all", names);
    fs.rmSync(path.join(options.out, "callgrind"), { recursive: true, force: true });
    const measured = await measureAll(
        profile,
        options.out,
        index.edits.map((workload) => ({
            name: workload.name,
            document: workload.document.path,
            scripts: workload.path,
            measure: measureOf(workload.document),
            expected: workload.scripts.map((script) => ({
                name: script.name,
                family: script.family,
                edits: script.steps.length - 1
            }))
        })),
        options.quiet
    );
    const rows = index.edits.map((workload) => ({
        name: workload.name,
        document: workload.document.name,
        source: workload.document.shape ?? "grammar corpus",
        alphabet: workload.document.alphabet,
        size: workload.document.shape ? workload.document.size : null,
        bytes: workload.document.bytes,
        ...measured.get(workload.name)
    }));
    const commit = spawnSync("git", ["rev-parse", "HEAD"], { cwd: root, encoding: "utf8" }).stdout.trim();
    return {
        report: {
            schemaVersion: SCHEMA_VERSION,
            commit,
            requests: { most: REQUESTS, large: LARGE_REQUESTS, edits: LARGE_EDITS },
            toolchain: versions,
            binaries: { "markdown-core completion": { sha256: binaries["markdown-core completion"].sha256 } },
            profile: { compiler: profile.compiler, flags: profile.flags },
            workloads: { version: index.version, digest: index.digest, count: index.edits.length },
            groups: groupRequests(rows),
            costliest: costliest(rows),
            results: rows.map((row) => ({
                name: row.name,
                source: row.source,
                alphabet: row.alphabet,
                size: row.size,
                bytes: row.bytes,
                scripts: row.scripts.length,
                skipped: row.scripts.filter((script) => script.skipped).length,
                ir: row.scripts.some((script) => script.requests.length)
                    ? summary(row.scripts.flatMap((script) => script.requests))
                    : null
            }))
        },
        rows
    };
}

async function main() {
    const options = parseArguments(process.argv.slice(2));
    const { report, rows } = await measureSet(options);
    fs.mkdirSync(options.out, { recursive: true });
    fs.writeFileSync(path.join(options.out, "completion.json"), `${JSON.stringify(report, null, 2)}\n`);
    /* Every request, for a reader who wants the distribution behind a figure. */
    fs.writeFileSync(path.join(options.out, "requests.json"), `${JSON.stringify(rows)}\n`);
    const markdown = markdownReport(report);
    fs.writeFileSync(path.join(options.out, "completion.md"), markdown);
    if (!options.quiet) process.stdout.write(markdown);
}

if (isMainThread && import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    main().catch((error) => fail(error.message));
}
