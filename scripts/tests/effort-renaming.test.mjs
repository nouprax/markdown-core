import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import test from "node:test";
import {
    auditRenamingObstructions,
    ownerShape,
    renamingObstructions,
    renamingPreimages,
    renamingReview,
    validateRenamingProofs
} from "../lib/effort-renaming.mjs";
import { effortReview } from "../lib/pair-effort.mjs";
import { productionProofs } from "../lib/pair-productions.mjs";

const node = (...children) => ({ kind: "owner", children });
const chain = (length) => (length === 1 ? node() : node(chain(length - 1)));

// Replay reviewed shapes to test the checker/transport, not parser semantics.
// The native corpus audit independently executes all 1,032 grammar obligations.
function fromShape(shape) {
    let offset = 0;
    const read = () => node(...Array.from({ length: shape[offset++] }, read));
    const result = read();
    assert.equal(offset, shape.length);
    return result;
}
function replay(engine, input) {
    if (input[0] === 42) return chain(input.length === 7 ? 5 : 4);
    assert.equal(engine, "core");
    const certificate = renamingObstructions.find((entry) => entry.marker.charCodeAt(0) === input[0]);
    assert.ok(certificate);
    const row = renamingPreimages(certificate)[input[certificate.width]];
    assert.deepEqual(input, row.input);
    return fromShape(row.shape);
}

test("preimages cover every byte and allow permutations of payload, whitespace and escapes", () => {
    for (const certificate of renamingObstructions) {
        const marker = certificate.marker.charCodeAt(0);
        const inputs = renamingPreimages(certificate);
        assert.deepEqual(
            inputs.map((row) => row.byte),
            Array.from({ length: 256 }, (_, i) => i)
        );
        assert.equal(new Set(inputs.map((row) => row.input.toString("hex"))).size, 256);
        for (const { byte, input } of inputs) {
            assert.equal(input.length, 2 * certificate.width + 1);
            assert.equal(input[certificate.width], byte);
            if (byte === marker) continue; // Impossible inverse of x under this constraint.
            const permutation = Array.from({ length: 256 }, (_, i) => i);
            for (const [from, to] of [
                [marker, 42],
                [byte, 120]
            ]) {
                const other = permutation.indexOf(to);
                [permutation[from], permutation[other]] = [permutation[other], permutation[from]];
            }
            assert.equal(new Set(permutation).size, 256);
            assert.equal(permutation[marker], 42);
            assert.deepEqual(
                input.map((value) => permutation[value]),
                Buffer.from(`${"*".repeat(certificate.width)}x${"*".repeat(certificate.width)}`)
            );
        }
    }
});

test("owner topology distinguishes equal node counts and sibling order", () => {
    const target = chain(4);
    const siblings = node(node(node(), node()));
    assert.equal(ownerShape(target).length, ownerShape(siblings).length);
    assert.notDeepEqual(ownerShape(target), ownerShape(siblings));
    assert.notDeepEqual(ownerShape(node(node(node()), node())), ownerShape(node(node(), node(node()))));
    assert.deepEqual(ownerShape({ ...target, kind: "retagged", fields: { literal: "other" } }), ownerShape(target));
    assert.throws(() => ownerShape({}), /missing owner/);
    const cyclic = node();
    cyclic.children.push(cyclic);
    assert.throws(() => ownerShape(cyclic), /repeated owner/);
});

test("audit checks every raw inverse and both full-parser targets without text decoding", () => {
    const calls = [];
    const receipts = auditRenamingObstructions((engine, input) => {
        assert.ok(Buffer.isBuffer(input));
        calls.push([engine, input.toString("hex")]);
        return replay(engine, input);
    });
    assert.equal(calls.length, 1032);
    assert.equal(calls.filter(([engine]) => engine === "cmark").length, 4);
    for (const certificate of renamingObstructions) {
        const expected = renamingPreimages(certificate).map((row) => row.input.toString("hex"));
        const observed = calls.filter(([, hex]) => hex.startsWith(certificate.marker.charCodeAt(0).toString(16)));
        assert.deepEqual(
            observed.map(([, hex]) => hex),
            expected
        );
    }
    for (const receipt of receipts) {
        assert.equal(receipt.sourceCases, 256);
        assert.equal(receipt.targetCases, 2);
        assert.equal(
            receipt.sourceShapes.reduce((sum, group) => sum + group.count, 0),
            256
        );
        for (const group of receipt.sourceShapes) assert.notDeepEqual(group.shape, receipt.targetShape);
    }
});

test("a collision at ff, noncolliding grammar drift and native parser failure all fail closed", () => {
    for (const replacement of [chain(4), chain(6)]) {
        assert.throws(
            () =>
                auditRenamingObstructions((engine, input) =>
                    input.equals(Buffer.from([43, 255, 43])) ? replacement : replay(engine, input)
                ),
            /source byte 0xff classification changed/
        );
    }
    assert.throws(
        () =>
            auditRenamingObstructions((engine, input) => {
                if (input.includes(128)) throw new Error("native parse failed");
                return replay(engine, input);
            }),
        /native parse failed/
    );
    for (const failed of ["core", "cmark"]) {
        assert.throws(
            () =>
                auditRenamingObstructions((engine, input) =>
                    engine === failed && input[0] === 42 ? node() : replay(engine, input)
                ),
            /target classification changed/
        );
    }
});

test("six scoped mapping refutations never become whole-parser optimal-cost inequalities", () => {
    const proofs = ["insertion-strong-v1", ...productionProofs.keys()];
    validateRenamingProofs(proofs);
    assert.throws(() => validateRenamingProofs(proofs.filter((id) => id !== "run-mark-v2")), /stale/);
    const affected = proofs.filter((proof) => renamingReview(proof));
    assert.equal(affected.length, 6);
    assert.equal(proofs.length - affected.length, 37);
    for (const proof of proofs) {
        const review = effortReview({ contract: { proof } });
        assert.equal(review.status, "unproved");
        assert.equal(review.scope, "full-parser-optimum");
        if (affected.includes(proof)) {
            assert.equal(review.alphabetRenaming.status, "refuted");
            assert.equal(review.alphabetRenaming.scope, "whole-domain-byte-permutation-with-ordered-owner-bijection");
            assert.equal(review.alphabetRenaming.targetByte, 42);
        } else assert.equal(review.alphabetRenaming, undefined);
    }
    assert.equal(renamingReview("invented"), null);
});
