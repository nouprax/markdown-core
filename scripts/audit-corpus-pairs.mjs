/** Grammar-corpus conformance, followed by the separate legacy structural audit.
 * Native execution checks grammar-derived expectations; the grammar proofs are
 * in benchmark-grammar-corpus.md, not inferred from native tree topology. */
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { equalProofTrees, proofTree, proofWorkload, structuralPair, validatePairs } from "./lib/corpus-pairs.mjs";
import { productionProofs } from "./lib/pair-productions.mjs";
import { boundarySource, pairReview } from "./lib/pair-review.mjs";
import { parseCanonicalDump, parseUpstreamXml } from "./lib/upstream-cmark.mjs";
import { buildGrammarCorpus, auditGrammarCorpus } from "./lib/grammar-corpus.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const BENCHMARKS = path.join(root, "packages/markdown-core/benchmarks");
const DUMP = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");

function fail(message) {
    process.stderr.write(`corpus pair audit: ${message}\n`);
    process.exit(1);
}

/* The PINNED oracle, resolved from the pin rather than from whatever is
 * installed. `init-environment.sh` installs into a directory named for the
 * version and does not remove its siblings, so an older checkout sits beside
 * the current one and picking the first that happens to sort would validate
 * these pairs against a different engine than the benchmark measures them
 * against. The pin lives in one place and both read it -- there is no second
 * copy of the version here to fall out of step with it.
 */
