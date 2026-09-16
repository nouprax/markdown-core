/**
 * WHAT THE STAGE BENCHMARK CAN STILL SEE.
 *
 * The point of the staged benchmark is to locate hot paths. It can only report
 * on code some document drives: a construct absent from the corpus is not
 * reported as slow or as fast, it is not reported at all, and the profile
 * describes a subset of the parser while reading like the whole.
 *
 * That was not hypothetical. On the 33-case corpus this was written against,
 * the corpus built 26 of the 43 node kinds the dialect names, and the largest
 * single cost in the source stage was table's REFUSAL path on documents
 * containing no table -- every grammar that builds one went unmeasured.
 *
 * THIS IS NOT A COVERAGE GATE, and must not become one. Coverage was how the
 * blindness was diagnosed; it is not the goal, and a percentage climbing is not
 * progress. This repository retired execution coverage for that reason --
 * `scripts/audit-ci-policy.sh` refuses a coverage job outright, "use semantic
 * contract tests" -- so nothing here reports a ratio and nothing here has a
 * floor. Two laws, each naming a specific thing that is true or false:
 *
 *   Every node kind the dialect names must be BUILT by some document. The
 *   obvious alternative -- no element file at zero coverage -- is VACUOUS: the
 *   block dispatcher asks every attached element about every line, so stripping
 *   every callout still leaves `callout.c` at 40.4% from being asked and
 *   declining. Only a claimed construct puts a node in a tree.
 *
 *   Every grammar entry the corpus must measure must have RUN, IN THE CASE THAT
 *   EXISTS TO DRIVE IT, because one kind is not one grammar: the grid, pipe,
 *   simple and multiline table parsers all build `Table`, so deleting the grid
 *   case leaves every kind built while the benchmark stops measuring that
 *   parser. Naming the case matters as much as naming the grammar -- accepting
 *   any document lets `mixed-extended`, which concatenates every sample, answer
 *   for all of them, and a grammar reached only inside the concatenation has no
 *   isolated profile to read.
 *
 *   node scripts/audit-corpus-reach.mjs [--json FILE]
 */

import { execFileSync, spawn, spawnSync } from "node:child_process";
import readline from "node:readline";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const KIND_TABLE = path.join(root, "packages/markdown-core/elements/ast.c");

function fail(message) {
    process.stderr.write(`corpus reach audit FAILED\n  ${message}\n`);
    process.exit(1);
}

function parseArguments(argv) {
    const options = { json: null };
    for (let i = 0; i < argv.length; i++) {
        const flag = argv[i];
        if (flag === "--json") options.json = argv[++i];
        else fail(`unknown flag ${flag}`);
    }
    return options;
}

/* The kind table is one name per line and already read as data by the
 * projection audit; this reads the same table for the same reason. */
function dialectKinds() {
    const source = fs.readFileSync(KIND_TABLE, "utf8");
    const table = /const char \*const names\[\] = \{([\s\S]*?)\};/.exec(source);
    if (!table) fail(`no kind-name table in ${path.relative(root, KIND_TABLE)}`);
    const kinds = [...table[1].matchAll(/"([A-Za-z]+)"/g)].map((m) => m[1]).filter((name) => name !== "None");
    if (kinds.length < 2) fail("the kind-name table parsed to fewer than two kinds");
    return kinds;
}

/* Generated fresh every run, into a directory this audit owns.
 *
 * Reusing `build/benchmark-stages/corpus` would audit whatever a previous run
 * left there: an edited manifest or sample would be ignored while the directory
 * existed, and documents for cases that no longer exist would still be
 * enumerated. The audit would then pass on the previous revision's corpus,
 * which is the one failure a coverage gate must not have. Writing it costs a
 * fraction of a second, so there is nothing to reuse it for. */
function corpusDocuments(directory) {
    execFileSync(
        "node",
        [path.join(root, "scripts/benchmark-stages.mjs"), "--corpus-only", "--quiet", "--out", directory],
        { cwd: root, stdio: ["ignore", "ignore", "pipe"] }
    );
    const corpus = path.join(directory, "corpus");
    /* One scale is enough: the second repeats the same shapes at a larger size,
     * so it moves instruction counts, not which constructs appear. */
    const documents = fs
        .readdirSync(corpus)
        .filter((name) => name.endsWith(".x1.md"))
        .sort()
        .map((name) => path.join(corpus, name));
    if (!documents.length) fail("the generated corpus holds no .x1.md documents");
    return documents;
}

/* The deep-nesting cases dump gigabytes, so the kinds are read off a stream a
 * line at a time rather than from a buffered result.
 *
 * The reading is done here rather than by piping the dump through `grep`,
 * because a pipeline means a shell, and a shell means the corpus path and the
 * checkout path -- neither of which this script chooses -- are parsed as
 * command text. There is nothing a shell was providing that a line reader does
 * not, so the child is executed directly with its arguments as arguments. */
