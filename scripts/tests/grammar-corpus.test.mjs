import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import {
    buildGrammarCorpus,
    historicalHosts,
    grammarCatalog,
    grammarCertificates,
    grammarNormalForm,
    grammarUnit,
    instantiateGrammar,
    recognizeBody,
    recognizeBoundary,
    encodeBoundary,
    renderBody,
    recognizePairedDocument,
    encodePairedDocument,
    recomposeHost,
    writeGrammarCorpus,
    grammarSourceIdentity,
    grammarMarkdown,
    grammarEngines,
    validateGrammarCertificates
} from "../lib/grammar-corpus.mjs";
import { pairReviews } from "../lib/pair-review.mjs";
import { productionProofs } from "../lib/pair-productions.mjs";
import { featureGrammars, finiteLexicons } from "../lib/grammar-features.mjs";
import {
    featureCoverage,
    validateFeatureCoverage,
    specificationSections,
    reviewedSections
} from "../lib/grammar-coverage.mjs";
import { sectionDispositions } from "../lib/grammar-sections.mjs";

const root = fileURLToPath(new URL("../../", import.meta.url));
const full = grammarCertificates.filter((c) => c.scope === "paired-document-grammar");
const split = grammarCertificates.filter((c) => c.scope === "boundary-grammar");
const fresh = () => ({
    key: "firstkey",
    target: "separatetarget",
    value: "longvalue",
    anchor: "anchor",
    words: ["different", "phrase", "lengths"],
    body: ["start", ["nested", ["leaf"], "end"], "finish"],
    tail: ["other", "paragraph"],
    start: 17,
    mode: 2
});

test("catalog is a complete reviewable corpus with exact grammar/proof/example provenance", () => {
    validateGrammarCertificates();
    assert.equal(grammarCertificates.length, 194);
    assert.equal(full.length, 182);
    assert.equal(split.length, 12);
    assert.deepEqual(new Set(grammarCertificates.flatMap((c) => c.legacy)), new Set(pairReviews.keys()));
    assert.deepEqual(
        new Set(grammarCertificates.map((c) => c.structuralPredecessor).filter(Boolean)),
        new Set(["insertion-strong-v1", ...productionProofs.keys()])
    );
    assert.deepEqual(
        JSON.parse(fs.readFileSync(path.join(root, "packages/markdown-core/benchmarks/grammar-corpus.json"))),
        grammarCatalog()
    );
});

test("grammar translations have both inverse laws for independent field values", () => {
    for (const c of full) {
        assert.deepEqual(grammarNormalForm(c.id, "dialect"), grammarNormalForm(c.id, "common"));
        for (const index of [0, 1, 3, 5, 9, 15, 23]) {
            const sample = grammarUnit(c.id, index);
            const arbitrary = instantiateGrammar(c.id, fresh());
            for (const row of [sample, arbitrary]) {
                const left = recognizePairedDocument(c.id, "dialect", row.dialect.source);
                const right = recognizePairedDocument(c.id, "common", row.common.source);
                assert.deepEqual(left, right, c.id);
                assert.equal(encodePairedDocument(c.id, "dialect", right), row.dialect.source);
                assert.equal(encodePairedDocument(c.id, "common", left), row.common.source);
            }
        }
    }
    // A key is not a second spelling of the body or of another numeric counter.
    const original = fresh(),
        changed = { ...original, key: "newkey" };
    const a = instantiateGrammar("record-span", original),
        b = instantiateGrammar("record-span", changed);
    assert.notDeepEqual(a.derivation, b.derivation);
    assert.equal(a.derivation[0].find(([key]) => key === "body")[1].value[0], "start");
    assert.deepEqual(
        a.derivation[0].filter(([key]) => key !== "key"),
        b.derivation[0].filter(([key]) => key !== "key")
    );
});

test("recursive grammar preserves arbitrary depth, siblings, and literal widths", () => {
    let body = ["a".repeat(512)];
    for (let depth = 0; depth < 64; depth++) body = ["first", body, ["sibling", "words"], "last"];
    for (const marker of ["**", "++"]) assert.deepEqual(recognizeBody(renderBody(body, marker), marker), body);
    for (const source of [
        "",
        " a",
        "a ",
        "a  b",
        "**a**",
        "a **b**",
        "a **b c",
        "a b**",
        "a **b**c",
        "a \\*b c",
        "a\nb",
        "a **b **c** d**"
    ]) {
        assert.throws(() => recognizeBody(source), source);
    }
});

