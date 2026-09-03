/**
 * Shared machinery for the three position oracles.
 *
 * A source position in this engine is a (line, column) pair counted in BYTES
 * from 1, and the canonical dump prints one closed interval of them per node
 * as `scope=L:C..L:C`. Nothing in the repository checked that those numbers
 * name anything: the golden dumps assert them, and both parity gates compare
 * structure and text while ignoring position entirely. A well-formed but
 * wrong position therefore sails through those gates — which is how a
 * position landing four columns past the end of its own line came to be
 * asserted as expected output.
 *
 * The three oracles built on this module ask three different questions, and
 * the split is deliberate:
 *
 *   audit-inline-sourcepos    does an authority outside this repository agree?
 *   audit-scope-containment   is the tree's geometry consistent with itself?
 *   audit-position-places     are both coordinates valid and ordered?
 *
 * None subsumes another. Upstream cmark-gfm carries several of the same
 * position defects, so it cannot be the authority for them; containment is
 * blind to a whole subtree shifted by the same amount; and a coordinate can be
 * a real byte while sitting in the wrong place.
 */

import { execFileSync } from "node:child_process";
import { Buffer } from "node:buffer";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";

import { readExamples } from "./fixture-corpus.mjs";

/**
 * The thirteen kinds `markdown_core_node_kind_name` gives an inline node.
 * Kept here rather than derived, because the derivation would need the engine
 * and the point of these oracles is to judge the engine.
 */
export const INLINE_KINDS = new Set([
    "Text",
    "SoftBreak",
    "LineBreak",
    "Code",
    "HTML",
    "Formula",
    "Emphasis",
    "Strong",
    "Strikethrough",
    "Link",
    "Image",
    "Directive",
    "FootnoteReference"
]);

/** Every `.txt` spec fixture, in one deterministic order. */
export const FIXTURE_DIR = "packages/markdown-core/tests/fixtures";

export function fixtureCorpus(root) {
    return fs
        .readdirSync(path.join(root, FIXTURE_DIR))
        .filter((entry) => entry.endsWith(".txt"))
        .sort()
        .flatMap((entry) => readExamples(root, `${FIXTURE_DIR}/${entry}`));
}

/**
 * Byte length of every source line.
 *
 * Split on the same three terminators `S_parse_source` recognizes, because a
 * fixture does carry a CRLF (regression.txt's line-ending example) and reading
 * it as one line would put every position after it on the wrong row. The empty
 * element a trailing newline leaves is dropped: a document ending in `\n` has
 * no line after it, and keeping one would let an off-the-end position pass.
 */
export function lineLengths(input) {
    const lines = input.split(/\r\n|\r|\n/).map((line) => Buffer.byteLength(line, "utf8"));
    if (lines.length > 0 && lines[lines.length - 1] === 0) lines.pop();
    return lines;
}

const SCOPE = /^(-?\d+):(-?\d+)\.\.(-?\d+):(-?\d+)$/;

/** `scope=L:C..L:C` -> `{ start: [line, column], end: [line, column] }`. */
export function readScope(node) {
    const match = SCOPE.exec(node.fields.scope ?? "");
    if (match === null) return null;
    const [startLine, startColumn, endLine, endColumn] = match.slice(1).map(Number);
    return { start: [startLine, startColumn], end: [endLine, endColumn] };
}

export const formatScope = (scope) => `${scope.start[0]}:${scope.start[1]}..${scope.end[0]}:${scope.end[1]}`;

/** Document order on two coordinates. */
export const before = (left, right) => left[0] < right[0] || (left[0] === right[0] && left[1] < right[1]);

/** A coordinate on line zero cannot participate in geometry comparisons. */
export const onLineZero = (scope) => scope.start[0] === 0 || scope.end[0] === 0;

/**
 * Preorder walk yielding the index chain from the root, so a finding names a
 * node in a way that survives the fixture gaining an example above it. The
 * document is `0`; its second child is `0.1`.
 */
