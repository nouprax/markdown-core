import { readFile, readdir, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";

// Every canonical dump prints one `scope=L:C..L:C` per node. Two classes of
// that output are not positions at all, and both exist for the same reason:
// a closed (line, column) interval cannot express an empty range, so a
// zero-width node has no honest spelling in this representation.
//
//   sentinel  `0:0..0:0`  — the node is recorded as having no position. Written
//               for soft and hard breaks by construction, and for a Text whose
//               content was removed (core/inlines.c:2494-2505 computes
//               `end_column = start_column - 1`, declines to emit it, and
//               zeroes the node instead).
//   negative  `end < start` — the same arithmetic, emitted rather than zeroed.
//               `HTMLBlock scope=1:1..0:0` ends on a line that does not exist.
//
// Neither survives M3: an extent is (identity, length) and expresses length
// zero natively, so both classes resolve to a real place. Until then this
// audit is a ratchet — the counts may shrink and may never grow, exactly as
// specs/coverage/policy.json holds the unpinned surface. When both reach zero
// the ledger, and this script, are deleted rather than kept at zero.
//
// Evidence and the elimination path: docs/reviews/2026-08-05-m3-endstate-poc.md

const root = process.cwd();
const ledgerPath = path.join(root, "specs/scope-sanity/ledger.json");
const fixturesDir = path.join(root, "packages/markdown-core/tests/fixtures");
const canonicalDir = path.join(root, "specs/canonical-ast");
const update = process.argv.includes("--update");

const scopePattern = /scope=(\d+):(\d+)\.\.(\d+):(\d+)/;
// The opener carries the example's extension set (`example table`,
// `example footnotes autolink strikethrough table`, `example disabled`), so
// the name is a prefix and not the whole line. Requiring the whole line drops
// 39 of the 860 blocks, and a ratchet that silently stops watching part of
// its subject is worse than no ratchet.
const exampleOpen = /^`{20,}\s*example\b/;
const exampleClose = /^`{20,}\s*$/;

// A `.txt` fixture is Markdown prose wrapping example blocks, and an example
// block is an input, a `.` separator, and the expected dump. Only the dump is
// this audit's subject: the input is arbitrary Markdown, so a fixture is free
// to contain the literal text `scope=0:0..0:0` as content, and counting that
// would fail verify:core on an innocent fixture. A `.ast` file is a dump with
// no wrapper, so it is read whole.
const dumpLines = (text, isFixture) => {
    if (!isFixture) return text.split("\n");
    const lines = [];
    let state = "prose";
    for (const line of text.split("\n")) {
        if (state === "prose") {
            if (exampleOpen.test(line)) state = "input";
            continue;
        }
        if (state === "input") {
            if (line === ".") state = "expected";
            continue;
        }
        if (exampleClose.test(line)) state = "prose";
        else lines.push(line);
    }
    return lines;
};

const sources = [
    ...(await readdir(fixturesDir))
        .filter((entry) => entry.endsWith(".txt"))
        .map((entry) => ["packages/markdown-core/tests/fixtures", entry]),
    ...(await readdir(canonicalDir))
        .filter((entry) => entry.endsWith(".ast"))
        .map((entry) => ["specs/canonical-ast", entry])
].sort(([leftDir, left], [rightDir, right]) => `${leftDir}/${left}`.localeCompare(`${rightDir}/${right}`));

const measured = {};
let scanned = 0;
for (const [dir, entry] of sources) {
    const relative = `${dir}/${entry}`;
    const text = await readFile(path.join(root, relative), "utf8");
    let sentinel = 0;
    let negative = 0;
    for (const line of dumpLines(text, relative.endsWith(".txt"))) {
        const scope = line.match(scopePattern);
        if (scope === null) continue;
        scanned += 1;
        const [startLine, startColumn, endLine, endColumn] = scope.slice(1).map(Number);
        if (startLine === 0 && startColumn === 0 && endLine === 0 && endColumn === 0) sentinel += 1;
        else if (endLine < startLine || (endLine === startLine && endColumn < startColumn)) negative += 1;
    }
    if (sentinel > 0 || negative > 0) measured[relative] = { sentinel, negative };
}

const ledger = JSON.parse(await readFile(ledgerPath, "utf8"));
const total = (counts) => Object.values(counts).reduce((sum, entry) => sum + entry.sentinel + entry.negative, 0);

if (update) {
    ledger.budget = measured;
    await writeFile(ledgerPath, `${JSON.stringify(ledger, null, 4)}\n`);
    process.stdout.write(
        `Scope-sanity ledger rewritten: ${total(measured)} rows across ${Object.keys(measured).length} files.\n`
    );
} else {
    const failures = [];
    const slack = [];
    for (const [relative, counts] of Object.entries(measured)) {
        const budget = ledger.budget[relative] ?? { sentinel: 0, negative: 0 };
        for (const kind of ["sentinel", "negative"]) {
            if (counts[kind] > budget[kind])
                failures.push(`${relative}: ${kind} rows grew ${budget[kind]} -> ${counts[kind]}`);
            else if (counts[kind] < budget[kind])
                slack.push(`${relative}: ${kind} rows shrank ${budget[kind]} -> ${counts[kind]}`);
        }
    }
    for (const relative of Object.keys(ledger.budget)) {
        if (relative in measured) continue;
        slack.push(`${relative}: no offending rows remain`);
    }
    if (failures.length > 0)
        throw new Error(
            `${failures.join("\n")}\nA scope that is a sentinel or a negative range is not a position. See ${ledger.rule}`
        );
    if (slack.length > 0) {
        throw new Error(
            `${slack.join("\n")}\nThe ledger only shrinks, so record the gain: pnpm run audit:scope-sanity --update`
        );
    }
    process.stdout.write(
        `Scope sanity: ${total(measured)} unresolved rows (${scanned} scopes scanned) — at budget, only-shrink holds.\n`
    );
}
