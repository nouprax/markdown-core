import assert from "node:assert/strict";
import test from "node:test";

import { parseCallgrind } from "../callgrind.mjs";
import {
    bindingCost,
    everyStep,
    flatStream,
    groupWindows,
    neverWorse,
    parseCost,
    regressions,
    stepCost,
    summary,
    windowEnds,
    WINDOWS
} from "../stream-gates.mjs";

/* A synthetic per-window dump in callgrind's own format: each function with
 * its self cost, the calls it makes and how often it is called, a call's
 * cost being its callee's inclusive cost. Every count is Ir, with Dr a
 * quarter and Dw an eighth. */
const dump = (functions) => {
    const inclusive = (name) =>
        functions[name].self + (functions[name].calls ?? []).reduce((sum, callee) => sum + inclusive(callee), 0);
    const ids = new Map();
    const id = (name) => {
        if (ids.has(name)) return `(${ids.get(name)})`;
        ids.set(name, ids.size + 1);
        return `(${ids.size}) ${name}`;
    };
    const counts = (ir) => `0 ${ir} ${ir / 4} ${ir / 8}`;
    const lines = ["version: 1", "events: Ir Dr Dw", "fl=(1) stream_runner.c"];
    for (const [name, { self, calls = [] }] of Object.entries(functions)) {
        lines.push(`fn=${id(name)}`, counts(self));
        for (const callee of calls) {
            lines.push(`cfn=${id(callee)}`, `calls=${functions[callee].times ?? 1} 0`, counts(inclusive(callee)));
        }
    }
    return parseCallgrind([...lines, ""].join("\n"));
};
/* A session window: the runner's main calling the step entry. */
const window = (ir, { calls = 1, entry = "bench_apply_step'main" } = {}) =>
    dump({ "main'(below main)": { self: 40, calls: [entry] }, [entry]: { self: ir, times: calls } });
/* A `reparse` window: the step calls `Document.parse`, which creates the
 * parser, starts the transaction, enters the two parse stages -- the second
 * reaching the first again for mapped content -- and releases; the step then
 * frees the document of the step before. */
const BOUNDARIES = ["S_parse_source", "S_finish_parse"];
const reparse = ({ entry = "bench_apply_step'main", parse = true } = {}) =>
    dump({
        "main'(below main)": { self: 40, calls: [entry] },
        [entry]: { self: 160, calls: ["markdown_core_document_parse_in", "markdown_core_document_free"] },
        markdown_core_document_parse_in: {
            self: 80,
            calls: [
                "markdown_core_core_parser",
                "markdown_core_parser_start",
                "markdown_core_parser_commit",
                "markdown_core_parser_finalize"
            ]
        },
        markdown_core_core_parser: { self: 2000 },
        markdown_core_parser_start: { self: 400 },
        markdown_core_parser_commit: { self: 16, calls: parse ? ["S_parse_source'markdown_core_parser_commit"] : [] },
        "S_parse_source'markdown_core_parser_commit": { self: 3000 },
        markdown_core_parser_finalize: {
            self: 24,
            calls: [...(parse ? ["S_finish_parse.constprop.0'markdown_core_parser_finalize"] : []), "S_release_parse"]
        },
        "S_finish_parse.constprop.0'markdown_core_parser_finalize": {
            self: 1200,
            calls: ["S_parse_source'S_finish_parse.constprop.0"]
        },
        "S_parse_source'S_finish_parse.constprop.0": { self: 400 },
        S_release_parse: { self: 320 },
        markdown_core_document_free: { self: 800 }
    });
/* A `binding` window: the step enters the subject, whose MCB3 entry runs
 * the session's step and encodes the document it published; the subject
 * then releases the message and reads the document. */
const binding = ({
    entry = "bench_apply_step'main",
    wire = "markdown_core_wire_session_append",
    session = "markdown_core_session_append",
    step = true
} = {}) =>
    dump({
        "main'(below main)": { self: 40, calls: [entry] },
        [entry]: { self: 32, calls: [`binding_append'${entry}`] },
        [`binding_append'${entry}`]: {
            self: 24,
            calls: [
                `${wire}'binding_append`,
                "markdown_core_wire_free'binding_append",
                "session_document'binding_append"
            ]
        },
        [`${wire}'binding_append`]: {
            self: 16,
            calls: [...(step ? [`${session}'${wire}`] : []), `document_bytes'${wire}`]
        },
        [`${session}'${wire}`]: { self: 9000 },
        [`document_bytes'${wire}`]: { self: 2400 },
        "markdown_core_wire_free'binding_append": { self: 8, calls: ["free'markdown_core_wire_free"] },
        "free'markdown_core_wire_free": { self: 80 },
        "session_document'binding_append": { self: 8 }
    });
const costs = (values) => values.map((ir) => stepCost(window(ir)).Ir);

test("a session window's cost is the inclusive cost of its calls into a step entry", () => {
    assert.deepEqual(stepCost(window(8000)), { calls: 1, Ir: 8000, Dr: 2000, Dw: 1000 });
    assert.deepEqual(stepCost(window(8000, { calls: 3 })), { calls: 3, Ir: 8000, Dr: 2000, Dw: 1000 });
    assert.deepEqual(stepCost(window(8000, { entry: "bench_flush'main" })), {
        calls: 1,
        Ir: 8000,
        Dr: 2000,
        Dw: 1000
    });
});

test("a reparse window counts the two parse stages of its Document.parse and nothing around them", () => {
    /* 3,000 reading the source and 1,600 building the tree, the nested read
     * included once; creation, start, release and the free are left out. */
    assert.deepEqual(parseCost(reparse(), BOUNDARIES), { calls: 1, Ir: 4600, Dr: 1150, Dw: 575 });
    assert.deepEqual(parseCost(reparse({ entry: "bench_flush'main" }), BOUNDARIES), {
        calls: 1,
        Ir: 4600,
        Dr: 1150,
        Dw: 575
    });
});

