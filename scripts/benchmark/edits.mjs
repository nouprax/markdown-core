#!/usr/bin/env node
/**
 * The edit and stream benchmark (docs/plans/2026-09-29-incremental-gates.md,
 * sections 5 and 9).
 *
 * It measures what an application pays per edit or per streamed chunk: each
 * benchmark workload of workloads.mjs, run through a subject by
 * `edit_runner`, under callgrind, one dump per window. A window's cost is the
 * edge into `bench_apply_step`; everything else in the process, including
 * opening the subject on its document, is outside every window.
 *
 * The build, its provenance and identity, the measurement's isolation and the
 * callgrind invocation are run.mjs's own (`prepareBuild`, `profileRun`), so an
 * edit report and a stage report of one commit describe the same binaries
 * built the same way. `oneshot_ir` is the stage runner's measurement of the
 * workload's final text, both stages, exactly as the one-shot benchmark
 * measures a document.
 *
 * Until the session API exists the only subject is `reparse`, which reports
 * the R column (section 7, step 0).
 *
 *   node scripts/benchmark/edits.mjs [--out DIR] [--set all|corpus]
 *                                    [--workload NAME]... [--quiet]
 *
 * `--set corpus` measures the grammar corpus's workloads alone; `--workload`
 * narrows either set to the named workloads.
 */

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { isMainThread, parentPort, Worker, workerData } from "node:worker_threads";

import { parseCallgrind } from "./callgrind.mjs";
import { groupWindows, summary, windowCost, windowEnds } from "./edit-gates.mjs";
import { EDIT_RUNNER, measure, prepareBuild, profileRun } from "./run.mjs";
import { STAGES } from "./stage-budget.mjs";
import { BENCHMARK_SETS, streamChunks, writeBenchmarkWorkloads } from "./workloads.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const SUBJECT = "reparse";

function fail(message) {
    console.error(`benchmark:edits: ${message}`);
    process.exit(1);
}

function parseArguments(argv) {
    const options = { out: path.join(root, "build/benchmark-edits"), set: "all", workloads: [], quiet: false };
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
        } else {
            fail(`unknown argument: ${flag}`);
        }
    }
    return options;
}

/**
 * One workload, measured: the subject's windows and the one-shot parse of the
 * text it ends with. Runs in a worker; every path is absolute.
 */
