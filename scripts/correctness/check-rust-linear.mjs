// Parse cost of the Rust parser is linear in the size of the source, however
// the source nests or repeats one construct. Each shape of
// `examples/linear.rs` in the crate is parsed at its size and at four times
// it under callgrind, which counts the instructions of parsing and dropping
// the tree: instruction counts do not depend on the machine or its load. A
// linear cost counts about four times as many at the larger size, a square
// one sixteen; more than `LIMIT` times fails.
//
// `node scripts/correctness/check-rust-linear.mjs [shape...]`
import { execFile, execFileSync } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { availableParallelism, tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const run = promisify(execFile);
const repo = fileURLToPath(new URL("../..", import.meta.url));
const crate = join(repo, "packages/markdown-core-rs");
const target = process.env.CARGO_TARGET_DIR ?? join(crate, "target");
const LIMIT = 4.5;

execFileSync("cargo", ["build", "--release", "--quiet", "--example", "linear", "--target-dir", target], {
    cwd: crate,
    stdio: "inherit"
});
const binary = join(target, "release/examples/linear");
const only = new Set(process.argv.slice(2));
const shapes = execFileSync(binary, { encoding: "utf8" })
    .trim()
    .split("\n")
    .map((line) => line.split(" "))
    .map(([name, size]) => ({ name, size: Number(size) }))
    .filter(({ name }) => only.size === 0 || only.has(name));
const scratch = mkdtempSync(join(tmpdir(), "linear-"));

/// The instructions of `run` in the example for `name` of `size`.
async function count(name, size) {
    const { stderr } = await run(
        "valgrind",
        [
            "--tool=callgrind",
            `--callgrind-out-file=${join(scratch, `${name}-${size}.out`)}`,
            "--toggle-collect=linear::run",
            binary,
            name,
            String(size)
        ],
        { maxBuffer: 1 << 26 }
    );
    const found = /Collected : (\d+)/.exec(stderr);
    if (!found) throw new Error(`no count for ${name} ${size}:\n${stderr}`);
    return Number(found[1]);
}

const jobs = shapes.flatMap(({ name, size }) => [
    [name, size],
    [name, 4 * size]
]);
const counts = new Map();
let next = 0;
await Promise.all(
    Array.from({ length: Math.min(availableParallelism(), jobs.length) }, async () => {
        while (next < jobs.length) {
            const [name, size] = jobs[next++];
            counts.set(`${name} ${size}`, await count(name, size));
        }
    })
);
rmSync(scratch, { recursive: true, force: true });

let failed = 0;
for (const { name, size } of shapes) {
    const small = counts.get(`${name} ${size}`);
    const wide = counts.get(`${name} ${4 * size}`);
    const ratio = wide / small;
    const over = ratio > LIMIT;
    if (over) failed += 1;
    console.log(
        `${over ? "FAIL" : "ok  "} ${name.padEnd(24)} ${String(small).padStart(12)} Ir at ${size}, ` +
            `${String(wide).padStart(13)} at ${4 * size}: ${ratio.toFixed(2)}x`
    );
}
if (failed > 0) {
    console.error(`${failed} of ${shapes.length} shapes cost more than ${LIMIT}x at four times the size`);
    process.exit(1);
}
console.log(`${shapes.length} shapes parse in linear time (at most ${LIMIT}x at four times the size)`);
