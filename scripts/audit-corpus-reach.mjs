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
        if (flag !== "--json") fail(`unknown flag ${flag}`);
        /* A value is required and must be a path, not the next flag. Taking
         * `argv[++i]` unchecked let `--json` as the last argument run the whole
         * audit, exit zero, and write nothing -- so automation that asked for
         * the artifact got a pass and no artifact -- and `--json --bogus` wrote
         * a file named after a flag instead of rejecting it. */
        const value = argv[++i];
        if (value === undefined || value.startsWith("-")) fail("--json needs a file path");
        options.json = value;
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
/* A sample with no case of its own is a construct the benchmark can no longer
 * profile in isolation.
 *
 * `mixed-extended` concatenates every sample, so deleting a dedicated case
 * leaves its construct still parsed, still building its kinds, and still
 * invisible to any law that asks whether SOME document did it -- while the one
 * document that could have shown that construct's cost on its own is gone. The
 * check is structural because it can be: the manifest either gives a sample a
 * case to itself or it does not. */
/* What each dedicated case must still build, read from the manifest.
 *
 * Existence is not enough and the earlier rule only asked for that: a sample
 * keeps its case while its CONTENT drifts, and the construct quietly stops
 * being parsed. Rewriting `inline-embedded.md` as ordinary link text left the
 * case in place, and `Embedded` still appeared elsewhere in the corpus, so
 * every law passed with the dedicated image profile gone.
 *
 * Checked against the case's OWN document, which is why a shared kind still
 * works as proof -- `Embedded` appearing in `inline-links-flat` says nothing
 * about whether `inline-embedded` still builds one. */
function declaredBuilds() {
    const manifest = JSON.parse(
        fs.readFileSync(path.join(root, "packages/markdown-core/benchmarks/corpus.json"), "utf8")
    );
    const declared = new Map();
    for (const entry of manifest.cases ?? []) {
        if (entry.builds?.length) declared.set(entry.name, entry.builds);
    }
    return declared;
}

/**
 * ISOMORPH PAIRS: where a ratio for a dialect-only construct comes from.
 *
 * cmark reads `++adds++` as a paragraph and `$x$` as text, so the ratio
 * against it on a dialect document is the cost of NOT having the feature. It
 * bounds what the construct costs and cannot say whether the construct is
 * slow, which is the only question the profile exists to answer.
 *
 * An isomorph pair answers it. The same document is written twice, once with
 * the dialect marker and once with a CommonMark marker of the same shape, and
 * the two are the SAME BYTES under a single-character substitution --
 * `++adds++` and `**adds**`, `%%hidden%%` and ``` ``hidden`` ```, `$x$` and
 * `` `x` ``. Three numbers then decompose the ratio:
 *
 *   ours(dialect) / ours(isomorph)   what this grammar costs over a
 *                                    CommonMark grammar building the same tree
 *   ours(isomorph) / cmark(isomorph) what this parser costs on the shape
 *                                    itself, where cmark did the same job
 *
 * and their product is ours(dialect) / cmark(isomorph): a same-job ratio for a
 * construct cmark does not implement, because cmark built the same tree from
 * the isomorphic document. The first factor is the one that names a grammar to
 * go look at.
 *
 * THE PAIRING IS A FACT, NOT A CLAIM, and this is what checks it. A pair whose
 * two documents parse to different trees is two different measurements
 * presented as one, and the report would attribute the difference in the trees
 * to the grammar. So: the substitution must reproduce the isomorph byte for
 * byte, and the two dumps must be identical once the kind names are erased --
 * same spans, same literals, same children, same attributes, differing only in
 * which grammar built each node.
 *
 * It is not a formality. The pairs it REJECTED are the reason it exists: a
 * grid table against a pipe table (a grid cell holds a paragraph, a pipe cell
 * holds inlines), a definition list against a bullet list (the definition
 * groups term and body under one node, the list does not), and a comment
 * against strong emphasis (strong parses its body, a comment keeps it
 * literal). Each of those looked isomorphic and was not.
 */
