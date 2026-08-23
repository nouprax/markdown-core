#!/usr/bin/env node
/**
 * Requirement 11a's four laws, over every fixture example.
 *
 * A parse produces, beside the tree, a CONCRETE RECORD SET: every byte of the
 * normalized source in exactly one region, each region with exactly one owner
 * and one role. `markdown-core --concrete` prints it — a `line` row per line
 * of the normalized source and a `region` row per region, both in source
 * order, each region carrying its owner's tree path and scope. There is no
 * public reader yet; requirement 12 is where a document keeps this view.
 *
 *   L1  the regions on a line tile it exactly.
 *   L2  every region lies inside its owner's scope, and a node's regions lie
 *       inside its parent's CONTENT. The second clause is 11b's; see
 *       `checkContainment` for the measurement that put it there.
 *   L3  concatenating the regions in order reproduces the normalized source
 *       byte for byte.
 *   L4  the records are complete for lines 1…N once line N has been fed.
 *
 * TWO OF THE FOUR HOLD BY CONSTRUCTION AND ARE STILL CHECKED HERE, because
 * "by construction" is a property of today's code and this file is what makes
 * it a property of tomorrow's. `S_claim_region` lays regions down left to
 * right from a cursor that only moves forward, so L1 is the statement that the
 * cursor was never bypassed; and since every region is a range OF the source,
 * a tiling of [0, size) concatenates to the source, which is L3. A gate that
 * only checked the interesting law would not notice the day someone writes a
 * region by hand.
 *
 * L4 IS THE ONE THE OWNER ASKED FOR, and it is what stops the record set being
 * built at close. It is checked by re-parsing every LINE-BOUNDARY PREFIX of an
 * input and requiring the prefix's records to be exactly the full document's
 * records restricted to those bytes. A close-time construction passes L1, L2
 * and L3 and fails this.
 *
 * What L4 compares is `(start, length, role)` and not the owner. A region's
 * owner is reassigned exactly once, and only when the owning node is destroyed
 * — a paragraph that turned out to be nothing but reference definitions hands
 * its bytes to its parent as DISCARDED rather than leaving a record naming a
 * freed node. That is neither creating a region nor moving one, and Step 11c
 * deletes the case by giving a definition its own node to own them.
 *
 *   node scripts/audit-concrete-records.mjs [--update] [--verbose]
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { fixtureCorpus, loadLedger, reconcileLedger, requireBinary, runBinary } from "./lib/source-positions.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const LEDGER = "specs/concrete/records.json";
const ledger = loadLedger(root, LEDGER);
const update = process.argv.includes("--update");
const verbose = process.argv.includes("--verbose");

const ours = requireBinary(root, "build/cmake/packages/markdown-core/core/markdown-core", "pnpm build:c");

/** Parse `--concrete` output into its header, line index and region rows. */
function readConcrete(text) {
    const lines = text.split("\n");
    const header = /^concrete source=(\d+) lines=(\d+) regions=(\d+)$/.exec(lines[0]);
    if (!header) throw new Error(`concrete dump has no header: ${JSON.stringify(lines[0])}`);
    const source = Number(header[1]);
    const lineStarts = [];
    const regions = [];
    for (const row of lines.slice(1)) {
        const line = /^line (\d+) (\d+)$/.exec(row);
        if (line) {
            lineStarts.push(Number(line[2]));
            continue;
        }
        const region = /^region (\d+) (\d+) (MARKER|CONTENT|DISCARDED) (\S+) (\S+) (\d+):(\d+)\.\.(\d+):(\d+)$/.exec(
            row
        );
        if (region) {
            regions.push({
                start: Number(region[1]),
                length: Number(region[2]),
                role: region[3],
                path: region[4],
                kind: region[5],
                scope: [
                    [Number(region[6]), Number(region[7])],
                    [Number(region[8]), Number(region[9])]
                ]
            });
            continue;
        }
        // Everything after the record set is the tree dump the CLI still
        // prints; the record rows are contiguous and come first.
        if (row.startsWith("region ") || row.startsWith("line ")) throw new Error(`unparsable record row: ${row}`);
    }
    return { source, lineStarts, regions };
}

const concrete = (input) => readConcrete(runBinary(ours, ["--profile", ledger.profile, "--concrete"], input));

/** A (line, column) scope coordinate as an offset into the normalized source. */
const offsetOf = (lineStarts, [line, column], source) => {
    if (line < 1 || line > lineStarts.length) return null;
    const at = lineStarts[line - 1] + column - 1;
    return at < 0 || at > source ? null : at;
};

/** L1 + L3: the regions tile [0, source) with no gap and no overlap. */
function checkTiling({ source, regions }) {
    const faults = [];
    let at = 0;
    for (const region of regions) {
        if (region.length <= 0) faults.push({ law: "L1", fault: "empty-region", at: region.start });
        else if (region.start !== at) faults.push({ law: "L1", fault: region.start > at ? "gap" : "overlap", at });
        at = region.start + region.length;
    }
    if (at !== source) faults.push({ law: "L3", fault: "does-not-reproduce-source", at, expected: source });
    return faults;
}

