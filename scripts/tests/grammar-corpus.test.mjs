import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import {
    buildGrammarCorpus,
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
    validateGrammarCertificates
} from "../lib/grammar-corpus.mjs";
import { pairReviews } from "../lib/pair-review.mjs";
import { productionProofs } from "../lib/pair-productions.mjs";

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
    assert.equal(grammarCertificates.length, 47);
    assert.equal(full.length, 35);
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
    const a = instantiateGrammar("callout", fresh());
    const b = instantiateGrammar("callout", { ...fresh(), mode: 1 });
    assert.notEqual(a.dialect.source, b.dialect.source);
    assert.equal(a.common.source, b.common.source); // concrete noninjective full-host proposal
    assert.equal(a.boundary.dialect, b.boundary.dialect);
});

test("scaled corpus repeats derivations, keeps metadata document-initial, and records exact normal forms", () => {
    const corpus = buildGrammarCorpus();
    assert.equal(corpus.cases.length, 236);
    for (const proof of corpus.proofs) {
        assert.equal(proof.units, 12 * proof.scale);
        if (proof.id.startsWith("metadata")) {
            assert.equal(proof.hosts.dialect.source.split("---\n").length - 1, 2);
            assert.equal(proof.hosts.dialect.pieces.filter((p) => p.kind === "slot").length, 2 * proof.units);
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
            "scripts/audit-corpus-pairs.mjs",
            "scripts/init-environment.sh",
            "packages/markdown-core/benchmarks/grammar-corpus.json",
            "docs/architecture/benchmark-grammar-corpus.md"
        ];
        for (const file of paths) {
            fs.mkdirSync(path.dirname(path.join(directory, file)), { recursive: true });
            fs.cpSync(path.join(root, file), path.join(directory, file), { recursive: true });
        }
        const original = grammarSourceIdentity(directory);
        assert.equal(original, grammarSourceIdentity());
        fs.appendFileSync(path.join(directory, paths.at(-1)), "\nChanged proof obligation.\n");
        assert.notEqual(original, grammarSourceIdentity(directory));
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