function isomorphPairs() {
    const manifest = JSON.parse(
        fs.readFileSync(path.join(root, "packages/markdown-core/benchmarks/corpus.json"), "utf8")
    );
    const declarations = manifest.isomorphs ?? [];
    if (!Array.isArray(declarations)) fail("corpus.json: isomorphs must be a list of pairs");
    const cases = new Map((manifest.cases ?? []).map((entry) => [entry.name, entry]));
    const pairs = [];
    for (const declaration of declarations) {
        const { case: name, isomorph, substitution, ignore = [], claim } = declaration;
        const both = [name, isomorph];
        for (const side of both) {
            const entry = cases.get(side);
            if (!entry) fail(`corpus.json: isomorph pair names case ${side}, which the manifest does not define`);
            if (entry.samples?.length !== 1) {
                fail(`corpus.json: isomorph pair side ${side} must be one case over one sample`);
            }
        }
        if (!claim) fail(`corpus.json: isomorph pair ${name} states no claim about why the two are the same shape`);
        /* The dialect side has to be the side with something to isolate, and
         * the isomorph side has to be one a reference actually implements --
         * otherwise the pair produces two bounds rather than a ratio. */
        if (cases.get(name).dialect !== "extended") {
            fail(`corpus.json: isomorph pair ${name} is not an extended-dialect case, so it isolates nothing`);
        }
        if (cases.get(isomorph).dialect !== "commonmark" && cases.get(isomorph).gfm !== true) {
            fail(`corpus.json: isomorph ${isomorph} is neither CommonMark nor GFM, so no reference implements it`);
        }
        if (!substitution || typeof substitution !== "object" || Array.isArray(substitution)) {
            fail(`corpus.json: isomorph pair ${name} declares no substitution`);
        }
        /* Single characters both sides, so the two documents have the same
         * length and every node lands on the same source position. A
         * substitution that changed a length would still produce a tree, and
         * every `scope=` in the comparison below would then differ for a reason
         * that has nothing to do with either grammar. */
        for (const [from, to] of Object.entries(substitution)) {
            if ([...from].length !== 1 || [...to].length !== 1) {
                fail(`corpus.json: isomorph pair ${name} substitutes ${from} for ${to}, which is not byte for byte`);
            }
            if (from === to) fail(`corpus.json: isomorph pair ${name} substitutes ${from} for itself`);
        }
        pairs.push({
            name,
            isomorph,
            ignore,
            sample: (side) => path.join(root, "packages/markdown-core/benchmarks/samples", cases.get(side).samples[0]),
            /* One pass over the characters, not one replacement per entry: run
             * in sequence, `+`->`*` followed by `~`->`*` is the same thing, but
             * `+`->`~` followed by `~`->`*` is not what the manifest says. */
            apply: (text) => [...text].map((character) => substitution[character] ?? character).join("")
        });
    }
    return pairs;
}

/* The kind name is what the pair is allowed to differ in, so it is what the
 * comparison erases; a kind's own bookkeeping field is erased only where the
 * manifest names it, and only if it is really there. */
function canonicalDump(text, ignore) {
    return text
        .split("\n")
        .map((line) => {
            let canonical = line.replace(/^([\s│├└─]*)[A-Za-z]+\b/u, "$1KIND");
            for (const key of ignore) {
                canonical = canonical.replace(new RegExp(`\\s${key}=(?:"(?:[^"\\\\]|\\\\.)*"|\\S+)`, "gu"), "");
            }
            return canonical;
        })
        .join("\n");
}

function isomorphFailures(cli, pairs) {
    const failures = [];
    for (const pair of pairs) {
        const dialect = fs.readFileSync(pair.sample(pair.name), "utf8");
        const common = fs.readFileSync(pair.sample(pair.isomorph), "utf8");
        const substituted = pair.apply(dialect);
        if (substituted === dialect) {
            failures.push(`${pair.name}: the declared substitution changes nothing in the dialect document`);
            continue;
        }
        if (substituted !== common) {
            failures.push(
                `${pair.name}: substituting in the dialect document does not reproduce ${pair.isomorph}, ` +
                    `so the two are not the same document under a change of marker`
            );
            continue;
        }
        const dumps = [pair.name, pair.isomorph].map((side) => {
            const result = spawnSync(cli, [pair.sample(side)], { encoding: "utf8", timeout: 600_000 });
            requireClean(result, "the dump CLI", pair.sample(side));
            return result.stdout;
        });
        /* A named exception that matches nothing is an allowance for a
         * difference that is no longer there, and the next one to appear would
         * pass under it. */
        for (const key of pair.ignore) {
            if (!new RegExp(`\\s${key}=`, "u").test(dumps[0])) {
                failures.push(`${pair.name}: ignores ${key}=, which no node in its dialect document carries`);
            }
        }
        const [left, right] = dumps.map((dump) => canonicalDump(dump, pair.ignore));
        if (left !== right) {
            const at = left.split("\n").findIndex((line, index) => line !== right.split("\n")[index]);
            failures.push(
                `${pair.name} and ${pair.isomorph} do not parse to the same tree, first at line ${at + 1}:\n` +
                    `      ${left.split("\n")[at]}\n      ${right.split("\n")[at]}`
            );
        }
    }
    return failures;
}

