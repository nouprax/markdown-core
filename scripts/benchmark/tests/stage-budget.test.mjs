import assert from "node:assert/strict";
import test from "node:test";
import { ONESHOT_IR_LIMIT, STAGES, stageBudget } from "../stage-budget.mjs";

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
/* One instruction past the limit on a base of 1000. */
const over = Math.round(1000 * ONESHOT_IR_LIMIT) + 1;
const verdicts = (rows) => rows.map((entry) => `${entry.case} ${entry.stage} ${entry.passed}`);

test("the budget covers both parse stages", () => {
    assert.deepEqual(STAGES, ["source_to_buffer", "buffer_to_ast"]);
});
test("one source regression fails even when the aggregate improves", () => {
    assert.deepEqual(
        verdicts(stageBudget([row("a", over, 500), row("b", 500, 500)], [row("a", 1000, 1000), row("b", 1000, 1000)])),
        ["a source_to_buffer false", "a buffer_to_ast true", "b source_to_buffer true", "b buffer_to_ast true"]
    );
});
test("one AST regression fails even when the source stage and the aggregate improve", () => {
    assert.deepEqual(
        verdicts(stageBudget([row("a", 500, over), row("b", 500, 500)], [row("a", 1000, 1000), row("b", 1000, 1000)])),
        ["a source_to_buffer true", "a buffer_to_ast false", "b source_to_buffer true", "b buffer_to_ast true"]
    );
});
test("a stage exactly at the limit passes", () => {
    const at = Math.round(1000 * ONESHOT_IR_LIMIT);
    assert.ok(stageBudget([row("a", at, at)], [row("a", 1000, 1000)]).every((entry) => entry.passed));
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