/**
 * L2, first clause: a region lies inside its owner's scope.
 *
 * THE SECOND CLAUSE — "descendants lie inside their ancestor's CONTENT" — is
 * NOT checked here, and the wide form was measured before it was dropped. At
 * 11a every region belongs to a BLOCK, and a block child of a block is not
 * inside its parent's content in any sense a block partition can express: a
 * block quote has no content buffer at all, and a table's content regions are
 * the paragraph's from before it was retyped. Applied to blocks the clause
 * reported 78 rows, every one of them a `table_row` and every one of them that
 * same fact. The clause is about INLINE regions inside the block whose content
 * they were cut from, and there are none until 11b, which is where it lands
 * with something to say.
 */
function checkContainment({ source, lineStarts, regions }) {
    const faults = [];
    for (const region of regions) {
        const start = offsetOf(lineStarts, region.scope[0], source);
        const end = offsetOf(lineStarts, region.scope[1], source);
        const stop = region.start + region.length;
        if (start === null || end === null) {
            faults.push({ law: "L2", fault: "owner-scope-is-not-a-place", path: region.path, kind: region.kind });
            continue;
        }
        // A scope's end names the LAST BYTE, so the owner covers [start, end],
        // and a block's last line ending is one past that end.
        if (region.start < start) {
            faults.push({
                law: "L2",
                fault: "region-before-owner",
                kind: region.kind,
                role: region.role,
                short: start - region.start
            });
        } else if (stop > end + 2) {
            faults.push({
                law: "L2",
                fault: "region-after-owner",
                kind: region.kind,
                role: region.role,
                over: stop - (end + 2)
            });
        }
    }
    return faults;
}

/**
 * L4: the records for lines 1…N are what a parse of lines 1…N produces.
 *
 * Every line boundary of the input is a prefix, and each prefix's records must
 * be the full document's records restricted to the prefix's bytes. Compared on
 * `(start, length, role)`; see the note at the top of this file for why the
 * owner is not part of the comparison.
 */
function checkCompleteness(input, full) {
    const faults = [];
    const lines = input.split("\n");
    if (lines[lines.length - 1] === "") lines.pop();
    for (let n = 1; n < lines.length; n++) {
        const prefix = `${lines.slice(0, n).join("\n")}\n`;
        let records;
        try {
            records = concrete(prefix);
        } catch {
            faults.push({ law: "L4", fault: "prefix-did-not-parse", lines: n });
            continue;
        }
        // Compared BYTE BY BYTE on the role, not region by region: adjacent
        // regions with one owner and one role are stored as one, so a document
        // whose first three lines are one paragraph has one region where its
        // one-line prefix has one shorter region, and that is the same
        // attribution rather than a different one.
        const roles = (records_, upto) => {
            const out = new Array(upto).fill("?");
            for (const region of records_.regions)
                for (let at = region.start; at < region.start + region.length && at < upto; at++) out[at] = region.role;
            return out;
        };
        const want = roles(full, records.source);
        const got = roles(records, records.source);
        const at = want.findIndex((role, index) => role !== got[index]);
        if (at >= 0) {
            faults.push({ law: "L4", fault: "prefix-role-differs", whole: want[at], alone: got[at] });
            if (verbose) process.stdout.write(`    lines 1..${String(n)} byte ${String(at)}\n`);
            break;
        }
    }
    return faults;
}

const measured = [];
let scanned = 0;
let regionCount = 0;
let prefixes = 0;
for (const example of fixtureCorpus(root)) {
    const records = concrete(example.input);
    scanned += 1;
    regionCount += records.regions.length;
    const faults = [...checkTiling(records), ...checkContainment(records)];
    // L4 re-parses every line-boundary prefix, so it costs a process per line
    // rather than a process per example. It runs over every example short
    // enough for that to stay a gate rather than a benchmark; the property is
    // structural, so the bound costs coverage of a shape and not of a case.
    const lineCount = example.input.split("\n").length - 1;
    if (lineCount > 1 && lineCount <= (ledger.completenessMaxLines ?? 0)) {
        prefixes += lineCount - 1;
        faults.push(...checkCompleteness(example.input, records));
    }
    if (faults.length > 0) measured.push({ source: example.source, input: example.input, findings: faults });
}

if (verbose)
    for (const entry of measured)
        for (const finding of entry.findings)
            process.stdout.write(`${entry.source} ${finding.law} ${finding.fault} ${finding.path ?? ""}\n`);

process.stdout.write(
    `  ${String(regionCount)} regions over ${String(scanned)} examples; L4 checked over ${String(prefixes)} line-boundary prefixes.\n`
);

reconcileLedger({
    root,
    ledgerPath: LEDGER,
    ledger,
    measured,
    update,
    subject: "concrete records",
    scanned: regionCount
});