test("the source recognizers reject syntax outside each grammar, including complete EOF", () => {
    for (const c of full) {
        const row = grammarUnit(c.id, 2);
        for (const side of ["dialect", "common"]) {
            const source = row[side].source;
            assert.throws(() => recognizePairedDocument(c.id, side, source + "!"), c.id);
            assert.throws(() => recognizePairedDocument(c.id, side, "!" + source), c.id);
            assert.throws(() => recognizePairedDocument(c.id, side, ""), c.id);
        }
    }
    assert.throws(() => recognizePairedDocument("insertion-strong", "dialect", "probe +++a+++ end\n\n"));
    assert.throws(() => recognizePairedDocument("cross-link", "dialect", 'probe [[target|quoted"value]] end\n\n'));
    assert.throws(() => recognizePairedDocument("decimal-list", "dialect", "01. a\n\n"));
    assert.throws(() => recognizePairedDocument("record-span", "missing", ""));
});

test("list equivalence retains all blank lines and decimal spellings", () => {
    for (const id of ["decimal-list", "task-value"]) {
        const source =
            id === "decimal-list" ? "987654321. a\n\n\n2. b **c** d\n\n\n\n" : "- [~] a\n\n\n- [~] b **c** d\n\n\n\n";
        const decoded = recognizePairedDocument(id, "dialect", source);
        assert.deepEqual(
            decoded.map((item) => item.blankLines),
            [2, 3]
        );
        const target = encodePairedDocument(id, "common", decoded);
        assert.equal(encodePairedDocument(id, "dialect", recognizePairedDocument(id, "common", target)), source);
        assert.notDeepEqual(decoded, recognizePairedDocument(id, "dialect", source.replace("\n\n\n", "\n\n")));
    }
});

test("boundary splits keep every byte and every independent field, with measured entry frames", () => {
    for (const c of split) {
        const row = instantiateGrammar(c.id, fresh());
        assert.equal(row.boundary.dialect, row.boundary.common);
        const decoded = recognizeBoundary(c.id, row.boundary.dialect);
        assert.equal(encodeBoundary(c.id, decoded), row.boundary.dialect);
        assert.throws(() => recognizeBoundary(c.id, row.boundary.dialect + "!"));
        assert.throws(() => recognizeBoundary(c.id, row.boundary.dialect.replace("\n\n", "\n")));
        assert.ok(row.fields.length > 0 && row.boundary.dialect.length > 0);
        for (const side of ["dialect", "common"]) {
            const host = row[side];
            assert.equal(recomposeHost(host), host.source);
            assert.ok(host.pieces.some((p) => p.kind === "residual" && p.source.length));
            const broken = JSON.parse(JSON.stringify(host));
            broken.pieces[1].start++;
            assert.throws(() => recomposeHost(broken), /missing or overlapping/u);
            const truncated = JSON.parse(JSON.stringify(host));
            truncated.pieces[0].end++;
            assert.throws(() => recomposeHost(truncated), /extent/u);
        }
    }
});

test("scaled corpus repeats derivations, keeps metadata document-initial, and records exact normal forms", () => {
    const corpus = buildGrammarCorpus();
    assert.equal(corpus.cases.length, 824);
    for (const proof of corpus.proofs) {
        assert.ok(proof.units >= 12 * proof.scale);
        assert.equal(
            proof.units,
            corpus.proofs.find((first) => first.id === proof.id && first.scale === 1).units * proof.scale
        );
        if (proof.id.startsWith("metadata")) {
            assert.equal(proof.hosts.dialect.source.split("---\n").length - 1, 2);
            assert.equal(
                proof.hosts.dialect.pieces.filter((p) => p.kind === "slot").length,
                proof.rows[0].dialect.pieces.filter((p) => p.kind === "slot").length * proof.units
            );
        }
        if (proof.scope === "paired-document-grammar") {
            const a = recognizePairedDocument(proof.id, "dialect", proof.hosts.dialect.source);
            assert.deepEqual(a, recognizePairedDocument(proof.id, "common", proof.hosts.common.source));
            assert.equal(encodePairedDocument(proof.id, "dialect", a), proof.hosts.dialect.source);
        }
    }
    assert.throws(() => buildGrammarCorpus({ units: 0 }));
    assert.throws(() => buildGrammarCorpus({ scale: 1.5 }));
});

