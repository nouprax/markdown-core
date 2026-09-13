#!/usr/bin/env node
/** Checks editor positions against the native cmark coordinate contract.
 * The empty document's 1:1..0:0 sentinel is valid only for zero-byte input.
 * Ordinary coordinates retain the existing source-place checks. This audit
 * validates producers; bindings must never repair or convert their output.
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
const corpus = [
    { source: "native:zero-byte-document", input: "", args: [] },
    ...configuredFixtureCorpus(root),
    ...canonicalCorpus(root)
];
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
        if (
            example.input === "" &&
            node.kind === "Document" &&
            scope.start[0] === 1 &&
            scope.start[1] === 1 &&
            scope.end[0] === 0 &&
            scope.end[1] === 0
        )
            continue;
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
