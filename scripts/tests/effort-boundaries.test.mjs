import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import {
    admitBoundary,
    encodeParents,
    ownershipFixtures,
    splitWhitespaceBoundary,
    boundaryFixtures,
    boundaryOperations,
    boundaryOracle,
    boundaryPairAudit,
    verifyBoundaryReceipt
} from "../lib/effort-boundaries.mjs";
import { readBoundaryEdges, boundaryMarkdown, boundaryIdentity } from "../lib/measure-effort.mjs";

const fixture = (operation, text, options = {}) => ({ id: "test", operation, input: Buffer.from(text), ...options });
const output = (operation, text) => Buffer.from(boundaryOracle(fixture(operation, text)).hex, "hex").toString();
test("identity covers indirect cost dependencies and newly added library helpers", (t) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "boundary-identity-"));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    for (const file of [
        "docs/architecture",
        "scripts/lib",
        "scripts/benchmark-stages.mjs",
        "packages/markdown-core/benchmarks"
    ])
        fs.cpSync(new URL(`../../${file}`, import.meta.url), path.join(root, file), { recursive: true });
    const before = boundaryIdentity(root);
    for (const file of ["measurement.mjs", "callgrind.mjs", "compile-identity.mjs", "pair-review.mjs"]) {
        const target = path.join(root, "scripts/lib", file);
        const source = fs.readFileSync(target);
        fs.appendFileSync(target, "\n// changed dependency\n");
        assert.notEqual(boundaryIdentity(root), before, file);
        fs.writeFileSync(target, source);
        assert.equal(boundaryIdentity(root), before);
    }
    fs.mkdirSync(path.join(root, "scripts/lib/nested"));
    fs.writeFileSync(path.join(root, "scripts/lib/nested/helper.mjs"), "export const setting = 1;\n");
    assert.notEqual(boundaryIdentity(root), before);
});
test("normalization contracts preserve bytes, padding and escape consumption", () => {
    assert.equal(output("copy", "a\0b"), "a\0b");
    assert.equal(output("trim", " \t\na\0b\r\n"), "a\0b");
    assert.equal(output("whitespace", " \t\na \r b"), " a b");
    assert.equal(output("unescape", "\\*\\a\\\\*\\"), "*\\a\\*\\");
    assert.equal(output("code", "\n \n"), "   ");
    assert.equal(output("code", "  a\nb  "), " a b ");
});
test("fresh search includes complete memo and terminal cursor, not just match", () => {
    const result = boundaryOracle(fixture("closer", "a```b``c`", { ticks: 2 }));
    assert.equal(result.result, 7);
    assert.equal(result.position, 7);
    assert.equal(result.cache[3], 1);
    assert.equal(result.cache[2], 5);
    assert.equal(result.cache[1], 0);
    assert.equal(result.scanned, 0);
    const miss = boundaryOracle(fixture("closer", "a```b", { ticks: 2 }));
    assert.equal(miss.position, 5);
    assert.equal(miss.scanned, 1);
    assert.equal(miss.result, 0);
});
test("admission rejects boundary-changing promises instead of weakening the theorem", () => {
    for (const op of ["trim", "whitespace"])
        for (const input of ["\v", "\f"]) assert.throws(() => admitBoundary(fixture(op, input)));
    for (const input of ["x\0y", "x\ry"])
        for (const op of ["code", "closer"]) assert.throws(() => admitBoundary(fixture(op, input)));
    for (const options of [{ ticks: 0 }, { ticks: 81 }, { start: -1 }, { start: 4 }, { start: 1 }])
        assert.throws(() => admitBoundary(fixture("closer", "``", options)));
    assert.throws(() => admitBoundary(fixture("invented", "a")));
});
test("every receipt and every state field is checked", () => {
    const f = fixture("closer", "a``", { ticks: 2 });
    const good = boundaryOracle(f);
    verifyBoundaryReceipt(f, JSON.stringify(good), 1);
    for (const field of ["position", "result", "scanned", "hex", "cache"]) {
        const bad = { ...good, [field]: null };
        assert.throws(() => verifyBoundaryReceipt(f, JSON.stringify(bad), 1));
    }
    assert.throws(() => verifyBoundaryReceipt(f, JSON.stringify(good), 2));
});
test("all fixtures obey domain; old proofs never inherit local certificates", () => {
    const fixtures = boundaryFixtures();
    assert.equal(new Set(fixtures.map((f) => f.id)).size, fixtures.length);
    fixtures.forEach((f) => boundaryOracle(f));
    assert.deepEqual(new Set(fixtures.filter((f) => f.measure).map((f) => f.operation)), new Set(boundaryOperations));
    const manifest = JSON.parse(
        fs.readFileSync(new URL("../../packages/markdown-core/benchmarks/corpus.json", import.meta.url))
    );
    const audit = boundaryPairAudit(manifest.pairs);
    assert.equal(audit.length, 43);
    assert.ok(audit.every((row) => row.full.status === "unproved" && row.local.residual));
});
test("measurement refuses missing edges and false positive call counts", () => {
    const dump =
        "events: Ir Dr Dw\nfn=main\ncfn=bench_effort_prepare\ncalls=1 0\n1 100 1 1\ncfn=bench_effort_operation\ncalls=16 0\n1 200 2 2\ncfn=bench_effort_release\ncalls=1 0\n1 50 1 1\n";
    assert.equal(readBoundaryEdges(dump, 16).operation.cost.Ir, 200);
    assert.throws(() => readBoundaryEdges(dump, 15));
    assert.throws(() => readBoundaryEdges(dump.replace("bench_effort_release", "other"), 16));
    assert.throws(() => readBoundaryEdges(dump.replace("200 2 2", "0 2 2"), 16));
});
test("unmeasured report cannot manufacture an effort ratio", () => {
    const text = boundaryMarkdown({
        identity: "test",
        checkedFixtures: 1,
        cases: [{ id: "x", engines: {} }],
        pairs: []
    });
    assert.match(text, /Full-parser certificates: 0/u);
    assert.doesNotMatch(text, /\d\.\d{3}x/u);
});

