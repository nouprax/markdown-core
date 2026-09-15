/**
 * WHAT THE STAGE CORPUS ACTUALLY REACHES.
 *
 * The staged benchmark reports instruction counts per parse phase, and those
 * counts only describe the code the corpus reaches. A construct absent from the
 * corpus is not reported as slow or as fast; it is not reported at all, and the
 * profile silently describes a subset of the parser as though it were the
 * whole.
 *
 * That was not hypothetical. Measured on the 33-case corpus this audit was
 * written against, `table.c` ran at 39.0% and the corpus produced 26 of the 43
 * node kinds the dialect names. The largest single cost in the source stage was
 * table's REFUSAL path on documents containing no table, while every grammar
 * that builds one went unmeasured.
 *
 * THE LAW IS ABOUT NODES, NOT LINES. The obvious rule -- no element file at
 * zero coverage -- is VACUOUS, and measurably so: the block dispatcher asks
 * every attached element about every line, so an element whose grammar never
 * claims anything still runs. Stripping every callout from the corpus leaves
 * `callout.c` at 40.4%, not 0%, purely from being asked and declining.
 *
 * So the law is that every node kind the dialect can name must actually be
 * BUILT somewhere in the corpus. Declining costs an element nothing here: only
 * a claimed construct puts a node in a tree. On the corpus that prompted this,
 * the rule fails on 17 kinds -- Formula, Strikethrough, Cite, Mark, Insertion,
 * Span, Superscript, Subscript, DefinitionList, Definition, TableCaption,
 * Citation, Footnote, Specimen, Metadata and the rest -- which is exactly the
 * set the profile was blind to.
 *
 * ONE KIND IS NOT ONE GRAMMAR. Several grammars can build the same kind, so the
 * census above cannot tell them apart: the grid, pipe, simple and multiline
 * table parsers all produce `Table`, `TableRow` and `TableCell`, and deleting
 * the grid case still leaves every kind built by the other three. Task markers
 * and bare autolinks have no kind of their own at all.
 *
 * So a case may also declare, in the manifest, the grammar ENTRY it exists to
 * drive, and that function is required to have run. Measured, the discrimination
 * is not marginal: the grid case drives `table_parse_grid` to 79.1% while the
 * other three only brush its guard clause at 6.1%, which is why the bar is a
 * majority of the function rather than merely reaching it.
 *
 * The line measurement carries two denominators,
 * because they mean different things. Hand-written code is the parser: a line
 * the corpus never runs is a line the profile cannot see, and that is what the
 * floor is set on. Generated code is not: `*_scanners.c` are re2c DFAs and
 * `*.inc` are Unicode tables, whose line counts are dominated by state
 * transitions and codepoint ranges. A representative corpus will never hit
 * those, nor should it try -- chasing that number would mean filling the corpus
 * with inputs chosen to walk a state machine rather than inputs that look like
 * documents. They are reported and deliberately not gated.
 *
 *   node scripts/audit-corpus-coverage.mjs [--floor N] [--json FILE]
 */

import { execFileSync, spawn, spawnSync } from "node:child_process";
import readline from "node:readline";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const KIND_TABLE = path.join(root, "packages/markdown-core/elements/ast.c");
const CLI = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");

/* Set just under the measured value so ordinary drift does not fail the build,
 * while a corpus that stops reaching the parser does. Raise it when the corpus
 * genuinely improves; never lower it to make a red build green. */
const DEFAULT_FLOOR = 73.0;

function fail(message) {
    process.stderr.write(`corpus coverage audit FAILED\n  ${message}\n`);
    process.exit(1);
}

const isGenerated = (file) => file.endsWith("_scanners.c") || file.endsWith(".inc");
/* `main.c` is the dump CLI that drives the corpus, not parser code, and
 * counting the harness as parse phase would flatter the number. */
const isParsePhase = (file) => file !== "core/main.c";

