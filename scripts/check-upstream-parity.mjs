#!/usr/bin/env node
/**
 * Upstream-parity gate.
 *
 * Parses every corpus input with both this repository's parser and upstream
 * cmark-gfm, normalizes the two ASTs into one comparable form, and requires
 * them to agree except where `specs/upstream-parity/deltas.json` registers a
 * difference.
 *
 * This is the only thing in the repository that checks Markdown Core's
 * semantics against an authority outside itself. The spec fixtures cannot: the
 * expected blocks are canonical dumps this parser produced, so they pin the
 * behaviour without proving it right.
 *
 *   node scripts/check-upstream-parity.mjs [--limit N] [--verbose]
 */

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { readExamples } from "./lib/fixture-corpus.mjs";

import {
    applyUpstreamFootnoteModel,
    applyUpstreamReferenceModel,
    liftFootnoteDefinitions,
    normalize,
    parseCanonicalDump,
    parseUpstreamXml,
    render,
    unknownKinds
} from "./lib/upstream-cmark.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const policy = JSON.parse(fs.readFileSync(path.join(root, "specs/upstream-parity/deltas.json"), "utf8"));

const args = process.argv.slice(2);
const verbose = args.includes("--verbose");
const limitIndex = args.indexOf("--limit");
const limit = limitIndex >= 0 ? Number(args[limitIndex + 1]) : Infinity;

// Both binaries are built products of this repository's own toolchain, so a
// missing one is a setup error with a specific fix, not a reason to skip.
const ours = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");
const upstream = path.join(root, `.tools/cmark-gfm/${policy.upstream.version}/build/src/cmark-gfm`);
for (const [binary, fix] of [
    [ours, "pnpm build:c"],
    [upstream, "scripts/init-environment.sh --install upstream-cmark"]
]) {
    if (!fs.existsSync(binary)) {
        process.stderr.write(`upstream parity: missing ${path.relative(root, binary)}\nBuild it with: ${fix}\n`);
        process.exit(1);
    }
}

// The four upstream extensions this repository also ships. Its own four stay
// off through the parser's `gfm` profile, so both sides parse one language.
const UPSTREAM_EXTENSIONS = ["table", "strikethrough", "autolink", "tasklist", "footnotes"];

function runUpstream(input, flags) {
    const argv = ["--to", "xml", ...flags];
    for (const extension of UPSTREAM_EXTENSIONS) argv.push("-e", extension);
    return execFileSync(upstream, argv, { input, encoding: "utf8", maxBuffer: 1 << 28 });
}

function runOurs(input, profile) {
    return execFileSync(ours, ["--profile", profile], { input, encoding: "utf8", maxBuffer: 1 << 28 });
}

/**
 * The corpus is policy-driven. The GFM spec fixture carries the examples the
 * upstream project is specified by; the extension fixtures are here for a
 * different reason, and it is the one that makes them worth running.
 *
 * This repository's own syntax has no external authority for what it means —
 * but it has one for what it must *not* do. `[[wiki]]` and `![[embed]]`
 * collide with CommonMark's nested-bracket link forms, so running those inputs
 * with the extensions off proves the collision is opt-in: with `--profile gfm`
 * the parse must be upstream's, byte for byte.
 */
// `--corpus` replaces the policy's corpus for one run. It exists for
// scripts/fuzz-parity.mjs, which feeds generated inputs through this gate
// rather than reimplementing the comparison; without it the fuzzer would have
// to rewrite the policy file and could leave it damaged if interrupted.
const corpusOverride = process.argv.indexOf("--corpus");
// A corpus entry is a path, or an object naming the option profile the file is
// written for. Smart punctuation is the case that needs it: both parsers
// default it off, so a fixture that asserts what `"quotes"` become has to be
// run with it on — on both sides, or the comparison is between two languages.
function corpus() {
    const entries = corpusOverride >= 0 ? [process.argv[corpusOverride + 1]] : (policy.corpus ?? []);
    return entries.flatMap((entry) => {
        const file = typeof entry === "string" ? entry : entry.file;
        const profile = typeof entry === "string" ? "gfm" : entry.profile;
        const flags = typeof entry === "string" ? [] : (entry.upstreamFlags ?? []);
        return readExamples(root, file).map((example) => ({
            line: example.source,
            input: example.input,
            profile,
            flags
        }));
    });
}

function compare(input, profile = "gfm", flags = []) {
    const upstreamTree = liftFootnoteDefinitions(normalize(parseUpstreamXml(runUpstream(input, flags)), "upstream"));
    // `footnote-resolution-model` is applied before `normalize`, which keeps
    // only the compared fields and so drops the labels the model reads.
    const ourTree = liftFootnoteDefinitions(
        normalize(
            applyUpstreamReferenceModel(applyUpstreamFootnoteModel(parseCanonicalDump(runOurs(input, profile)))),
            "ours"
        )
    );
    const unmapped = new Set([...unknownKinds(upstreamTree), ...unknownKinds(ourTree)]);
    return { upstream: render(upstreamTree), ours: render(ourTree), unmapped };
}