async function kindsProduced(cli, documents) {
    const seen = new Set();
    for (const document of documents) {
        /* The complexity shapes are excluded, and only here. `chain-list-depth`
         * is 32,765 levels deep, and `markdown_core_document_dump` materialises
         * the whole canonical dump in one buffer before a byte is written, so
         * asking these for a tree costs gigabytes of RSS in the child whatever
         * this end does with the stream -- enough to lose a hosted runner. They
         * are shapes for depth, not for constructs, and contribute no kind that
         * the other cases do not build; the grammar pass below still runs
         * them, through a binary that never dumps. */
        if (path.basename(document).startsWith("chain-")) continue;
        const child = spawn(cli, [document], { stdio: ["ignore", "pipe", "ignore"], timeout: 600_000 });
        const lines = readline.createInterface({ input: child.stdout, crlfDelay: Infinity });
        for await (const line of lines) {
            const match = /([A-Za-z]+) scope=/.exec(line);
            if (match) seen.add(match[1]);
        }
        const [code, signal] = await new Promise((resolve) => child.on("close", (c, s) => resolve([c, s])));
        if (signal) fail(`the dump CLI was killed by ${signal} on ${path.basename(document)}`);
        if (code !== 0) fail(`the dump CLI exited ${code} on ${path.basename(document)}`);
    }
    return seen;
}

/* A function only brushed by a guard clause is not a grammar the corpus drives,
 * so a declared entry has to be mostly executed. Measured on the four table
 * grammars: the case that owns one reaches 64-79% of it, while the cases that
 * do not reach 6.1%. */
const EXERCISED_FLOOR = 50.0;

/* The grammar entries the corpus must drive, declared BESIDE the cases rather
 * than inside them. A case that named its own obligation could retire it by
 * being deleted -- the corpus would stop measuring that grammar and the audit
 * would still pass, because the requirement left with the case. Kept here, a
 * deleted case leaves its grammar required and undriven, and the audit says so.
 * Retiring one is then an edit to this list: a visible, reviewable act, the way
 * removing a kind from the table in `ast.c` would be. */
function requiredGrammars() {
    const manifest = JSON.parse(
        fs.readFileSync(path.join(root, "packages/markdown-core/benchmarks/corpus.json"), "utf8")
    );
    const required = manifest.requiredGrammars ?? {};
    if (typeof required !== "object" || Array.isArray(required)) {
        fail("corpus.json: requiredGrammars must map each grammar entry to the case that drives it");
    }
    return Object.entries(required);
}

function resetCounters(buildDir) {
    for (const dir of gcdaDirectories(buildDir)) {
        for (const name of fs.readdirSync(dir).filter((entry) => entry.endsWith(".gcda"))) {
            fs.rmSync(path.join(dir, name), { force: true });
        }
    }
}

/* Which functions this one document drove, read with the counters reset so the
 * result is that document's alone rather than the corpus's. */
function functionsFor(cli, buildDir, document) {
    resetCounters(buildDir);
    requireClean(
        spawnSync(cli, ["--document", document], { stdio: "ignore", timeout: 600_000 }),
        "the parse runner",
        document
    );
    const percent = new Map();
    for (const dir of gcdaDirectories(buildDir)) {
        const gcda = fs
            .readdirSync(dir)
            .filter((n) => n.endsWith(".gcda"))
            .map((n) => path.join(dir, n));
        const gcov = spawnSync("gcov", ["-f", "-n", "-o", dir, ...gcda], { cwd: dir, encoding: "utf8" });
        /* A gcov that fails reads as a function that never ran, which is the
         * wrong answer given confidently: the grammar law would report the case
         * as not driving its parser when the truth is that nothing was read. */
        requireClean(gcov, "gcov", dir);
        const out = gcov.stdout ?? "";
        for (const match of out.matchAll(/Function '([^']+)'\nLines executed:([\d.]+)%/g)) {
            percent.set(match[1], Math.max(percent.get(match[1]) ?? 0, Number(match[2])));
        }
    }
    return percent;
}

/* Both binaries, instrumented, built from the working tree every run.
 *
 * `markdown-core` is the dump CLI: the kind census needs a tree it can read.
 * `markdown_core_stage_runner` parses and frees without dumping, and that is
 * what the grammar pass uses -- `elements/ast.c` is the SERIALIZER, 1,174
 * lines of it, and running the dumper counted the writer as parse phase and
 * flattered the number this audit exists to report. */
