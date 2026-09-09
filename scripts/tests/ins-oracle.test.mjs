import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import fs from "node:fs";
import test from "node:test";
import {
    oracle,
    fromTokens,
    fromCanonical,
    assertCanaries,
    validatePolicy,
    verifyComparison
} from "../lib/ins-oracle.mjs";

const cases = JSON.parse(
    fs.readFileSync(new URL("../../specs/oracles/markdown-it-ins/corpus.json", import.meta.url), "utf8")
);
const policy = JSON.parse(
    fs.readFileSync(new URL("../../specs/oracles/markdown-it-ins/deltas.json", import.meta.url), "utf8")
);

test("oracle is active and preserves plus placement, nesting and opaque tokens", () => {
    assertCanaries();
    const node = fromTokens(oracle.parse("++++a++++", {})).children[0].children[0];
    assert.equal(node.kind, "Insertion");
    assert.equal(node.children[0].kind, "Insertion");
    assert.equal(node.children[0].children[0].literal, "a");
    assert.throws(() => fromTokens([{ type: "ins_close", nesting: -1 }]), /unbalanced/);
    assert.throws(() => fromTokens([{ type: "ins_open", nesting: 1 }]), /unclosed/);
    assert.throws(() => fromTokens([{ type: "new_kind", nesting: 0 }]), /unmapped/);
    assert.throws(() => fromCanonical({ kind: "NewKind", children: [] }), /unmapped/);
});

test("empty table fields are projected without discarding real token sections", () => {
    const source = "| head |\n| --- |\n| ++body++ |\n";
    const table = fromTokens(oracle.parse(source, {})).children[0];
    assert.equal(table.children[0].kind, "TableHead");
    assert.equal(table.children[1].kind, "TableBody");
    assert.equal(table.children[1].children[0].children[0].children[0].kind, "Insertion");
    assert.deepEqual(
        fromCanonical({ kind: "Table", fields: {}, children: [{ kind: "TableFoot", fields: {}, children: [] }] }),
        { kind: "Table", children: [] }
    );
});

test("corpus and divergence policy reject missing, duplicated and stale evidence", () => {
    assert.equal(validatePolicy(policy, cases).size, policy.expectedDivergences.length);
    assert.throws(() => validatePolicy(policy, []), /empty/);
    assert.throws(() => validatePolicy(policy, cases.slice(1)), /corpus changed/);
    assert.throws(() => validatePolicy({ ...policy, baselineGaps: [{ item: "I1" }] }, cases), /no.*gaps/);
    for (const mutate of [
        (p) => p.expectedDivergences.push(p.expectedDivergences[0]),
        (p) => (p.expectedDivergences[0].id = "removed-case"),
        (p) => (p.expectedDivergences[0].input += "changed"),
        (p) => (p.expectedDivergences[0].reason = ""),
        (p) => (p.expectedDivergences[0].oracleDigest = "invalid")
    ]) {
        const changed = JSON.parse(JSON.stringify(policy));
        mutate(changed);
        assert.throws(() => validatePolicy(changed, cases), /invalid or stale/);
    }
});

test("a mismatch needs exact digests and an agreement retires its exception", () => {
    const digest = (value) => createHash("sha256").update(value).digest("hex");
    const entry = { oracleDigest: digest("oracle"), markdownCoreDigest: digest("product") };
    assert.equal(verifyComparison(undefined, "equal", "equal"), false);
    assert.equal(verifyComparison(entry, "oracle", "product"), true);
    assert.throws(() => verifyComparison(undefined, "oracle", "product"), /unregistered/);
    assert.throws(() => verifyComparison(entry, "changed", "product"), /changed/);
    assert.throws(() => verifyComparison(entry, "oracle", "changed"), /changed/);
    assert.throws(() => verifyComparison(entry, "equal", "equal"), /now agrees/);
});
