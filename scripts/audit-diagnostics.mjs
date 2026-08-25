#!/usr/bin/env node
/**
 * Requirement 13's laws, over every fixture example.
 *
 * A parse produces, beside the two total views, an ORDERED LIST OF DIAGNOSTICS
 * — `(severity, code, scope, message)` — and `markdown-core --diagnostics`
 * prints it: one `diagnostics count=N truncated=B` header and one `diagnostic`
 * row per entry. Recording is a RETAIN CALL and not a parse option, which is
 * what makes the first law below checkable at all: the same binary can be run
 * both ways over the same bytes.
 *
 *   D1  NEUTRALITY, and it is the whole of "a lost diagnostic is not a lost
 *       parse" that a corpus can see: for every input the semantic tree AND the bytes
 *       its scopes index are byte-identical with diagnostics on and off.
 *       Checked by running `--source-index` twice — which prints the source
 *       size, the line index and then the tree — and requiring the two outputs
 *       to differ by exactly the diagnostic rows. An engine that reported a diagnostic by changing
 *       what it built would fail here and nowhere else.
 *
 *   D2  EVERY SCOPE IS A PLACE in the normalized source: both ends name a real
 *       byte of a real line, the end does not precede the start, and neither
 *       end is a line ending. A diagnostic exists so a consumer can be sent to
 *       the source; one that names a coordinate the source has not got is
 *       worse than none, and §4.5's "a well-formed but wrong position sails
 *       through the ratchet" is why this is a law and not a review item.
 *
 *   D3  EVERY MESSAGE IS ONE NON-EMPTY UTF-8 LINE. The wire format is line
 *       oriented and an excerpt is cut out of the source, so a label spanning
 *       a line ending would otherwise turn one diagnostic into two rows —
 *       measured on `spec.txt` while this gate was being written, which is the
 *       only reason it was a one-line fix rather than a corrupt oracle.
 *
 * And a CENSUS: the exact set of diagnostics the corpus produces, kept in
 * `specs/diagnostics/census.json` and compared in both directions. A code that
 * stops firing fails as loudly as one that starts, because the failure mode of
 * a diagnostic is silence and a count cannot tell six of one from three of
 * two.
 *
 *   node scripts/audit-diagnostics.mjs [--update] [--verbose]
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { fixtureCorpus, requireBinary, runBinary } from "./lib/source-positions.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const CENSUS = "specs/diagnostics/census.json";
const update = process.argv.includes("--update");
const verbose = process.argv.includes("--verbose");

const ours = requireBinary(root, "build/cmake/packages/markdown-core/core/markdown-core", "pnpm build:c");

const DIAGNOSTIC = /^diagnostic (WARNING|ERROR) ([a-z-]+) (\d+):(\d+)\.\.(\d+):(\d+) (.*)$/;
const HEADER = /^diagnostics count=(\d+)$/;

/**
 * The byte length of each line of the normalized source, EXCLUDING its line
 * ending — read out of the `--source-index` header and line index rather than
 * recomputed from the input, because the source a diagnostic's scope indexes
 * is the normalized one and the two differ wherever a NUL or a CRLF was.
 */
function lineContentLengths(indexText) {
    const lines = indexText.split("\n");
    const header = /^concrete source=(\d+) lines=(\d+)$/.exec(lines[0]);
    if (header === null) throw new Error("--source-index did not begin with its header");
    const size = Number(header[1]);
    const offsets = [];
    for (const line of lines.slice(1)) {
        const row = /^line (\d+) (\d+)$/.exec(line);
        if (row === null) break;
        offsets.push(Number(row[2]));
    }
    return offsets.map((offset, index) => {
        const end = index + 1 < offsets.length ? offsets[index + 1] : size;
        // Every line of the normalized source ends with exactly one `\n`:
        // `S_process_line` appends one when the author wrote none.
        return Math.max(0, end - offset - 1);
    });
}

/** Split a `--source-index --diagnostics` run into its diagnostic rows and the rest. */
function partition(text) {
    const kept = [];
    const rows = [];
    let sawHeader = false;
    for (const line of text.split("\n")) {
        if (HEADER.test(line)) {
            sawHeader = true;
            continue;
        }
        if (line.startsWith("diagnostic ")) {
            rows.push(line);
            continue;
        }
        kept.push(line);
    }
    if (!sawHeader) throw new Error("--diagnostics printed no header");
    return { rest: kept.join("\n"), rows };
}

const failures = [];
const measured = [];
const perCode = new Map();
let examples = 0;
let total = 0;
let inversions = 0;