test("a binding window counts its messages' encode and release, and neither the session's step nor the subject", () => {
    /* 16 in the entry, 2,400 encoding and 88 releasing the message. */
    assert.deepEqual(bindingCost(binding()), { calls: 1, Ir: 2504, Dr: 626, Dw: 313 });
    assert.deepEqual(
        bindingCost(
            binding({
                entry: "bench_flush'main",
                wire: "markdown_core_wire_session_flush",
                session: "markdown_core_session_flush"
            })
        ),
        { calls: 1, Ir: 2504, Dr: 626, Dw: 313 }
    );
});

test("a window that never entered the step, never parsed, or counted nothing, is refused", () => {
    assert.throws(() => stepCost(window(8000, { entry: "markdown_core_document_parse'main" })), /no call/);
    assert.throws(() => stepCost(window(0)), /positive integer/);
    assert.throws(() => parseCost(reparse({ entry: "markdown_core_document_parse'main" }), BOUNDARIES), /no call/);
    assert.throws(() => parseCost(reparse({ parse: false }), BOUNDARIES), /exactly one function/);
    assert.throws(() => bindingCost(binding({ entry: "markdown_core_document_parse'main" })), /no call/);
    assert.throws(() => bindingCost(binding({ step: false })), /no session step/);
});

test("windows split long streams into 1,024 contiguous windows of near-equal step count", () => {
    assert.deepEqual(windowEnds(3, false), [1, 2, 3]);
    const ends = windowEnds(4115, false);
    assert.equal(ends.length, WINDOWS);
    assert.equal(ends.at(-1), 4115);
    const widths = ends.map((end, index) => end - (index ? ends[index - 1] : 0));
    assert.ok(Math.max(...widths) - Math.min(...widths) <= 1);
});

test("a stepwise stream has one window per step however long it is", () => {
    assert.deepEqual(windowEnds(3, true), [1, 2, 3]);
    assert.deepEqual(
        windowEnds(4115, true),
        Array.from({ length: 4115 }, (_, index) => index + 1)
    );
});

test("a summary reports nearest-rank percentiles, the maximum and the total", () => {
    const values = Array.from({ length: 100 }, (_, index) => index + 1);
    assert.deepEqual(summary(values), { p50: 50, p95: 95, max: 100, total: 5050 });
    assert.deepEqual(summary([7]), { p50: 7, p95: 7, max: 7, total: 7 });
    assert.throws(() => summary([]));
});

test("6.2 rejects stream windows whose cost grows with the text already streamed", () => {
    const length = 1024 * 64;
    const stream = (cost) =>
        windowEnds(1024, false).map((end, index) => ({ end: (end * length) / 1024, Ir: cost(index) }));
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

test("S / R and B / S come from the streams 6.3 measures, and a long stream's S joins its group's column", () => {
    const row = (steps, windows, session, binding) => ({
        steps,
        windows,
        session: { windows: session },
        binding: binding && { windows: binding },
        oneshot: { ir: 1 }
    });
    const [group] = groupWindows(
        [row(2, [10, 10, 10], [11, 12, 10], [3, 3, 3]), row(WINDOWS + 1, [10, 10], [500, 900], null)],
        () => "all"
    );
    assert.equal(group.totalRatio, 33 / 30);
    assert.equal(group.stepRatio, 1.2);
    assert.equal(group.bindingRatio, 9 / 33);
    assert.equal(group.session.max, 900);
    const [long] = groupWindows([row(WINDOWS + 1, [10, 10], [500, 900], null)], () => "long");
    assert.equal(long.totalRatio, null);
    assert.equal(long.stepRatio, null);
    assert.equal(long.bindingRatio, null);
    assert.equal(long.session.total, 1400);
    /* A stepwise stream's R ran every step, so 6.3 measures it however long it is. */
    const every = Array.from({ length: WINDOWS + 2 }, () => 10);
    const [stepwise] = groupWindows(
        [
            row(
                WINDOWS + 1,
                every,
                every.map((cost, step) => (step ? cost : 30)),
                null
            )
        ],
        () => "stepwise"
    );
    assert.equal(stepwise.totalRatio, (10 * (WINDOWS + 1) + 30) / (10 * (WINDOWS + 2)));
    assert.equal(stepwise.stepRatio, 3);
    assert.equal(stepwise.bindingRatio, null);
});

test("a row's windows are its steps when its R ran every step and the flush", () => {
    assert.equal(everyStep({ steps: 2, windows: [1, 1, 1] }), true);
    assert.equal(everyStep({ steps: WINDOWS + 1, windows: Array.from({ length: WINDOWS + 1 }, () => 1) }), false);
});

test("6.3 rejects a stream whose total, the flush included, is above 1.25 times reparse's", () => {
    assert.deepEqual(neverWorse(costs([125, 125]), costs([100, 100])), []);
    /* A step above the margin is the stream's to pay for, not a failure. */
    assert.deepEqual(neverWorse(costs([200, 50]), costs([100, 100])), []);
    assert.deepEqual(
        neverWorse(costs([126, 125]), costs([100, 100])).map((entry) => entry.ratio),
        [1.255]
    );
    /* The flush is the last window: a stream of 1,024 chunks has 1,025. */
    const full = Array.from({ length: WINDOWS + 1 }, () => 100);
    const room = 1.25 * 100 * (WINDOWS + 1) - 100 * WINDOWS;
    assert.deepEqual(neverWorse([...full.slice(0, -1), room], full), []);
    assert.equal(neverWorse([...full.slice(0, -1), room + 1], full).length, 1);
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