test("artifact identity binds proof text and source dependencies; corpus is deterministic", () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "grammar-corpus-"));
    try {
        const a = writeGrammarCorpus(path.join(directory, "a"), { units: 2, scale: 1 });
        const b = writeGrammarCorpus(path.join(directory, "b"), { units: 2, scale: 1 });
        assert.equal(a.identity, b.identity);
        assert.notEqual(a.identity, writeGrammarCorpus(path.join(directory, "c"), { units: 3, scale: 1 }).identity);
        const paths = [
            "scripts/lib",
            "scripts/benchmark-stages.mjs",
            "scripts/init-environment.sh",
            "packages/markdown-core/benchmarks/grammar-corpus.json",
            "packages/markdown-core/benchmarks/grammar-coverage.json",
            "docs/specs/dialect",
            "docs/specs/dialect.md",
            "docs/architecture/benchmark-grammar-coverage.md",
            "docs/architecture/benchmark-grammar-corpus.md"
        ];
        for (const file of paths) {
            fs.mkdirSync(path.dirname(path.join(directory, file)), { recursive: true });
            fs.cpSync(path.join(root, file), path.join(directory, file), { recursive: true });
        }
        const original = grammarSourceIdentity(directory);
        assert.equal(original, grammarSourceIdentity());
        // Native correctness inputs are deliberately outside grammar provenance.
        const correctness = path.join(directory, "packages/markdown-core/tests/fixtures/new-correctness.txt");
        fs.mkdirSync(path.dirname(correctness), { recursive: true });
        fs.writeFileSync(correctness, "A new native regression fixture.\n");
        fs.appendFileSync(path.join(directory, "scripts/lib/upstream-cmark.mjs"), "\n// Changed parity projection.\n");
        assert.equal(original, grammarSourceIdentity(directory));
        fs.appendFileSync(path.join(directory, paths.at(-1)), "\nChanged proof obligation.\n");
        assert.notEqual(original, grammarSourceIdentity(directory));
    } finally {
        fs.rmSync(directory, { recursive: true, force: true });
    }
});

test("regeneration removes obsolete generated halves and scales from archived corpus", () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "grammar-rerun-"));
    try {
        writeGrammarCorpus(directory, { units: 1, scale: 2 });
        fs.writeFileSync(path.join(directory, "grammar-removed-boundary-dialect.x1.md"), "obsolete");
        fs.writeFileSync(path.join(directory, "notes.md"), "review notes");
        const current = writeGrammarCorpus(directory, { units: 1, scale: 1 });
        assert.deepEqual(
            fs.readdirSync(directory).sort(),
            ["grammar-corpus.json", "notes.md", ...current.cases.map((item) => `${item.name}.x${item.scale}.md`)].sort()
        );
        assert.equal(fs.readFileSync(path.join(directory, "notes.md"), "utf8"), "review notes");
    } finally {
        fs.rmSync(directory, { recursive: true, force: true });
    }
});

test("the real benchmark CLI selects the entire certified pair, including boundary hosts", () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "grammar-cli-"));
    try {
        execFileSync(
            process.execPath,
            [
                path.join(root, "scripts/benchmark-stages.mjs"),
                "--grammar-corpus",
                "--corpus-only",
                "--quiet",
                "--case",
                "grammar-grid-cell-boundary-dialect",
                "--out",
                directory
            ],
            { cwd: root }
        );
        const units = JSON.parse(fs.readFileSync(path.join(directory, "units.json")));
        assert.deepEqual(
            Object.keys(units).sort(),
            ["boundary-common", "boundary-dialect", "host-common", "host-dialect"]
                .map((part) => `grammar-grid-cell-${part}`)
                .sort()
        );
        assert.ok(fs.existsSync(path.join(directory, "corpus/grammar-corpus.json")));
    } finally {
        fs.rmSync(directory, { recursive: true, force: true });
    }
});

test("grammar reporting keeps the boundary ratio separate and rejects missing measured halves", () => {
    const corpus = buildGrammarCorpus({ units: 1, scale: 1 });
    const proofs = corpus.proofs.filter((p) => p.id === "grid-cell");
    const stages = (ir) => ({ stages: { source_to_buffer: { ir }, buffer_to_ast: { ir } } });
    const cases = corpus.cases
        .filter((c) => c.id === "grid-cell")
        .map((c) => ({
            ...c,
            case: c.name,
            engines: { "markdown-core": stages(c.part === "host" ? 900 : 200), cmark: stages(100) }
        }));
    const report = { grammarCorpus: { identity: "test", certificates: corpus.certificates, proofs }, cases };
    const output = grammarMarkdown(report);
    assert.match(output, /2\.000x/u);
    assert.doesNotMatch(output, /9\.000x/u);
    assert.match(output, /1800/u);
    assert.throws(
        () => grammarMarkdown({ ...report, cases: cases.filter((c) => c.part !== "boundary" || c.side !== "common") }),
        /missing/u
    );
});

