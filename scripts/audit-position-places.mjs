#!/usr/bin/env node
/**
 * Position oracle (c): a position must be a place.
 *
 * The engine's positions are (line, column) pairs counted in BYTES from 1, so
 * a coordinate is a place exactly when its line exists in the input and its
 * column names a byte on that line. Nothing checked that, and it is the
 * cheapest possible statement about a position: it needs no authority, no
 * second parser and no model of what the node ought to cover. It is also the
 * only one of the three oracles that reads the INPUT, which is why it catches
 * defects the other two cannot — a consumed span that crossed a line ending
 * and was reported on the start line lands past the end of that line, and both
 * the tree's geometry and upstream agree with it.
 *
 * Three faults, and the split matters because they have different owners:
 *
 *   off-line     the line is not in the document at all.
 *   off-column   the column is past the end of a line that exists.
 *   zero-column  column 0 on a line that exists. There is no column 0; it is
 *                how "ends at the line ending" and "covers nothing" get
 *                spelled when the only vocabulary available is a coordinate.
 *
 * What this oracle deliberately does NOT count is a coordinate on line zero.
 * `scripts/audit-scope-sanity.mjs` owns those — the `0:0..0:0` sentinel and
 * the line-zero-with-a-column shape — and a row two ratchets both claim can be
 * cleared from each by moving to the other.
 *
 *   node scripts/audit-position-places.mjs [--update] [--verbose]
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { parseCanonicalDump } from "./lib/upstream-cmark.mjs";
import {
    INLINE_KINDS,
    fixtureCorpus,
    formatScope,
    lineLengths,
    loadLedger,
    onLineZero,
    readScope,
    reconcileLedger,
    requireBinary,
    runBinary,
    walkWithPath
} from "./lib/source-positions.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const LEDGER = "specs/positions/places.json";
const ledger = loadLedger(root, LEDGER);
const update = process.argv.includes("--update");
const verbose = process.argv.includes("--verbose");

const ours = requireBinary(root, "build/cmake/packages/markdown-core/core/markdown-core", "pnpm build:c");

const fault = ([line, column], lengths) => {
    if (line > lengths.length) return "off-line";
    if (column === 0) return "zero-column";
    if (column > lengths[line - 1]) return "off-column";
    return "place";
};

const measured = [];
const surveyed = { inline: 0, block: 0 };
let scanned = 0;
let deferred = 0;
for (const example of fixtureCorpus(root)) {
    const tree = parseCanonicalDump(runBinary(ours, ["--profile", ledger.profile], example.input));
    const lengths = lineLengths(example.input);
    const findings = [];
    for (const { node, nodePath } of walkWithPath(tree)) {
        const scope = readScope(node);
        if (scope === null) continue;
        surveyed[INLINE_KINDS.has(node.kind) ? "inline" : "block"] += 1;
        if (onLineZero(scope)) {
            deferred += 1;
            continue;
        }
        scanned += 1;
        const start = fault(scope.start, lengths);
        const end = fault(scope.end, lengths);
        if (start === "place" && end === "place") continue;
        findings.push({
            nodePath,
            kind: node.kind,
            phase: INLINE_KINDS.has(node.kind) ? "inline" : "block",
            scope: formatScope(scope),
            start,
            end
        });
    }
    if (findings.length > 0) measured.push({ source: example.source, input: example.input, findings });
}

if (verbose)
    for (const entry of measured)
        for (const finding of entry.findings)
            process.stdout.write(
                `${entry.source} ${finding.kind} scope=${finding.scope} start=${finding.start} end=${finding.end}\n`
            );

const registered = { inline: 0, block: 0 };
for (const entry of measured) for (const finding of entry.findings) registered[finding.phase] += 1;
process.stdout.write(
    `  ${String(registered.inline)} of ${String(surveyed.inline)} inline nodes, ` +
        `${String(registered.block)} of ${String(surveyed.block)} block nodes; ` +
        `${String(deferred)} scopes deferred to scripts/audit-scope-sanity.mjs (a coordinate on line zero).\n`
);
reconcileLedger({ root, ledgerPath: LEDGER, ledger, measured, update, subject: "position places", scanned });
