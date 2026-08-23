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
 *   L2  every region lies inside its owner's scope, and an INLINE region lies
 *       inside the scope of the block whose content it was cut from. The
 *       second clause is 11b's and it lands here with 11b; see
 *       `checkContainment` for the measurement that shaped it.
 *   L3  concatenating the regions in order reproduces the normalized source
 *       byte for byte.
 *   L5  an inline node's scope is exactly the bytes it and its descendants
 *       own. This is the one that says 11b happened at all: L1 to L4 are all
 *       true of the day before it.
 *   L6  an inline node's OWN regions carry the role its kind admits — a `Text`
 *       owns no marker, an `Emphasis` owns nothing else.
 *   L4  the records are complete for lines 1…N once line N has been fed — for
 *       the BLOCK attribution over every byte, and for 11b's INLINE refinement
 *       over the bytes in a block the prefix had already closed. See
 *       `checkCompleteness`: an inline record is made when its block closes,
 *       so the block a prefix leaves open cannot have the same ones.
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
    const nodes = [];
    for (const row of lines.slice(1)) {
        const line = /^line (\d+) (\d+)$/.exec(row);
        if (line) {
            lineStarts.push(Number(line[2]));
            continue;
        }
        const node = /^node (\S+) (\S+) (-?\d+):(-?\d+)\.\.(-?\d+):(-?\d+)$/.exec(row);
        if (node) {
            nodes.push({
                path: node[1],
                kind: node[2],
                scope: [
                    [Number(node[3]), Number(node[4])],
                    [Number(node[5]), Number(node[6])]
                ]
            });
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
    return { source, lineStarts, regions, nodes };
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
 * L2. FIRST CLAUSE: a region lies inside its owner's scope.
 *
 * SECOND CLAUSE, 11b's: an INLINE region lies inside the scope of the block
 * whose content it was cut from. That is requirement 11b's law read from the
 * region side -- every byte of a block's CONTENT is owned by exactly one inline
 * node or by the block itself, so an inline region that reached outside its
 * block would be a byte attributed to a construct that did not come from there.
 *
 * The WIDE form was measured at 11a before it was dropped: applied to BLOCKS,
 * "descendants lie inside their ancestor's CONTENT" reported 78 rows, every one
 * a `table_row`, because a block child of a block is not inside its parent's
 * content in any sense a block partition can express -- a block quote has no
 * content buffer at all. Restricting the clause to inline owners is what makes
 * it say something rather than restate the block model's shape.
 */
function checkContainment({ source, lineStarts, regions }) {
    const faults = [];
    /* The enclosing BLOCK of an inline region: the nearest ancestor path that
     * some region names with a block kind. Paths are dotted node paths, so an
     * ancestor is a prefix of one. */
    const blockScope = new Map();
    for (const region of regions) {
        if (isBlockOwner(region)) blockScope.set(region.path, region.scope);
    }
    const enclosingBlock = (path) => {
        for (let at = path.lastIndexOf("."); at > 0; at = path.lastIndexOf(".", at - 1)) {
            const found = blockScope.get(path.slice(0, at));
            if (found) return found;
        }
        return blockScope.get("0") ?? null;
    };
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
        if (isBlockOwner(region)) continue;
        const block = enclosingBlock(region.path);
        const blockStart = block && offsetOf(lineStarts, block[0], source);
        const blockEnd = block && offsetOf(lineStarts, block[1], source);
        if (blockStart === null || blockEnd === null || blockStart === undefined) {
            faults.push({ law: "L2", fault: "inline-block-is-not-a-place", kind: region.kind });
        } else if (region.start < blockStart || stop > blockEnd + 2) {
            faults.push({
                law: "L2",
                fault: "inline-region-outside-its-block",
                kind: region.kind,
                role: region.role,
                over: Math.max(blockStart - region.start, stop - (blockEnd + 2))
            });
        }
    }
    return faults;
}

/**
 * L5, AND IT IS THE ONE THAT SAYS 11b HAPPENED: an inline node's scope is
 * exactly the bytes it and its descendants own.
 *
 * L1 through L4 are all satisfied by a record set that names no inline node at
 * all -- "every byte of a block's CONTENT is owned by an inline node OR BY THE
 * BLOCK" is true of the day before 11b, and so is the tiling, and so is the
 * completeness. Deleting the whole inline refinement passes them. This is what
 * it does not pass: the emphasis that covers `*em*` must own `*` and `*`, its
 * text child must own `em`, and between them they must own the six bytes the
 * emphasis says it covers -- no more, no fewer.
 *
 * A node whose scope is not a place is skipped and counted; those are the
 * families `specs/positions/places.json` carries, and re-reporting them here
 * would say the same thing twice.
 */
/* L6's vocabulary: the roles an inline node's OWN regions may carry.
 *
 * The role rule is "a byte is CONTENT when it survives into the owner's literal
 * as itself, MARKER when it does not", and that is not something an oracle can
 * read off the tree -- but its CONSEQUENCE per kind is. A `Text` is its own
 * literal, so none of its bytes is a marker. An `Emphasis` has no literal at
 * all, so every byte it owns for itself is one. A `Code` span has both: ticks
 * and content. Without this, flipping a delimiter's role kills nothing.
 *
 * DISCARDED is allowed wherever CONTENT is: it is the same claim about the
 * bytes -- they are not the construct's syntax -- with the extra fact that the
 * literal did not keep them. */
const OWN_ROLES = {
    // A `Text` genuinely owns markers: `&amp;`, `\\*` and a smart quote all
    // reach its literal as bytes the source did not write, so the vocabulary
    // is open here and the law says nothing about this kind. Every other kind
    // below is exact.
    text: ["CONTENT", "MARKER", "DISCARDED"],
    html: ["CONTENT", "DISCARDED"],
    soft_break: ["MARKER"],
    line_break: ["MARKER"],
    emphasis: ["MARKER"],
    strong: ["MARKER"],
    strikethrough: ["MARKER"],
    link: ["MARKER"],
    image: ["MARKER"],
    link_reference: ["MARKER"],
    image_reference: ["MARKER"],
    footnote_reference: ["MARKER"],
    directive: ["MARKER", "CONTENT"],
    directive_label: ["MARKER"],
    code: ["MARKER", "CONTENT"],
    formula: ["MARKER", "CONTENT"]
};

function checkInlineRoles({ regions }) {
    const faults = [];
    for (const region of regions) {
        if (isBlockOwner(region)) continue;
        const allowed = OWN_ROLES[region.kind];
        if (!allowed) {
            faults.push({ law: "L6", fault: "inline-kind-has-no-role-vocabulary", kind: region.kind });
        } else if (!allowed.includes(region.role)) {
            faults.push({ law: "L6", fault: "inline-role-not-allowed", kind: region.kind, role: region.role });
        }
    }
    return faults;
}

function checkInlineCoverage({ source, lineStarts, regions, nodes }) {
    const faults = [];
    const owned = new Map();
    for (const region of regions) {
        const run = owned.get(region.path) ?? [];
        run.push([region.start, region.start + region.length]);
        owned.set(region.path, run);
    }
    for (const node of nodes) {
        // The two DEFINITION kinds are checked here too, and that is
        // requirement 11c's claim: a reference definition and a footnote
        // definition OWN THEIR SOURCE BYTES, so the partition is total for a
        // definition-bearing document and a definition that lost a
        // duplicate-label contest keeps its bytes like any other. No other
        // block kind is: L5 is about what a node's own bytes are, and a
        // container block's bytes are its children's by construction.
        if (BLOCK_KINDS.has(node.kind) && node.kind !== "reference_definition" && node.kind !== "footnote_definition")
            continue;
        const start = offsetOf(lineStarts, node.scope[0], source);
        const end = offsetOf(lineStarts, node.scope[1], source);
        if (start === null || end === null || end < start) {
            skippedNodes += 1;
            continue;
        }
        // Its own bytes and its descendants': a path is an ancestor of another
        // when it is a dotted prefix of it.
        const spans = [];
        for (const [path, run] of owned) {
            if (path === node.path || path.startsWith(`${node.path}.`)) spans.push(...run);
        }
        spans.sort((a, b) => a[0] - b[0]);
        let at = start;
        let gap = false;
        for (const [from, to] of spans) {
            if (from > at) {
                gap = true;
                break;
            }
            if (to > at) at = to;
        }
        coveredNodes += 1;
        if (node.kind === "reference_definition" || node.kind === "footnote_definition") coveredDefinitions += 1;
        // The scope's end names the LAST BYTE, and a node whose last byte is a
        // line ending owns one more than `end - start + 1`.
        if (gap || at < end + 1) {
            faults.push({ law: "L5", fault: "inline-scope-not-covered", kind: node.kind, short: end + 1 - at });
        } else if (at > end + 2) {
            faults.push({ law: "L5", fault: "inline-owns-past-its-scope", kind: node.kind, over: at - (end + 2) });
        }
    }
    return faults;
}

/**
 * L4: the records for lines 1…N are what a parse of lines 1…N produces —
 * FOR EVERY BYTE IN A BLOCK THE PREFIX HAD ALREADY CLOSED.
 *
 * The bound is 11b's, and it is a statement about this engine rather than a
 * convenience. A block's INLINE records are made when the block's inlines are
 * parsed, which is when the block CLOSES; the last block a prefix opened is
 * closed by the end of the input, so its inlines are parsed over truncated
 * content. `$$` on its own line is a paragraph; `$$⏎x⏎$$` is a formula, and the
 * same bytes are CONTENT in the first reading and MARKER in the second. No
 * ordering of the parse can avoid that: the construct is not there yet.
 *
 * So the comparison stops at the first byte of the prefix's LAST TOP-LEVEL
 * BLOCK, and the count of bytes actually compared is printed, because a bound
 * that quietly swallowed the whole document would read as agreement. Every
 * byte before it is in a block both parses closed, and there the law is exactly
 * what it was: same bytes, same roles, no exceptions.
 */
let compared = 0;
let offered = 0;
let coveredNodes = 0;
let coveredDefinitions = 0;
let skippedNodes = 0;

/* The two halves of L4 are checked over different byte sets, so the check needs
 * to know which regions are BLOCK attribution and which are 11b's inline
 * refinement of it. A kind in neither list is an error rather than a guess: a
 * new node kind must be classified, not silently treated as one or the other. */
const BLOCK_KINDS = new Set([
    "document",
    "block_quote",
    "list",
    "list_item",
    "code_block",
    "html_block",
    "paragraph",
    "heading",
    "thematic_break",
    "footnote_definition",
    "reference_definition",
    "table",
    "table_row",
    "table_header",
    "table_cell",
    "formula_block",
    "directive_block",
    // The tasklist extension retypes a list item and names it itself.
    "tasklist"
]);
const INLINE_KINDS = new Set([
    "text",
    "soft_break",
    "line_break",
    "code",
    "html",
    "emphasis",
    "strong",
    "link",
    "image",
    "footnote_reference",
    "link_reference",
    "image_reference",
    "strikethrough",
    "formula",
    "directive",
    "directive_label"
]);
const isBlockOwner = (region) => {
    if (BLOCK_KINDS.has(region.kind)) return true;
    if (INLINE_KINDS.has(region.kind)) return false;
    throw new Error(`concrete records: region owner kind \`${region.kind}\` is in neither vocabulary`);
};

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
        const roles = (records_, upto, coarse) => {
            const out = new Array(upto).fill("?");
            for (const region of records_.regions) {
                // BLOCK attribution, which every byte has: an inline region
                // only ever refines a byte the block already called CONTENT.
                const role = coarse && !isBlockOwner(region) ? "CONTENT" : region.role;
                for (let at = region.start; at < region.start + region.length && at < upto; at++) out[at] = role;
            }
            return out;
        };
        // The prefix's last top-level block: the highest first path component
        // any of its regions names. Everything at or after its first byte is
        // in a block the prefix closed EARLY, and its inline records are the
        // records of a shorter block.
        const topOf = (region) => {
            const parts = region.path.split(".");
            return parts.length > 1 ? Number(parts[1]) : -1;
        };
        const last = Math.max(-1, ...records.regions.map(topOf));
        const openFrom =
            last < 0 ? records.source : Math.min(...records.regions.filter((r) => topOf(r) === last).map((r) => r.start));
        compared += openFrom;
        offered += records.source;
        // BLOCK attribution over EVERY byte the prefix produced, which is 11a's
        // law unweakened, and the INLINE refinement over the bytes in a block
        // the prefix had already closed.
        const coarseWant = roles(full, records.source, true);
        const coarseGot = roles(records, records.source, true);
        const coarseAt = coarseWant.findIndex((role, index) => role !== coarseGot[index]);
        if (coarseAt >= 0) {
            faults.push({
                law: "L4",
                fault: "prefix-block-role-differs",
                whole: coarseWant[coarseAt],
                alone: coarseGot[coarseAt]
            });
            if (verbose) process.stdout.write(`    lines 1..${String(n)} byte ${String(coarseAt)} (block)\n`);
            break;
        }
        const want = roles(full, openFrom, false);
        const got = roles(records, openFrom, false);
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
    const faults = [
        ...checkTiling(records),
        ...checkContainment(records),
        ...checkInlineCoverage(records),
        ...checkInlineRoles(records)
    ];
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
    `  ${String(regionCount)} regions over ${String(scanned)} examples; L4 checked over ${String(prefixes)} ` +
        `line-boundary prefixes; L5 over ${String(coveredNodes)} nodes, ${String(coveredDefinitions)} of them a ` +
        `definition that must own its own bytes (${String(skippedNodes)} skipped: their scope is not a place). ` +
        `BLOCK attribution over all ${String(offered)} bytes, the INLINE refinement over ` +
        `${String(compared)} of them ` +
        `(${String(Math.round((100 * compared) / Math.max(1, offered)))}%; the rest are in the block the prefix left open).\n`
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