test("historical residual features have actual hosts and cannot pass through a shared subset label", () => {
    assert.equal(Object.keys(historicalHosts).length, 8);
    for (const [id, record] of Object.entries(historicalHosts)) {
        const entry = grammarCertificates.find((c) => c.id === id);
        assert.deepEqual(entry.legacy, [record.legacy]);
        if (entry.scope === "boundary-grammar")
            assert.notEqual(grammarUnit(id).dialect.source, grammarUnit(id).boundary.dialect);
        else assert.ok(grammarUnit(id).derivation);
    }
});

test("the complete syntax and element inventories fail closed when a feature, rule, or source changes", () => {
    const corpus = buildGrammarCorpus({ units: 1, scale: 1 });
    const coverage = validateFeatureCoverage(root, corpus);
    assert.equal(coverage.features, 30);
    assert.equal(coverage.elements, 32);
    assert.equal(coverage.sections, 136);
    assert.throws(
        () =>
            validateFeatureCoverage(root, {
                ...corpus,
                certificates: corpus.certificates.filter((entry) => entry.id !== "metadata-types")
            }),
        /coverage changed|unknown grammar certificate/u
    );
    assert.throws(
        () =>
            featureCoverage(root, { ...corpus, cases: corpus.cases.filter((entry) => entry.id !== "gfm-pipe-table") }),
        /missing executable/u
    );
    const source = "# Syntax\n\n```markdown\n## Example\n```\n\n## Rule\nOne.\n";
    assert.deepEqual(
        specificationSections(source).map((section) => section.title),
        ["Syntax", "Rule"]
    );
    assert.notDeepEqual(specificationSections(source), specificationSections(source.replace("One.", "Two.")));
    assert.ok(
        featureCoverage(root, corpus).features.every((entry) => entry.certificates.length && entry.sections.length)
    );
});

test("section coverage requires explicit proof links before a ledger can be regenerated", () => {
    const sections = specificationSections(fs.readFileSync(path.join(root, "docs/specs/dialect/formulas.md"), "utf8"));
    const certificates = new Map(grammarCertificates.map((certificate) => [certificate.id, certificate]));
    assert.throws(
        () =>
            reviewedSections(
                "formulas",
                [
                    ...sections,
                    {
                        title: "New formula production",
                        line: 999,
                        level: 2,
                        sha256: "new"
                    }
                ],
                certificates
            ),
        /explicit disposition/u
    );
    assert.throws(() => reviewedSections("formulas", sections.slice(1), certificates), /explicit disposition/u);
    const decisions = { ...sectionDispositions.formulas };
    decisions.Formulas = { kind: "grammar", certificates: [] };
    assert.throws(
        () => reviewedSections("formulas", sections, certificates, decisions),
        /missing grammar certificate/u
    );
    decisions.Formulas = { kind: "grammar", certificates: ["unproved-formula"] };
    assert.throws(
        () => reviewedSections("formulas", sections, certificates, decisions),
        /unknown grammar certificate/u
    );
    decisions.Formulas = { kind: "context", reason: "" };
    assert.throws(() => reviewedSections("formulas", sections, certificates, decisions), /context needs a reason/u);
    const ledger = featureCoverage(root, buildGrammarCorpus({ units: 1, scale: 1 }));
    const all = ledger.features.flatMap((feature) => feature.sections);
    assert.equal(all.filter((section) => section.disposition.kind === "grammar").length, 132);
    assert.equal(all.filter((section) => section.disposition.kind === "context").length, 4);
    assert.deepEqual(
        new Set(all.flatMap((section) => section.disposition.certificates ?? [])),
        new Set(certificates.keys())
    );
});

test("shared features use the same source grammar and bytes", () => {
    for (const feature of featureGrammars.values())
        if (feature.identity) {
            assert.deepEqual(feature.dialect, feature.common);
            const row = grammarUnit(feature.id, 15);
            assert.equal(row.dialect.source, row.common.source, feature.id);
        }
});