function instrumentedBuild(buildDir) {
    const run = (command, args) =>
        execFileSync(command, args, { cwd: root, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] });
    run("cmake", [
        "-S",
        root,
        "-B",
        buildDir,
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DCMAKE_C_FLAGS=-O0 --coverage",
        "-DCMAKE_EXE_LINKER_FLAGS=--coverage",
        "-DMARKDOWN_CORE_STATIC=ON",
        "-DMARKDOWN_CORE_BENCHMARKS=ON"
    ]);
    run("cmake", [
        "--build",
        buildDir,
        "--target",
        "markdown-core",
        "--target",
        "markdown_core_stage_runner",
        "--parallel",
        String(os.cpus().length)
    ]);
    const binaries = {
        dump: path.join(buildDir, "packages/markdown-core/core/markdown-core"),
        parse: path.join(buildDir, "packages/markdown-core/benchmarks/markdown_core_stage_runner")
    };
    for (const [role, file] of Object.entries(binaries)) {
        if (!fs.existsSync(file)) fail(`the coverage build produced no ${role} binary at ${file}`);
    }
    return binaries;
}

/* A document the parser cannot finish is not a weaker measurement, it is a
 * broken one: the audit would go on to read counters from a process that died
 * partway and report whatever the surviving documents happened to cover. */
function requireClean(result, what, document) {
    if (result.error) fail(`${what} could not run on ${path.basename(document)}: ${result.error.message}`);
    if (result.signal) fail(`${what} was killed by ${result.signal} on ${path.basename(document)}`);
    if (result.status !== 0) fail(`${what} exited ${result.status} on ${path.basename(document)}`);
}

function gcdaDirectories(buildDir) {
    const directories = new Set();
    const walk = (dir) => {
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            const full = path.join(dir, entry.name);
            if (entry.isDirectory()) walk(full);
            else if (entry.name.endsWith(".gcda")) directories.add(dir);
        }
    };
    walk(buildDir);
    return [...directories].sort();
}

const options = parseArguments(process.argv.slice(2));
const kinds = dialectKinds();
const corpusDir = fs.mkdtempSync(path.join(os.tmpdir(), "corpus-reach-"));
process.on("exit", () => fs.rmSync(corpusDir, { recursive: true, force: true }));
const documents = corpusDocuments(corpusDir);

process.stdout.write(`Corpus reach over ${documents.length} documents\n\n`);

const required = requiredGrammars();
const driven = new Set();
const undriven = [];
let unbuilt;
{
    const buildDir = fs.mkdtempSync(path.join(os.tmpdir(), "corpus-coverage-"));
    try {
        /* Everything reads binaries built here, from the working tree. Reusing
         * whatever `build/` happened to hold would let a source change that
         * stops building a kind still report every kind built, which is the
         * same staleness the corpus itself was fixed for. */
        const binaries = instrumentedBuild(buildDir);
        const produced = await kindsProduced(binaries.dump, documents);
        unbuilt = kinds.filter((kind) => !produced.has(kind));
        /* The census ran the dumper, whose lines are not the parse phase, so
         * its counters are discarded before anything is measured. */
        resetCounters(buildDir);
        /* Only the named cases are run here, each alone with the counters
         * reset, so what that document drives is separated from what the
         * corpus drives together. */
        for (const [grammar, name] of required) {
            const document = documents.find((file) => path.basename(file) === `${name}.x1.md`);
            if (!document) {
                undriven.push(`${grammar} names case ${name}, and the corpus holds no document for it`);
                continue;
            }
            const percent = functionsFor(binaries.parse, buildDir, document).get(grammar);
            if (percent === undefined) {
                undriven.push(`${grammar} was never compiled into the parser`);
            } else if (percent < EXERCISED_FLOOR) {
                undriven.push(
                    `${name} drives only ${percent.toFixed(1)}% of ${grammar}, under the ` +
                        `${EXERCISED_FLOOR.toFixed(1)}% a case that owns a grammar reaches`
                );
            } else {
                driven.add(grammar);
            }
        }
    } finally {
        fs.rmSync(buildDir, { recursive: true, force: true });
    }
}
process.stdout.write("\n");

process.stdout.write(
    `  node kinds built     ${kinds.length - unbuilt.length}/${kinds.length}\n` +
        `  grammars driven      ${required.length - undriven.length}/${required.length}\n`
);

if (options.json) {
    fs.writeFileSync(
        options.json,
        `${JSON.stringify(
            {
                documents: documents.length,
                kinds: { named: kinds.length, built: kinds.length - unbuilt.length, unbuilt }
            },
            null,
            4
        )}\n`
    );
}

const failures = [];
if (unbuilt.length) {
    failures.push(
        `the corpus never builds these node kinds, so the staged profile says nothing about the grammars ` +
            `that produce them: ${unbuilt.join(", ")}`
    );
}
if (undriven.length) {
    failures.push(`grammars the corpus must measure and no longer does:\n    ${undriven.join("\n    ")}`);
}
if (failures.length) {
    process.stderr.write(`corpus reach audit FAILED\n${failures.map((m) => `  ${m}`).join("\n")}\n`);
    process.exit(1);
}
process.stdout.write("corpus reach audit passed\n");
