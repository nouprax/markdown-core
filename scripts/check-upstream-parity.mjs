#!/usr/bin/env node
/**
 * Upstream-parity gate.
 *
 * Runs one of the two C-family authorities through the same fail-closed
 * comparison algorithm: cmark for CommonMark, cmark-gfm for its extension
 * layer. Each has its own corpus and delta registry.
 *
 * Product-owned expected blocks are canonical dumps this parser produced, so
 * they pin behaviour without independently proving it right. These external
 * parsers provide that independent evidence within their disjoint scopes.
 *
 *   node scripts/check-upstream-parity.mjs --oracle commonmark|gfm
 *                                          [--limit N] [--verbose]
 */

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { readExamples, selectExamples } from "./lib/fixture-corpus.mjs";

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
const args = process.argv.slice(2);
const verbose = args.includes("--verbose");
const limitIndex = args.indexOf("--limit");
const limit = limitIndex >= 0 ? Number(args[limitIndex + 1]) : Infinity;
const oracleIndex = args.indexOf("--oracle");
const oracleName = oracleIndex >= 0 ? args[oracleIndex + 1] : "commonmark";
const ORACLES = {
    commonmark: {
        policyPath: "specs/oracles/cmark/deltas.json",
        binary(policy) {
            return `.tools/cmark/${policy.upstream.version}/build/src/cmark`;
        },
        install: "scripts/init-environment.sh --install oracle-cmark",
        label: "cmark",
        profile: "commonmark",
        extensions: []
    },
    gfm: {
        policyPath: "specs/oracles/cmark-gfm/deltas.json",
        binary(policy) {
            return `.tools/cmark-gfm/${policy.upstream.version}/build/src/cmark-gfm`;
        },
        install: "scripts/init-environment.sh --install oracle-cmark-gfm",
        label: "cmark-gfm",
        profile: "gfm",
        extensions: ["table", "strikethrough", "autolink", "tasklist", "footnotes"]
    }
};
const oracle = ORACLES[oracleName];
if (!oracle) {
    process.stderr.write(`upstream parity: unknown oracle ${JSON.stringify(oracleName)}; use commonmark or gfm\n`);
    process.exit(2);
}
const policyPath = oracle.policyPath;
const policy = JSON.parse(fs.readFileSync(path.join(root, policyPath), "utf8"));

// Both binaries are built products of this repository's own toolchain, so a
// missing one is a setup error with a specific fix, not a reason to skip.
const ours = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");
const upstream = path.join(root, oracle.binary(policy));
for (const [binary, fix] of [
    [ours, "pnpm build:c"],
    [upstream, oracle.install]
]) {
    if (!fs.existsSync(binary)) {
        process.stderr.write(`upstream parity: missing ${path.relative(root, binary)}\nBuild it with: ${fix}\n`);
        process.exit(1);
    }
}

function runUpstream(input, flags) {
    const argv = ["--to", "xml", ...flags];
    for (const extension of oracle.extensions) argv.push("-e", extension);
    return execFileSync(upstream, argv, { input, encoding: "utf8", maxBuffer: 1 << 28 });
}

function runOurs(input, profile) {
    return execFileSync(ours, ["--profile", profile], { input, encoding: "utf8", maxBuffer: 1 << 28 });
}

/**
 * The corpus is policy-driven. CommonMark and GFM select different sections
 * and profiles so a parser is never treated as authoritative outside its
 * language layer.
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
        const profile = typeof entry === "string" ? oracle.profile : (entry.profile ?? oracle.profile);
        const flags = typeof entry === "string" ? [] : (entry.upstreamFlags ?? []);
        const selection = typeof entry === "string" ? undefined : entry.selection;
        const selected = selectExamples(readExamples(root, file), selection, policyPath);
        return selected.map((example) => ({
            line: example.source,
            input: example.input,
            profile,
            flags
        }));
    });
}

/* Which projections actually acted, accumulated over the whole corpus. A
 * projected delta that never acts asserts nothing, which is the same hole the
 * input-keyed half closed years ago by requiring every registered input to
 * still reproduce. */
const fired = new Set();

