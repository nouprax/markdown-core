import assert from "node:assert/strict";
import test from "node:test";

import { callEdge, calleesOf, callersOf, costRecord, parseCallgrind } from "../callgrind.mjs";

/**
 * A dump exercising every piece of the format the stage benchmark depends on:
 * name compression in three separate id namespaces, a function whose id
 * collides with a file's id, two callers of one callee, and call records whose
 * cost line is inclusive rather than self cost.
 */
const DUMP = `version: 1
creator: callgrind-3.22.0
pid: 1234
cmd:  ./runner --document corpus.md
part: 1
desc: Trigger: Program termination
positions: line
events: Ir Dr Dw

ob=(1) /tmp/runner
fl=(1) blocks.c
fn=(1) parse_document
10 40 10 5
cfn=(2) read_source
calls=1 100
10 900 300 100
cfn=(3) build_tree
calls=1 200
+2 1500 400 250

fl=(2) inlines.c
fn=(2) read_source
100 700 250 80
cfn=(4) scan_line
calls=7 300
* 200 50 20

fn=(3) build_tree
200 300 90 60
cfn=(2)
calls=2 100
+1 600 120 90
cfn=(5) consolidate
calls=1 400
-3 600 190 100

fn=(4) scan_line
300 200 50 20

fn=(5) consolidate
400 600 190 100

summary: 4000 1000 500
totals: 4000 1000 500
`;

test("the parser reads events, summary and totals", () => {
    const profile = parseCallgrind(DUMP);
    assert.deepEqual(profile.events, ["Ir", "Dr", "Dw"]);
    assert.deepEqual(profile.summary, [4000, 1000, 500]);
    assert.deepEqual(profile.totals, [4000, 1000, 500]);
});

test("a call record's cost is the edge's, not the caller's self cost", () => {
    const profile = parseCallgrind(DUMP);
    assert.deepEqual(profile.self.get("parse_document"), [40, 10, 5]);
    assert.deepEqual(callEdge(profile, "parse_document", "read_source").cost, [900, 300, 100]);
    assert.deepEqual(callEdge(profile, "parse_document", "build_tree").cost, [1500, 400, 250]);
});

test("function ids are compressed in their own namespace", () => {
    // `fl=(2)` is inlines.c and `fn=(2)` is read_source: one shared table would
    // decode the callee of build_tree as a file name.
    const profile = parseCallgrind(DUMP);
    const nested = callEdge(profile, "build_tree", "read_source");
    assert.equal(nested.calls, 2);
    assert.deepEqual(nested.cost, [600, 120, 90]);
});

test("a callee reached from two stages keeps the edges apart", () => {
    const profile = parseCallgrind(DUMP);
    const callers = callersOf(profile, "read_source")
        .map((edge) => edge.caller)
        .sort();
    assert.deepEqual(callers, ["build_tree", "parse_document"]);
});

test("relative and wildcard positions do not become costs", () => {
    const profile = parseCallgrind(DUMP);
    // `* 200 50 20` is a self-cost line at the previous position.
    assert.deepEqual(callEdge(profile, "read_source", "scan_line").cost, [200, 50, 20]);
    assert.equal(callEdge(profile, "read_source", "scan_line").calls, 7);
    assert.deepEqual(callEdge(profile, "build_tree", "consolidate").cost, [600, 190, 100]);
});

test("callees are enumerated for a stage breakdown", () => {
    const profile = parseCallgrind(DUMP);
    const callees = calleesOf(profile, "build_tree")
        .map((edge) => edge.callee)
        .sort();
    assert.deepEqual(callees, ["consolidate", "read_source"]);
});

test("costs are named by the declared events", () => {
    const profile = parseCallgrind(DUMP);
    const edge = callEdge(profile, "parse_document", "read_source");
    assert.deepEqual(costRecord(profile, edge.cost), { Ir: 900, Dr: 300, Dw: 100 });
});

test("an undefined name id is rejected rather than silently renamed", () => {
    assert.throws(() => parseCallgrind("events: Ir\nfn=(9)\n1 2\n"), /reference to undefined name id \(9\)/u);
});

test("a cost line shorter than the event list counts the rest as zero", () => {
    const profile = parseCallgrind("events: Ir Dr Dw\nfn=solo\n1 42\n");
    assert.deepEqual(costRecord(profile, profile.self.get("solo")), { Ir: 42, Dr: 0, Dw: 0 });
});

test("source attribution follows inline files without assigning callee costs to callers", () => {
    const profile = parseCallgrind(`events: Ir Dr Dw
fl=(1) node.c
fn=(1) make_node
1 10 2 3
fi=(2) inline.h
2 20 4 5
cfi=(3) allocator.c
cfn=(2) allocate
calls=1 10
* 100 30 40
* 30 6 7
fe=(1)
3 40 8 9
fl=(3)
fn=(2)
10 100 30 40
`);
    assert.deepEqual(profile.selfByFile.get("node.c"), [50, 10, 12]);
    assert.deepEqual(profile.selfByFile.get("inline.h"), [50, 10, 12]);
    assert.deepEqual(profile.selfByFile.get("allocator.c"), [100, 30, 40]);
    assert.equal(
        [...profile.selfByFile.values()].reduce((sum, cost) => sum + cost[0], 0),
        200
    );
    assert.equal(
        [...profile.self.values()].reduce((sum, cost) => sum + cost[0], 0),
        200
    );
});
