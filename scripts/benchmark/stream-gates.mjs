/**
 * THE STREAM GATES' ARITHMETIC (docs/plans/2026-09-29-incremental-gates.md,
 * sections 5 and 6), separate from the driver so that each rule is tested on
 * synthetic profiles without building or measuring anything.
 *
 * A workload's costs are its windows' costs, read from each of the runner's
 * per-window dumps: a session's window is the whole of its calls into
 * `bench_apply_step` and `bench_flush`, a `reparse` window is the parse
 * inside them, and a `binding` window is what its messages add to the
 * session's steps. Every rule compares Ir; data references are reported
 * beside it.
 */

import { baseName, costRecord, foldClones, stageCalls } from "./callgrind.mjs";
import { STAGE_IR_LIMIT } from "./stage-budget.mjs";

/** The steps' entries, as stream_runner.c names them: a chunk and the flush. */
export const STEP_ENTRIES = Object.freeze(["bench_apply_step", "bench_flush"]);
/**
 * The MCB3 entries the `binding` subject steps its session through, and the
 * release of each message (wire/markdown_core_wire.h).
 */
export const BINDING_ENTRIES = Object.freeze([
    "markdown_core_wire_session_append",
    "markdown_core_wire_session_flush",
    "markdown_core_wire_free"
]);
/** The session's own steps inside those entries: the S column's work. */
export const SESSION_STEPS = Object.freeze(["markdown_core_session_append", "markdown_core_session_flush"]);
/**
 * The names a base revision from before the rename may give those entries
 * (incremental_harness.h, THE BASE'S NAMES); only the base's build answers
 * to them. Gone once no base predates the rename.
 */
export const FORMER_NAMES = Object.freeze({
    markdown_core_session_flush: "markdown_core_session_finalize",
    markdown_core_wire_session_flush: "markdown_core_wire_session_finalize"
});
/** The most windows a stream 6.3 does not measure at every step is split into (5.1). */
export const WINDOWS = 1024;
/** 6.2: a later window of a stream against an earlier one. */
export const FLATNESS_LIMIT = 1.25;
/** 6.3: a subject's stream total against `reparse`'s on the same steps. */
export const REPARSE_LIMIT = 1.25;
/** 6.4: the same rule the one-shot gate applies per document and stage. */
export const REGRESSION_LIMIT = STAGE_IR_LIMIT;

/**
 * A window's calls into a step entry, whatever their callers' contexts. A
 * window that made none is a runner that measured nothing, and is refused
 * rather than counted as free.
 */
function stepEdges(profile) {
    const edges = [...profile.edges.values()].filter((edge) => STEP_ENTRIES.includes(baseName(edge.callee)));
    if (!edges.length) throw new Error(`a window made no call to ${STEP_ENTRIES.join(" or ")}`);
    return edges;
}

/** The record of a window whose `steps` cost what `edges` did beyond what `inside` did. */
function windowRecord(profile, steps, edges, inside = []) {
    const cost = [];
    for (const edge of edges) edge.cost.forEach((value, index) => (cost[index] = (cost[index] ?? 0) + value));
    for (const edge of inside) edge.cost.forEach((value, index) => (cost[index] = (cost[index] ?? 0) - value));
    const record = costRecord(profile, cost);
    if (!Number.isSafeInteger(record.Ir) || record.Ir <= 0) {
        throw new Error("a window's instruction count is not a positive integer");
    }
    return {
        calls: steps.reduce((sum, edge) => sum + edge.calls, 0),
        Ir: record.Ir,
        Dr: record.Dr ?? 0,
        Dw: record.Dw ?? 0
    };
}

/**
 * A session window's cost: the whole of its steps. An append is the
 * session's streaming parse, and the session is opened before the runner
 * zeroes the counts, so no window creates it (5.1).
 */
export function stepCost(profile) {
    const steps = stepEdges(profile);
    return windowRecord(profile, steps, steps);
}

/**
 * A `reparse` window's cost: the parse stages of the `Document.parse` its
 * step calls, read as the one-shot benchmark reads a document's
 * (`boundaries`, each entered from outside every stage). Creating the
 * parser, starting and releasing its transaction and freeing the step
 * before's document are not parsing, and are in no window (5.1; G12, G13).
 */
export function parseCost(profile, boundaries) {
    const steps = stepEdges(profile);
    const entered = stageCalls(foldClones(profile), boundaries);
    if (!entered || entered.some((stage) => !stage.edges.length)) {
        throw new Error(`a window does not enter each of ${boundaries.join(", ")} from exactly one function`);
    }
    return windowRecord(
        profile,
        steps,
        entered.flatMap((stage) => stage.edges)
    );
}

/**
 * A `binding` window's cost: what its messages add to the session's steps,
 * the MCB3 encode of each document the window publishes and the release of
 * its message -- the C side of a binding's build of its values (plan D5).
 */
export function bindingCost(profile) {
    const steps = stepEdges(profile);
    const entries = [...profile.edges.values()].filter((edge) => BINDING_ENTRIES.includes(baseName(edge.callee)));
    const sessions = [...profile.edges.values()].filter(
        (edge) => BINDING_ENTRIES.includes(baseName(edge.caller)) && SESSION_STEPS.includes(baseName(edge.callee))
    );
    if (!sessions.length) throw new Error(`a window made no session step through ${BINDING_ENTRIES.join(" or ")}`);
    return windowRecord(profile, steps, entries, sessions);
}

/**
 * The chunk windows of 5.1: one per step for a `stepwise` stream, which 6.3
 * measures at every step (decision G17), and otherwise at most 1,024; window
 * k of W ends after step floor((k + 1) n / W). The flush's window follows
 * the last.
 */
