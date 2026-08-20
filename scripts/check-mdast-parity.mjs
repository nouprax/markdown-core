#!/usr/bin/env node
/**
 * mdast-parity gate.
 *
 * The second of two external oracles. cmark-gfm is the authority for the base
 * language and cannot judge the constructs it does not implement, so for
 * directives, math, footnote placement, and the reference-link model the
 * authority is the unified/remark ecosystem — the one this repository's
 * extensions were written against.
 *
 * Both parsers parse the same corpus, their trees are normalized into one
 * comparable form, and anything that still differs is either registered in
 * `specs/mdast-parity/deltas.json` or a defect.
 *
 *   node scripts/check-mdast-parity.mjs [--verbose]
 */

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { unified } from "unified";
import remarkParse from "remark-parse";
import remarkGfm from "remark-gfm";
import remarkDirective from "remark-directive";
import remarkMath from "remark-math";

import { readExamples } from "./lib/fixture-corpus.mjs";
import { dropEmptyText, fromMdast, MDAST_COMPARED } from "./lib/mdast-oracle.mjs";
import { liftFootnoteDefinitions, parseCanonicalDump, render } from "./lib/upstream-cmark.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const policyPath = "specs/mdast-parity/deltas.json";
const policy = JSON.parse(fs.readFileSync(path.join(root, policyPath), "utf8"));
const verbose = process.argv.includes("--verbose");

const ours = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");
if (!fs.existsSync(ours)) {
    process.stderr.write(`mdast parity: missing ${path.relative(root, ours)}\nBuild it with: pnpm build:c\n`);
    process.exit(1);
}

const processor = unified().use(remarkParse).use(remarkGfm).use(remarkDirective).use(remarkMath);

/**
 * Projects a normalized tree down to the fields this oracle compares. Both
 * sides go through the same function, so neither can be compared on a field
 * the other never carries.
 */
function project(node) {
    const children = [];
    for (const child of node.children) {
        // `:red[]` carries an empty label in this repository's AST and no
        // children at all in mdast, which cannot express the difference
        // between it and `:red`. Dropping the empty node compares what both
        // models can state.
        if (child.kind === "DirectiveLabel" && child.children.length === 0) continue;
        const projected = project(child);
        const previous = children[children.length - 1];
        if (previous && previous.kind === "Text" && projected.kind === "Text") {
            previous.fields.literal += projected.fields.literal;
        } else {
            children.push(projected);
        }
    }
    const fields = {};
    for (const key of MDAST_COMPARED[node.kind] ?? []) {
        // This repository's dump names an image's target `source`; mdast and
        // cmark both call it a destination. One name reaches the comparison.
        let value =
            node.kind === "Image" && key === "destination"
                ? (node.fields.destination ?? node.fields.source)
                : node.fields[key];
        // Both sides spell a directive's attributes as sorted `key="value"`
        // pairs now; the dump brackets the group so it reads as one field, and
        // spells an empty container `[]` where the oracle spells it "null".
        if (key === "attributes" && typeof value === "string" && value.startsWith("[")) {
            const inner = value.slice(1, -1);
            value = inner === "" ? "null" : inner;
        }
        if (value === undefined || value === "") value = key === "literal" ? "" : "null";
        fields[key] = String(value);
    }
    return { kind: node.kind, fields, children };
}

function unknownKinds(node, found = new Set()) {
    if (node.kind.startsWith("?")) found.add(node.kind.slice(1));
    for (const child of node.children) unknownKinds(child, found);
    return found;
}

function compare(input) {
    const theirs = project(dropEmptyText(liftFootnoteDefinitions(fromMdast(processor.parse(input)))));
    const mine = project(
        dropEmptyText(
            liftFootnoteDefinitions(
                parseCanonicalDump(execFileSync(ours, ["--profile", "gfm-extended"], { input, encoding: "utf8" }))
            )
        )
    );
    return {
        remark: render(theirs),
        ours: render(mine),
        unmapped: new Set([...unknownKinds(theirs), ...unknownKinds(mine)])
    };
}

// The oracle must be able to see a difference before its agreement means
// anything. This is the difference it found when it was built — inline math
// padding.
//
// RE-PINNED TO THE 1.0 BASELINE, 2026-08-20. The canary asserted the STRIPPED
// form, `literal="mid"`, because the engine it was written against strips the
// padding. The reset baseline does not: `strip_inline_math_padding` is Step 6a
// of the reconstruction and has not been re-applied, so this engine emits
// `literal=" mid "` and remark emits the stripped form. The canary therefore
// asserts the unstripped literal, which serves the identical purpose — the
// comparison can still see this field, and the difference it can see is a real
// one this engine currently has. Step 6a flips this assertion back to
// `literal="mid"`, and that flip is part of what proves Step 6a landed.
const selfTest = compare("text $$ mid $$ text\n");
if (!selfTest.ours.includes('literal=" mid "')) {
    process.stderr.write(
        "mdast parity: the self-test input no longer produces the padded-and-stripped form.\n" +
            "The comparison may be projecting away the field it is supposed to check.\n"
    );
    process.exit(1);
}

// A registered divergence is one where remark is not the authority — GitHub's
// renderer decides `$...$`, and canonical-ast.md decides an unresolved
// footnote reference. Each must still reproduce: an entry describing a
// difference that has gone away would keep excusing a comparison nobody has
// looked at since.
const expected = new Map((policy.expectedDivergences ?? []).map((entry) => [entry.input, entry]));
const reproduced = new Set();

