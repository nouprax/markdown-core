#!/usr/bin/env node
/**
 * Position oracle (a): inline `Code` and `HTML` against upstream cmark-gfm.
 *
 * cmark-gfm is asked for `--to xml --sourcepos` over the CommonMark spec
 * fixture and its inline code and raw-HTML positions are compared with this
 * engine's. Those two kinds, and no others, for one reason each:
 *
 *   they are where the engine is KNOWN to be wrong. `adjust_subj_node_newlines`
 *   is the only code that walks a consumed span's line endings, it is gated
 *   behind an option nothing sets, and inline code and raw HTML are the two
 *   core constructs that can span a line. Every disagreement this reads today
 *   is a span that crossed a newline and was reported as if it had not.
 *
 *   upstream is a usable authority for them and NOT for the rest. cmark-gfm
 *   carries this repository's autolink column defect, its closing-bracket link
 *   start, its whole-run emphasis start, and its consumed-definition line;
 *   widening this oracle to every inline kind would make it go red on the
 *   commits that FIX those. Containment and place-ness are what watch them,
 *   and they need no outside authority.
 *
 * The comparison pairs the two sides' code/HTML nodes in document order. A
 * length mismatch is a hard error rather than a skipped example: the parity
 * gate already proves the two trees agree in shape over this corpus, so a
 * mismatch means one of the two parsers changed and this oracle is comparing
 * unrelated nodes.
 *
 *   node scripts/audit-inline-sourcepos.mjs [--update] [--verbose]
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { readExamples } from "./lib/fixture-corpus.mjs";
import { parseCanonicalDump, parseUpstreamXml } from "./lib/upstream-cmark.mjs";
import {
    formatScope,
    loadLedger,
    readScope,
    reconcileLedger,
    requireBinary,
    runBinary,
    walkWithPath
} from "./lib/source-positions.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const LEDGER = "specs/positions/inline-sourcepos.json";
const ledger = loadLedger(root, LEDGER);
const update = process.argv.includes("--update");
const verbose = process.argv.includes("--verbose");

// The upstream pin lives in one place, and it is the parity policy that states
// it. Copying the version into this ledger would let the two drift and read as
// agreement.
const upstreamVersion = loadLedger(root, "specs/oracles/cmark-gfm/deltas.json").upstream.version;

const ours = requireBinary(root, "build/cmake/packages/markdown-core/core/markdown-core", "pnpm build:c");
const upstream = requireBinary(
    root,
    `.tools/cmark-gfm/${upstreamVersion}/build/src/cmark-gfm`,
    "scripts/init-environment.sh --install upstream-cmark"
);

// The same five extensions and the same profile `check-upstream-parity.mjs`
// uses, so both sides parse one language. A position oracle that compared two
// different languages would report their difference as a position defect.
const UPSTREAM_FLAGS = ["table", "strikethrough", "autolink", "tasklist", "footnotes"].flatMap((extension) => [
    "-e",
    extension
]);

const SUBJECT = new Set(["Code", "HTML"]);
const collect = (tree) => [...walkWithPath(tree)].filter(({ node }) => SUBJECT.has(node.kind));

const UPSTREAM_SOURCEPOS = /^(\d+):(\d+)-(\d+):(\d+)$/;

const measured = [];
let scanned = 0;
for (const example of readExamples(root, ledger.corpus)) {
    const mine = collect(parseCanonicalDump(runBinary(ours, ["--profile", ledger.profile], example.input)));
    const theirs = collect(
        parseUpstreamXml(runBinary(upstream, ["--to", "xml", "--sourcepos", ...UPSTREAM_FLAGS], example.input))
    );
    if (mine.length !== theirs.length)
        throw new Error(
            `${example.source}: ${String(mine.length)} inline Code/HTML nodes here, ${String(theirs.length)} upstream — ` +
                `the two trees no longer pair, so no position comparison is meaningful. Check scripts/check-upstream-parity.mjs first.`
        );

    const findings = [];
    for (const [index, { node, nodePath }] of mine.entries()) {
        scanned += 1;
        const scope = readScope(node);
        const theirScope = UPSTREAM_SOURCEPOS.exec(theirs[index].node.fields.sourcepos ?? "");
        if (scope === null || theirScope === null)
            throw new Error(`${example.source}: a ${node.kind} carries no readable position on one of the two sides.`);
        const here = formatScope(scope);
        const there = `${theirScope[1]}:${theirScope[2]}..${theirScope[3]}:${theirScope[4]}`;
        if (here !== there) findings.push({ nodePath, kind: node.kind, ours: here, upstream: there });
    }
    if (findings.length > 0) measured.push({ source: example.source, input: example.input, findings });
}

if (verbose)
    for (const entry of measured)
        for (const finding of entry.findings)
            process.stdout.write(`${entry.source} ${finding.kind} ours=${finding.ours} upstream=${finding.upstream}\n`);

reconcileLedger({ root, ledgerPath: LEDGER, ledger, measured, update, subject: "inline sourcepos", scanned });