export function windowEnds(steps, stepwise) {
    const windows = stepwise ? steps : Math.min(steps, WINDOWS);
    return Array.from({ length: windows }, (_, window) => Math.floor(((window + 1) * steps) / windows));
}

/**
 * Whether a measured workload's windows are its steps, which is when `reparse`
 * ran every step and 6.3 compares the session with it: its chunk windows and
 * the flush's.
 */
export function everyStep(row) {
    return row.windows.length === row.steps + 1;
}

/** Nearest-rank percentile of a non-empty list. */
function percentile(sorted, share) {
    return sorted[Math.max(0, Math.ceil(share * sorted.length) - 1)];
}

/** The sum of a workload's window costs. */
const total = (costs) => costs.reduce((sum, cost) => sum + cost, 0);

/** 5.2: p50, p95, maximum and total over a workload's windows. */
export function summary(values) {
    if (!values.length) throw new Error("a workload has no windows");
    const sorted = [...values].sort((left, right) => left - right);
    return {
        p50: percentile(sorted, 0.5),
        p95: percentile(sorted, 0.95),
        max: sorted.at(-1),
        total: total(values)
    };
}

/**
 * 6.2 for a stream of `length` bytes: with m(p) the highest cost of every
 * window that ends at or before byte p, m(n/4) and m(n) are each at most 1.25
 * times m(n/16). `windows` lists each chunk window's end byte and Ir, in
 * order; the flush is bounded by 6.3 (decision G16).
 */
export function flatStream(windows, length) {
    const highest = (point) =>
        windows.filter((window) => window.end <= point).reduce((most, window) => Math.max(most, window.Ir), 0);
    const base = highest(length / 16);
    if (!base) throw new Error("no window ends in the first sixteenth of the stream");
    return [length / 4, length]
        .map((point) => ({ point, ratio: highest(point) / base }))
        .filter((entry) => entry.ratio > FLATNESS_LIMIT);
}

/**
 * 6.3, for a stream `reparse` ran at every step: its steps, the flush
 * included, cost at most 1.25 times `reparse` on the same steps, in total
 * (decision G15). The stream's ratio when it is above the limit.
 */
export function neverWorse(steps, reparse) {
    if (steps.length !== reparse.length) throw new Error("the subjects ran different step counts");
    const ratio = total(steps) / total(reparse);
    return ratio > REPARSE_LIMIT ? [{ ratio }] : [];
}

/**
 * 6.4: a workload's p95, maximum and total are each at most 1.02 times the
 * base revision's, measured with the same subject on both sides.
 */
export function regressions(current, base) {
    return ["p95", "max", "total"]
        .map((metric) => ({ metric, ratio: current[metric] / base[metric] }))
        .filter((entry) => entry.ratio > REGRESSION_LIMIT);
}

/**
 * The R, S and B columns by group, each pooled over every window of every
 * workload in the group that has it: `key` names a workload's group.
 * `totalRatio` is S's total over R's on the streams 6.3 measures, those
 * `reparse` ran at every step, `stepRatio` their highest S / R of one step,
 * `bindingRatio` B's total over S's on the streams B runs, and the one-shot
 * figure sums the final texts' parses.
 */
export function groupWindows(results, key) {
    const groups = new Map();
    for (const row of results) {
        const name = key(row);
        const group = groups.get(name) ?? {
            key: name,
            workloads: 0,
            windows: [],
            session: [],
            binding: [],
            measured: 0,
            stepRatio: 0,
            sessionTotal: 0,
            reparseTotal: 0,
            bindingTotal: 0,
            boundTotal: 0,
            oneshot: 0
        };
        group.workloads++;
        group.windows.push(...row.windows);
        if (row.session) group.session.push(...row.session.windows);
        if (row.session && everyStep(row)) {
            group.measured++;
            group.sessionTotal += total(row.session.windows);
            group.reparseTotal += total(row.windows);
            row.session.windows.forEach(
                (cost, step) => (group.stepRatio = Math.max(group.stepRatio, cost / row.windows[step]))
            );
        }
        if (row.binding) {
            group.binding.push(...row.binding.windows);
            group.bindingTotal += total(row.binding.windows);
            group.boundTotal += total(row.session.windows);
        }
        group.oneshot += row.oneshot.ir;
        groups.set(name, group);
    }
    return [...groups.values()].map((group) => ({
        key: group.key,
        workloads: group.workloads,
        windows: group.windows.length,
        reparse: summary(group.windows),
        session: group.session.length ? summary(group.session) : null,
        binding: group.binding.length ? summary(group.binding) : null,
        totalRatio: group.measured ? group.sessionTotal / group.reparseTotal : null,
        stepRatio: group.measured ? group.stepRatio : null,
        bindingRatio: group.binding.length ? group.bindingTotal / group.boundTotal : null,
        oneshot: group.oneshot
    }));
}

/**
 * The edit workloads' R column by group (5.1): each script's windows, pooled
 * over every script of every edit workload whose scripts `key` puts in the
 * group. `key` names a script's group from its workload and the script.
 */
export function groupEdits(results, key) {
    const groups = new Map();
    for (const row of results)
        for (const script of row.scripts) {
            const name = key(row, script);
            const group = groups.get(name) ?? { key: name, scripts: 0, edits: 0, windows: [] };
            group.scripts++;
            group.edits += script.edits;
            group.windows.push(...script.windows);
            groups.set(name, group);
        }
    return [...groups.values()].map((group) => ({
        key: group.key,
        scripts: group.scripts,
        edits: group.edits,
        windows: group.windows.length,
        reparse: summary(group.windows)
    }));
}
