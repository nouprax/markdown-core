/**
 * D9: whether a reference resolves must not depend on how many resolved first.
 *
 * **This gate was REGISTERED RED until Step 9b.2 and is now GREEN with an empty
 * ledger.** It is still the mdast backlog's shape -- the ledger names what is
 * wrong, and the gate fails both when a registered row stops reproducing and
 * when a new one appears -- so an empty ledger is the strongest state it has,
 * not a retired one: a row appearing here fails the run.
 *
 * Resolving a reference USED TO COPY the definition's destination and title
 * into the node, so `markdown_core_map_lookup` carried a running budget --
 * `max(100000, input size)` bytes summed over successful lookups -- and simply
 * stopped resolving once it was spent. Two properties died:
 *
 *   UNIFORM       N references to one label are identical, so they must all
 *                 resolve or none must. Under the budget the first k resolved
 *                 and the rest degraded to text, with k a function of the
 *                 destination's length.
 *   INDEPENDENT   `[b]` resolves in a two-line document. Prefix that document
 *                 with an unrelated `[a]` big enough to spend the budget and
 *                 the identical `[b]` became `Text literal="[b]"`. The
 *                 contamination crossed labels.
 *
 * **Deleting the budget was never the fix**, which is why this was pinned
 * rather than repaired for so long: with it gone and nothing in its place,
 * `reference_expansion_bound` in `complexity_runner.c` measured 204.678x -- 656
 * KB of input producing 134 MB of copied destinations. The budget bought a
 * linear output bound by breaking resolution.
 *
 * A reference that NAMES its definition instead of copying it buys both, and
 * that is Step 9b.2's model change: `LinkReference` and `ImageReference` carry
 * an association and no destination, the map holds labels and no resource,
 * there is nothing to charge and no budget. `reference_expansion_bound` now
 * measures 0.399x on the same input while both properties below hold.
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
// A reference RESOLVED is a `LinkReference` naming that identifier; a reference
// that did not is prose, brackets intact. Neither is stated by a destination
// any more: the node carries none.
const resolved = (tree, identifier) =>
    [...walkWithPath(tree)].filter(({ node }) => node.kind === "LinkReference" && node.fields.identifier === identifier)
        .length;
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
    const yes = resolved(tree, "a");
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
    const alone = resolved(parse(tail), "b");
    const prefix = `[a]: ${DESTINATION}\n\n${"[a]\n\n".repeat(ledger.contaminationReferences)}`;
    const contaminated = resolved(parse(prefix + tail), "b");
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
