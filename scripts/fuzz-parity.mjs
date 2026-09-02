#!/usr/bin/env node
/**
 * Differential parity fuzzing.
 *
 * The two parity gates compare this parser against external authorities over a
 * corpus people wrote. That corpus reaches the constructs someone thought to
 * write down; this reaches the ones nobody did — nesting, adjacency, and
 * truncation that hand-written examples do not produce.
 *
 * Generation is seeded and reproducible: a failure prints the seed and the
 * iteration that produced it, and re-running with `--seed` replays exactly the
 * same inputs. Randomness that could not be replayed would report defects
 * nobody can then look at.
 *
 *   node scripts/fuzz-parity.mjs [--oracle upstream|mdast] [--seed N]
 *                                [--iterations N] [--verbose]
 */

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { readExamples } from "./lib/fixture-corpus.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));

function argument(name, fallback) {
    const index = process.argv.indexOf(`--${name}`);
    return index >= 0 ? process.argv[index + 1] : fallback;
}
const oracleName = argument("oracle", "upstream");
const seed = Number(argument("seed", "1"));
const iterations = Number(argument("iterations", "300"));
const verbose = process.argv.includes("--verbose");

// xorshift64*, so a seed replays byte for byte across hosts and Node versions.
function prng(state) {
    let value = BigInt(state) || 1n;
    const mask = (1n << 64n) - 1n;
    return () => {
        value ^= value >> 12n;
        value = (value << 25n) & mask;
        value ^= value >> 27n;
        return Number(((value * 2685821657736338717n) & mask) >> 33n) / 2 ** 31;
    };
}

/**
 * Fragments are the lines of the corpora the parity gates already use, so the
 * generator produces material both parsers were built to handle rather than
 * arbitrary bytes. Arbitrary bytes are a different test — they belong to the
 * existing libFuzzer harnesses, which check that nothing crashes; this one
 * checks that two implementations agree, which needs inputs that mean
 * something to both.
 *
 * The list comes from the oracle's own policy rather than a copy kept here, so
 * widening a gate's corpus widens what the fuzzer recombines. A copy would
 * quietly stay narrow and still describe itself as the gate's corpus.
 */
function fragments(entries) {
    const seen = new Set();
    for (const entry of entries) {
        const file = typeof entry === "string" ? entry : entry.file;
        for (const example of readExamples(root, file)) {
            for (const line of example.input.split("\n")) {
                if (line.length && line.length < 80) seen.add(line);
            }
        }
    }
    return [...seen];
}

const ORACLES = {
    upstream: {
        policy: "specs/oracles/cmark-gfm/deltas.json",
        gate: "scripts/check-upstream-parity.mjs",
        // Upstream reads a task item's checked state with a substring search
        // for `[x]` over the whole line, so an unchecked item that acquires a
        // literal `[x]` through recombination reproduces the registered
        // `tasklist-checked-marker` difference — endlessly, and in inputs no
        // registry entry can name in advance. Checked items are exercised by
        // the corpus gate, which reads the fixtures unrecombined. `"title" ok`
        // is the corpus's one title-then-junk line: recombined under any
        // definition line it reproduces `refdef-title-rewind` wherever that
        // label is also referenced. `\\|` is the same shape for
        // `table-split-lead-spelling`: any fragment carrying an escaped
        // backslash before a pipe diverges the moment recombination parks it
        // above a header and delimiter row. A single `\|` decodes identically
        // on both sides and stays in the pool. `[^` is the newest and it is the
        // mdast oracle's reason arriving here: whether a footnote call resolves
        // depends on a definition elsewhere in the document, and since Step 9a
        // an UNRESOLVED call keeps its interior where upstream flattens it
        // (`footnote-failed-call-interior`). Recombination separates a call
        // from its definition by construction, so any line carrying one
        // diverges. Both sides are exercised unrecombined by the corpus gate.
        excludeFragments: ["[x]", "[X]", '"title" ok', "\\\\|", "[^"]
    },
    mdast: {
        policy: "specs/oracles/remark/deltas.json",
        gate: "scripts/check-mdast-parity.mjs",
        // See the note on `pool`. Dollar math: GitHub is the authority, not
        // remark. Footnote references: whether one resolves depends on a
        // definition elsewhere in the document, so a fragment that agrees in
        // its own corpus diverges the moment recombination separates it from
        // its definition. Bare URLs: recombined next to a directive label that
        // never closes, they reach `autolink-after-failed-label`. All three are
        // registered differences whose scope is wider than the one input each
        // entry names, which is what an input-keyed registry cannot express.
        excludeFragments: ["$", "[^", "://"]
    }
};
const oracle = ORACLES[oracleName];
if (!oracle) {
    process.stderr.write(`fuzz-parity: unknown oracle "${oracleName}"\n`);
    process.exit(2);
}

