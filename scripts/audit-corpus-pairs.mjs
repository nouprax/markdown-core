/**
 * EVERY PAIR, CHECKED AGAINST THE REFERENCE ENGINE'S OWN OUTPUT.
 *
 * `scripts/audit-corpus-reach.mjs` holds each pair against THIS parser: both
 * sides build the same number of the construct, each side counted by the kind
 * it builds. That is necessary and it is not sufficient. The whole claim of a
 * logical pair is that the reference did the SAME JOB on the paired document,
 * and this parser's opinion of the paired document is not evidence of that: a
 * twin that cmark reads as ordinary prose dumps perfectly well through our
 * CLI, and the ratio that comes out divides by an engine that built nothing.
 *
 * That defect is not hypothetical. `pair-specimen-common` measured a document
 * in which cmark-gfm DISCARDED all 1,961 footnote definitions because none was
 * referenced, and the pair reported a shape ratio of 2.27x that was a division
 * by an engine throwing text away.
 *
 * So each pair declares, in `corpus.json`, how the reference COUNTS its half --
 * an XML element name, or an HTML marker where cmark-gfm's XML renderer prints
 * `<unknown>` for an extension node it has no name for, as it does for the
 * footnote definition. The count from the reference's own output must equal the
 * count from ours. A pair that declares nothing fails: a pair nobody checks
 * against the reference is exactly the pair that needs checking.
 *
 * This lives beside the parity oracles rather than in the reach audit because
 * it needs the pinned reference BINARIES, which that job already builds, and
 * because the reach audit must stay runnable without them.
 */
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

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
    return file;
}

function main() {
    if (!fs.existsSync(DUMP)) fail(`this parser is not built; run: pnpm build:c`);
    const cmark = oracle("cmark", "cmark", "CMARK_VERSION", "CMARK_COMMIT");
    const gfm = oracle("cmark-gfm", "cmark-gfm", "CMARK_GFM_VERSION", "CMARK_GFM_COMMIT");
    const GFM_EXTENSIONS = ["table", "strikethrough", "autolink", "tasklist", "footnotes"];
    const manifest = JSON.parse(fs.readFileSync(path.join(BENCHMARKS, "corpus.json"), "utf8"));
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

    const failures = [];
    const declarations = manifest.logicalIsomorphs ?? [];
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
                continue;
            }
            shown.push(`${kind}=${twin}`);
        }
        if (broke) continue;
        process.stdout.write(`  ${pair.case.padEnd(28)} ${shown.join("  ")}\n`);
    }

    process.stdout.write(
        `\n  pairs checked against the reference ${declarations.length - failures.length}/${declarations.length}\n`
    );
    if (failures.length) {
        process.stderr.write(`corpus pair audit FAILED\n    ${failures.join("\n    ")}\n`);
        process.exit(1);
    }
    process.stdout.write("corpus pair audit passed\n");
}

main();
