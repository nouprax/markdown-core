#!/usr/bin/env node
/**
 * Stage 1 box one, the measurable half: WHEN LINE i+1 ARRIVES, HOW FAR BACK
 * INTO THE ALREADY-EMITTED TREE DOES THE CHANGE REACH?
 *
 * §0 claims three things about per-line pausable state. Two of them are
 * measurable today, with no engine change, because a one-shot parse of lines
 * 1…i is exactly what a snapshot at boundary i must equal:
 *
 *   "the state after N lines IS the snapshot"  -> is dump(1…i) a prefix of
 *                                                 dump(1…i+1), or does the
 *                                                 append rewrite history?
 *   "the tail is the only unresolved thing"    -> how MANY dump lines does one
 *                                                 append disturb, and how far
 *                                                 from the end do they start?
 *
 * REACH is the metric: the number of already-emitted dump lines, counted back
 * from the end of dump(1…i), before the first line that the append changed.
 * Reach 0 means the append only added. Reach 3 means it rewrote the last three
 * nodes. Reach = the whole dump means it rewrote the document.
 *
 * The third claim — O(line) per append — is a timing question and is NOT
 * measured here; this is the correctness half.
 */

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { readExamples } from "../lib/fixture-corpus.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const cli = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), "tail-reach-"));
process.on("exit", () => fs.rmSync(scratch, { recursive: true, force: true }));

function dump(text, profile) {
    const file = path.join(scratch, "in.md");
    fs.writeFileSync(file, text);
    try {
        // THE TRAILING EMPTY LINE IS DROPPED, and the third cut of this probe
        // did not drop it: `split("\n")` leaves `""` at the end of every dump,
        // so any append that adds a node displaces that `""` and scores a
        // reach of at least 1. It reported 64% of appends at reach 1-2, which
        // was that empty string and nothing else.
        const out = execFileSync(cli, ["--profile", profile, file], { encoding: "utf8" }).split("\n");
        if (out.at(-1) === "") out.pop();
        return out;
    } catch {
        return null;
    }
}

/**
 * `scope=` AND `children=` ARE STRIPPED BEFORE COMPARING, and the first cut of
 * this probe did not strip them, which made every reading useless: the dump's
 * first line is `Document scope=1:1..N:M`, so appending any line grows the root
 * scope, the very first line differs, and reach is the whole dump every time.
 * It reported 0.00% append-only over 1,304 appends, which is an artifact and
 * not a finding.
 *
 * Scope ends and child counts on the OPEN SPINE necessarily move when the
 * document grows — that is O(depth) and the model predicts it. What the model
 * forbids is an already-emitted node changing KIND or CONTENT, so that is what
 * is compared. The spine churn is counted separately below.
 *
 * THE TREE-DRAWING CONNECTORS ARE STRIPPED FOR THE SAME REASON, and the second
 * cut of this probe did not strip them: `└── Text` becomes `├── Text` the
 * moment a sibling is added after it, so a purely additive append reported a
 * rewrite of every node it made non-last. Depth is kept — it is structure —
 * and the connector glyphs are not.
 */
const PREFIX = /^[\u2502\u251c\u2514\u2500 ]*/u;
const shape = (line) => {
    const prefix = PREFIX.exec(line)?.[0] ?? "";
    const depth = Math.round(prefix.length / 4);
    const body = line
        .slice(prefix.length)
        .replace(/ scope=\S+/g, "")
        .replace(/ children=\d+/g, "");
    return `${String(depth)}:${body}`;
};

/** Lines of `before` whose SHAPE the append changed, counted back from its end. */
function reach(before, after) {
    const a = before.map(shape);
    const b = after.map(shape);
    let common = 0;
    while (common < a.length && common < b.length && a[common] === b[common]) common++;
    return a.length - common;
}

/** Lines whose shape is identical but whose scope or child count moved. */
function spineChurn(before, after) {
    let churn = 0;
    for (let i = 0; i < Math.min(before.length, after.length); i++) {
        if (shape(before[i]) === shape(after[i]) && before[i] !== after[i]) churn++;
    }
    return churn;
}

function measure(text, profile) {
    const lines = text.split("\n");
    if (lines.at(-1) === "") lines.pop();
    const rows = [];
    let previous = null;
    for (let i = 1; i <= lines.length; i++) {
        const current = dump(`${lines.slice(0, i).join("\n")}\n`, profile);
        if (!current) return null;
        if (previous) {
            const r = reach(previous, current);
            const from = previous.length - r;
            rows.push({
                line: i,
                reach: r,
                churn: spineChurn(previous, current),
                emitted: previous.length,
                changed: [...previous.slice(from).map(shape), "=>", ...current.slice(from, from + r + 1).map(shape)]
            });
        }
        previous = current;
    }
    return rows;
}

