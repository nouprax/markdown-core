import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import { provenPair, pairRatios, proofTree, proofWorkload, spanLanguage, validatePairs } from "../lib/corpus-pairs.mjs";
import { parseCanonicalDump, parseUpstreamXml } from "../lib/upstream-cmark.mjs";
import { markdownReport } from "../benchmark-stages.mjs";
import { publishesRatio } from "../lib/corpus-splits.mjs";

const manifest = () =>
    JSON.parse(
        fs.readFileSync(new URL("../../packages/markdown-core/benchmarks/corpus.json", import.meta.url), "utf8")
    );
const pair = { case: "dialect", isomorph: "common", contract: { proof: "insertion-strong-v1" } };
const candidate = { case: "candidate", isomorph: "twin", contract: { pending: "No structural mapping." } };
const document = (body) => `probe ++${body}++\n\n`;

test("every workload has one explicit contract and candidates cannot enter equivalent-work results", () => {
    const declared = manifest();
    assert.equal(validatePairs(declared).length, 31);
    assert.deepEqual(
        declared.pairs.filter(provenPair).map((pair) => pair.case),
        ["pair-insertion-dialect"]
    );
    const costs = { dialect: 240, common: 160, reference: 100 };
    assert.deepEqual(pairRatios(candidate, costs), {
        proven: false,
        grammar: 1.5,
        shape: 1.6,
        quotient: 2.4,
        sameJob: null
    });
    assert.equal(pairRatios(pair, costs).sameJob, 2.4);
    const contaminated = pairRatios(candidate, { ...costs, carries: ["anchor"] });
    assert.equal(contaminated.grammar, null);
    assert.equal(contaminated.shape, null);
    assert.equal(contaminated.quotient, 2.4);
    assert.equal(contaminated.sameJob, null);
});

test("manifest edits cannot self-certify by inventing proofs or omitting obligations", () => {
    for (const contract of [
        undefined,
        {},
        { proof: "invented" },
        { pending: "" },
        { proof: "insertion-strong-v1", pending: "later" }
    ]) {
        const declared = manifest();
        declared.pairs[0].contract = contract;
        assert.throws(() => validatePairs(declared), /contract|proof/);
    }
    const legacy = manifest();
    legacy.isomorphs = [];
    assert.throws(() => validatePairs(legacy), /single pairs registry/);
    const duplicate = manifest();
    duplicate.pairs.push(duplicate.pairs[0]);
    assert.throws(() => validatePairs(duplicate), /repeated pair half/);
});

test("a candidate's GFM flag cannot bypass its pending contract", () => {
    const declared = manifest();
    const caption = declared.cases.find((entry) => entry.name === "pair-tcaption-dialect");
    assert.equal(caption.gfm, true);
    assert.equal(publishesRatio(caption, declared), false);
    assert.equal(publishesRatio({ name: "plain-gfm", gfm: true }, declared), true);
});

test("the proof covers varying words, sibling spans, nested spans and document repetition", () => {
    const bodies = ["a", "longword", "a b c", "outer ++inner++ tail", "left ++a++ middle ++b c++ right"];
    for (let depth = 0, body = "center"; depth < 32; depth++, body = `left ++${body}++ right`) bodies.push(body);
    for (const body of bodies) {
        const source = document(body);
        const common = source.replaceAll("++", "**");
        assert.deepEqual(proofWorkload(pair, source, common), spanLanguage(source, "++"));
        assert.equal(proofWorkload(pair, source.repeat(3), common.repeat(3)).children.length, 3);
    }
    const sample = fs.readFileSync(
        new URL("../../packages/markdown-core/benchmarks/samples/pair-insertion-dialect.md", import.meta.url),
        "utf8"
    );
    assert.equal(proofWorkload(pair, sample, sample.replaceAll("++", "**")).children.length, 3);
});

test("domain boundaries reject the whole-language counterexamples and hidden extra work", () => {
    assert.throws(() => spanLanguage("probe ***a***\n\n", "**"), /outside/);
    for (const source of [
        "probe +++a+++\n\n",
        "probe **a**\n\n",
        "probe ++a ++b++++\n\n",
        "probe ++a++x\n\n",
        "probe ++++\n\n",
        "probe ++ a++\n\n",
        "probe ++a ++\n\n",
        "probe ++a\nb++\n\n",
        "probe ++a\\+b++\n\n",
        "probe ++a&amp;b++\n\n",
        "probe ++[a](b)++\n\n",
        "probe ++a++ {.c}\n\n",
        ""
    ]) {
        assert.throws(() => spanLanguage(source, "++"), /outside/);
    }
    assert.throws(() => proofWorkload(pair, document("a"), "probe **b**\n\n"), /does not commute/);
});

test("valid deep compositions do not depend on the JavaScript recursion limit", () => {
    const body = "left ++".repeat(5000) + "center" + "++ right".repeat(5000);
    const source = document(body);
    assert.equal(proofWorkload(pair, source, source.replaceAll("++", "**")).kind, "Document");
});