test("reference measurements are limited to the exact certified input side", () => {
    for (const document of buildGrammarCorpus({ units: 1, scale: 1 }).cases) {
        const engines = grammarEngines(document);
        if (document.side === "dialect" || document.part === "host") assert.deepEqual(engines, ["markdown-core"]);
        else assert.deepEqual(engines, ["markdown-core", document.gfm ? "cmark-gfm" : "cmark"]);
    }
    assert.throws(() => grammarEngines({ side: "common", part: "unknown" }));
});

test("reference-label bounds are part of the grammar and generated at their exact limits", () => {
    for (const certificate of full) {
        const normal = grammarNormalForm(certificate.id, "dialect");
        for (const field of normal.fields ?? [])
            if (field.maxBytes) {
                const p = { ...fresh(), [field.name]: "a".repeat(field.maxBytes) };
                const row = instantiateGrammar(certificate.id, p);
                assert.equal(row.derivation[0].find(([name]) => name === field.name)[1].value.length, field.maxBytes);
                assert.throws(
                    () => instantiateGrammar(certificate.id, { ...p, [field.name]: p[field.name] + "a" }),
                    /byte bound/u
                );
                const generated = grammarUnit(certificate.id, 11);
                assert.equal(
                    generated.derivation[0].find(([name]) => name === field.name)[1].value.length,
                    field.maxBytes
                );
            }
    }
});

test("every measured scale exhausts finite grammar alternatives even with one requested unit", () => {
    for (const units of [1, 12]) {
        const corpus = buildGrammarCorpus({ units, scale: 2 });
        for (const proof of corpus.proofs) {
            const fields = (proof.normalForm.fields ?? []).filter((field) => finiteLexicons[field.grammar]);
            if (!fields.length) continue;
            const cardinalities = fields.map((field) => Object.values(finiteLexicons[field.grammar])[0].length);
            const expected = cardinalities.reduce((count, size) => count * size, 1);
            const tuples = proof.rows.map((row) =>
                JSON.stringify(fields.map((field) => row.derivation[0].find(([name]) => name === field.name)[1].value))
            );
            assert.equal(new Set(tuples).size, expected, `${proof.id} scale ${proof.scale}`);
            for (const side of ["dialect", "common"]) {
                const document = corpus.cases.find(
                    (entry) => entry.id === proof.id && entry.scale === proof.scale && entry.side === side
                );
                assert.equal(recognizePairedDocument(proof.id, side, document.text).length, proof.rows.length);
            }
        }
    }
});

test("finite substitutions and bindings retain every alternative, state, and equality constraint", () => {
    for (const id of ["alpha-list", "upper-list", "roman-list", "upper-roman-list"])
        for (let start = 1; start <= 26; start++) {
            const row = instantiateGrammar(id, { ...fresh(), start });
            assert.equal(row.derivation[0].find(([name]) => name === "ordinal")[1].value, start);
            assert.equal(
                encodePairedDocument(id, "dialect", recognizePairedDocument(id, "common", row.common.source)),
                row.dialect.source
            );
        }
    for (const tokens of Object.values(finiteLexicons.ordinal)) assert.equal(new Set(tokens).size, 26);
    const rows = [0, 1, 2].map((mode) => instantiateGrammar("callout", { ...fresh(), mode }));
    assert.equal(new Set(rows.map((row) => row.common.source)).size, 3);
    assert.equal(new Set(rows.map((row) => row.dialect.source)).size, 3);
    const source = instantiateGrammar("specimen-graph", fresh()).dialect.source;
    assert.throws(
        () =>
            recognizePairedDocument(
                "specimen-graph",
                "dialect",
                source.replace("(@firstkey) start", "(@wrongkey) start")
            ),
        /inconsistent binding/u
    );
    assert.throws(() => recognizePairedDocument("roman-list", "dialect", "i. a\niiii. b\n\n"));
    for (const reset of ["1", "999999999"]) {
        const row = instantiateGrammar("specimen-reset", { ...fresh(), reset });
        assert.ok(row.common.source.includes(`${reset}. `));
        assert.equal(
            encodePairedDocument(
                "specimen-reset",
                "dialect",
                recognizePairedDocument("specimen-reset", "common", row.common.source)
            ),
            row.dialect.source
        );
    }
    for (const reset of ["0", "0001", "1000000000"])
        assert.throws(() => instantiateGrammar("specimen-reset", { ...fresh(), reset }));
});