function measureWorkload(profile, out, workload) {
    const dump = path.join(out, "callgrind", `${workload.name}.out`);
    const final = path.join(out, "final", `${workload.name}.md`);
    fs.mkdirSync(path.dirname(final), { recursive: true });
    const document = path.join(out, "workloads", workload.document);
    const input = workload.script
        ? ["--script", path.join(out, "workloads", workload.file), "--name", workload.script]
        : ["--stream", workload.stream, "--tokens", path.join(out, "workloads", "token-sizes.txt")];
    const stdout = profileRun(
        profile,
        EDIT_RUNNER,
        ["--subject", SUBJECT, "--document", document, ...input, "--final", final],
        dump,
        out
    );
    const receipt = /steps=(\d+) windows=(\d+) bytes=(\d+) root_children=(\d+)/u.exec(stdout);
    if (!receipt) throw new Error(`${workload.name} produced no receipt`);
    const [steps, windows, bytes, rootChildren] = receipt.slice(1).map(Number);
    if (windows !== windowEnds(steps).length) throw new Error(`${workload.name} reported ${windows} windows`);

    /* One dump per window, then the process's own. The profile of the most
     * expensive window is kept as the workload's raw profile; the rest are
     * read and removed, because a long stream writes a thousand of them. */
    const costs = [];
    let highest = 0;
    for (let window = 1; window <= windows; window++) {
        const file = `${dump}.${window}`;
        const cost = windowCost(parseCallgrind(fs.readFileSync(file, "utf8")));
        costs.push(cost);
        if (cost.Ir > costs[highest].Ir) highest = window - 1;
    }
    if (fs.existsSync(`${dump}.${windows + 1}`)) throw new Error(`${workload.name} dumped more windows than it ran`);
    fs.renameSync(`${dump}.${highest + 1}`, `${dump}.max`);
    for (let window = 1; window <= windows; window++) fs.rmSync(`${dump}.${window}`, { force: true });
    fs.rmSync(dump, { force: true });

    const oneshot = measure(profile, "markdown-core", { case: workload.name, file: final }, out);
    if (oneshot.receiptBytes !== bytes || oneshot.rootChildren !== rootChildren) {
        throw new Error(`${workload.name}: the one-shot parse of the final text disagrees with the subject's`);
    }
    fs.rmSync(oneshot.dump, { force: true });
    fs.rmSync(final, { force: true });
    return {
        steps,
        bytes,
        windows: costs.map((cost) => cost.Ir),
        reparse: {
            ir: summary(costs.map((cost) => cost.Ir)),
            dataRefs: costs.reduce((sum, cost) => sum + cost.Dr + cost.Dw, 0),
            maxWindow: highest + 1
        },
        oneshot: { ir: STAGES.reduce((sum, stage) => sum + oneshot.stages[stage].cost.Ir, 0) }
    };
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

/* Every workload across one worker per available processor. The counts do
 * not depend on what else runs: that is why they are counts. */
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

const number = (value) => value.toLocaleString("en-US");

/**
 * The human report: per document source, family and size, the R column pooled
 * over every window of every workload in the group, and the one-shot parse of
 * the final texts.
 */
export function markdownReport(report) {
    const lines = [
        "# Edit and stream benchmark",
        "",
        `Subject: \`${report.subject}\`. Workloads: ${number(report.results.length)} (\`${report.workloads.version}\`, digest \`${report.workloads.digest.slice(0, 16)}\`).`,
        "Every figure is Ir per window, the edge into `bench_apply_step`; the one-shot column parses each workload's final text once, both stages.",
        "",
        "| Documents | Family | Size | Workloads | Windows | R p50 | R p95 | R max | R total | One-shot total |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
    ];
    const groups = groupWindows(report.results, (row) => JSON.stringify([row.source, row.family, row.size]));
    for (const group of groups) {
        const [source, family, size] = JSON.parse(group.key);
        const { p50, p95, max, total } = group.reparse;
        lines.push(
            `| ${source} | ${family} | ${size ? number(size) : ""} | ${[group.workloads, group.windows, p50, p95, max, total, group.oneshot].map(number).join(" | ")} |`
        );
    }
    return lines.join("\n");
}

async function main() {
    const options = parseArguments(process.argv.slice(2));
    const { profile, versions, binaries } = prepareBuild({ out: options.out });
    const index = writeBenchmarkWorkloads(path.join(options.out, "workloads"), options.set);
    const named = new Set(options.workloads);
    const unknown = options.workloads.filter((name) => !index.workloads.some((workload) => workload.name === name));
    if (unknown.length) fail(`no workload is named ${unknown.join(", ")}`);
    const selected = index.workloads.filter((workload) => !named.size || named.has(workload.name));
    fs.rmSync(path.join(options.out, "callgrind"), { recursive: true, force: true });
    fs.rmSync(path.join(options.out, "final"), { recursive: true, force: true });
    const measured = await measureAll(
        profile,
        options.out,
        selected.map((workload) => ({
            name: workload.name,
            document: workload.document.path,
            file: workload.file,
            script: workload.script?.name,
            stream: workload.stream
        })),
        options.quiet
    );
    const results = selected.map((workload) => {
        const result = measured.get(workload.name);
        if (workload.stream) {
            /* Each window's end byte, for the stream form of 6.2. */
            const ends = streamChunks(Buffer.from(workload.document.text), workload.stream);
            result.windowEnds = windowEnds(ends.length).map((step) => ends[step - 1]);
        }
        return {
            name: workload.name,
            document: workload.document.name,
            source: workload.document.shape ?? "grammar corpus",
            alphabet: workload.document.alphabet,
            size: workload.document.shape ? workload.document.size : null,
            family: workload.family,
            ...result
        };
    });
    const report = {
        schemaVersion: 1,
        subject: SUBJECT,
        toolchain: versions,
        binaries,
        profile: { compiler: profile.compiler, flags: profile.flags },
        workloads: { version: index.version, set: index.set, digest: index.digest, count: index.workloads.length },
        artifacts: path.relative(root, options.out),
        results
    };
    const json = path.join(options.out, "edits.json");
    const markdown = path.join(options.out, "edits.md");
    fs.writeFileSync(json, `${JSON.stringify(report)}\n`);
    const rendered = markdownReport(report);
    fs.writeFileSync(markdown, `${rendered}\n`);
    if (!options.quiet) process.stdout.write(`${rendered}\n`);
    console.error(`wrote ${path.relative(root, json)} and ${path.relative(root, markdown)}`);
}

if (isMainThread && import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    main().catch((error) => fail(error.message));
}