for (const example of fixtureCorpus(root)) {
    examples += 1;
    const silent = runBinary(ours, ["--source-index"], example.input);
    const loud = runBinary(ours, ["--source-index", "--diagnostics"], example.input);
    const { rest, rows } = partition(loud);

    // D1.
    if (rest !== silent) {
        failures.push(`${example.source}: the tree or the records moved when diagnostics were recorded`);
    }
    if (rows.length === 0) continue;

    const lengths = lineContentLengths(silent);
    const findings = [];
    let previous = null;
    for (const row of rows) {
        const match = DIAGNOSTIC.exec(row);
        if (match === null) {
            failures.push(`${example.source}: unreadable diagnostic row ${JSON.stringify(row)}`);
            continue;
        }
        const [, severity, code, startLine, startColumn, endLine, endColumn] = match;
        const message = match[7];
        const scope = {
            start: [Number(startLine), Number(startColumn)],
            end: [Number(endLine), Number(endColumn)]
        };
        // D2.
        const place = ([line, column]) =>
            line >= 1 && line <= lengths.length && column >= 1 && column <= lengths[line - 1];
        const ordered =
            scope.start[0] < scope.end[0] || (scope.start[0] === scope.end[0] && scope.start[1] <= scope.end[1]);
        if (!place(scope.start) || !place(scope.end) || !ordered) {
            failures.push(
                `${example.source}: ${code} names ${scope.start.join(":")}..${scope.end.join(":")}, which is not a place`
            );
        }
        // D3. The one-line half is checked by the row grammar above and by D1
        // — a `\n` inside a message makes one row two, and the second lands in
        // `rest`. What is left is the CUT: an excerpt is 40 bytes of the source
        // and backing off to a code-point boundary is what keeps it UTF-8, so
        // a replacement character in a message means the cut landed inside a
        // sequence. No fixture in this repository contains U+FFFD, which is
        // what makes the test exact rather than a heuristic.
        if (message.length === 0) {
            failures.push(`${example.source}: ${code} carries an empty message`);
        }
        if (message.includes("\uFFFD")) {
            failures.push(`${example.source}: ${code}'s message was cut inside a UTF-8 sequence`);
        }
        if (
            previous !== null &&
            (scope.start[0] < previous[0] || (scope.start[0] === previous[0] && scope.start[1] < previous[1]))
        ) {
            inversions += 1;
        }
        previous = scope.start;
        total += 1;
        perCode.set(code, (perCode.get(code) ?? 0) + 1);
        findings.push({ severity, code, scope: `${scope.start.join(":")}..${scope.end.join(":")}`, message });
    }
    if (findings.length > 0) measured.push({ input: example.source, findings });
}

if (failures.length > 0) {
    process.stderr.write(`${failures.join("\n")}\n`);
    process.stderr.write(`diagnostics audit: ${String(failures.length)} violation(s)\n`);
    process.exit(1);
}

const censusPath = path.join(root, CENSUS);
if (update) {
    fs.mkdirSync(path.dirname(censusPath), { recursive: true });
    fs.writeFileSync(
        censusPath,
        `${JSON.stringify({ script: `scripts/${path.basename(fileURLToPath(import.meta.url))}`, expected: measured }, null, 4)}\n`
    );
    process.stdout.write(
        `diagnostics census rewritten — ${String(total)} diagnostics over ${String(measured.length)} inputs.\n`
    );
} else {
    const census = JSON.parse(fs.readFileSync(censusPath, "utf8"));
    const key = (input, finding) => JSON.stringify([input, finding]);
    const expected = new Set(
        census.expected.flatMap((entry) => entry.findings.map((finding) => key(entry.input, finding)))
    );
    const actual = new Set(measured.flatMap((entry) => entry.findings.map((finding) => key(entry.input, finding))));
    const appeared = [...actual].filter((row) => !expected.has(row));
    const cleared = [...expected].filter((row) => !actual.has(row));
    if (appeared.length > 0 || cleared.length > 0) {
        const describe = (rows, verb) => rows.map((row) => `  ${verb} ${row}`);
        process.stderr.write(
            [
                "diagnostics census: the census and the engine disagree.",
                ...describe(appeared, "APPEARED"),
                ...describe(cleared, "CLEARED "),
                "A diagnostic that moved is a behaviour change: record it in the commit that caused it —",
                `  node ${census.script} --update`,
                ""
            ].join("\n")
        );
        process.exit(1);
    }
}

if (verbose) {
    for (const [code, count] of [...perCode.entries()].sort()) process.stdout.write(`  ${code}: ${String(count)}\n`);
}
process.stdout.write(
    `diagnostics: ${String(total)} over ${String(examples)} examples, ${String(perCode.size)} of 6 codes exercised, ` +
        `${String(inversions)} out of source order — the tree and the source index are byte-identical either way.\n`
);
