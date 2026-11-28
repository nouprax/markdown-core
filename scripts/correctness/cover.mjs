// Transition coverage of the Rust parser, for a set of inputs: which steps of
// its state machine run after which. A step decides its successor, so the
// pairs are the branches of the machine.
//
// `node scripts/correctness/cover.mjs` reports the coverage of the fixtures;
// with `--check`, it fails unless the fixtures reach exactly the transitions
// of the closure (`tests/edges.txt` in the crate, written by `minimal.mjs`).
import { Buffer } from "node:buffer";
import { execFile, execFileSync } from "node:child_process";
import { promisify } from "node:util";
import { existsSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { readExamples } from "../shared/fixture-corpus.mjs";

const repo = fileURLToPath(new URL("../..", import.meta.url));
const crate = join(repo, "packages/markdown-core-rs");
const target = join(process.env.CARGO_TARGET_DIR ?? join(crate, "target"), "cover");

/// Build the parser with coverage and return the path of its binary.
export function build() {
    execFileSync("cargo", ["build", "--release", "--quiet", "--features", "cover", "--target-dir", target], {
        cwd: crate,
        stdio: "inherit"
    });
    return join(target, "release/markdown-core");
}

/// The records of a batch: a byte count, a line ending, and that many bytes.
function records(out) {
    const found = [];
    let at = 0;
    while (at < out.length) {
        const line = out.indexOf(10, at);
        const size = Number(out.subarray(at, line).toString());
        found.push(out.subarray(line + 1, line + 1 + size).toString());
        at = line + 1 + size;
    }
    return found;
}

function batch(inputs) {
    return Buffer.concat(
        inputs.map((input) => {
            const bytes = Buffer.from(input, "utf8");
            return Buffer.concat([Buffer.from(`${bytes.length}\n`), bytes]);
        })
    );
}

/// Every transition seen, `last|step`, by number: results hold the
/// numbers, so that many fit in memory.
export const transitions = [];
const numbers = new Map();
function number(edge) {
    let found = numbers.get(edge);
    if (found === undefined) {
        found = transitions.length;
        transitions.push(edge);
        numbers.set(edge, found);
    }
    return found;
}

/// The results of a batch run on `inputs`: for each, its dump and the set of
/// its transitions, by number.
function results(inputs, out) {
    const found = records(out);
    const names = [];
    // Rows repeat across the inputs of a batch, whose step numbers they share.
    const rows = new Map();
    return inputs.map((input, index) => {
        const edges = new Set();
        for (const row of found[2 * index + 1].split("\n")) {
            if (!row) continue;
            let edge = rows.get(row);
            if (edge === undefined) {
                const [last, step] = row.split("\t");
                if (last.startsWith("=")) {
                    names[Number(last.slice(1))] = step;
                    continue;
                }
                edge = number(`${last === "-" ? "" : names[Number(last)]}|${names[Number(step)]}`);
                rows.set(row, edge);
            }
            edges.add(edge);
        }
        return { input, dump: found[2 * index], edges };
    });
}

/// Parse each of `inputs`; return, for each, its dump and the set of its
/// transitions.
export function run(binary, inputs) {
    const dir = mkdtempSync(join(tmpdir(), "cover-"));
    try {
        const file = join(dir, "inputs");
        writeFileSync(file, batch(inputs));
        return results(inputs, execFileSync(binary, ["--batch", file], { maxBuffer: 1 << 30 }));
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
}

const execute = promisify(execFile);

/// As [`run`][], in a process of its own, so that several run at once.
export async function start(binary, inputs) {
    const dir = mkdtempSync(join(tmpdir(), "cover-"));
    try {
        const file = join(dir, "inputs");
        writeFileSync(file, batch(inputs));
        try {
            const { stdout } = await execute(binary, ["--batch", file], { maxBuffer: 1 << 30, encoding: "buffer" });
            return results(inputs, stdout);
        } catch {
            // Some input crashed the parser: run them one at a time to find it.
            return inputs.map((input) => {
                try {
                    return run(binary, [input])[0];
                } catch (error) {
                    return { input, dump: null, edges: new Set(), crash: String(error.stderr ?? error).trim() };
                }
            });
        }
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
}

/// Every step of the state machine, as `Name(Step)`: the variants of the
/// `Step` enum of each module that `state::Name` names.
export function steps() {
    const src = join(crate, "src");
    const state = readFileSync(join(src, "state.rs"), "utf8");
    const start = state.indexOf("pub enum Name {");
    const names = state.slice(start, state.indexOf("\n}", start));
    const out = [];
    for (const [, name, path] of names.matchAll(/^\s+(\w+)\(([\w:]+)::Step\)/gm)) {
        const module = join(src, path.replace(/^crate::/, "").replaceAll("::", "/"));
        const file = existsSync(`${module}.rs`) ? `${module}.rs` : join(module, "mod.rs");
        const text = readFileSync(file, "utf8");
        const body = /pub enum Step \{([\s\S]*?)\n\}/.exec(text)[1].replace(/\/\/.*$/gm, "");
        for (const [, variant] of body.matchAll(/^ {4}([A-Z]\w*)/gm)) out.push(`${name}(${variant})`);
    }
    return out;
}

/// The `Name(Step)` of a recorded step, without what its step carries.
export const step = (recorded) => /^\w+\(\w+/.exec(recorded)?.[0].concat(")") ?? recorded;

/// The transitions of the closure, one `last|step` a line.
export const record = join(crate, "tests/edges.txt");

/// The inputs of every fixture example, but those of the files `skip` names.
export function fixtures(skip = []) {
    const dir = "packages/markdown-core/tests/fixtures";
    return readdirSync(join(repo, dir))
        .filter((name) => name.endsWith(".txt") && !skip.includes(name))
        .flatMap((name) => readExamples(repo, join(dir, name)).map((example) => example.input));
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
    const binary = build();
    const inputs = fixtures();
    const results = run(binary, inputs);
    const all = new Set();
    const reached = new Set();
    for (const { edges } of results) {
        for (const edge of edges) {
            all.add(edge);
            reached.add(step(transitions[edge].split("|")[1]));
        }
    }
    const known = steps();
    const missed = known.filter((name) => !reached.has(name));
    console.log(
        `${inputs.length} fixture inputs: ${known.length - missed.length}/${known.length} steps, ${all.size} transitions`
    );
    console.log(`steps no fixture reaches: ${missed.join(" ")}`);
    if (process.argv.includes("--check")) {
        const recorded = new Set(readFileSync(record, "utf8").split("\n").filter(Boolean));
        const seen = new Set([...all].map((edge) => transitions[edge]));
        const lost = [...recorded].filter((edge) => !seen.has(edge));
        const fresh = [...seen].filter((edge) => !recorded.has(edge));
        for (const edge of lost) console.log(`no fixture reaches ${edge}`);
        for (const edge of fresh) console.log(`not in the closure: ${edge}`);
        if (lost.length || fresh.length) {
            console.log(
                "The fixtures must reach exactly the closure's transitions. A parser change that moves them\n" +
                    "needs the closure again: node scripts/correctness/closure.mjs, then\n" +
                    "node scripts/correctness/minimal.mjs --write, and the new dumps reviewed."
            );
            process.exit(1);
        }
        console.log(`the fixtures reach the ${recorded.size} transitions of the closure`);
    }
}