const policy = JSON.parse(fs.readFileSync(path.join(root, oracle.policy), "utf8"));
const registered = new Set((policy.expectedDivergences ?? []).map((entry) => entry.input));

// A registered divergence names one exact input, but the construct inside it
// diverges wherever it appears. Recombining corpus lines would therefore
// rediscover those constructs endlessly and report each recombination as new,
// which is noise rather than a finding. Lines drawn from a registered input
// are dropped from the pool so the generator explores only the space where the
// two implementations are supposed to agree.
const divergentLines = new Set();
for (const entry of policy.expectedDivergences ?? []) {
    for (const line of entry.input.split("\n")) if (line.length) divergentLines.add(line);
}
// A registry entry names one input, but some entries stand for a disagreement
// that is broader than the input they cite. Dollar-delimited math is the case:
// GitHub is the authority for `$...$` and `$$...$$`, not remark, and the two
// disagree about the construct generally rather than on three specific
// strings. Generating those fragments would rediscover that in every
// recombination, so the oracle's own exclusions say where it is authoritative.
const pool = fragments(policy.corpus ?? []).filter(
    (line) => !divergentLines.has(line) && !(oracle.excludeFragments ?? []).some((pattern) => line.includes(pattern))
);
if (pool.length === 0) {
    process.stderr.write("fuzz-parity: every corpus fragment was excluded; nothing would be generated.\n");
    process.exit(1);
}
const random = prng(seed);
const pick = (items) => items[Math.min(items.length - 1, Math.floor(random() * items.length))];

function generate() {
    const lines = [];
    const count = 1 + Math.floor(random() * 6);
    for (let i = 0; i < count; i++) {
        const fragment = pick(pool);
        const roll = random();
        if (roll < 0.15) lines.push(fragment.slice(0, 1 + Math.floor(random() * fragment.length)));
        else if (roll < 0.3) lines.push(`${fragment}${pick(pool)}`);
        else if (roll < 0.4) lines.push("");
        else lines.push(fragment);
    }
    return `${lines.join("\n")}\n`;
}

/**
 * Each candidate is checked by running the real gate over a one-example
 * fixture. Reusing the gate rather than reimplementing the comparison is what
 * keeps the fuzzer honest: it cannot drift into accepting something the gate
 * would reject, and every registered divergence and normalization applies
 * unchanged.
 */
// Per-run scratch. A shared directory works until two runs overlap — the CI
// job runs both oracles, and a seed sweep runs several at once — and then the
// first to finish deletes the directory the others are still writing into.
const scratch = path.join(root, `build/fuzz-parity/${oracleName}-${String(seed)}-${String(process.pid)}`);
fs.mkdirSync(scratch, { recursive: true });
const fixture = path.join(scratch, "candidate.md");
const fence = "`".repeat(32);

function diverges(input) {
    fs.writeFileSync(fixture, `${fence} example\n${input}.\nplaceholder\n${fence}\n`);
    try {
        execFileSync("node", [path.join(root, oracle.gate), "--corpus", path.relative(root, fixture)], {
            cwd: root,
            encoding: "utf8",
            stdio: "pipe"
        });
        return null;
    } catch (error) {
        return String(error.stdout ?? "") + String(error.stderr ?? "");
    }
}

const failures = [];
try {
    for (let iteration = 0; iteration < iterations; iteration++) {
        const input = generate();
        if (registered.has(input)) continue;
        const report = diverges(input);
        if (report) failures.push({ iteration, input, report });
        if (verbose && iteration % 50 === 0) {
            process.stdout.write(`  ${String(iteration)}/${String(iterations)}\n`);
        }
    }
} finally {
    fs.rmSync(scratch, { recursive: true, force: true });
}

process.stdout.write(
    `fuzz-parity [${oracleName}]: ${String(iterations - failures.length)}/${String(iterations)} generated inputs agree ` +
        `(seed ${String(seed)}, ${String(pool.length)} fragments)\n`
);

if (failures.length) {
    process.stderr.write(`\nfuzz-parity [${oracleName}] FAILED: ${String(failures.length)} input(s) diverge\n`);
    for (const failure of failures.slice(0, verbose ? failures.length : 3)) {
        process.stderr.write(`\n  iteration ${String(failure.iteration)} (replay: --seed ${String(seed)})\n`);
        process.stderr.write(`  ${JSON.stringify(failure.input)}\n`);
        process.stderr.write(failure.report.replace(/^/gm, "  "));
    }
    process.stderr.write(
        "\nA divergence is a defect or a deliberate difference; the parity registries decide which.\n"
    );
    process.exit(1);
}

process.stdout.write(`\nfuzz-parity [${oracleName}] passed.\n`);