function samplesWithoutTheirOwnCase() {
    const manifest = JSON.parse(
        fs.readFileSync(path.join(root, "packages/markdown-core/benchmarks/corpus.json"), "utf8")
    );
    const dedicated = new Set();
    for (const entry of manifest.cases ?? []) {
        if (entry.samples?.length === 1) dedicated.add(entry.samples[0]);
    }
    return fs
        .readdirSync(path.join(root, "packages/markdown-core/benchmarks/samples"))
        .filter((name) => name.endsWith(".md") && !dedicated.has(name))
        .sort();
}

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
async function kindsProduced(cli, documents, unmatched) {
    const seen = new Set();
    const perCase = new Map();
    /* Which declared dialect-only fields this case's tree actually carries.
     * Read from the same stream as the kinds: a second dump pass over the whole
     * corpus costs minutes, and the answer is on lines this one already has. */
    const carried = new Map();
    const fields = [...unmatched.keys()];
    for (const document of documents) {
        /* The complexity shapes are excluded, and only here. `chain-list-depth`
         * is 32,765 levels deep, and `markdown_core_document_dump` materialises
         * the whole canonical dump in one buffer before a byte is written, so
         * asking these for a tree costs gigabytes of RSS in the child whatever
         * this end does with the stream -- enough to lose a hosted runner. They
         * are shapes for depth, not for constructs, and contribute no kind that
         * the other cases do not build. They are still PARSED below, through a
         * binary that never dumps -- only the tree is skipped, never the
         * document. */
        if (path.basename(document).startsWith("chain-")) continue;
        const mine = new Set();
        const mineUnmatched = new Set();
        const child = spawn(cli, [document], { stdio: ["ignore", "pipe", "ignore"], timeout: 600_000 });
        const lines = readline.createInterface({ input: child.stdout, crlfDelay: Infinity });
        for await (const line of lines) {
            const match = /([A-Za-z]+) scope=/.exec(line);
            if (match) {
                seen.add(match[1]);
                mine.add(match[1]);
            }
            for (const field of fields) {
                /* `null` and `{}` are the dump's two spellings of "this kind has
                 * the field and it is unset". A field that is merely PRESENT is
                 * not evidence: every node prints `anchor=`, and almost all of
                 * them print it null. */
                const value = new RegExp(`\\s${field}=("(?:[^"\\\\]|\\\\.)*"|\\{[^}]*\\}|\\S+)`, "u").exec(line);
                if (value && value[1] !== "null" && value[1] !== "{}") mineUnmatched.add(field);
            }
        }
        const name = path.basename(document).replace(/\.x1\.md$/u, "");
        perCase.set(name, mine);
        carried.set(name, mineUnmatched);
        const [code, signal] = await new Promise((resolve) => child.on("close", (c, s) => resolve([c, s])));
        if (signal) fail(`the dump CLI was killed by ${signal} on ${path.basename(document)}`);
        if (code !== 0) fail(`the dump CLI exited ${code} on ${path.basename(document)}`);
    }
    return { seen, perCase, carried };
}

/* A function only brushed by a guard clause is not a grammar the corpus drives,
 * so a declared entry has to be mostly executed. Measured on the four table
 * grammars: the case that owns one reaches 64-79% of it, while the cases that
 * do not reach 6.1%. */
const EXERCISED_FLOOR = 50.0;

/* The AST fields cmark's tree has no counterpart for, declared beside the cases
 * for the same reason `requiredGrammars` is: a field named only by the case
 * that carries it would retire itself the moment that case was deleted.
 *
 * This is what stops a ratio being printed for a document the two engines do
 * not build the same tree from. `dialect: "commonmark"` says the SYNTAX is
 * CommonMark; it says nothing about the output, and heading anchors are the
 * case where the two part company -- this dialect derives one for every
 * heading, so a document with a heading is never cmark's tree. */
