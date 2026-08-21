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
 * ONE EXCEPTION, taken at 0a.12b and narrow on purpose (Q40). A line of L bytes
 * has L+1 boundaries, and the last of them is where the line ending lives. So
 * column L+1 IS a place — but only for a node that IS a line ending, i.e. a
 * SoftBreak or a LineBreak. The general form of the rule was measured before it
 * was rejected: admitting L+1 for every kind would have excused TWELVE rows
 * already in this ledger (eleven Text, one Emphasis) that are wrong for other
 * reasons. The narrow form excuses none of them, because no break node was ever
 * registered here — before 0a.12b they all carried 0:0..0:0 and were deferred to
 * scripts/audit-scope-sanity.mjs.
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

/* Q40, and nothing wider: only these two kinds may sit on a line ending. */
const LINE_ENDING_KINDS = new Set(["SoftBreak", "LineBreak"]);

const fault = ([line, column], lengths, kind) => {
    if (line > lengths.length) return "off-line";
    if (column === 0) return "zero-column";
    if (column > lengths[line - 1]) {
        return LINE_ENDING_KINDS.has(kind) && column === lengths[line - 1] + 1 ? "place" : "off-column";
    }
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
        const start = fault(scope.start, lengths, node.kind);
        const end = fault(scope.end, lengths, node.kind);
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
