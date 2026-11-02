import assert from "node:assert/strict";
import test from "node:test";

import { parseCallgrind } from "../callgrind.mjs";
import {
    BUDGET,
    budgetOf,
    costliest,
    editsOf,
    groupRequests,
    LARGE_EDITS,
    LARGE_REQUESTS,
    markdownReport,
    measureOf,
    requestCost,
    REQUESTS,
    requestsOf
} from "../completion.mjs";

/* One request's dump: the runner's loop calls bench_request once, which calls
 * the edit and the completion; call lines carry inclusive cost. */
const dump = (calls = 1) => `version: 1
creator: callgrind-3.22.0
events: Ir

fl=(1) completion_runner.c
fn=(1) run_script
10 5
cfn=(2) bench_request
calls=${calls} 87
10 1000

fn=(2) bench_request
87 10
cfn=(3) markdown_core_lsp_update
calls=1 1
88 600
cfn=(4) markdown_core_lsp_complete
calls=1 1
89 390

fl=(2) server.c
fn=(3) markdown_core_lsp_update
1 600
fn=(4) markdown_core_lsp_complete
1 390

totals: 1005
`;

test("a request's cost is the edge into bench_request, split into its edit and completion", () => {
    assert.deepEqual(requestCost(parseCallgrind(dump())), { Ir: 1000, edit: 600, complete: 390 });
});

test("a dump without exactly one request is refused rather than counted", () => {
    assert.throws(() => requestCost(parseCallgrind(dump(2))), /2 calls/);
    const empty = parseCallgrind(`version: 1\nevents: Ir\n\nfl=(1) a.c\nfn=(1) main\n1 5\n\ntotals: 5\n`);
    assert.throws(() => requestCost(empty), /0 calls/);
});

test("large shapes are cut to their first edits, smaller documents measured over every edit", () => {
    assert.deepEqual(measureOf({ size: null }), { most: REQUESTS, edits: null });
    assert.deepEqual(measureOf({ size: 16384 }), { most: REQUESTS, edits: null });
    assert.deepEqual(measureOf({ size: 65536 }), { most: LARGE_REQUESTS, edits: LARGE_EDITS });
    assert.equal(editsOf(64, measureOf({ size: 262144 })), LARGE_EDITS);
    assert.equal(editsOf(3, measureOf({ size: 262144 })), 3);
    assert.equal(editsOf(64, measureOf({ size: null })), 64);
    assert.equal(requestsOf(5, REQUESTS), 5);
    assert.equal(requestsOf(64, REQUESTS), REQUESTS);
});

test("the budget holds the grammar corpus to the first entry and stops at 256 KiB", () => {
    assert.equal(budgetOf(null), BUDGET[0].milliseconds);
    assert.equal(budgetOf(16384), 1);
    assert.equal(budgetOf(65536), 10);
    assert.equal(budgetOf(262144), 10);
    assert.equal(budgetOf(1048576), null);
});

const script = (name, requests, skipped = false) => ({
    name,
    family: "typing",
    skipped,
    edits: requests.length,
    bytes: 100,
    requests,
    edit: requests.map((ir) => ir - 10),
    complete: requests.map(() => 10)
});
const rows = [
    { name: "b.edits", size: null, scripts: [script("typing-word", [30, 10, 20]), script("far", [], true)] },
    { name: "prose-ascii-256k.edits", size: 262144, scripts: [script("typing-word", [500, 700])] },
    { name: "a.edits", size: null, scripts: [script("typing-word", [40])] }
];

test("groups pool every request of their documents' size, smallest first", () => {
    const groups = groupRequests(rows);
    assert.deepEqual(
        groups.map((group) => [group.key, group.budget, group.workloads, group.scripts, group.skipped, group.requests]),
        [
            ["grammar", 1, 2, 3, 1, 4],
            ["shapes-262144", 10, 1, 1, 0, 2]
        ]
    );
    assert.deepEqual(groups[0].ir, { p50: 20, p95: 40, max: 40, total: 100 });
    assert.deepEqual(groups[0].edit, { p50: 10, p95: 30, max: 30, total: 60 });
    assert.deepEqual(groups[1].complete, { p50: 10, p95: 10, max: 10, total: 20 });
});

test("the costliest requests name where they ran, and the report renders every group", () => {
    const top = costliest(rows, 2);
    assert.deepEqual(
        top.map((entry) => [entry.workload, entry.request, entry.Ir]),
        [
            ["prose-ascii-256k.edits", 2, 700],
            ["prose-ascii-256k.edits", 1, 500]
        ]
    );
    const markdown = markdownReport({
        commit: "a".repeat(40),
        requests: { most: REQUESTS, large: LARGE_REQUESTS, edits: LARGE_EDITS },
        toolchain: { compiler: "cc", valgrind: "valgrind-3.22.0" },
        binaries: { "markdown-core completion": { sha256: "f".repeat(64) } },
        workloads: { digest: "e".repeat(64), count: 3 },
        groups: groupRequests(rows),
        costliest: top
    });
    assert.match(markdown, /\| Grammar corpus \| 1 ms \| 2 \| 3 \| 1 \| 4 \| 20 \| 40 \| 40 \| 30 \| 10 \|/);
    assert.match(markdown, /\| Shapes, 256 KiB \| 10 ms \|/);
    assert.match(markdown, /\| prose-ascii-256k\.edits \| typing-word \| 2 \| 100 \| 700 \| 690 \| 10 \|/);
});