function oracle(name, binary, versionKey, commitKey) {
    const script = fs.readFileSync(path.join(root, "scripts/init-environment.sh"), "utf8");
    const version = new RegExp(`^${versionKey}=(.+)$`, "mu").exec(script)?.[1];
    const commit = new RegExp(`^${commitKey}=([0-9a-f]{40})$`, "mu").exec(script)?.[1];
    if (!version || !commit) fail(`scripts/init-environment.sh does not pin ${name}`);
    const checkout = path.join(root, ".tools", name, version);
    const install = `scripts/init-environment.sh --install oracle-${name}`;
    const file = path.join(checkout, "build/src", binary);
    if (!fs.existsSync(file)) fail(`the pinned ${name} ${version} oracle is not built; run: ${install}`);
    const head = execFileSync("git", ["-C", checkout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
    if (head !== commit) {
        fail(
            `the ${name} oracle checkout is at ${head}, but ${name} ${version} is pinned to ${commit}; run: ${install}`
        );
    }
    /* HEAD is not the tree. `--install` checks the commit out without cleaning
     * what is already there, so an edited source file or a stale build sits at
     * the pinned commit and this would certify every pair against it while
     * calling the result pinned. The benchmark driver rejects a dirty oracle
     * for the same reason and this is the same check, untracked files
     * included: a stray `src/config.h` is not tracked and not ignored by
     * cmark's .gitignore, and the source directory is on the include path
     * ahead of the build directory, so it shadows the generated header and the
     * binary is no longer the pinned commit while a tracked-only check calls
     * the tree clean. */
    const dirty = execFileSync("git", ["-C", checkout, "status", "--porcelain", "--untracked-files=all"], {
        encoding: "utf8"
    }).trim();
    if (dirty) {
        fail(`the ${name} oracle checkout has local modifications, so it is not ${name} ${version}:\n${dirty}`);
    }
    return file;
}

function main() {
    if (!fs.existsSync(DUMP)) fail(`this parser is not built; run: pnpm build:c`);
    const cmark = oracle("cmark", "cmark", "CMARK_VERSION", "CMARK_COMMIT");
    const gfm = oracle("cmark-gfm", "cmark-gfm", "CMARK_GFM_VERSION", "CMARK_GFM_COMMIT");
    const GFM_EXTENSIONS = ["table", "strikethrough", "autolink", "tasklist", "footnotes"];
    const grammarAudit = auditGrammarCorpus(buildGrammarCorpus(), (engine, input) => {
        const binary = engine === "core" ? DUMP : engine === "gfm" ? gfm : cmark;
        const args =
            engine === "core"
                ? []
                : [...(engine === "gfm" ? GFM_EXTENSIONS.flatMap((extension) => ["-e", extension]) : []), "-t", "xml"];
        const output = execFileSync(binary, args, {
            input,
            encoding: "utf8",
            maxBuffer: 32 << 20,
            timeout: 10000,
            killSignal: "SIGKILL"
        });
        return engine === "core" ? parseCanonicalDump(output) : parseUpstreamXml(output);
    });
    process.stdout.write(`  grammar-certified corpus: ${JSON.stringify(grammarAudit)}\n`);
    const manifest = JSON.parse(fs.readFileSync(path.join(BENCHMARKS, "corpus.json"), "utf8"));
    validatePairs(manifest);
    const cases = new Map((manifest.cases ?? []).map((entry) => [entry.name, entry]));

    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "corpus-pairs-"));
    process.on("exit", () => fs.rmSync(directory, { recursive: true, force: true }));
    execFileSync(
        "node",
        [path.join(root, "scripts/benchmark-stages.mjs"), "--corpus-only", "--quiet", "--out", directory],
        {
            cwd: root,
            stdio: ["ignore", "ignore", "pipe"]
        }
    );
    const corpus = path.join(directory, "corpus");
    const document = (name) => path.join(corpus, `${name}.x1.md`);
    const ours = (name, kind) => {
        const tree = execFileSync(DUMP, [document(name)], { encoding: "utf8", maxBuffer: 1 << 30 });
        return (tree.match(new RegExp(`(?:^|\\s)${kind} scope=`, "gmu")) ?? []).length;
    };
    const theirs = (name, kind, reference) => {
        const entry = cases.get(name);
        const format = reference.html === undefined ? "xml" : "html";
        const rendered = entry?.gfm
            ? execFileSync(gfm, [...GFM_EXTENSIONS.flatMap((e) => ["-e", e]), "-t", format, document(name)], {
                  encoding: "utf8",
                  maxBuffer: 1 << 30
              })
            : execFileSync(cmark, ["-t", format, document(name)], { encoding: "utf8", maxBuffer: 1 << 30 });
        if (reference.html !== undefined) return rendered.split(reference.html).length - 1;
        /* `<item>` and `<item sourcepos=...>` are the same element. A LIST of
         * element names is summed, because one of our kinds can be two of
         * theirs: cmark-gfm's header row is `table_header` and every other row
         * is `table_row`, and both are a TableRow here. */
        const elements = Array.isArray(reference.xml) ? reference.xml : [reference.xml];
        return elements.reduce(
            (total, element) => total + (rendered.match(new RegExp(`<${element}[ >/]`, "gu")) ?? []).length,
            0
        );
    };
    const named = (reference) =>
        reference.html === undefined
            ? (Array.isArray(reference.xml) ? reference.xml : [reference.xml]).map((e) => `<${e}>`).join("+")
            : reference.html;

    // A candidate's kind census detects workload drift, not structural equivalence.
    const census = (name) => {
        const tree = execFileSync(DUMP, [document(name)], { encoding: "utf8", maxBuffer: 1 << 30 });
        const counts = new Map();
        for (const [, kind] of tree.matchAll(/(?:^|\s)([A-Za-z]+) scope=/gmu)) {
            counts.set(kind, (counts.get(kind) ?? 0) + 1);
        }
        return counts;
    };

    const failures = [];
    // A proof is checked against all three executions, not two censuses.
    for (const pair of manifest.pairs.filter(structuralPair)) {
        const scales = fs.readdirSync(corpus).filter((file) => file.startsWith(`${pair.case}.x`));
        if (!scales.length) fail(`${pair.case}: no generated proof workloads`);
        const trials = [...scales];
        const production = productionProofs.get(pair.contract.proof);
        if (production) {
            const values = production.unique
                ? ["999999", "000000", "123456"]
                : ["999999", "000000", "123456", "000000"];
            for (const [name, template] of [
                [pair.case, production.dialect],
                [pair.isomorph, production.common]
            ]) {
                fs.writeFileSync(
                    path.join(corpus, `${name}.domain.md`),
                    values.map((n) => template.replaceAll("{n:6}", n)).join("")
                );
            }
            trials.push(`${pair.case}.domain.md`);
        }
        for (const file of trials) {
            const suffix = file.slice(pair.case.length);
            const input = (name) => path.join(corpus, name + suffix);
            const expected = proofWorkload(
                pair,
                fs.readFileSync(input(pair.case), "utf8"),
                fs.readFileSync(input(pair.isomorph), "utf8")
            );
            for (const [side, name] of [
                ["dialect", pair.case],
                ["common", pair.isomorph],
                ["reference", pair.isomorph]
            ]) {
                const output =
                    side === "reference"
                        ? execFileSync(
                              cases.get(name).gfm ? gfm : cmark,
                              [
                                  ...(cases.get(name).gfm ? GFM_EXTENSIONS.flatMap((e) => ["-e", e]) : []),
                                  "-t",
                                  "xml",
                                  input(name)
                              ],
                              { encoding: "utf8", maxBuffer: 1 << 30 }
                          )
                        : execFileSync(DUMP, [input(name)], { encoding: "utf8", maxBuffer: 1 << 30 });
                const tree = side === "reference" ? parseUpstreamXml(output) : parseCanonicalDump(output);
                const referenceHtml =
                    side === "reference" && productionProofs.get(pair.contract.proof)?.referenceHtml
                        ? execFileSync(gfm, [...GFM_EXTENSIONS.flatMap((e) => ["-e", e]), "-t", "html", input(name)], {
                              encoding: "utf8",
                              maxBuffer: 1 << 30
                          })
                        : undefined;
                if (!equalProofTrees(proofTree(pair, side, tree, expected, referenceHtml), expected)) {
                    failures.push(`${file}: ${side} output violates ${pair.contract.proof}`);
                }
            }
        }
        process.stdout.write(
            `  ${pair.case}: ${pair.contract.proof} (complete ordered trees, ${scales.length} scales${production ? ", domain extremes" : ""})\n`
        );
    }
    for (const pair of manifest.pairs.filter((pair) => pair.contract.review)) {
        const review = pairReview(pair);
        if (review.baseline) {
            for (const file of fs.readdirSync(corpus).filter((file) => file.startsWith(`${pair.case}.x`))) {
                const suffix = file.slice(pair.case.length);
                const source = fs.readFileSync(path.join(corpus, file), "utf8");
                const baseline = fs.readFileSync(path.join(corpus, review.baseline + suffix), "utf8");
                if (boundarySource(review.id, source) !== baseline) failures.push(`${file}: boundary source mismatch`);
                // Parse both complete documents: cuts must remain valid parser workloads.
                execFileSync(DUMP, [path.join(corpus, file)], { maxBuffer: 1 << 30 });
                execFileSync(DUMP, [path.join(corpus, review.baseline + suffix)], { maxBuffer: 1 << 30 });
            }
        }
        process.stdout.write(
            `  reviewed ${review.id}: ${review.outcome}; ${review.proofs.length} proof domains${review.baseline ? `; ${review.baseline}` : ""}\n`
        );
    }
    const brokenSubstitutions = new Set();
    // Keep the old sample witnesses after their individual adjudications.
    // Unlike the proved-domain checks above, these counts do not observe order,
    // parentage or payload values and cannot authorize a same-job comparison.
    const elements = manifest.substitutionReference ?? {};
    const substitutions = manifest.pairs.filter((pair) => pair.substitution);
    const reached = new Set();
    for (const pair of substitutions) {
        const mine = census(pair.isomorph);
        for (const kind of mine.keys()) reached.add(kind);
        let broke = false;
        const shown = [];
        for (const [kind, built] of mine) {
            const element = elements[kind];
            if (!element) {
                failures.push(
                    `${pair.isomorph} builds ${built} ${kind} and corpus.json does not say which element the ` +
                        `reference builds for it. A substitution pair claims the whole tree, so a kind with no ` +
                        `declared element is unchecked rather than absent`
                );
                broke = true;
                continue;
            }
            const theirCount = theirs(pair.isomorph, kind, { xml: element });
            if (theirCount !== built) {
                failures.push(
                    `${pair.isomorph} builds ${built} ${kind} through this parser and ${theirCount} ` +
                        `<${element}> through cmark. A substitution pair's halves are the same bytes under a ` +
                        `change of marker, and the ratio it feeds divides by cmark on this document`
                );
                broke = true;
                continue;
            }
            shown.push(`${kind}=${built}`);
        }
        /* A declared element nothing ever builds rots the same way a stale
         * `shared` entry does, so it is named once across the three pairs
         * below rather than passed over here. The kinds are collected as the
         * loop goes, since dumping these documents a second time to ask the
         * same question costs a full parse each. */
        if (broke) {
            brokenSubstitutions.add(pair.case);
            continue;
        }
        process.stdout.write(`  ${pair.case.padEnd(28)} ${shown.join("  ")}\n`);
    }
    {
        const stale = Object.keys(elements).filter((kind) => !reached.has(kind));
        if (stale.length) {
            failures.push(
                `substitutionReference declares an element for ${stale.join(", ")}, which no substitution pair ` +
                    `builds. An entry nothing reaches is not checked by anything and will rot`
            );
        }
    }

    /* Failed PAIRS, not failure messages. One pair appends a message per
     * construct it counts plus one per absence, so subtracting the message
     * count reports fewer pairs checked than were checked and goes negative
     * once a pair breaks on more kinds than there are pairs. Every message is
     * still printed; only the tally reads this. */
    const broken = new Set();
    const declarations = manifest.pairs.filter((pair) => pair.counts);
    for (const pair of declarations) {
        /* EVERY construct the pair's claim counts, not only the primary one. A
         * pair that also counts items and paragraphs is claiming the reference
         * built those too, and checking the list alone would pass a reference
         * that emitted one list per unit while losing everything inside it. */
        const wanted = [
            { kind: pair.counts.isomorph, mineKind: pair.counts.case },
            ...(pair.alsoCounts ?? []).map((also) => ({ kind: also.isomorph, mineKind: also.case }))
        ];
        const reference = pair.reference ?? {};
        let broke = false;
        const shown = [];
        for (const { kind } of wanted) {
            const declared = reference[kind];
            if (!declared || (declared.xml === undefined && declared.html === undefined)) {
                failures.push(
                    `${pair.case} counts ${kind} on ${pair.isomorph} and does not say how the reference counts ` +
                        `it. Every construct a pair's claim rests on must be checked against the engine that ` +
                        `is supposed to have built it`
                );
                broke = true;
                broken.add(pair.case);
                continue;
            }
            const twin = ours(pair.isomorph, kind);
            const built = theirs(pair.isomorph, kind, declared);
            if (built !== twin) {
                failures.push(
                    `${pair.isomorph} builds ${twin} ${kind} through this parser and ${built} ` +
                        `${named(declared)} through its own reference. The pair's whole claim is that the ` +
                        `reference did the same job on this document, and it did not`
                );
                broke = true;
                broken.add(pair.case);
                continue;
            }
            shown.push(`${kind}=${twin}`);
        }
        /* AND THE ABSENCE, through the reference too. A pair held by a
         * `fallback` rests on a kind being absent from both documents, and the
         * reach audit can only establish that through THIS parser. If the
         * reference stopped consuming the twin's construct -- cmark declining
         * `[ref]: /t` as a link reference definition, say -- the counted kind
         * would be untouched while the reference additionally parsed thousands
         * of paragraphs, and the benchmark would divide by an engine doing more
         * work than the pair claims. The invariant is an absence, so it is
         * checked where the absence is supposed to hold.
         *
         * `absent.case` is not checked here: it names the DIALECT document, and
         * no reference parses that document as the pair's claim. */
        for (const kind of [pair.fallback, ...(pair.absent?.isomorph ?? [])].filter(Boolean)) {
            const declared = reference[kind];
            if (!declared || (declared.xml === undefined && declared.html === undefined)) {
                failures.push(
                    `${pair.case} is held by ${kind} being absent and does not say how the reference counts ` +
                        `it. An absence this parser alone confirms is half an invariant`
                );
                broke = true;
                broken.add(pair.case);
                continue;
            }
            const built = theirs(pair.isomorph, kind, declared);
            if (built !== 0) {
                failures.push(
                    `${pair.isomorph} builds ${built} ${named(declared)} through its own reference, and the ` +
                        `pair is held by ${kind} being absent. The reference is no longer doing the job the ` +
                        `pair claims it does`
                );
                broke = true;
                broken.add(pair.case);
                continue;
            }
            shown.push(`${kind}=0`);
        }
        if (broke) continue;
        process.stdout.write(`  ${pair.case.padEnd(28)} ${shown.join("  ")}\n`);
    }

    process.stdout.write(
        `\n  legacy witnesses checked against the reference ${declarations.length - broken.size + substitutions.length - brokenSubstitutions.size}/${declarations.length + substitutions.length}\n`
    );
    if (failures.length) {
        process.stderr.write(`corpus pair audit FAILED\n    ${failures.join("\n    ")}\n`);
        process.exit(1);
    }
    process.stdout.write(
        "corpus pair audit passed (47 grammar certificates; legacy structural contracts remain separate)\n"
    );
}

main();