function parseArguments(argv) {
    const options = { floor: DEFAULT_FLOOR, json: null };
    for (let i = 0; i < argv.length; i++) {
        const flag = argv[i];
        if (flag === "--floor") {
            const value = Number(argv[++i]);
            if (!Number.isFinite(value) || value < 0 || value > 100) fail("--floor takes a percentage");
            options.floor = value;
        } else if (flag === "--json") options.json = argv[++i];
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
        const child = spawn(cli, [document], { stdio: ["ignore", "pipe", "ignore"], timeout: 600_000 });
        const lines = readline.createInterface({ input: child.stdout, crlfDelay: Infinity });
        for await (const line of lines) {
            const match = /([A-Za-z]+) scope=/.exec(line);
            if (match) seen.add(match[1]);
        }
        await new Promise((resolve) => child.on("close", resolve));
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
    const required = manifest.requiredGrammars ?? [];
    if (!Array.isArray(required)) fail("corpus.json: requiredGrammars must be a list of function names");
    return required;
}

/* Per-function coverage for one document, read with the counters reset so the
 * result is that document's alone rather than the corpus's. */
function functionsFor(cli, buildDir, document) {
    for (const file of gcdaDirectories(buildDir).flatMap((dir) =>
        fs
            .readdirSync(dir)
            .filter((n) => n.endsWith(".gcda"))
            .map((n) => path.join(dir, n))
    )) {
        fs.rmSync(file, { force: true });
    }
    spawnSync(cli, [document], { stdio: "ignore", timeout: 600_000 });
    const percent = new Map();
    for (const dir of gcdaDirectories(buildDir)) {
        const gcda = fs
            .readdirSync(dir)
            .filter((n) => n.endsWith(".gcda"))
            .map((n) => path.join(dir, n));
        const out = spawnSync("gcov", ["-f", "-n", "-o", dir, ...gcda], { cwd: dir, encoding: "utf8" }).stdout ?? "";
        for (const match of out.matchAll(/Function '([^']+)'\nLines executed:([\d.]+)%/g)) {
            percent.set(match[1], Math.max(percent.get(match[1]) ?? 0, Number(match[2])));
        }
    }
    return percent;
}

function coverageBuild(buildDir) {
    const run = (command, args) =>
        execFileSync(command, args, { cwd: root, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] });
    run("cmake", [
        "-S",
        root,
        "-B",
        buildDir,
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DCMAKE_C_FLAGS=-O0 --coverage",
        "-DCMAKE_EXE_LINKER_FLAGS=--coverage"
    ]);
    run("cmake", ["--build", buildDir, "--target", "markdown-core", "--parallel", String(os.cpus().length)]);
    const cli = path.join(buildDir, "packages/markdown-core/core/markdown-core");
    if (!fs.existsSync(cli)) fail(`the coverage build produced no dump CLI at ${cli}`);
    return cli;
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

function readCoverage(buildDir) {
    let output = "";
    for (const dir of gcdaDirectories(buildDir)) {
        const gcda = fs
            .readdirSync(dir)
            .filter((n) => n.endsWith(".gcda"))
            .map((n) => path.join(dir, n));
        const result = spawnSync("gcov", ["-n", "-o", dir, ...gcda], { cwd: dir, encoding: "utf8" });
        output += result.stdout ?? "";
    }
    const files = {};
    for (const match of output.matchAll(/File '([^']+)'\nLines executed:([\d.]+)% of (\d+)/g)) {
        const [, file, percent, total] = match;
        if (!file.includes("/packages/markdown-core/")) continue;
        const key = file.split("/packages/markdown-core/")[1];
        if (key.startsWith("tests/") || key.startsWith("benchmarks/") || key.endsWith(".h")) continue;
        if (!isParsePhase(key)) continue;
        const lines = Number(total);
        files[key] = { percent: Number(percent), lines, executed: Math.round((lines * Number(percent)) / 100) };
    }
    if (!Object.keys(files).length) fail("gcov reported no parse-phase files; the coverage build produced no data");
    return files;
}

function summarise(files) {
    const group = (predicate) => {
        const entries = Object.entries(files).filter(([key]) => predicate(key));
        const lines = entries.reduce((sum, [, v]) => sum + v.lines, 0);
        const executed = entries.reduce((sum, [, v]) => sum + v.executed, 0);
        return { entries, lines, executed, percent: lines ? (executed / lines) * 100 : 100 };
    };
    return { handWritten: group((key) => !isGenerated(key)), generated: group(isGenerated) };
}

