#!/usr/bin/env node
/**
 * Position oracle (b): the tree's geometry must agree with itself.
 *
 * Two invariants, over every example in every spec fixture:
 *
 *   CONTAINMENT   each of a child's source ranges lies inside one of its
 *                 parent's.
 *   NO OVERLAP    two siblings never claim the same byte, and each starts
 *                 after the one before it starts.
 *
 * The second half is the one that earns this file. Containment alone cannot
 * see the emphasis defect at all — `***a**` yields a `Text "*"` spanning
 * columns 1..3 and a `Strong` starting at column 1, and both sit happily
 * inside their paragraph. Two nodes owning one byte is not a containment
 * violation; it is a statement that the same input was consumed twice, and
 * nothing in the repository could say so before this.
 *
 * This cannot be an upstream comparison. cmark-gfm builds emphasis by the same
 * after-the-fact re-parenting and produces the same overlap, and it takes a
 * link's start from the closing bracket the same way, so asking it would
 * return agreement on exactly the rows that are wrong.
 *
 * Nodes with no position are skipped rather than judged here. The fail-closed
 * position-place oracle rejects line-zero and reversed scopes; comparing one
 * of those as geometry would only duplicate that finding.
 *
 *   node scripts/audit/check-scope-containment.mjs [--update] [--verbose]
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { parseCanonicalDump } from "../shared/upstream-cmark.mjs";
import {
    before,
    fixtureCorpus,
    formatScopes,
    loadLedger,
    onLineZero,
    readScopes,
    reconcileLedger,
    requireBinary,
    runBinary,
    walkWithPath
} from "./source-positions.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const LEDGER = "specs/positions/containment.json";
const ledger = loadLedger(root, LEDGER);
const update = process.argv.includes("--update");
const verbose = process.argv.includes("--verbose");

const ours = requireBinary(root, "build/cmake/packages/markdown-core/core/markdown-core", "pnpm build:c");

// Dump groups have no scope of their own. Their rows/affixes still belong
// geometrically to the nearest scoped owner, including across group boundaries.
function* positionedChildren(node, nodePath) {
    for (const [index, child] of node.children.entries()) {
        const childPath = `${nodePath}.${String(index)}`;
        const scopes = readScopes(child);
        if (scopes === null) yield* positionedChildren(child, childPath);
        else if (!onLineZero(scopes)) yield { child, childPath, scopes };
    }
}

// Closed byte intervals; an empty range (its end before its start) names a
// place, not a byte.
const holds = (outer, inner) => !before(inner.start, outer.start) && !before(outer.end, inner.end);
const empty = (scope) => before(scope.end, scope.start);
const share = (left, right) =>
    !empty(left) && !empty(right) && !before(left.end, right.start) && !before(right.end, left.start);

const measured = [];
let scanned = 0;
let skipped = 0;
for (const example of fixtureCorpus(root)) {
    const tree = parseCanonicalDump(runBinary(ours, [], example.input));
    const findings = [];
    for (const { node, nodePath } of walkWithPath(tree)) {
        const parent = readScopes(node);
        if (parent === null) continue;

        const children = [...positionedChildren(node, nodePath)];

        if (!onLineZero(parent))
            for (const { child, childPath, scopes } of children) {
                scanned += 1;
                if (!scopes.every((scope) => parent.some((outer) => holds(outer, scope))))
                    findings.push({
                        nodePath: childPath,
                        violation: "containment",
                        kind: child.kind,
                        scope: formatScopes(scopes),
                        parentKind: node.kind,
                        parentScope: formatScopes(parent)
                    });
            }
        else skipped += children.length;

        // Document metadata and content are separate ownership edges, printed
        // in field order rather than source order.
        const sequences =
            node.kind === "Document"
                ? [
                      children.filter(({ child }) => child.kind === "Metadata"),
                      children.filter(({ child }) => child.kind !== "Metadata")
                  ]
                : node.kind === "Table"
                  ? [
                        children.filter(({ child }) => child.kind === "TableCaption"),
                        children.filter(({ child }) => child.kind !== "TableCaption")
                    ]
                  : [children];
        for (const siblings of sequences)
            for (let index = 1; index < siblings.length; index += 1) {
                const left = siblings[index - 1];
                const right = siblings[index];
                scanned += 1;
                if (
                    !before(left.scopes[0].start, right.scopes[0].start) ||
                    left.scopes.some((scope) => right.scopes.some((other) => share(scope, other)))
                )
                    findings.push({
                        nodePath: right.childPath,
                        violation: "sibling-overlap",
                        kind: right.child.kind,
                        scope: formatScopes(right.scopes),
                        previousKind: left.child.kind,
                        previousScope: formatScopes(left.scopes)
                    });
            }
    }
    if (findings.length > 0) measured.push({ source: example.source, input: example.input, findings });
}

if (verbose)
    for (const entry of measured)
        for (const finding of entry.findings)
            process.stdout.write(
                `${entry.source} ${finding.violation} ${finding.kind} ${finding.scope} vs ` +
                    `${finding.parentKind ?? finding.previousKind} ${finding.parentScope ?? finding.previousScope}\n`
            );

process.stdout.write(`  ${String(skipped)} child relations skipped: the parent has no position.\n`);
reconcileLedger({ root, ledgerPath: LEDGER, ledger, measured, update, subject: "scope containment", scanned });
