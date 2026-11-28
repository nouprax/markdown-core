// The minimal test set: from the closure pool (`closure.mjs`), a smallest
// set of inputs, by greedy set cover, that reaches every transition the pool
// reaches; then the C parser and the Rust parser compared on it, past the
// registered departures.
//
// `node scripts/correctness/minimal.mjs [--pool FILE] [--out FILE]` writes
// the set, and lists the inputs where the parsers differ.
import { execFileSync } from "node:child_process";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { readExamples } from "../shared/fixture-corpus.mjs";
import { build, run } from "./cover.mjs";

const repo = fileURLToPath(new URL("../..", import.meta.url));
const flag = (name, fallback) => {
    const at = process.argv.indexOf(name);
    return at < 0 ? fallback : process.argv[at + 1];
};
const { closed, pool } = JSON.parse(readFileSync(flag("--pool", "build/closure/pool.json"), "utf8"));
const out = flag("--out", "build/closure/minimal.json");

const results = run(
    build(),
    pool.map((entry) => entry.input)
);

// Greedy set cover: take the input that reaches the most transitions not yet
// reached, the first such in pool order on a tie, until none is left. Its
// size is within a factor ln(n) + 1 of the smallest cover.
const left = new Set(results.flatMap(({ edges }) => [...edges]));
const total = left.size;
const chosen = [];
const unused = new Set(results.keys());
while (left.size) {
    let best = -1;
    let gain = 0;
    for (const index of unused) {
        let count = 0;
        for (const edge of results[index].edges) if (left.has(edge)) count += 1;
        if (count > gain) {
            gain = count;
            best = index;
        }
    }
    unused.delete(best);
    chosen.push(best);
    for (const edge of results[best].edges) left.delete(edge);
}
chosen.sort((a, b) => a - b);
console.log(
    `pool ${pool.length} inputs (${closed ? "closed" : "NOT closed"}), ${total} transitions; minimal set ${chosen.length} inputs`
);

// The inputs of the registered departures from C (`tests/departures.txt`).
const departures = new Set();
const ledger = readFileSync(join(repo, "packages/markdown-core-rs/tests/departures.txt"), "utf8");
for (const [, file, number] of ledger.matchAll(/^`{32} departure (\S+) (\d+)$/gm)) {
    const examples = readExamples(repo, `packages/markdown-core/tests/fixtures/${file}`, {
        includeExampleNumber: true
    });
    const found = examples.find((example) => example.example === Number(number));
    if (found) departures.add(found.input);
}

// Compare with C, one process per input, as the parity gates do.
const c = join(repo, "build/side/packages/markdown-core/core/markdown-core");
const differ = [];
for (const index of chosen) {
    const { input, dump } = results[index];
    const theirs = execFileSync(c, [], { input, encoding: "utf8", maxBuffer: 1 << 28 });
    if (theirs !== dump && !departures.has(input)) differ.push(input);
}
console.log(`C and Rust differ on ${differ.length} of ${chosen.length}, past the registered departures`);
for (const input of differ) console.log(JSON.stringify(input));

mkdirSync(dirname(out), { recursive: true });
writeFileSync(
    out,
    JSON.stringify({ closed, transitions: total, inputs: chosen.map((index) => results[index].input), differ }, null, 1)
);
