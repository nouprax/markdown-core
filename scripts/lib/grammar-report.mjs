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

export function grammarComparisons(report) {
    const grammar = report.grammarCorpus;
    assert.ok(grammar && Array.isArray(grammar.certificates) && Array.isArray(grammar.proofs));
    const certificates = new Map();
    for (const certificate of grammar.certificates) {
        id(certificate.id);
        id(certificate.certificate);
        assert.ok(["paired-document-grammar", "boundary-grammar"].includes(certificate.scope));
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
        rows = [];
    for (const proof of grammar.proofs) {
        const certificate = certificates.get(proof.certificate);
        assert.ok(certificate, "unknown grammar certificate");
        assert.equal(proof.scope, certificate.scope);
        const proofKey = id(proof.certificate);
        assert.ok(!proofs.has(proofKey), "duplicate measured proof");
        proofs.add(proofKey);
        const part = proof.scope === "boundary-grammar" ? "boundary" : "paired";
        const find = (name) => cases.get(id(proof.names[name]));
        const a = find(`${part}-dialect`),
            b = find(`${part}-common`);
        const hosts = part === "boundary" ? [find("host-dialect"), find("host-common")] : [];
        if (![a, b, ...hosts].some(Boolean)) continue;
        assert.ok(a && b && hosts.every(Boolean), "missing measured grammar counterpart or host");
        for (const [index, document] of [a, b, ...hosts].entries()) {
            assert.equal(document.side, index % 2 === 0 ? "dialect" : "common");
            assert.equal(document.part, index < 2 ? part : "host");
            assert.equal(document.certificate, proof.certificate);
            assert.equal(document.units, positive(proof.units));
            positive(document.bytes);
            const name = id(document.case);
            assert.ok(!consumed.has(name), "measurement reused by another proof");
            consumed.add(name);
        }
        assert.equal(typeof b.gfm, "boolean");
        const reference = b.gfm ? "cmark-gfm" : "cmark";
        const aIr = instructions(a, "markdown-core"),
            bIr = instructions(b, "markdown-core"),
            rIr = instructions(b, reference);
        rows.push({
            certificate: certificate.certificate,
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
            hostIr: hosts.length ? instructions(hosts[0], "markdown-core") : null
        });
    }
    assert.equal(consumed.size, cases.size, "measurement has no grammar proof");
    assert.ok(rows.length, "no measured grammar comparisons");
    return rows;
}