function unmatchedFields() {
    const manifest = JSON.parse(
        fs.readFileSync(path.join(root, "packages/markdown-core/benchmarks/corpus.json"), "utf8")
    );
    const declared = manifest.unmatchedFields ?? {};
    if (typeof declared !== "object" || Array.isArray(declared)) {
        fail("corpus.json: unmatchedFields must map each dialect-only AST field to why cmark has no counterpart");
    }
    for (const [field, spec] of Object.entries(declared)) {
        if (typeof spec !== "object" || spec === null || Array.isArray(spec)) {
            fail(`corpus.json: unmatchedFields.${field} must be an object`);
        }
        if (typeof spec.reason !== "string" || !spec.reason.trim()) {
            fail(`corpus.json: unmatchedFields.${field} must say why cmark has no counterpart for it`);
        }
        if (!Array.isArray(spec.excludes) || !spec.excludes.length) {
            fail(
                `corpus.json: unmatchedFields.${field} must name the call edges that do this work, or the ` +
                    `driver has nothing to take off this side and the ratio stays uncomparable`
            );
        }
        for (const edge of spec.excludes) {
            if (typeof edge !== "string" || !/^\S+\s*->\s*\S+$/u.test(edge)) {
                fail(
                    `corpus.json: unmatchedFields.${field} exclusion ${JSON.stringify(edge)} is not "caller -> callee"`
                );
            }
        }
    }
    return new Map(Object.entries(declared));
}

/* What each case CLAIMS it carries, so the claim and the tree can disagree.
 *
 * Only the cases whose number is a DIVISION against a reference on the same
 * bytes -- the CommonMark group and the GFM group -- are held to this. A case
 * already reported as a bound has nothing to protect: its number is not a
 * comparison whatever its tree carries, and requiring a declaration there would
 * be a list nobody reads, kept true by nobody. The key is the corpus's syntax
 * label, not the group the case ends up in, because the declaration is what
 * moves it between groups. The dialect half of an isomorph pair is `extended`
 * and so falls out here anyway; its CommonMark half does NOT, because that half
 * gets a printed ratio of its own and the pair law says nothing about whether
 * cmark builds its tree. */
function declaredUnmatched() {
    const manifest = JSON.parse(
        fs.readFileSync(path.join(root, "packages/markdown-core/benchmarks/corpus.json"), "utf8")
    );
    const claimed = new Map();
    for (const entry of manifest.cases ?? []) {
        const fields = entry.unmatched ?? [];
        if (!Array.isArray(fields) || fields.some((field) => typeof field !== "string")) {
            fail(`corpus.json: ${entry.name}.unmatched must be a list of dialect-only field names`);
        }
        if (entry.dialect !== "commonmark" && entry.gfm !== true) {
            if (fields.length) {
                fail(
                    `corpus.json: ${entry.name} declares unmatched output, but its number is already a bound, ` +
                        `so the declaration changes nothing and will rot`
                );
            }
            continue;
        }
        claimed.set(entry.name, new Set(fields));
    }
    return claimed;
}

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
const sampleCount = fs
    .readdirSync(path.join(root, "packages/markdown-core/benchmarks/samples"))
    .filter((name) => name.endsWith(".md")).length;
