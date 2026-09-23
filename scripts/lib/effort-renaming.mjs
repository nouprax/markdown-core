/** Finite obstructions to whole-domain alphabet conjugacy, NOT optimum inequality.
 * See benchmark-effort-renaming.md for the universally quantified theorem.
 */
import assert from "node:assert/strict";
import { Buffer } from "node:buffer";

export const renamingObstructions = [
    {
        id: "plus-to-star-v1",
        marker: "+",
        width: 1,
        proofs: ["insertion-strong-v1", "run-insertion-v2"],
        targetShape: [1, 1, 1, 0],
        otherwise: [1, 1, 0],
        exceptions: [
            { bytes: [9, 32], shape: [1, 1, 1, 1, 0] },
            { bytes: [10, 13], shape: [1, 2, 0, 0] }
        ]
    },
    {
        id: "equals-to-star-v1",
        marker: "=",
        width: 1,
        proofs: ["run-mark-v2"],
        targetShape: [1, 1, 1, 0],
        otherwise: [1, 1, 0],
        exceptions: []
    },
    {
        id: "tilde-to-star-v1",
        marker: "~",
        width: 3,
        proofs: ["run-strike-v2", "run-sub-v2"],
        targetShape: [1, 1, 1, 1, 0],
        otherwise: [1, 0],
        exceptions: []
    },
    {
        id: "caret-to-star-v1",
        marker: "^",
        width: 2,
        proofs: ["run-super-v2"],
        targetShape: [1, 1, 1, 0],
        otherwise: [1, 3, 0, 0, 0],
        exceptions: [
            { bytes: [91], shape: [1, 2, 1, 0, 0] },
            { bytes: [92], shape: [1, 2, 0, 0] }
        ]
    }
];

function freeze(value) {
    if (value && typeof value === "object") {
        Object.values(value).forEach(freeze);
        Object.freeze(value);
    }
    return value;
}
freeze(renamingObstructions);

const byProof = new Map();
for (const certificate of renamingObstructions) {
    for (const proof of certificate.proofs) {
        assert.ok(!byProof.has(proof), `duplicate renaming obstruction: ${proof}`);
        byProof.set(proof, certificate);
    }
}

export function renamingReview(proof) {
    const certificate = byProof.get(proof);
    return certificate
        ? Object.freeze({
              status: "refuted",
              scope: "whole-domain-byte-permutation-with-ordered-owner-bijection",
              certificate: certificate.id,
              sourceByte: certificate.marker.charCodeAt(0),
              targetByte: 42
          })
        : null;
}

export function validateRenamingProofs(proofs) {
    const known = new Set(proofs);
    for (const proof of byProof.keys()) assert.ok(known.has(proof), `stale renaming obstruction: ${proof}`);
}

/** Preorder arities determine an ordered rooted tree, without erasing owners.
 * These witnesses have no owned inline fields or non-tree ownership edges.
 * Tags/fields are deliberately ignored: a shape mismatch is already enough
 * to refute ANY tag bijection, while a match cannot establish equivalence.
 */
export function ownerShape(tree) {
    const pending = [tree];
    const seen = new Set();
    const shape = [];
    while (pending.length) {
        const node = pending.pop();
        assert.ok(node && Array.isArray(node.children), "missing owner tree");
        assert.ok(!seen.has(node), "repeated owner in tree");
        seen.add(node);
        shape.push(node.children.length);
        for (let i = node.children.length - 1; i >= 0; i--) pending.push(node.children[i]);
    }
    return shape;
}

/** For EVERY permutation pi with pi(marker)='*', pi^-1('*'^k x '*'^k)
 * is marker^k b marker^k, for b=pi^-1(x). Enumerating all bytes covers every
 * possible inverse, even allowing arbitrary changes to the other 255 bytes.
 * b=marker cannot occur in such a permutation; checking it too is harmless.
 */
export function renamingPreimages(certificate) {
    const marker = Buffer.alloc(certificate.width, certificate.marker);
    return Array.from({ length: 256 }, (_, byte) => ({
        byte,
        input: Buffer.concat([marker, Buffer.from([byte]), marker]),
        shape: certificate.exceptions.find((group) => group.bytes.includes(byte))?.shape ?? certificate.otherwise
    }));
}

/** The parse callback receives raw bytes, never UTF-8-decoded source strings.
 * Parser errors and any drift in the reviewed finite classification fail closed.
 */
export function auditRenamingObstructions(parse) {
    return renamingObstructions.map((certificate) => {
        const target = Buffer.from(`${"*".repeat(certificate.width)}x${"*".repeat(certificate.width)}`);
        for (const engine of ["core", "cmark"]) {
            assert.deepEqual(
                ownerShape(parse(engine, target)),
                certificate.targetShape,
                `${certificate.id}: ${engine} target classification changed`
            );
        }
        const shapes = new Map();
        for (const { byte, input, shape } of renamingPreimages(certificate)) {
            const actual = ownerShape(parse("core", input));
            const context = `${certificate.id}: source byte 0x${byte.toString(16).padStart(2, "0")}`;
            assert.deepEqual(actual, shape, `${context} classification changed`);
            assert.notDeepEqual(actual, certificate.targetShape, `${context} no longer separates owners`);
            const key = JSON.stringify(actual);
            shapes.set(key, (shapes.get(key) ?? 0) + 1);
        }
        return {
            certificate: certificate.id,
            sourceCases: 256,
            targetCases: 2,
            target: target.toString("ascii"),
            targetShape: certificate.targetShape,
            sourceShapes: [...shapes].map(([shape, count]) => ({ shape: JSON.parse(shape), count }))
        };
    });
}