test("boundary split is lossless and keeps every incompatible byte as residual", () => {
    for (const text of ["", "a", "\v\f", "\va\f", "a\vb\fc"]) {
        const input = Buffer.from(text);
        const segments = splitWhitespaceBoundary(input);
        assert.deepEqual(Buffer.concat(segments.map((s) => s.input)), input);
        let offset = 0;
        for (const segment of segments) {
            assert.equal(segment.start, offset);
            assert.equal(segment.end - segment.start, segment.input.length);
            if (segment.kind === "matched")
                for (const operation of ["trim", "whitespace"]) admitBoundary({ operation, input: segment.input });
            else assert.ok(segment.input.length === 1 && [11, 12].includes(segment.input[0]));
            offset = segment.end;
        }
    }
});

test("ownership boundary covers every proof at two scales and rejects cycles or extra roots", () => {
    const rows = ownershipFixtures();
    assert.equal(rows.length, 86);
    assert.equal(new Set(rows.map((r) => r.proof)).size, 43);
    for (const parents of [[0], [0xffffffff, 1], [0xffffffff, 0xffffffff], [0xffffffff, 2, 0]])
        assert.throws(() => admitBoundary({ operation: "owners", input: encodeParents(parents) }));
    const result = boundaryOracle({ operation: "owners", input: encodeParents([0xffffffff, 0, 0, 1]) });
    const words = Buffer.from(result.hex, "hex");
    assert.equal(words.readUInt32LE(12), 1); // root first
    assert.equal(words.readUInt32LE(16), 2); // root last
    assert.equal(words.readUInt32LE(28), 2); // owner 1 next
    assert.equal(words.readUInt32LE(32), 3); // owner 1 first
    assert.equal(words.readUInt32LE(44), 1); // owner 2 previous
});
