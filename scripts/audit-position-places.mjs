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
 * Three faults are rejected. `zero-column` is not one of them, because that
 * was this plan's rule and not the engine's: it said "there is no column 0",
 * the 57 rows it produced were closed at §4.14.11c2 by walking every such end
 * back to the previous line's last byte, and the walk is deleted with the
 * ruling. What upstream cmark-gfm reports — `code_block
 * sourcepos="3:5-4:0"` for an indented block closed by a blank line — is the
 * boundary form, and it is correct.
 *
 *   off-line     the line is not in the document at all.
 *   off-column   the column is past the LAST BOUNDARY of a line that exists,
 *                which is L+1 and not L.
 *   reversed     the scope's end precedes its start.
 *
 * Q40's narrow exception goes with the same ruling: it admitted column L+1 for
 * a `SoftBreak` and a `LineBreak` alone, on the reading that L+1 was a place
 * only for a node that IS a line ending. Under boundary semantics L+1 is the
 * last boundary of every line and needs no exception. The twelve rows that
 * reading was protecting (eleven `Text`, one `Emphasis`) are long gone — this
 * ledger has been EMPTY since §4.14.11c2 — so nothing is excused by widening
 * it, which is the same test 0a.12b applied and failed.
 *
 * Line zero and reversed scopes used to be owned by a separate shrinking
 * ledger. Its final row was the empty table cell at `3:6..3:5`; after the table
 * source-position producer was fixed, that ledger reached zero and was
 * removed. This fail-closed oracle now rejects both shapes directly.
 *
 *   node scripts/audit-position-places.mjs [--update] [--verbose]
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { parseCanonicalDump } from "./lib/upstream-cmark.mjs";
import {
    before,
    canonicalCorpus,
    configuredFixtureCorpus,
    INLINE_KINDS,
    formatScope,
    lineLengths,
    loadLedger,
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
    if (line < 1) return "off-line";
    if (column < 0) return "off-column";
    if (column === 0) return line >= 1 && line <= lengths.length + 1 ? "place" : "off-line";
    if (line > lengths.length) return "off-line";
    return column > lengths[line - 1] + 1 ? "off-column" : "place";
};

const measured = [];
const surveyed = { inline: 0, block: 0 };
let scanned = 0;
const corpus = [...configuredFixtureCorpus(root), ...canonicalCorpus(root)];
for (const example of corpus) {
    const tree = parseCanonicalDump(
        example.binary ? runBinary(example.binary, example.args) : runBinary(ours, example.args, example.input)
    );
    const lengths = lineLengths(example.input);
    const findings = [];
    for (const { node, nodePath } of walkWithPath(tree)) {
        const scope = readScope(node);
        if (scope === null) continue;
        surveyed[INLINE_KINDS.has(node.kind) ? "inline" : "block"] += 1;
        scanned += 1;
        const start = fault(scope.start, lengths);
        const end = fault(scope.end, lengths);
        const order = before(scope.end, scope.start) ? "reversed" : "ordered";
        if (start === "place" && end === "place" && order === "ordered") continue;
        findings.push({
            nodePath,
            kind: node.kind,
            phase: INLINE_KINDS.has(node.kind) ? "inline" : "block",
            scope: formatScope(scope),
            start,
            end,
            order
        });
    }
    if (findings.length > 0) measured.push({ source: example.source, input: example.input, findings });
}

if (verbose)
    for (const entry of measured)
        for (const finding of entry.findings)
            process.stdout.write(
                `${entry.source} ${finding.kind} scope=${finding.scope} start=${finding.start} end=${finding.end} ` +
                    `order=${finding.order}\n`
            );

const registered = { inline: 0, block: 0 };
for (const entry of measured) for (const finding of entry.findings) registered[finding.phase] += 1;
process.stdout.write(
    `  ${String(registered.inline)} of ${String(surveyed.inline)} inline nodes, ` +
        `${String(registered.block)} of ${String(surveyed.block)} block nodes.\n`
);
reconcileLedger({ root, ledgerPath: LEDGER, ledger, measured, update, subject: "position places", scanned });
