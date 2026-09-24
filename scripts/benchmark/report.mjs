/** Numeric projection shared by artifact reports and the privileged PR publisher. */
import assert from "node:assert/strict";

const id = (value) => {
    assert.equal(typeof value, "string");
    assert.match(value, /^[a-z0-9][a-z0-9-]{0,127}$/u);
    return value;
};
const positive = (value) => {
    assert.ok(Number.isSafeInteger(value) && value > 0, "expected positive benchmark count");
    return value;
};
const instructions = (document, engine) => {
    const stages = document.engines[engine]?.stages;
    assert.ok(stages, "missing grammar measurement");
    return positive(
        ["source_to_buffer", "buffer_to_ast"].reduce((sum, name) => {
            const stage = stages[name];
            const count = positive(stage?.cost?.Ir);
            if (stage.ir !== undefined) assert.equal(stage.ir, count, "inconsistent stage instructions");
            return sum + count;
        }, 0)
    );
};

const references = new Set(["cmark", "cmark-gfm"]);
/* The letters a certificate's documents spell their words with (corpus.mjs):
 * each certificate is measured once in each. */
const alphabets = new Set(["ascii", "utf8"]);

/** Equivalences compare Core with a reference on the same common input. A
 * certificate built around a construct no reference implements has no such
 * counterpart; its rows compare Core with Core on a byte-neutral control and
 * are returned apart, so they cannot enter a reference aggregate. */
export function grammarComparisons(report) {
    const grammar = report.grammarCorpus;
    assert.ok(grammar && Array.isArray(grammar.certificates) && Array.isArray(grammar.proofs));
    const certificates = new Map();
    for (const certificate of grammar.certificates) {
        id(certificate.id);
        id(certificate.certificate);
        assert.ok(["paired-document-grammar", "boundary-grammar"].includes(certificate.scope));
        if (certificate.reference === null) {
            assert.equal(certificate.scope, "paired-document-grammar");
            id(certificate.rejects);
        } else assert.ok(references.has(certificate.reference), "unknown grammar reference");
        assert.ok(!certificates.has(certificate.certificate), "duplicate grammar certificate");
        certificates.set(certificate.certificate, certificate);
    }
    const cases = new Map();
    for (const document of report.cases) {
        const name = id(document.case);
        assert.ok(!cases.has(name), "duplicate grammar measurement");
        cases.set(name, document);
    }
    const consumed = new Set(),
        proofs = new Set(),
        equivalences = [],
        rejections = [];
    for (const proof of grammar.proofs) {
        const certificate = certificates.get(proof.certificate);
        assert.ok(certificate, "unknown grammar certificate");
        assert.equal(proof.scope, certificate.scope);
        assert.ok(alphabets.has(proof.alphabet), "unknown grammar alphabet");
        const proofKey = `${id(proof.certificate)} ${proof.alphabet}`;
        assert.ok(!proofs.has(proofKey), "duplicate measured proof");
        proofs.add(proofKey);
        const part = proof.scope === "boundary-grammar" ? "boundary" : "paired";
        const reference = certificate.reference;
        // A rejection has a control exactly when it gives no reason for lacking one.
        const controlled = reference === null && certificate.uncontrolled === undefined;
        if (reference === null && !controlled) assert.ok(typeof certificate.uncontrolled === "string");
        assert.equal(proof.names["paired-control"] !== undefined, controlled, "control does not match its certificate");
        const expected = [
            [`${part}-dialect`, "dialect", part, false],
            [`${part}-common`, "common", part, reference !== null],
            ...(part === "boundary"
                ? [
                      ["host-dialect", "dialect", "host", false],
                      ["host-common", "common", "host", false]
                  ]
                : []),
            ...(controlled ? [["paired-control", "control", "paired", false]] : [])
        ];
        const found = expected.map(([name]) => cases.get(id(proof.names[name])));
        if (!found.some(Boolean)) continue;
        assert.ok(found.every(Boolean), "missing measured grammar counterpart, host or control");
        for (const [index, document] of found.entries()) {
            const [, side, kind, measuredByReference] = expected[index];
            assert.equal(document.side, side);
            assert.equal(document.part, kind);
            assert.equal(document.certificate, proof.certificate);
            assert.equal(document.alphabet, proof.alphabet);
            assert.equal(document.units, positive(proof.units));
            positive(document.bytes);
            assert.deepEqual(
                Object.keys(document.engines).sort(),
                measuredByReference ? ["markdown-core", reference].sort() : ["markdown-core"],
                "a reference measured an input it has no certified counterpart for"
            );
            const name = id(document.case);
            assert.ok(!consumed.has(name), "measurement reused by another proof");
            consumed.add(name);
        }
        const [a, b, ...rest] = found;
        const bIr = instructions(b, "markdown-core");
        if (reference === null) {
            const c = controlled ? rest[0] : null;
            if (c) assert.equal(c.bytes, b.bytes, "a control changed the document's width");
            const cIr = c ? instructions(c, "markdown-core") : null;
            rejections.push({
                certificate: certificate.certificate,
                alphabet: proof.alphabet,
                construct: certificate.rejects,
                units: proof.units,
                bytes: b.bytes,
                bIr,
                cIr,
                bc: c ? bIr / cIr : null,
                excess: c ? (bIr - cIr) / proof.units : null
            });
            continue;
        }
        const aIr = instructions(a, "markdown-core"),
            rIr = instructions(b, reference);
        equivalences.push({
            certificate: certificate.certificate,
            alphabet: proof.alphabet,
            scope: proof.scope,
            units: proof.units,
            aBytes: a.bytes,
            bBytes: b.bytes,
            aIr,
            bIr,
            rIr,
            reference,
            ab: aIr / bIr,
            br: bIr / rIr,
            ar: aIr / rIr,
            hostIr: rest.length ? instructions(rest[0], "markdown-core") : null
        });
    }
    assert.equal(consumed.size, cases.size, "measurement has no grammar proof");
    assert.ok(equivalences.length + rejections.length, "no measured grammar comparisons");
    return { equivalences, rejections };
}