/**
 * A registered delta is handled one of two ways, and which one is a claim about
 * how wide the difference is.
 *
 * A *model* delta is a rule, not a list of strings: it shows up wherever the
 * construct appears, so it is applied as a projection in the normalizer above
 * and named here only so the two stay in step. An *input* delta is a point
 * difference in upstream's own behaviour with no rule to project — it is keyed
 * by the exact input that exhibits it.
 *
 * Either way the entry must still reproduce. A registry entry describing a
 * difference that has gone away would otherwise sit here forever excusing a
 * comparison nobody has looked at since.
 */
const NORMALIZED_DELTAS = new Set([
    "own-extensions",
    "footnote-definition-placement",
    "footnote-resolution-model",
    "reference-definition-node"
]);
for (const delta of policy.deltas) {
    if (!NORMALIZED_DELTAS.has(delta.id) && !(policy.expectedDivergences ?? []).some((e) => e.id === delta.id)) {
        process.stderr.write(
            `upstream parity: registered delta \`${delta.id}\` is neither projected by the normalizer nor\n` +
                "keyed to an input. A delta the gate does not act on is prose, not a rule.\n"
        );
        process.exit(1);
    }
}
const expected = new Map((policy.expectedDivergences ?? []).map((entry) => [entry.input, entry]));
const reproduced = new Set();

const cases = corpus().slice(0, limit);
const divergent = [];
const unmappedKinds = new Set();
for (const testCase of cases) {
    let result;
    try {
        result = compare(testCase.input, testCase.profile, testCase.flags);
    } catch (error) {
        divergent.push({ ...testCase, failure: String(error).slice(0, 300) });
        continue;
    }
    for (const kind of result.unmapped) unmappedKinds.add(kind);
    const registered = expected.get(testCase.input);
    if (result.upstream !== result.ours) {
        if (registered) reproduced.add(testCase.input);
        else divergent.push({ ...testCase, ...result });
    } else if (registered) {
        divergent.push({ ...testCase, settled: registered, ...result });
    }
}

// Under `--corpus` the corpus is one generated input, so an entry going
// unexercised says nothing. Over the policy's own corpus it says the entry is
// no longer reachable, which is the same rot as one that stopped reproducing.
if (corpusOverride < 0 && limit === Infinity) {
    for (const [input, entry] of expected) {
        // Keyed by INPUT, not by id. Keyed by id, one input that still
        // diverges vouched for every other input registered under the same
        // id, so a registration that had stopped being true sat there
        // excusing a comparison nobody ran — which is the exact failure the
        // delta block above refuses.
        if (!reproduced.has(input)) {
            divergent.push({ line: "specs/upstream-parity/deltas.json", input, unreachable: entry });
        }
    }
}

process.stdout.write(
    `upstream parity: ${String(cases.length - divergent.length)}/${String(cases.length)} inputs agree with ` +
        `cmark-gfm ${policy.upstream.version}\n`
);
process.stdout.write(`  registered deltas: ${policy.deltas.map((d) => d.id).join(", ")}\n`);
process.stdout.write(`  registered divergences: ${String(reproduced.size)}/${String(expected.size)} inputs reproduced\n`);

if (unmappedKinds.size) {
    // Two unmapped kinds render alike and would compare equal, so this reads as
    // agreement when it is actually an untranslated part of the AST.
    process.stderr.write(
        `\nupstream parity FAILED: ${String(unmappedKinds.size)} node kind(s) have no mapping: ` +
            `${[...unmappedKinds].join(", ")}\n` +
            "Add them to scripts/lib/upstream-cmark.mjs; an unmapped kind compares equal to itself and proves nothing.\n"
    );
    process.exit(1);
}

if (divergent.length) {
    process.stderr.write(`\nupstream parity FAILED: ${String(divergent.length)} input(s) diverge\n`);
    for (const entry of divergent.slice(0, verbose ? divergent.length : 5)) {
        process.stderr.write(`\n  ${entry.line}\n`);
        process.stderr.write(`${JSON.stringify(entry.input)}\n`);
        if (entry.failure) {
            process.stderr.write(`    harness error: ${entry.failure}\n`);
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
                    "    Upstream may have fixed it. Re-review the entry before removing it.\n"
            );
            continue;
        }
        process.stderr.write(`    --- cmark-gfm ---\n${entry.upstream.replace(/^/gm, "    ")}\n`);
        process.stderr.write(`    --- markdown-core ---\n${entry.ours.replace(/^/gm, "    ")}\n`);
    }
    if (!verbose && divergent.length > 5) {
        process.stderr.write(`\n  ... ${String(divergent.length - 5)} more; re-run with --verbose\n`);
    }
    process.stderr.write(
        "\nEach divergence is either a defect or a deliberate difference. A deliberate one is\n" +
            "registered in specs/upstream-parity/deltas.json and written into docs/specs/canonical-ast.md;\n" +
            "it is never accepted on the grounds that this implementation is obviously right.\n"
    );
    process.exit(1);
}

process.stdout.write("\nupstream parity gate passed.\n");
