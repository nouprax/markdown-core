/**
 * THE STREAM GATES' ARITHMETIC (docs/plans/2026-09-29-incremental-gates.md,
 * sections 5 and 6), separate from the driver so that each rule is tested on
 * synthetic profiles without building or measuring anything.
 *
 * A workload's costs are its windows' costs, read from each of the runner's
 * per-window dumps: a session's window is the whole of its calls into
 * `bench_apply_step` and `bench_finalize`, and a `reparse` window is the parse
 * inside them. Every rule compares Ir; data references are reported beside
 * it.
 */

import { baseName, costRecord, foldClones, stageCalls } from "./callgrind.mjs";
import { STAGE_IR_LIMIT } from "./stage-budget.mjs";

/** The steps' entries, as stream_runner.c names them: a chunk and the finalize. */
export const STEP_ENTRIES = Object.freeze(["bench_apply_step", "bench_finalize"]);
/** The most windows a workload is split into (5.1). */
export const WINDOWS = 1024;
/** 6.2: a later window of a stream against an earlier one. */
export const FLATNESS_LIMIT = 1.25;
/** 6.3: a subject's step against `reparse` on the same step. */
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

/** The record of a window whose `steps` cost what `edges` did. */
function windowRecord(profile, steps, edges) {
    const cost = [];
    for (const edge of edges) edge.cost.forEach((value, index) => (cost[index] = (cost[index] ?? 0) + value));
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
 * The chunk windows of 5.1: window k ends after step floor((k + 1) n / W).
 * The finalize's window follows the last.
 */
export function windowEnds(steps) {
    const windows = Math.min(steps, WINDOWS);
    return Array.from({ length: windows }, (_, window) => Math.floor(((window + 1) * steps) / windows));
}

/** Nearest-rank percentile of a non-empty list. */
function percentile(sorted, share) {
    return sorted[Math.max(0, Math.ceil(share * sorted.length) - 1)];
}

/** 5.2: p50, p95, maximum and total over a workload's windows. */
export function summary(values) {
    if (!values.length) throw new Error("a workload has no windows");
    const sorted = [...values].sort((left, right) => left - right);
    return {
        p50: percentile(sorted, 0.5),
        p95: percentile(sorted, 0.95),
        max: sorted.at(-1),
        total: values.reduce((sum, value) => sum + value, 0)
    };
}

/**
 * 6.2 for a stream of `length` bytes: with m(p) the highest cost of every
 * window that ends at or before byte p, m(n/4) and m(n) are each at most 1.25
 * times m(n/16). `windows` lists each window's end byte and Ir, in order.
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
 * 6.3, for a stream of at most 1,024 chunks: every step, the finalize
 * included, costs at most 1.25 times `reparse` on the same step. A longer
 * stream is bounded by 6.2.
 */
export function neverWorse(steps, reparse) {
    if (steps.length !== reparse.length) throw new Error("the subjects ran different step counts");
    return steps
        .map((cost, step) => ({ step, ratio: cost / reparse[step] }))
        .filter((entry) => entry.ratio > REPARSE_LIMIT);
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
 * The R and S columns by group, each pooled over every window of every
 * workload in the group that has it: `key` names a workload's group. `ratio`
 * is the group's highest S / R of one step (6.3), and the one-shot figure
 * sums the final texts' parses.
 */
export function groupWindows(results, key) {
    const groups = new Map();
    for (const row of results) {
        const name = key(row);
        const group = groups.get(name) ?? { key: name, workloads: 0, windows: [], session: [], ratio: 0, oneshot: 0 };
        group.workloads++;
        group.windows.push(...row.windows);
        if (row.session) {
            group.session.push(...row.session.windows);
            row.session.windows.forEach(
                (cost, step) => (group.ratio = Math.max(group.ratio, cost / row.windows[step]))
            );
        }
        group.oneshot += row.oneshot.ir;
        groups.set(name, group);
    }
    return [...groups.values()].map(({ key: name, workloads, windows, session, ratio, oneshot }) => ({
        key: name,
        workloads,
        windows: windows.length,
        reparse: summary(windows),
        session: session.length ? summary(session) : null,
        ratio: session.length ? ratio : null,
        oneshot
    }));
}
