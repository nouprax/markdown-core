import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import {
    equalProofTrees,
    pairingIdentity,
    structuralPair,
    pairRatios,
    proofTree,
    proofWorkload,
    spanLanguage,
    validatePairs
} from "../lib/corpus-pairs.mjs";
import { parseCanonicalDump, parseUpstreamXml } from "../lib/upstream-cmark.mjs";
import { markdownReport } from "../benchmark-stages.mjs";
import { productionProofs, productionWorkload, productionTree } from "../lib/pair-productions.mjs";
import { boundarySource, pairReviews } from "../lib/pair-review.mjs";
import { caseClosure } from "../lib/corpus-splits.mjs";
import { publishesRatio } from "../lib/corpus-splits.mjs";
import { effortModel, effortReview, validateEffortReviews } from "../lib/pair-effort.mjs";

const manifest = () =>
    JSON.parse(
        fs.readFileSync(new URL("../../packages/markdown-core/benchmarks/corpus.json", import.meta.url), "utf8")
    );
const pair = { case: "dialect", isomorph: "common", contract: { proof: "insertion-strong-v1" } };
const candidate = { case: "candidate", isomorph: "twin", contract: { pending: "No structural mapping." } };
const document = (body) => `probe ++${body}++\n\n`;

test("changing the pairing interpretation changes identity even with identical measured bytes", () => {
    const digest = pairingIdentity([pair], "proof", "checker");
    assert.equal(pairingIdentity([pair], "proof", "checker"), digest);
    assert.notEqual(pairingIdentity([candidate], "proof", "checker"), digest);
    assert.notEqual(pairingIdentity([pair], "revised proof", "checker"), digest);
    assert.notEqual(pairingIdentity([pair], "proof", "revised checker"), digest);
});

test("neither structural proofs nor candidates imply equal parser effort", () => {
    const declared = manifest();
    assert.equal(validatePairs(declared).length, 73);
    assert.equal(declared.pairs.filter(structuralPair).length, 43);
    assert.equal(declared.pairs.filter((pair) => pair.contract.review).length, 30);
    assert.equal(declared.pairs.filter((pair) => pair.contract.pending).length, 0);
    const costs = { dialect: 240, common: 160, reference: 100 };
    assert.deepEqual(pairRatios(candidate, costs), {
        structural: false,
        effort: effortReview(candidate),
        grammar: 1.5,
        shape: 1.6,
        quotient: 2.4
    });
    const proved = pairRatios(pair, costs);
    assert.equal(proved.structural, true);
    assert.equal(proved.quotient, 2.4);
    assert.equal(proved.effort.status, "unproved");
    assert.equal("sameJob" in proved, false);
    const contaminated = pairRatios(candidate, { ...costs, carries: ["anchor"] });
    assert.equal(contaminated.grammar, null);
    assert.equal(contaminated.shape, null);
    assert.equal(contaminated.quotient, 2.4);
    assert.equal("sameJob" in contaminated, false);
});

test("every structural proof has an explicit effort review and unknown proofs fail closed", () => {
    const proofs = ["insertion-strong-v1", ...productionProofs.keys()];
    validateEffortReviews(proofs);
    assert.throws(() => validateEffortReviews(proofs.slice(1)), /exactly/);
    assert.throws(() => validateEffortReviews([...proofs, "new-proof"]), /exactly/);
    for (const p of manifest().pairs.filter(structuralPair)) {
        const review = effortReview(p);
        assert.equal(review.model, effortModel);
        assert.equal(review.scope, "full-parser-optimum");
        assert.equal(review.status, "unproved");
        assert.ok(review.reason.length > 50);
    }
    assert.throws(() => effortReview({ case: "new", contract: { proof: "new-proof" } }), /missing parse-effort/);
});