function compare(input, profile = oracle.profile, flags = []) {
    const upstreamTree = liftFootnoteDefinitions(
        normalize(parseUpstreamXml(runUpstream(input, flags)), "upstream", fired),
        fired
    );
    // `footnote-resolution-model` is applied before `normalize`, which keeps
    // only the compared fields and so drops the labels the model reads.
    const ourTree = liftFootnoteDefinitions(
        normalize(
            applyUpstreamReferenceModel(
                applyUpstreamFootnoteModel(parseCanonicalDump(runOurs(input, profile)), fired),
                fired
            ),
            "ours",
            fired
        ),
        fired
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
const PROJECTED_DELTAS = new Set([
    "own-extensions",
    "footnote-definition-placement",
    "footnote-resolution-model",
    "reference-definition-node",
    "empty-text-node"
]);
/* `own-extensions` is the one projection that is not a tree rewrite: it is the
 * CORPUS PROFILE. The extension fixtures run under `--profile gfm`, which
 * detaches this repository's own two extensions so the comparison is of one
 * language, and there is nothing for a normalizer to report. Every other
 * projection acts on a tree and says so. */
const UNTRACKED_PROJECTIONS = new Set(["own-extensions"]);
for (const delta of policy.deltas) {
    if (!PROJECTED_DELTAS.has(delta.id) && !(policy.expectedDivergences ?? []).some((e) => e.id === delta.id)) {
        process.stderr.write(
            `upstream parity: registered delta \`${delta.id}\` is neither projected by the normalizer nor\n` +
                "keyed to an input. A delta the gate does not act on is prose, not a rule.\n"
        );
        process.exit(1);
    }
}
const expected = new Map((policy.expectedDivergences ?? []).map((entry) => [entry.input, entry]));
const reproduced = new Set();

// A PENDING entry describes a divergence a reconstruction step has not created
// yet: this engine agrees with upstream where the entry expects it to differ,
// and the entry moves into `deltas` in the step its `pendingStep` names. That
// was documented and nothing read it, so a step could land its fix, leave its
// entry pending and register a second id for the same difference with every
// gate green -- which is what Step 10 did. A pending input that has started
// diverging now fails here, naming the step that owes the activation.
const pending = new Map((policy.pendingExpectedDivergences ?? []).map((entry) => [entry.input, entry]));
const pendingStep = new Map((policy.pendingDeltas ?? []).map((delta) => [delta.id, delta.pendingStep]));

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
    const owed = pending.get(testCase.input);
    if (result.upstream !== result.ours) {
        if (registered) reproduced.add(testCase.input);
        else if (owed) divergent.push({ ...testCase, activate: owed, ...result });
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
            divergent.push({ line: policyPath, input, unreachable: entry });
        }
    }
}

process.stdout.write(
    `upstream parity: ${String(cases.length - divergent.length)}/${String(cases.length)} inputs agree with ` +
        `${oracle.label} ${policy.upstream.version}\n`
);
process.stdout.write(`  registered deltas: ${policy.deltas.map((d) => d.id).join(", ")}\n`);
process.stdout.write(
    `  registered divergences: ${String(reproduced.size)}/${String(expected.size)} inputs reproduced\n`
);

/* The projected half of the registry, held to the same rule as the keyed half.
 * A projection that never acts over the whole corpus is describing a difference
 * this engine does not have -- which is what `reference-definition-node` was
 * doing: registered as though Step 9b had landed, acting on nothing, and
 * excusing a comparison nobody was making. It is in `pendingDeltas` now, and a
 * pending projection that STARTS acting fails below for the same reason a
 * pending input that starts diverging does. */
if (corpusOverride < 0 && limit === Infinity) {
    const pendingIds = new Set((policy.pendingDeltas ?? []).map((delta) => delta.id));
    const activeIds = new Set(policy.deltas.map((delta) => delta.id));
    const tracked = [...PROJECTED_DELTAS].filter(
        (id) => activeIds.has(id) && !UNTRACKED_PROJECTIONS.has(id) && !pendingIds.has(id)
    );
    const silent = tracked.filter((id) => !fired.has(id));
    const pendingProjections = (policy.pendingDeltas ?? []).filter((delta) => delta.projected);
    const acting = pendingProjections.filter((delta) => fired.has(delta.id));
    process.stdout.write(
        `  registered projections: ${String(tracked.length - silent.length)}/${String(tracked.length)} acted` +
            `${pendingProjections.length ? `, ${String(pendingProjections.length)} pending` : ""}\n`
    );
    if (silent.length > 0) {
        process.stderr.write(
            `\nupstream parity FAILED: projected delta(s) ${silent.join(", ")} acted on no corpus input.\n` +
                "A projection that never acts describes a difference this engine does not have.\n" +
                "Move it to `pendingDeltas` with a `pendingStep`, or retire it.\n"
        );
        process.exit(1);
    }
    if (acting.length > 0) {
        for (const delta of acting) {
            process.stderr.write(
                `\nupstream parity FAILED: PENDING projection \`${delta.id}\` has started acting, so the step\n` +
                    `that creates it has landed: ${delta.pendingStep ?? "step unnamed"}.\n` +
                    "Move it from `pendingDeltas` into `deltas` in that step's commit.\n"
            );
        }
        process.exit(1);
    }
}

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
        if (entry.activate) {
            process.stderr.write(
                `    PENDING divergence \`${entry.activate.id}\` has started reproducing, so the step that\n` +
                    `    creates it has landed: ${pendingStep.get(entry.activate.id) ?? "step unnamed"}.\n` +
                    "    Move it from `pendingDeltas`/`pendingExpectedDivergences` into `deltas`/`expectedDivergences`\n" +
                    "    in that step's commit. Do not register a second entry for the same difference.\n"
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
        process.stderr.write(`    --- ${oracle.label} ---\n${entry.upstream.replace(/^/gm, "    ")}\n`);
        process.stderr.write(`    --- markdown-core ---\n${entry.ours.replace(/^/gm, "    ")}\n`);
    }
    if (!verbose && divergent.length > 5) {
        process.stderr.write(`\n  ... ${String(divergent.length - 5)} more; re-run with --verbose\n`);
    }
    process.stderr.write(
        "\nEach divergence is either a defect or a deliberate difference. A deliberate one is\n" +
            `registered in ${policyPath} and written into docs/specs/canonical-ast.md;\n` +
            "it is never accepted on the grounds that this implementation is obviously right.\n"
    );
    process.exit(1);
}

process.stdout.write("\nupstream parity gate passed.\n");