const options = parseArguments(process.argv.slice(2));
const kinds = dialectKinds();
const corpusDir = fs.mkdtempSync(path.join(os.tmpdir(), "corpus-reach-"));
process.on("exit", () => fs.rmSync(corpusDir, { recursive: true, force: true }));
const documents = corpusDocuments(corpusDir);

if (!fs.existsSync(CLI)) {
    /* Built here rather than demanded of the caller, so the audit states one
     * precondition -- a checkout -- and holds wherever it is run. */
    execFileSync("cmake", ["--preset", "default"], { cwd: root, stdio: ["ignore", "ignore", "pipe"] });
    execFileSync("cmake", ["--build", "--preset", "default", "--target", "markdown-core", "--parallel"], {
        cwd: root,
        stdio: ["ignore", "ignore", "pipe"]
    });
    if (!fs.existsSync(CLI)) fail(`no dump CLI at ${path.relative(root, CLI)} after building it`);
}
const produced = await kindsProduced(CLI, documents);
const unbuilt = kinds.filter((kind) => !produced.has(kind));

process.stdout.write(`Corpus reach over ${documents.length} documents\n\n`);

const required = requiredGrammars();
const driven = new Set();
const undriven = [];
let coverage;
{
    const buildDir = fs.mkdtempSync(path.join(os.tmpdir(), "corpus-coverage-"));
    try {
        const instrumented = coverageBuild(buildDir);
        /* Every case alone, with the counters reset, so what each document
         * drives is separated from what the corpus drives together. A grammar
         * is driven when some ONE case reaches it; spreading a function's lines
         * across several cases that each brush it is not the same thing. */
        for (const document of documents) {
            for (const [name, percent] of functionsFor(instrumented, buildDir, document)) {
                if (percent >= EXERCISED_FLOOR) driven.add(name);
            }
        }
        for (const name of required) {
            if (!driven.has(name)) {
                undriven.push(`${name} is driven by no case, so the benchmark no longer measures that grammar`);
            }
        }
        /* Then the whole corpus, for the aggregate. */
        for (const file of gcdaDirectories(buildDir).flatMap((dir) =>
            fs
                .readdirSync(dir)
                .filter((n) => n.endsWith(".gcda"))
                .map((n) => path.join(dir, n))
        )) {
            fs.rmSync(file, { force: true });
        }
        for (const document of documents) {
            spawnSync(instrumented, [document], { stdio: "ignore", timeout: 600_000 });
        }
        coverage = summarise(readCoverage(buildDir));
        process.stdout.write(
            `  hand-written      ${coverage.handWritten.executed}/${coverage.handWritten.lines} = ` +
                `${coverage.handWritten.percent.toFixed(1)}%\n` +
                `  generated         ${coverage.generated.executed}/${coverage.generated.lines} = ` +
                `${coverage.generated.percent.toFixed(1)}% (not gated)\n\n` +
                "Hand-written files the corpus reaches least:\n" +
                coverage.handWritten.entries
                    .slice()
                    .sort((a, b) => b[1].lines - b[1].executed - (a[1].lines - a[1].executed))
                    .slice(0, 10)
                    .map(
                        ([key, v]) =>
                            `  ${key.padEnd(38)} ${v.percent.toFixed(1).padStart(5)}%  ` +
                            `${String(v.lines - v.executed).padStart(5)} unreached`
                    )
                    .join("\n") +
                "\n"
        );
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
                kinds: { named: kinds.length, built: kinds.length - unbuilt.length, unbuilt },
                coverage: coverage && {
                    handWritten: { percent: coverage.handWritten.percent, lines: coverage.handWritten.lines },
                    generated: { percent: coverage.generated.percent, lines: coverage.generated.lines },
                    floor: options.floor
                }
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
if (coverage && coverage.handWritten.percent < options.floor) {
    failures.push(
        `hand-written parse coverage ${coverage.handWritten.percent.toFixed(1)}% is below the ` +
            `${options.floor.toFixed(1)}% floor`
    );
}
if (failures.length) {
    process.stderr.write(`corpus coverage audit FAILED\n${failures.map((m) => `  ${m}`).join("\n")}\n`);
    process.exit(1);
}
process.stdout.write("corpus coverage audit passed\n");
