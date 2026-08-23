#!/usr/bin/env node
/**
 * D9: whether a reference resolves must not depend on how many resolved first.
 *
 * **This gate is REGISTERED RED and must stay red until Step 9b.** It is the
 * mdast backlog's shape: the ledger names what is wrong, and the gate fails
 * both when a registered row stops reproducing and when a new one appears. A
 * row clearing here means the model changed, which is a thing to announce, not
 * to discover.
 *
 * Resolving a reference COPIES the definition's destination and title into the
 * node, so `markdown_core_map_lookup` carries a running budget — `max(100000,
 * input size)` bytes summed over successful lookups — and simply stops
 * resolving once it is spent. Two properties die:
 *
 *   UNIFORM       N references to one label are identical, so they must all
 *                 resolve or none must. Under the budget the first k resolve
 *                 and the rest degrade to text, with k a function of the
 *                 destination's length.
 *   INDEPENDENT   `[b]` resolves in a two-line document. Prefix that document
 *                 with an unrelated `[a]` big enough to spend the budget and
 *                 the identical `[b]` becomes `Text literal="[b]"`. The
 *                 contamination crosses labels.
 *
 * **Deleting the budget is not the fix**, and that is the whole reason this is
 * pinned rather than repaired: with it gone, `reference_expansion_bound` in
 * `complexity_runner.c` measures 204.678x — 656 KB of input producing 134 MB of
 * copied destinations. The budget buys a linear output bound by breaking
 * resolution. A reference that NAMES its definition instead of copying it buys
 * both, and that is Step 9b's model change and nothing smaller. It said 9a
 * until Step 9a.2 measured that there is no 9a-shaped fix: the reference map
 * is freed with the parser and the document holds only the root, so a Link
 * that borrows a map entry's destination dangles. Deleting the copy IS the
 * node model.
 *
 *   node scripts/audit-reference-order-independence.mjs [--update] [--verbose]
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { parseCanonicalDump } from "./lib/upstream-cmark.mjs";
import { loadLedger, reconcileLedger, requireBinary, runBinary, walkWithPath } from "./lib/source-positions.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const LEDGER = "specs/reference-resolution/ledger.json";
const ledger = loadLedger(root, LEDGER);
const update = process.argv.includes("--update");
const verbose = process.argv.includes("--verbose");

const ours = requireBinary(root, "build/cmake/packages/markdown-core/core/markdown-core", "pnpm build:c");
const parse = (input) => parseCanonicalDump(runBinary(ours, ["--profile", ledger.profile], input));
const resolved = (tree, label) =>
    [...walkWithPath(tree)].filter(({ node }) => node.kind === "Link" && node.fields.destination === label).length;
const unresolved = (tree, label) =>
    [...walkWithPath(tree)].filter(({ node }) => node.kind === "Text" && node.fields.literal === label).length;

// A destination long enough that the 100 KB floor runs out inside a document
// small enough to read, and short enough that the whole corpus parses fast.
const DESTINATION = `/${"u".repeat(ledger.destinationLength - 1)}`;

const measured = [];

// UNIFORM: N identical references to one label.
{
    const count = ledger.uniformReferences;
    const input = `[a]: ${DESTINATION}\n\n${"[a]\n\n".repeat(count)}`;
    const tree = parse(input);
    const yes = resolved(tree, DESTINATION);
    const no = unresolved(tree, "[a]");
    if (yes !== count)
        measured.push({
            source: "uniform",
            input: `[a]: /u*${String(ledger.destinationLength - 1)} then ${String(count)} references to [a]`,
            findings: [{ property: "uniform", references: count, resolved: yes, degradedToText: no }]
        });
}

// INDEPENDENT: the same reference, with and without an unrelated prefix.
{
    const tail = "[b]: /short\n\n[b]\n";
    const alone = resolved(parse(tail), "/short");
    const prefix = `[a]: ${DESTINATION}\n\n${"[a]\n\n".repeat(ledger.contaminationReferences)}`;
    const contaminated = resolved(parse(prefix + tail), "/short");
    if (alone !== contaminated)
        measured.push({
            source: "independent",
            input: tail,
            findings: [
                {
                    property: "independent",
                    aloneResolves: alone,
                    afterUnrelatedPrefixResolves: contaminated,
                    prefixReferences: ledger.contaminationReferences
                }
            ]
        });
}

if (verbose) for (const entry of measured) process.stdout.write(`${entry.source}: ${JSON.stringify(entry.findings)}\n`);

reconcileLedger({
    root,
    ledgerPath: LEDGER,
    ledger,
    measured,
    update,
    subject: "reference order independence",
    scanned: 2
});