// THE RECONSTRUCTION BACKLOG, which is not the same thing as a divergence.
// A registered divergence says "remark is not the authority here". A backlog
// entry says "remark is right and this engine has not caught up yet", and
// names the step that closes it. The gate knows about them so it can still
// fail on a NEW divergence, and it requires each one to STILL diverge: an
// entry that has quietly started agreeing is a step that landed without
// deleting its own entry, which is a failure too. When the list is empty,
// Stage 0 is done.
const backlog = new Map((policy.baselineBacklog ?? []).map((entry) => [entry.input, entry]));
const backlogSeen = new Set();

// `--corpus` replaces the policy's corpus for one run; see the note in
// scripts/check-upstream-parity.mjs.
const corpusOverride = process.argv.indexOf("--corpus");
const corpusFiles = corpusOverride >= 0 ? [process.argv[corpusOverride + 1]] : policy.corpus;
const cases = corpusFiles.flatMap((file) => readExamples(root, file));
const divergent = [];
const unmapped = new Set();
for (const testCase of cases) {
    let result;
    try {
        result = compare(testCase.input);
    } catch (error) {
        divergent.push({ ...testCase, failure: String(error).slice(0, 300) });
        continue;
    }
    for (const kind of result.unmapped) unmapped.add(kind);
    const registered = expected.get(testCase.input);
    if (result.remark !== result.ours) {
        if (registered) reproduced.add(registered.id);
        else if (backlog.has(testCase.input)) backlogSeen.add(testCase.input);
        else divergent.push({ ...testCase, ...result });
    } else if (registered) {
        divergent.push({
            ...testCase,
            settled: registered,
            remark: result.remark,
            ours: result.ours
        });
    }
}

// Under `--corpus` the corpus is one generated input, so an entry going
// unexercised says nothing. Over the policy's own corpus it says the entry is
// no longer reachable, which is the same rot as one that stopped reproducing:
// a difference that used to be checked and now is not.
if (corpusOverride < 0) {
    for (const [input, entry] of expected) {
        if (!reproduced.has(entry.id)) divergent.push({ source: policyPath, input, unreachable: entry });
    }
}

process.stdout.write(
    `mdast parity: ${String(cases.length - divergent.length)}/${String(cases.length)} inputs agree with remark\n`
);
process.stdout.write(`  corpus: ${policy.corpus.join(", ")}\n`);
process.stdout.write(`  registered shape deltas: ${policy.deltas.map((d) => d.id).join(", ")}\n`);
process.stdout.write(`  registered divergences: ${String(reproduced.size)}/${String(expected.size)} reproduced\n`);
if (backlog.size) {
    const byStep = new Map();
    for (const entry of backlog.values()) byStep.set(entry.closedBy, (byStep.get(entry.closedBy) ?? 0) + 1);
    process.stdout.write(`  reconstruction backlog: ${String(backlogSeen.size)}/${String(backlog.size)} still diverging\n`);
    for (const [step, count] of [...byStep].sort()) {
        process.stdout.write(`      ${String(count).padStart(2)}  ${step}\n`);
    }
    for (const [input, entry] of backlog) {
        if (!backlogSeen.has(input)) divergent.push({ source: policyPath, input, settledBacklog: entry });
    }
}

if (unmapped.size) {
    process.stderr.write(
        `\nmdast parity FAILED: ${String(unmapped.size)} node kind(s) have no mapping: ${[...unmapped].join(", ")}\n` +
            "Add them to scripts/lib/mdast-oracle.mjs; an unmapped kind compares equal to itself and proves nothing.\n"
    );
    process.exit(1);
}

if (divergent.length) {
    process.stderr.write(`\nmdast parity FAILED: ${String(divergent.length)} input(s) diverge\n`);
    for (const entry of divergent.slice(0, verbose ? divergent.length : 5)) {
        process.stderr.write(`\n  ${entry.source}\n${JSON.stringify(entry.input)}\n`);
        if (entry.failure) {
            process.stderr.write(`    harness error: ${entry.failure}\n`);
            continue;
        }
        if (entry.settledBacklog) {
            process.stderr.write(
                `    backlog entry now AGREES with remark. ${entry.settledBacklog.closedBy} has landed;\n` +
                    "    delete this entry from specs/mdast-parity/deltas.json in that same commit.\n"
            );
            continue;
        }
        if (entry.unreachable) {
            process.stderr.write(
                `    registered divergence \`${entry.unreachable.id}\` is no longer in the corpus, so\n` +
                    "    nothing checks that it still reproduces. Restore the input or retire the entry.\n"
            );
            continue;
        }
        if (entry.settled) {
            process.stderr.write(
                `    registered divergence \`${entry.settled.id}\` no longer reproduces: the two now agree.\n` +
                    "    Remove the entry from specs/mdast-parity/deltas.json, with review.\n"
            );
            continue;
        }
        process.stderr.write(`    --- remark ---\n${entry.remark.replace(/^/gm, "    ")}\n`);
        process.stderr.write(`    --- markdown-core ---\n${entry.ours.replace(/^/gm, "    ")}\n`);
    }
    if (!verbose && divergent.length > 5) {
        process.stderr.write(`\n  ... ${String(divergent.length - 5)} more; re-run with --verbose\n`);
    }
    process.stderr.write(
        "\nEach divergence is either a defect or a deliberate difference. A deliberate one is\n" +
            "registered in specs/mdast-parity/deltas.json and written into docs/deprecated/specs/canonical-ast.md.\n"
    );
    process.exit(1);
}

process.stdout.write("\nmdast parity gate passed.\n");
