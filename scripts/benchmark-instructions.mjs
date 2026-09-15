#!/usr/bin/env node
/**
 * The instruction lane of the C benchmark: deterministic instruction counts
 * for the workload cases, one parse and free each, counted by callgrind.
 *
 * For every case, bench_runner runs twice under `valgrind --tool=callgrind`:
 * once with --dry-run, which builds the input and parses nothing, and once
 * with --instructions, which parses and frees it exactly once. The
 * difference of the two counts is the instructions the parse and the free
 * cost, with the runner's own work (reading a sample, generating an input,
 * hashing it) cancelled out. A runner built with MARKDOWN_CORE_BENCH_CMARK
 * counts the pinned cmark oracle the same way with --reference cmark.
 *
 * Like the timing lane it is an opt-in local measurement, never a CI gate:
 * the numbers are exact for one build and one input, so a change of them is
 * something to read in a diff, not a threshold to fail on.
 *
 *   node scripts/benchmark-instructions.mjs --runner PATH --samples DIR
 *       [--workload NAME]... [--reference cmark] [--json FILE]
 */
import { execFileSync, spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

/** callgrind's total on stderr: `==pid== Collected : N`. */
export function parseCollected(stderr) {
    const match = /Collected\s*:\s*(\d+)/u.exec(stderr);
    if (!match) throw new Error("callgrind reported no instruction count");
    return Number(match[1]);
}

/** The runner's own line for the case it ran: `instructions case=... bytes=... sha256=...`. */
export function parseInstructionsLine(stdout) {
    const match =
        /^instructions case=(\S+) implementation=(\S+) stage=(\S+) bytes=(\d+) parses=(\d) sha256=([0-9a-f]{64})$/mu.exec(
            stdout
        );
    if (!match) throw new Error("bench_runner reported no instructions line");
    return {
        name: match[1],
        implementation: match[2],
        stage: match[3],
        bytes: Number(match[4]),
        parses: Number(match[5]),
        sha256: match[6]
    };
}

/** One row of the report. `coreIr` is the parse window and nothing else --
 * it is what this lane optimizes. The free window and the unreplicated read
 * ride along as context: teardown is not a target, and the fixed cost of a
 * parse is only visible before replication amortizes it away. */
export function caseRow(name, bytes, sha256, counts) {
    const row = { name, bytes, inputSha256: sha256, coreIr: counts.core };
    if (counts.coreFree !== undefined) row.coreFreeIr = counts.coreFree;
    if (counts.coreRaw !== undefined) {
        row.rawBytes = counts.rawBytes;
        row.coreRawIr = counts.coreRaw;
    }
    if (counts.cmark !== undefined) {
        row.cmarkIr = counts.cmark;
        row.ratio = row.cmarkIr > 0 ? row.coreIr / row.cmarkIr : null;
    }
    return row;
}

export function formatRow(row) {
    let line = `instructions case=${row.name} bytes=${row.bytes} core_ir=${row.coreIr}`;
    if (row.coreFreeIr !== undefined) line += ` core_free_ir=${row.coreFreeIr}`;
    if (row.coreRawIr !== undefined) line += ` raw_bytes=${row.rawBytes} core_raw_ir=${row.coreRawIr}`;
    if (row.cmarkIr !== undefined) {
        line += ` cmark_ir=${row.cmarkIr} ratio=${row.ratio === null ? "n/a" : row.ratio.toFixed(3)}`;
    }
    return line;
}

function parseArguments(argv) {
    const options = { workloads: [], reference: null, json: null, runner: null, samples: null };
    for (let i = 0; i < argv.length; i++) {
        const argument = argv[i];
        const value = () => {
            if (i + 1 >= argv.length) throw new Error(`${argument} needs a value`);
            return argv[++i];
        };
        if (argument === "--runner") options.runner = value();
        else if (argument === "--samples") options.samples = value();
        else if (argument === "--workload") options.workloads.push(value());
        else if (argument === "--reference") options.reference = value();
        else if (argument === "--json") options.json = value();
        else throw new Error(`unknown argument: ${argument}`);
    }
    if (!options.runner || !options.samples) {
        throw new Error(
            "usage: benchmark-instructions.mjs --runner PATH --samples DIR [--workload NAME]... [--reference cmark] [--json FILE]"
        );
    }
    if (options.reference && options.reference !== "cmark") throw new Error("the only reference is cmark");
    return options;
}

function runnerLines(runner, args) {
    return execFileSync(runner, args, { encoding: "utf8" }).split("\n").filter(Boolean);
}

/** One counted stage: callgrind collects only the window the runner opens
 * around the parse, or around the free, so nothing has to be subtracted back
 * out. LD_BIND_NOW resolves the shared library's lazily bound PLT entries at
 * load time -- otherwise the first call into it resolves them inside the
 * window, which is 3,542 instructions and 41.5% of an empty document's
 * parse, and which the statically linked reference never pays. */
function countRun(options, workload, name, implementation, stage, copies) {
    const args = [
        "--tool=callgrind",
        "--instr-atstart=no",
        "--callgrind-out-file=/dev/null",
        options.runner,
        "--workload",
        workload,
        "--case",
        name,
        "--samples",
        options.samples,
        "--instructions",
        "--implementation",
        implementation,
        "--stage",
        stage
    ];
    if (copies) args.push("--copies", String(copies));
    const result = spawnSync("valgrind", args, {
        encoding: "utf8",
        env: { ...process.env, LD_BIND_NOW: "1" }
    });
    if (result.error) throw new Error(`cannot run valgrind: ${result.error.message}`);
    if (result.status !== 0) throw new Error(`${name}: bench_runner failed under callgrind:\n${result.stderr}`);
    return { collected: parseCollected(result.stderr), report: parseInstructionsLine(result.stdout) };
}

function main() {
    const options = parseArguments(process.argv.slice(2));
    const workloads = options.workloads.length ? options.workloads : runnerLines(options.runner, ["--list"]);
    const report = { schema: 1, runtime: "c", lane: "instructions", reference: options.reference, workloads: [] };
    for (const workload of workloads) {
        const cases = runnerLines(options.runner, ["--list", "--workload", workload, "--samples", options.samples]);
        const rows = [];
        for (const name of cases) {
            const core = countRun(options, workload, name, "core", "parse", 0);
            const counts = {
                core: core.collected,
                coreFree: countRun(options, workload, name, "core", "free", 0).collected
            };
            /* The same case read at the size it has on disk. Only the sample
             * workload replicates a file, so only there does this differ. */
            const raw = countRun(options, workload, name, "core", "parse", 1);
            if (raw.report.bytes !== core.report.bytes) {
                counts.coreRaw = raw.collected;
                counts.rawBytes = raw.report.bytes;
            }
            if (options.reference) {
                counts.cmark = countRun(options, workload, name, "cmark", "parse", 0).collected;
            }
            const row = caseRow(name, core.report.bytes, core.report.sha256, counts);
            rows.push(row);
            console.log(`${formatRow(row)} workload=${workload}`);
        }
        report.workloads.push({ name: workload, cases: rows });
    }
    if (options.json) {
        fs.mkdirSync(path.dirname(options.json), { recursive: true });
        fs.writeFileSync(options.json, `${JSON.stringify(report, null, 2)}\n`);
    }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    try {
        main();
    } catch (error) {
        console.error(error.message);
        process.exit(1);
    }
}
