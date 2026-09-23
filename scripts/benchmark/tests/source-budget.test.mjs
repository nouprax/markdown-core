import assert from "node:assert/strict";
import test from "node:test";
import { sourceBudget } from "../source-budget.mjs";

const row = (name, ir, sha256 = "same-input") => ({
    case: name,
    bytes: 100,
    sha256,
    engines: { "markdown-core": { stages: { source_to_buffer: { cost: { Ir: ir } } } } }
});

test("one source regression fails even when the aggregate improves", () => {
    const result = sourceBudget([row("a", 103), row("b", 50)], [row("a", 100), row("b", 100)]);
    assert.deepEqual(
        result.map((entry) => entry.passed),
        [false, true]
    );
});
test("the source budget refuses mismatched, missing and duplicate workloads", () => {
    assert.throws(() => sourceBudget([row("a", 100, "changed")], [row("a", 100)]), /input mismatch/u);
    assert.throws(() => sourceBudget([], [row("a", 100)]), /equal, unique/u);
    assert.throws(() => sourceBudget([], []), /equal, unique/u);
    assert.throws(
        () => sourceBudget([row("a", 100), row("a", 100)], [row("a", 100), row("b", 100)]),
        /input mismatch/u
    );
    assert.throws(() => sourceBudget([row("a", 100), row("b", 100)], [row("a", 100), row("a", 100)]), /equal, unique/u);
});
test("a missing or invalid stage measurement never passes", () => {
    for (const value of [0, -1, NaN, Infinity, 1.5, undefined]) {
        assert.throws(() => sourceBudget([row("a", value)], [row("a", 100)]), /positive instruction/u);
    }
});
