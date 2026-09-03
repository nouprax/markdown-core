#!/usr/bin/env node
/**
 * Position oracle (b): the tree's geometry must agree with itself.
 *
 * Two invariants, over every example in every spec fixture:
 *
 *   CONTAINMENT   a child's extent lies inside its parent's.
 *   NO OVERLAP    two siblings never claim the same byte.
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
 *   node scripts/audit-scope-containment.mjs [--update] [--verbose]
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { parseCanonicalDump } from "./lib/upstream-cmark.mjs";
import {
    before,
    fixtureCorpus,
    formatScope,
    loadLedger,
    onLineZero,
    readScope,
    reconcileLedger,
    requireBinary,
    runBinary,
    walkWithPath
} from "./lib/source-positions.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const LEDGER = "specs/positions/containment.json";
const ledger = loadLedger(root, LEDGER);
const update = process.argv.includes("--update");
const verbose = process.argv.includes("--verbose");

const ours = requireBinary(root, "build/cmake/packages/markdown-core/core/markdown-core", "pnpm build:c");

const measured = [];
let scanned = 0;
let skipped = 0;
for (const example of fixtureCorpus(root)) {
    const tree = parseCanonicalDump(runBinary(ours, ["--profile", ledger.profile], example.input));
    const findings = [];
    for (const { node, nodePath } of walkWithPath(tree)) {
        const parent = readScope(node);
        if (parent === null) continue;

        // Positioned children only, in document order, with their own index
        // kept so a finding names the node and not the survivor's rank.
        const children = node.children
            .map((child, index) => ({ child, index, scope: readScope(child) }))
            .filter(({ scope }) => scope !== null && !onLineZero(scope));

        if (!onLineZero(parent))
            for (const { child, index, scope } of children) {
                scanned += 1;
                if (before(scope.start, parent.start) || before(parent.end, scope.end))
                    findings.push({
                        nodePath: `${nodePath}.${String(index)}`,
                        violation: "containment",
                        kind: child.kind,
                        scope: formatScope(scope),
                        parentKind: node.kind,
                        parentScope: formatScope(parent)
                    });
            }
        else skipped += children.length;

        for (let index = 1; index < children.length; index += 1) {
            const left = children[index - 1];
            const right = children[index];
            scanned += 1;
            // Closed byte intervals: adjacent siblings end and start one column
            // apart, so anything short of strictly-before shares a byte.
            if (!before(left.scope.end, right.scope.start))
                findings.push({
                    nodePath: `${nodePath}.${String(right.index)}`,
                    violation: "sibling-overlap",
                    kind: right.child.kind,
                    scope: formatScope(right.scope),
                    previousKind: left.child.kind,
                    previousScope: formatScope(left.scope)
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