const driven = new Set();
const undriven = [];
const drifted = [];
const pairs = isomorphPairs();
const unmatchedDeclared = unmatchedFields();
const claimedUnmatched = declaredUnmatched();
const misdeclared = [];
let notIsomorphic;
let unbuilt;
{
    const buildDir = fs.mkdtempSync(path.join(os.tmpdir(), "corpus-coverage-"));
    try {
        /* Everything reads binaries built here, from the working tree. Reusing
         * whatever `build/` happened to hold would let a source change that
         * stops building a kind still report every kind built, which is the
         * same staleness the corpus itself was fixed for. */
        const binaries = instrumentedBuild(buildDir);
        const census = await kindsProduced(binaries.dump, documents, unmatchedDeclared);
        unbuilt = kinds.filter((kind) => !census.seen.has(kind));
        /* Read off the SAMPLES rather than the generated corpus: the pairing is
         * a property of the two documents as written, and the corpus repeats
         * each of them to a byte target, which says nothing further about it. */
        notIsomorphic = isomorphFailures(binaries.dump, pairs);
        for (const [name, expected] of declaredBuilds()) {
            const built = census.perCase.get(name);
            if (!built) {
                drifted.push(`${name} declares what it builds but the corpus holds no document for it`);
                continue;
            }
            const missing = expected.filter((kind) => !built.has(kind));
            if (missing.length) {
                drifted.push(`${name} no longer builds ${missing.join(", ")}, so its sample stopped being that case`);
            }
        }
        /* What the trees actually carry, against what the cases claim. Both
         * directions fail: an undeclared field means the driver is about to
         * print a same-job ratio for a document cmark does not build the same
         * tree from, and a declared field the tree does not carry means a case
         * is being held out of the comparison for nothing. */
        const witnessed = new Map([...unmatchedDeclared.keys()].map((field) => [field, []]));
        for (const [name, claimed] of claimedUnmatched) {
            const carried = census.carried.get(name);
            if (!carried) {
                /* `chain-*` shapes are never dumped -- see kindsProduced -- so
                 * their trees cannot be read here. A claim on one could not be
                 * checked, and an unchecked claim is the thing this law exists
                 * to stop. */
                if (claimed.size) {
                    misdeclared.push(
                        `${name} claims unmatched output, and its tree is never dumped, so nothing checks it`
                    );
                }
                continue;
            }
            for (const field of carried) witnessed.get(field).push(name);
            const undeclared = [...carried].filter((field) => !claimed.has(field));
            const absent = [...claimed].filter((field) => !carried.has(field));
            if (undeclared.length) {
                misdeclared.push(
                    `${name} builds a tree carrying ${undeclared.join(", ")}, which cmark has no counterpart for, ` +
                        `and does not declare it -- so its ratio would be printed as a comparison`
                );
            }
            if (absent.length) {
                misdeclared.push(`${name} declares unmatched ${absent.join(", ")} that its tree does not carry`);
            }
        }
        for (const [field, cases] of witnessed) {
            /* A field nothing carries excludes nothing, so declaring it costs a
             * reader attention and buys no law. */
            if (!cases.length) misdeclared.push(`unmatchedFields declares ${field}, and no case's tree carries it`);
        }
        /* The census ran the dumper, whose lines are not the parse phase, so
         * its counters are discarded before anything is measured. */
        resetCounters(buildDir);
        /* Every document is parsed, including the complexity shapes the census
         * skips: a `chain-*` case that starts crashing or timing out is a
         * benchmark that will fail on its own adversarial inputs, and skipping
         * the tree is not a reason to skip the parse. */
        for (const document of documents) {
            requireClean(
                spawnSync(binaries.parse, ["--document", document], { stdio: "ignore", timeout: 600_000 }),
                "the parse runner",
                document
            );
        }
        /* Then the named cases, each alone with the counters reset, so what that
         * document drives is separated from what the corpus drives together. */
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

const orphaned = samplesWithoutTheirOwnCase();
process.stdout.write(
    `  node kinds built     ${kinds.length - unbuilt.length}/${kinds.length}\n` +
        `  grammars driven      ${required.length - undriven.length}/${required.length}\n` +
        `  samples with a case  ${sampleCount - orphaned.length}/${sampleCount}\n` +
        `  cases still building ${declaredBuilds().size - drifted.length}/${declaredBuilds().size}\n` +
        `  isomorph pairs held  ${pairs.length - notIsomorphic.length}/${pairs.length}\n` +
        `  unmatched declared   ${claimedUnmatched.size - misdeclared.length}/${claimedUnmatched.size}\n`
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
if (notIsomorphic.length) {
    failures.push(
        `these pairs are not the same document under a change of marker, so the ratio between them ` +
            `would attribute a difference in the trees to a grammar:\n    ${notIsomorphic.join("\n    ")}`
    );
}
if (drifted.length) {
    failures.push(`these cases no longer build what they exist for:\n    ${drifted.join("\n    ")}`);
}
if (orphaned.length) {
    failures.push(
        `these samples have no case of their own, so the benchmark cannot profile them apart from ` +
            `the concatenation they are embedded in: ${orphaned.join(", ")}`
    );
}
if (unbuilt.length) {
    failures.push(
        `the corpus never builds these node kinds, so the staged profile says nothing about the grammars ` +
            `that produce them: ${unbuilt.join(", ")}`
    );
}
if (undriven.length) {
    failures.push(`grammars the corpus must measure and no longer does:\n    ${undriven.join("\n    ")}`);
}
if (misdeclared.length) {
    failures.push(
        `a same-job ratio compares two engines that built the same tree, and these cases do not ` +
            `line up with what their trees carry:\n    ${misdeclared.join("\n    ")}`
    );
}
if (failures.length) {
    process.stderr.write(`corpus reach audit FAILED\n${failures.map((m) => `  ${m}`).join("\n")}\n`);
    process.exit(1);
}
process.stdout.write("corpus reach audit passed\n");