export function* walkWithPath(node, prefix = "0") {
    yield { node, nodePath: prefix };
    for (const [index, child] of node.children.entries()) yield* walkWithPath(child, `${prefix}.${index}`);
}

/** Resolves a built product of this repository's own toolchain, or explains how to build it. */
export function requireBinary(root, relative, fix) {
    const absolute = path.join(root, relative);
    if (!fs.existsSync(absolute)) {
        process.stderr.write(`missing ${relative}\nBuild it with: ${fix}\n`);
        process.exit(1);
    }
    return absolute;
}

export const runBinary = (binary, args, input) =>
    execFileSync(binary, args, { input, encoding: "utf8", maxBuffer: 1 << 28 });

/**
 * The ledger protocol the three oracles share.
 *
 * Each records the exact rows that are wrong today, grouped by the input that
 * produces them, and requires the measured set to match EXACTLY — not to stay
 * under a count. A count cannot tell a fix that cleared twelve rows from one
 * that cleared twelve and introduced one, and that is not a hypothetical:
 * un-gating the multi-line span adjustment is expected to do precisely that.
 *
 * So both directions fail without `--update`, and `--update` is a deliberate
 * act taken in the commit that moves the behaviour, whose message names the
 * rows. Rows that survive an update keep whatever `closedBy` they were given.
 */
export const ANNOTATIONS = ["class", "closedBy"];

export function reconcileLedger({
    root,
    ledgerPath,
    ledger,
    measured,
    update,
    subject,
    scanned,
    status = "still wrong"
}) {
    // A row's identity is what was MEASURED. `class` and `closedBy` are
    // annotations written by hand, so they neither distinguish two rows nor
    // survive being recomputed — an update carries them across instead.
    const rowKey = (input, finding) => {
        const identity = Object.fromEntries(Object.entries(finding).filter(([field]) => !ANNOTATIONS.includes(field)));
        return JSON.stringify([input, identity]);
    };

    const expectedRows = new Map();
    for (const entry of ledger.expected)
        for (const finding of entry.findings) expectedRows.set(rowKey(entry.input, finding), finding);

    const measuredRows = new Map();
    for (const entry of measured)
        for (const finding of entry.findings) measuredRows.set(rowKey(entry.input, finding), finding);

    if (update) {
        for (const entry of measured)
            for (const finding of entry.findings) {
                const previous = expectedRows.get(rowKey(entry.input, finding));
                for (const annotation of ANNOTATIONS)
                    if (previous?.[annotation] !== undefined) finding[annotation] = previous[annotation];
                finding.closedBy ??= "unassigned";
            }
        ledger.expected = measured;
        fs.writeFileSync(path.join(root, ledgerPath), `${JSON.stringify(ledger, null, 4)}\n`);
        process.stdout.write(
            `${subject}: ledger rewritten — ${String(measuredRows.size)} rows over ${String(measured.length)} inputs.\n`
        );
        return;
    }

    const appeared = [...measuredRows.keys()].filter((key) => !expectedRows.has(key));
    const cleared = [...expectedRows.keys()].filter((key) => !measuredRows.has(key));
    if (appeared.length > 0 || cleared.length > 0) {
        const describe = (keys, verb) =>
            keys.map((key) => {
                const [input, identity] = JSON.parse(key);
                return `  ${verb} ${JSON.stringify(input)} ${JSON.stringify(identity)}`;
            });
        throw new Error(
            [
                `${subject}: the ledger and the engine disagree.`,
                ...describe(appeared, "APPEARED"),
                ...describe(cleared, "CLEARED "),
                `A row that moved is a behaviour change: record it in the commit that caused it —`,
                `  node ${ledger.script} --update`
            ].join("\n")
        );
    }
    process.stdout.write(
        `${subject}: ${String(measuredRows.size)} registered rows ${status} (${String(scanned)} scanned) — ledger holds.\n`
    );
}

export function loadLedger(root, ledgerPath) {
    return JSON.parse(fs.readFileSync(path.join(root, ledgerPath), "utf8"));
}
