#!/usr/bin/env node
/**
 * Position oracle (c): a position must be a place.
 *
 * OWNER RULING, 2026-08-24, and it is what this oracle now checks:
 *
 *   "A scope's line and column do not stand for any source subrange, and no
 *    subrange can be taken with them. What they are for is telling an editor
 *    which line-and-column range this element occupies. So they are
 *    BOUNDARIES, not the byte range you took them for."
 *
 * A SCOPE IS A BOUNDARY PAIR, not a byte range. It tells an editor which
 * (line, column) rectangle an element occupies; it is not something a consumer
 * takes a substring with, and it never was. So a line of L bytes has L+1
 * boundaries on it, column 0 is the boundary before a line's first — which is
 * how "ended at the end of the line above" is spelled — and both are places.
 *
 * ~~Three faults~~ TWO, because `zero-column` was this plan's rule and not the
 * engine's: it said "there is no column 0", the 57 rows it produced were closed
 * at §4.14.11c2 by walking every such end back to the previous line's last
 * byte, and the walk is deleted with the ruling. What upstream cmark-gfm
 * reports — `code_block sourcepos="3:5-4:0"` for an indented block closed by a
 * blank line — is the boundary form, and it is correct.
 *
 *   off-line     the line is not in the document at all.
 *   off-column   the column is past the LAST BOUNDARY of a line that exists,
 *                which is L+1 and not L.
 *
 * Q40's narrow exception goes with the same ruling: it admitted column L+1 for
 * a `SoftBreak` and a `LineBreak` alone, on the reading that L+1 was a place
 * only for a node that IS a line ending. Under boundary semantics L+1 is the
 * last boundary of every line and needs no exception. The twelve rows that
 * reading was protecting (eleven `Text`, one `Emphasis`) are long gone — this
 * ledger has been EMPTY since §4.14.11c2 — so nothing is excused by widening
 * it, which is the same test 0a.12b applied and failed.
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

/* A line of L bytes carries boundaries 1 through L+1. Column 0 is the boundary
 * before a line's first, and an END there says the element stopped at the end
 * of the line above — so it is a place on any line the document could reach,
 * including the one past the last. */
const fault = ([line, column], lengths) => {
    if (column === 0) return line >= 1 && line <= lengths.length + 1 ? "place" : "off-line";
    if (line > lengths.length) return "off-line";
    return column > lengths[line - 1] + 1 ? "off-column" : "place";
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
