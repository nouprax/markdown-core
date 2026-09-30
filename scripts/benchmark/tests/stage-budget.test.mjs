import assert from "node:assert/strict";
import test from "node:test";
import { allowance, STAGE_IR_LIMIT, STAGE_IR_LIMITS, STAGES, stageBudget } from "../stage-budget.mjs";

const row = (name, source, ast = 100, sha256 = "same-input") => ({
    case: name,
    bytes: 100,
    sha256,
    engines: {
        "markdown-core": {
            stages: { source_to_buffer: { cost: { Ir: source } }, buffer_to_ast: { cost: { Ir: ast } } }
        }
    }
});
const verdicts = (rows) => rows.map((entry) => `${entry.case} ${entry.stage} ${entry.passed}`);

test("the budget covers both parse stages", () => {
    assert.deepEqual(STAGES, ["source_to_buffer", "buffer_to_ast"]);
});
test("one source regression fails even when the aggregate improves", () => {
    assert.deepEqual(verdicts(stageBudget([row("a", 103, 50), row("b", 50, 50)], [row("a", 100), row("b", 100)])), [
        "a source_to_buffer false",
        "a buffer_to_ast true",
        "b source_to_buffer true",
        "b buffer_to_ast true"
    ]);
});
test("every stage has a limit, and source_to_buffer holds the standing rule", () => {
    assert.deepEqual(Object.keys(STAGE_IR_LIMITS), STAGES);
    assert.equal(STAGE_IR_LIMITS.source_to_buffer, STAGE_IR_LIMIT);
    assert.equal(allowance(STAGE_IR_LIMIT), "+2%");
});
test("one AST regression fails even when the source stage and the aggregate improve", () => {
    assert.deepEqual(verdicts(stageBudget([row("a", 50, 111), row("b", 50, 50)], [row("a", 100), row("b", 100)])), [
        "a source_to_buffer true",
        "a buffer_to_ast false",
        "b source_to_buffer true",
        "b buffer_to_ast true"
    ]);
});
test("each stage is held to its own limit, and one exactly at it passes", () => {
    const at = (limit) => Math.round(1000 * limit);
    const exact = row("a", at(STAGE_IR_LIMITS.source_to_buffer), at(STAGE_IR_LIMITS.buffer_to_ast));
    assert.ok(stageBudget([exact], [row("a", 1000, 1000)]).every((entry) => entry.passed));
    const over = row("a", at(STAGE_IR_LIMITS.buffer_to_ast), at(STAGE_IR_LIMITS.buffer_to_ast) + 1);
    assert.deepEqual(verdicts(stageBudget([over], [row("a", 1000, 1000)])), [
        "a source_to_buffer false",
        "a buffer_to_ast false"
    ]);
});
test("the stage budget refuses mismatched, missing and duplicate workloads", () => {
    assert.throws(() => stageBudget([row("a", 100, 100, "changed")], [row("a", 100)]), /input mismatch/u);
    assert.throws(() => stageBudget([], [row("a", 100)]), /equal, unique/u);
    assert.throws(() => stageBudget([], []), /equal, unique/u);
    assert.throws(() => stageBudget([row("a", 100), row("a", 100)], [row("a", 100), row("b", 100)]), /input mismatch/u);
    assert.throws(() => stageBudget([row("a", 100), row("b", 100)], [row("a", 100), row("a", 100)]), /equal, unique/u);
});
test("a missing or invalid stage measurement never passes", () => {
    for (const value of [0, -1, NaN, Infinity, 1.5, undefined]) {
        assert.throws(() => stageBudget([row("a", value)], [row("a", 100)]), /positive instruction/u);
        const ast = row("a", 100);
        ast.engines["markdown-core"].stages.buffer_to_ast.cost.Ir = value;
        assert.throws(() => stageBudget([ast], [row("a", 100)]), /positive instruction/u);
    }
});