/**
 * DOES THE REACH GROW WITH THE DOCUMENT? The corpus answers "which constructs",
 * and every fixture example is small, so it cannot answer "how far". These two
 * do, and they are the ones that decide criterion 2.
 */
function growth(profile) {
    const rows = [];
    for (const n of [10, 50, 200, 800, 3200]) {
        const body = `See [foo] here.\n\n${"filler paragraph text\n\n".repeat(n)}`;
        const before = dump(body, profile);
        const after = dump(`${body}[foo]: /url\n`, profile);
        rows.push({
            kind: "late reference definition",
            size: n * 2 + 2,
            reach: reach(before, after),
            emitted: before.length
        });
    }
    for (const n of [1, 4, 16, 64, 256]) {
        const body = `${Array.from({ length: n }, () => "Foo").join("\n")}\n`;
        const before = dump(body, profile);
        const after = dump(`${body}===\n`, profile);
        rows.push({ kind: "setext underline", size: n, reach: reach(before, after), emitted: before.length });
    }
    return rows;
}

const profile = process.argv.includes("--gfm-extended") ? "gfm-extended" : "gfm";
const files = [
    "packages/markdown-core/tests/fixtures/spec.txt",
    "packages/markdown-core/tests/fixtures/regression.txt",
    "packages/markdown-core/tests/fixtures/extensions.txt"
];

const buckets = new Map();
const worst = [];
const samples = [];
let appends = 0;
let additive = 0;
let churnTotal = 0;
let churnMax = 0;

for (const file of files) {
    for (const example of readExamples(root, file)) {
        const rows = measure(example.input, profile);
        if (!rows) continue;
        for (const row of rows) {
            appends++;
            if (row.reach === 0) additive++;
            churnTotal += row.churn;
            if (row.churn > churnMax) churnMax = row.churn;
            const key =
                row.reach === 0
                    ? "0 (append only)"
                    : row.reach <= 2
                      ? "1-2"
                      : row.reach <= 8
                        ? "3-8"
                        : row.reach <= 32
                          ? "9-32"
                          : ">32";
            buckets.set(key, (buckets.get(key) ?? 0) + 1);
            if (row.reach > 0 && row.reach <= 2 && samples.length < 8) {
                samples.push({ at: `${example.source} line ${String(row.line)}`, changed: row.changed });
            }
            if (row.reach > 2) {
                worst.push({
                    reach: row.reach,
                    emitted: row.emitted,
                    at: `${example.source} line ${String(row.line)}`,
                    input: example.input
                });
            }
        }
    }
}

process.stdout.write(`stage1 tail reach [--profile ${profile}] over ${String(appends)} single-line appends\n\n`);
process.stdout.write(`  append-only (reach 0): ${String(additive)} = ${((100 * additive) / appends).toFixed(2)}%\n`);
process.stdout.write(
    `  spine churn (scope/children moved, shape identical): mean ${(churnTotal / appends).toFixed(2)}, max ${String(churnMax)}\n`
);
for (const key of ["0 (append only)", "1-2", "3-8", "9-32", ">32"]) {
    if (buckets.has(key)) process.stdout.write(`  reach ${key.padEnd(16)} ${String(buckets.get(key))}\n`);
}

process.stdout.write(`\n  what a reach of 1-2 actually is:\n`);
for (const sample of samples) {
    process.stdout.write(`    ${sample.at}\n      ${sample.changed.join("  |  ").slice(0, 150)}\n`);
}

worst.sort((a, b) => b.reach - a.reach);
process.stdout.write(`\n  deepest reaches (reach / emitted-lines / where):\n`);
for (const row of worst.slice(0, 12)) {
    const first = row.input.split("\n")[0].slice(0, 44);
    process.stdout.write(
        `    ${String(row.reach).padStart(4)} / ${String(row.emitted).padStart(4)}  ${row.at.padEnd(38)} ${JSON.stringify(first)}\n`
    );
}

process.stdout.write(`\n  does the reach GROW with size? (reach counts from the first changed node to the end)\n`);
process.stdout.write(
    `    ${"construct".padEnd(28)} ${"size".padStart(6)} ${"reach".padStart(7)} ${"emitted".padStart(8)}\n`
);
for (const row of growth(profile)) {
    process.stdout.write(
        `    ${row.kind.padEnd(28)} ${String(row.size).padStart(6)} ${String(row.reach).padStart(7)} ${String(row.emitted).padStart(8)}\n`
    );
}
