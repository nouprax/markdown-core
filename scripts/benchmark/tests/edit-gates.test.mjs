import assert from "node:assert/strict";
import test from "node:test";

import { parseCallgrind } from "../callgrind.mjs";
import {
    flatSteps,
    flatStream,
    neverWorse,
    regressions,
    summary,
    windowCost,
    windowEnds,
    WINDOWS
} from "../edit-gates.mjs";

/* A synthetic per-window dump, in callgrind's own format: the runner's main
 * calling the step entry once with the given inclusive cost. */
const window = (ir, { calls = 1, entry = "bench_apply_step'main" } = {}) =>
    parseCallgrind(
        [
            "version: 1",
            "events: Ir Dr Dw",
            "fl=(1) edit_runner.c",
            "fn=(1) main'(below main)",
            "0 40 4 2",
            `cfn=(2) ${entry}`,
            `calls=${calls} 0`,
            `0 ${ir} ${ir / 4} ${ir / 8}`,
            "fn=(2)",
            `0 ${ir} ${ir / 4} ${ir / 8}`,
            ""
        ].join("\n")
    );
const costs = (values) => values.map((ir) => windowCost(window(ir)).Ir);

test("a window's cost is the inclusive cost of its calls into the step entry", () => {
    assert.deepEqual(windowCost(window(8000)), { calls: 1, Ir: 8000, Dr: 2000, Dw: 1000 });
    assert.deepEqual(windowCost(window(8000, { calls: 3 })), { calls: 3, Ir: 8000, Dr: 2000, Dw: 1000 });
});

test("a window that never entered the step, or counted nothing, is refused", () => {
    assert.throws(() => windowCost(window(8000, { entry: "markdown_core_document_parse'main" })), /no call/);
    assert.throws(() => windowCost(window(0)), /positive integer/);
});

test("windows split long scripts into 1,024 contiguous windows of near-equal step count", () => {
    assert.deepEqual(windowEnds(3), [1, 2, 3]);
    const ends = windowEnds(4115);
    assert.equal(ends.length, WINDOWS);
    assert.equal(ends.at(-1), 4115);
    const widths = ends.map((end, index) => end - (index ? ends[index - 1] : 0));
    assert.ok(Math.max(...widths) - Math.min(...widths) <= 1);
});

test("a summary reports nearest-rank percentiles, the maximum and the total", () => {
    const values = Array.from({ length: 100 }, (_, index) => index + 1);
    assert.deepEqual(summary(values), { p50: 50, p95: 95, max: 100, total: 5050 });
    assert.deepEqual(summary([7]), { p50: 7, p95: 7, max: 7, total: 7 });
    assert.throws(() => summary([]));
});

test("6.2 rejects one step whose cost grows with the document, however few steps do", () => {
    const flat = costs([100, 100, 100, 100]);
    assert.deepEqual(flatSteps({ 16384: flat, 65536: flat, 262144: flat, 1048576: costs([100, 100, 125, 100]) }), []);
    const violations = flatSteps({
        16384: flat,
        65536: flat,
        262144: flat,
        1048576: costs([100, 100, 126, 100])
    });
    assert.deepEqual(
        violations.map((entry) => [entry.size, entry.step]),
        [[1048576, 2]]
    );
    assert.throws(() => flatSteps({ 16384: flat, 65536: costs([100]) }), /step counts differ/);
});

test("6.2 rejects stream windows whose cost grows with the text already streamed", () => {
    const length = 1024 * 64;
    const stream = (cost) => windowEnds(1024).map((end, index) => ({ end: (end * length) / 1024, Ir: cost(index) }));
    assert.deepEqual(
        flatStream(
            stream(() => 100),
            length
        ),
        []
    );
    /* A linear term in the text already streamed. */
    const linear = flatStream(
        stream((index) => 100 + index),
        length
    );
    assert.deepEqual(
        linear.map((entry) => entry.point),
        [length / 4, length]
    );
    /* One late spike fails m(n) alone. */
    const spike = flatStream(
        stream((index) => (index === 900 ? 200 : 100)),
        length
    );
    assert.deepEqual(
        spike.map((entry) => entry.point),
        [length]
    );
});

test("6.3 rejects a step above 1.25 times reparse on scripts of at most 1,024 steps", () => {
    assert.deepEqual(neverWorse(costs([125, 10]), costs([100, 100])), []);
    assert.deepEqual(
        neverWorse(costs([126, 10]), costs([100, 100])).map((entry) => entry.step),
        [0]
    );
    const long = Array.from({ length: WINDOWS + 1 }, () => 1000);
    assert.deepEqual(
        neverWorse(
            long,
            long.map(() => 1)
        ),
        []
    );
    assert.throws(() => neverWorse([1], [1, 2]), /different step counts/);
});

test("6.4 rejects a p95, maximum or total above 1.02 times the base", () => {
    const base = summary(costs([100, 100, 100, 100]));
    assert.deepEqual(regressions(summary(costs([102, 102, 102, 102])), base), []);
    assert.deepEqual(
        regressions(summary(costs([103, 103, 103, 103])), base).map((entry) => entry.metric),
        ["p95", "max", "total"]
    );
    assert.deepEqual(
        regressions(summary(costs([100, 100, 100, 103])), base).map((entry) => entry.metric),
        ["p95", "max"]
    );
    /* Cheap steps that grew move neither the p95 nor the maximum. */
    const mixed = (cheap) => [...Array(50).fill(cheap), ...Array(50).fill(100)];
    assert.deepEqual(
        regressions(summary(mixed(20)), summary(mixed(10))).map((entry) => entry.metric),
        ["total"]
    );
});