test("equal bytes/trees, a chosen trace and manifest assertions cannot self-certify optimal effort", () => {
    const source = document("a ++b++ c");
    const common = source.replaceAll("++", "**");
    assert.equal(source.length, common.length);
    proofWorkload(pair, source, common);
    const claimed = { ...pair, effort: { status: "certified", equalTrace: true, equalOptimalCost: true } };
    const result = pairRatios(claimed, { dialect: 100, common: 100, reference: 100 });
    assert.equal(result.effort.status, "unproved");
    assert.equal("sameJob" in result, false);
    const identical = manifest().pairs.find((p) => p.contract.proof === "decimal-list-v2");
    assert.equal(effortReview(identical).category, "identical-input-control");
    assert.equal(effortReview(identical).status, "unproved");
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
        pairingDigest: "fixture-identity",
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

test("reports publish structural quotients but no equal-effort median, including filtered runs", () => {
    const fixture = reportFixture([pair, candidate]);
    fixture.cases[2].gfm = true;
    fixture.cases[2].engines["markdown-core"].stages.source_to_buffer.ir = 99999;
    const report = markdownReport(fixture);
    assert.match(report, /1 structural control\(s\) and 1 candidate pair\(s\)/);
    assert.match(report, /0 certified equal-optimal-effort pairs/);
    assert.match(report, /\| insertion-strong-v1 \| unproved \|/);
    assert.match(report, /\| insertion-strong-v1 \| unproved \| refuted: plus-to-star-v1 \|/);
    assert.match(report, /they do not prove unequal optimal costs/);
    assert.match(report, /not admission conditions for equivalence through formal grammar rewrites/);
    assert.match(report, /Candidate pair diagnostics \(equivalence unproved\)/);
    assert.match(report, /No structural mapping/);
    assert.match(report, /\| Pairing contracts \| `fixture-identity` \|/);
    assert.doesNotMatch(report, /\| GFM extensions \|/);
    assert.doesNotMatch(report, /\| Dialect, proved domain \||Median Same-job|\| Same-job \|/);
    assert.throws(() => markdownReport({ ...fixture, schemaVersion: 3 }), /schema 4/);
    const candidateOnly = markdownReport(reportFixture([candidate]));
    assert.doesNotMatch(candidateOnly, /\| Dialect, proved domain \|/);
    assert.match(candidateOnly, /\| candidate \| twin \| 200 \| 200 \| 200 \| 1.00x \| 1.00x \| 1.00x \|/);
});

test("every reviewed pair resolves to measured proof domains and/or a concrete boundary", () => {
    const declared = manifest();
    assert.equal(pairReviews.size, 30);
    assert.equal([...pairReviews.values()].filter((review) => review.baseline).length, 12);
    for (const pair of declared.pairs.filter((pair) => pair.contract.review)) {
        const review = pairReviews.get(pair.contract.review);
        const closure = caseClosure(declared, [pair.isomorph]);
        assert.ok(review.proofs.length || review.baseline);
        for (const proof of review.proofs)
            assert.ok(closure.has(declared.pairs.find((other) => other.contract.proof === proof).case));
        if (review.baseline) assert.ok(closure.has(review.baseline));
        assert.equal(structuralPair(pair), false);
    }
    const broken = manifest();
    broken.cases = broken.cases.filter((entry) => entry.name !== "boundary-anchor-without");
    assert.throws(() => validatePairs(broken), /boundary baseline/);
    const noProof = manifest();
    noProof.pairs = noProof.pairs.filter((pair) => pair.contract.proof !== "simple-matrix-v2");
    assert.throws(() => validatePairs(noProof), /missing reconstructed proof/);
});

test("every production admits varying ordered units and rejects noninvertible or out-of-domain sources", () => {
    for (const [id, proof] of productionProofs) {
        const values = proof.unique
            ? ["999999", "000000", "123456", "111111"]
            : ["999999", "000000", "123456", "000000"];
        const render = (template) => values.map((n) => template.replaceAll("{n:6}", n)).join("");
        const dialect = render(proof.dialect);
        const common = render(proof.common);
        const result = productionWorkload(id, dialect, common);
        assert.deepEqual(result.values, values, id);
        assert.ok(result.children.length > values.length, id);
        assert.ok(
            result.children.some((node) => node.kind === "ThematicBreak"),
            id
        );
        assert.throws(() => productionWorkload(id, dialect, common.replace("999999", "999998")), /commute/);
        assert.throws(() => productionWorkload(id, dialect + "extra", common), /outside/);
        assert.throws(() => productionWorkload(id, dialect.replaceAll("999999", "1000000"), common), /outside/);
        assert.throws(() => productionWorkload(id, dialect.replace("***", "**"), common), /outside/);
        assert.throws(() => productionWorkload(id, "", ""), /commute/);
    }
});

test("semantic actions reject payload, order, default-field and ownership changes with unchanged censuses", () => {
    const id = "opaque-formula-v2";
    const proof = productionProofs.get(id);
    const expected = productionWorkload(
        id,
        proof.dialect.replaceAll("{n:6}", "000007"),
        proof.common.replaceAll("{n:6}", "000007")
    );
    const tree = parseCanonicalDump(`Document anchor=null attributes={} children=2
├── Paragraph anchor=null attributes={} children=3
│   ├── Text anchor=null attributes={} literal="probe " children=0
│   ├── Formula anchor=null attributes={} mode=embedded literal="body 000007" children=0
│   └── Text anchor=null attributes={} literal=" end" children=0
└── ThematicBreak anchor=null attributes={} children=0
`);
    assert.deepEqual(productionTree(id, "dialect", tree, expected), expected);
    for (const mutate of [
        (t) => (t.children[0].children[1].fields.literal = "body 000008"),
        (t) => (t.children[0].children[1].fields.mode = "standalone"),
        (t) => t.children[0].children.reverse(),
        (t) => (t.children[0].children[1].fields.attributes = "{.unexpected}"),
        (t) => t.children[1].children.push(t.children[0].children.pop())
    ]) {
        const changed = globalThis.structuredClone(tree);
        mutate(changed);
        assert.throws(() => productionTree(id, "dialect", changed, expected), /semantic tree mismatch/);
    }
});

test("boundaries are controlled interventions and cannot silently measure unchanged or empty hosts", () => {
    assert.equal(boundarySource("embed", "![[img|100x200]] and ![[img|label]]\n"), "![[img|]] and ![[img|label]]\n");
    assert.equal(boundarySource("caption", "Table: Caption 2\n| h |\n| --- |\n| v |\n"), "| h |\n| --- |\n| v |\n");
    assert.equal(boundarySource("metadata", "---\nstate: true\n---\n\nBody.\n"), "Body.\n");
    assert.throws(() => boundarySource("anchor", "unchanged\n"), /no work/);
    assert.throws(() => boundarySource("metadata", "---\nstate: true\n---\n\n"), /entire host/);
    assert.throws(() => boundarySource("invented", "text"), /unknown boundary/);
});

test("boundary reports keep signed interaction costs separate from formal medians", () => {
    const original = { case: "pair-anchor-dialect", isomorph: "pair-anchor-common", contract: { review: "anchor" } };
    const fixture = reportFixture([original]);
    const baseline = globalThis.structuredClone(fixture.cases[0]);
    baseline.case = "boundary-anchor-without";
    baseline.boundary = { match: original.case, cut: "anchor" };
    baseline.carries = [];
    baseline.engines["markdown-core"].stages.source_to_buffer = {
        ...baseline.engines["markdown-core"].stages.source_to_buffer,
        ir: 300
    };
    fixture.cases.push(baseline);
    const report = markdownReport(fixture);
    assert.match(report, /Reviewed corpus boundaries/);
    assert.match(report, /\| pair-anchor-dialect \| boundary-anchor-without \| 200 \| 400 \| -200 \| -200.00 \|/);
    assert.doesNotMatch(report, /\| Dialect, proved domain \|/);
    fixture.cases[2].units = 2;
    assert.throws(() => markdownReport(fixture), /boundary unit count mismatch/);
});

test("named graph domains reject duplicate definitions and require reference graph evidence", () => {
    const id = "specimen-graph-v2";
    const proof = productionProofs.get(id);
    const dialect = proof.dialect.replaceAll("{n:6}", "000007");
    const common = proof.common.replaceAll("{n:6}", "000007");
    assert.throws(() => productionWorkload(id, dialect.repeat(2), common.repeat(2)), /duplicate binding/);
    const expected = productionWorkload(id, dialect, common);
    const tree = parseUpstreamXml(
        `<document><paragraph><text>As </text><<unknown> /><text> shows.</text></paragraph><thematic_break /><<unknown>><paragraph><text>Example 000007.</text></paragraph></<unknown>></document>`
    );
    const html = proof.referenceHtml(["000007"]);
    assert.deepEqual(productionTree(id, "reference", tree, expected, html), expected);
    assert.throws(() => productionTree(id, "reference", tree, expected), /binding graph mismatch/);
    for (const changed of [
        html.replace('href="#fn-spec-000007"', 'href="#fn-spec-000008"'),
        html.replace('id="fn-spec-000007"', 'id="fn-spec-000008"'),
        html.replace(">1</a>", ">2</a>"),
        html.replace('href="#fnref-spec-000007"', 'href="#fnref-spec-000008"')
    ]) {
        assert.throws(() => productionTree(id, "reference", tree, expected, changed), /binding graph mismatch/);
    }
    assert.equal(
        boundarySource("specimenstart", "As (@spec-7) shows.\n\n(5@spec-7) Body.\n"),
        "As (@spec-7) shows.\n\n(@spec-7) Body.\n"
    );
});

test("production contracts reject owner erasure even when both concrete actions are otherwise exact", () => {
    for (const [id, erased] of [
        ["grid-cell-v2", "Callout"],
        ["loose-definition-v2", "Callout"],
        ["inline-directive-v2", "Embedded"],
        ["empty-directive-v2", "Embedded"]
    ]) {
        const proof = productionProofs.get(id);
        const action = proof.action;
        const erase = (tree) => ({
            ...tree,
            children: tree.children.flatMap((child) =>
                child.kind === erased ? child.children.map(erase) : [erase(child)]
            )
        });
        try {
            // The former checker accepted ANY independently exact action here,
            // returning the same flat unit number despite the missing owner.
            proof.action = (s, n, side) => action(s, n, side).map((tree) => (side === "common" ? erase(tree) : tree));
            assert.throws(
                () =>
                    productionWorkload(
                        id,
                        proof.dialect.replaceAll("{n:6}", "000007"),
                        proof.common.replaceAll("{n:6}", "000007")
                    ),
                /ownership mismatch/,
                id
            );
        } finally {
            proof.action = action;
        }
    }
});

test("projected production trees retain structured payloads rather than only a unit index", () => {
    const proof = productionProofs.get("grid-cell-v2");
    const tree = productionWorkload(
        proof.id,
        proof.dialect.replaceAll("{n:6}", "000007"),
        proof.common.replaceAll("{n:6}", "000007")
    );
    const owner = tree.children[0].children[0].children[0];
    assert.equal(owner.kind, "Callout");
    assert.deepEqual(
        owner.children.map((p) => p.children[0].fields.literal),
        ["body 000007", "tail 000007"]
    );
    const changed = globalThis.structuredClone(tree);
    changed.children[0].children[0].children[0].children[0].children[0].fields.literal = "body 000008";
    assert.equal(equalProofTrees(tree, changed), false);
});