const dump = `Document scope=1:1..1:11 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:11 anchor=null attributes={} children=2
    ├── Text scope=1:1..1:6 anchor=null attributes={} literal="probe " children=0
    └── Insertion scope=1:7..1:11 anchor=null attributes={} children=1
        └── Text scope=1:9..1:9 anchor=null attributes={} literal="a" children=0
`;
const xml =
    '<document><paragraph><text xml:space="preserve">probe </text><strong><text xml:space="preserve">a</text></strong></paragraph></document>';

test("the full-tree projection observes values, kinds, order, nesting and unexpected fields", () => {
    const expected = spanLanguage(document("a"), "++");
    const parsed = parseCanonicalDump(dump);
    assert.deepEqual(proofTree(pair, "dialect", parsed), expected);
    assert.deepEqual(proofTree(pair, "reference", parseUpstreamXml(xml)), expected);
    assert.deepEqual(proofTree(pair, "common", parseCanonicalDump(dump.replace("Insertion", "Strong"))), expected);
    const changed = globalThis.structuredClone(parsed);
    changed.children[0].children[1].children[0].fields.literal = "b";
    assert.notDeepEqual(proofTree(pair, "dialect", changed), expected);
    const reparented = globalThis.structuredClone(parsed);
    const paragraph = reparented.children[0];
    paragraph.children[1].children.unshift(paragraph.children.shift());
    assert.notDeepEqual(proofTree(pair, "dialect", reparented), expected); // same node census
    const reordered = globalThis.structuredClone(parsed);
    reordered.children[0].children.reverse();
    assert.notDeepEqual(proofTree(pair, "dialect", reordered), expected);
    for (const mutation of [
        (tree) => {
            tree.children[0].children[1].kind = "Mark";
        },
        (tree) => {
            tree.children[0].fields.extra = "work";
        },
        (tree) => {
            tree.children[0].fields.anchor = "name";
        },
        (tree) => {
            tree.children[0].fields.attributes = "{.c}";
        }
    ]) {
        const tree = globalThis.structuredClone(parsed);
        mutation(tree);
        assert.throws(() => proofTree(pair, "dialect", tree), /unmapped/);
    }
});

function reportFixture(pairs) {
    const stage = { ir: 100, dataRefs: 100, irPerByte: 1, dataRefsPerByte: 1 };
    const engine = { stages: { source_to_buffer: stage, buffer_to_ast: stage }, total: { ir: 250 } };
    return {
        schemaVersion: 4,
        cmark: { version: "test", commit: "123456789abc" },
        cmarkGfm: { version: "test", commit: "123456789abc" },
        toolchain: {
            compiler: "test",
            compilerDigest: "digest",
            compilerBinaries: "digest",
            libc: "test",
            libraries: { summary: "test", digest: "digest" },
            valgrind: "test",
            valgrindDigest: "digest",
            architecture: "test",
            target: "test",
            targetDigest: "digest",
            flags: "",
            dispatch: "digest",
            compiled: { objects: {}, shared: "" }
        },
        corpus: { digest: "test", cases: pairs.length * 2 },
        pairs,
        artifacts: "fixture",
        cases: pairs.flatMap((pair) =>
            [pair.case, pair.isomorph].map((name, i) => ({
                case: name,
                dialect: i ? "commonmark" : "extended",
                scale: 1,
                bytes: 100,
                units: 1,
                carries: [],
                engines: {
                    "markdown-core": globalThis.structuredClone(engine),
                    cmark: globalThis.structuredClone(engine)
                }
            }))
        )
    };
}

test("reports keep candidates out of formal medians, including filtered candidate-only runs", () => {
    const fixture = reportFixture([pair, candidate]);
    fixture.cases[2].gfm = true;
    fixture.cases[2].engines["markdown-core"].stages.source_to_buffer.ir = 99999;
    const report = markdownReport(fixture);
    assert.match(report, /1 proved-domain pair\(s\) and 1 candidate pair\(s\)/);
    assert.match(report, /\| Dialect, proved domain \|[^\n]+\| 1 \|/);
    assert.match(report, /Candidate pair diagnostics \(equivalence unproved\)/);
    assert.match(report, /No structural mapping/);
    assert.doesNotMatch(report, /\| GFM extensions \|/);
    assert.match(report, /\| Dialect, proved domain \|[^\n]+\| 1 \| 1.00x \|/);
    assert.throws(() => markdownReport({ ...fixture, schemaVersion: 3 }), /schema 4/);
    const candidateOnly = markdownReport(reportFixture([candidate]));
    assert.doesNotMatch(candidateOnly, /\| Dialect, proved domain \|/);
    assert.match(candidateOnly, /\| candidate \| twin \| 200 \| 200 \| 200 \| 1.00x \| 1.00x \| 1.00x \|/);
});
