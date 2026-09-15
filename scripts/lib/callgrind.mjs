/**
 * Callgrind output reader.
 *
 * The stage benchmark needs one thing the summary lines cannot give it: the
 * cost of a single call EDGE. `S_parse_source` runs both as the document's
 * source read and again, nested, while the AST is being built, so a
 * per-function inclusive total would count that nested work under both stages
 * and a per-function self total would count neither stage whole. Callgrind
 * records each caller/callee edge with the inclusive cost of the calls along
 * it, which is exactly the stage boundary, so this module reads the file
 * rather than `callgrind_annotate`'s rendered text.
 *
 * The format is the one documented in Callgrind's manual (`callgrind_format`):
 * a header of `key: value` lines, then a body whose position lines carry the
 * declared `events:` in order. Two pieces of it are load-bearing here.
 *
 *   Name compression. `fn=(3) name` defines id 3 and `fn=(3)` refers back to
 *   it. Ids live in three namespaces -- objects, files, functions -- and the
 *   called-side spellings (`cob=`, `cfi=`/`cfl=`, `cfn=`) share the namespace
 *   of their own side, so a reader that keeps one table decodes the wrong name
 *   as soon as a file and a function are given the same id.
 *
 *   Call records. A `calls=` line is followed by exactly one cost line, and
 *   that line is the INCLUSIVE cost of those calls, not self cost of the
 *   caller. Attributing it to the caller is how an edge reader silently turns
 *   into a self-cost reader.
 */

/** Positions may be absolute, relative (`+n`, `-n`), or `*`; costs never are. */
const POSITION = /^(0x[0-9a-fA-F]+|[+-]?\d+|\*)$/u;

/* Callers and callees are joined by NUL, which cannot occur in a symbol name.
 * Spelled as a code point so that the source file itself stays text. */
const EDGE_SEPARATOR = String.fromCharCode(0);

function resolveName(table, value) {
    const compressed = /^\((\d+)\)\s*(.*)$/u.exec(value);
    if (!compressed) return value;
    const [, id, name] = compressed;
    if (name) {
        table.set(id, name);
        return name;
    }
    const known = table.get(id);
    if (known === undefined) {
        throw new Error(`callgrind: reference to undefined name id (${id})`);
    }
    return known;
}

function addCost(target, costs) {
    for (let index = 0; index < costs.length; index++) {
        target[index] = (target[index] ?? 0) + costs[index];
    }
}

function numbers(value) {
    return value
        .split(/\s+/u)
        .filter(Boolean)
        .map((token) => Number.parseInt(token, 10));
}

/**
 * Parse one callgrind output file.
 *
 * Returns the declared event names, the file's own `summary`/`totals` lines,
 * the self cost of every function, and every call edge keyed by
 * `caller<NUL>callee`. Costs are arrays aligned with `events`.
 */
export function parseCallgrind(text) {
    const objects = new Map();
    const files = new Map();
    const functions = new Map();
    const self = new Map();
    const edges = new Map();

    let events = [];
    let positionCount = 1;
    let summary = null;
    let totals = null;
    let currentFunction = null;
    let calledFunction = null;
    let pendingCalls = 0;

    for (const raw of text.split(/\r?\n/u)) {
        const line = raw.trim();
        if (!line || line.startsWith("#")) continue;

        const header = /^([a-z]+):\s*(.*)$/u.exec(line);
        if (header) {
            const [, key, value] = header;
            if (key === "events") {
                events = value.split(/\s+/u).filter(Boolean);
            } else if (key === "positions") {
                positionCount = value.split(/\s+/u).filter(Boolean).length || 1;
            } else if (key === "summary") {
                summary = numbers(value);
            } else if (key === "totals") {
                totals = numbers(value);
            }
            continue;
        }

        const assignment = /^(ob|fl|fi|fe|fn|cob|cfi|cfl|cfn|calls|jump|jcnd)=(.*)$/u.exec(line);
        if (assignment) {
            const [, key, value] = assignment;
            switch (key) {
                case "ob":
                case "cob":
                    resolveName(objects, value);
                    break;
                case "fl":
                case "fi":
                case "fe":
                case "cfi":
                case "cfl":
                    resolveName(files, value);
                    break;
                case "fn":
                    currentFunction = resolveName(functions, value);
                    break;
                case "cfn":
                    calledFunction = resolveName(functions, value);
                    break;
                case "calls":
                    pendingCalls = numbers(value)[0] ?? 0;
                    break;
                default:
                    /* Jump records carry no cost of their own. */
                    break;
            }
            continue;
        }

        const tokens = line.split(/\s+/u);
        if (!POSITION.test(tokens[0])) continue;
        const costs = tokens.slice(positionCount).map((token) => Number.parseInt(token, 10));

        if (pendingCalls > 0 && calledFunction !== null && currentFunction !== null) {
            const key = `${currentFunction}${EDGE_SEPARATOR}${calledFunction}`;
            const edge = edges.get(key) ?? { caller: currentFunction, callee: calledFunction, calls: 0, cost: [] };
            edge.calls += pendingCalls;
            addCost(edge.cost, costs);
            edges.set(key, edge);
            pendingCalls = 0;
            calledFunction = null;
            continue;
        }
        if (currentFunction !== null) {
            const cost = self.get(currentFunction) ?? [];
            addCost(cost, costs);
            self.set(currentFunction, cost);
        }
    }

    return { events, summary, totals, self, edges };
}

/**
 * Rewrite every function name through `rename` and merge what collides.
 *
 * A compiler that specializes a function emits the clone under a decorated
 * name -- GCC's `.constprop.0` and friends -- and the clone carries the work
 * while the plain name carries none of it. Folding the clones back is the
 * difference between reading a stage boundary and reading a zero.
 */
export function foldNames(profile, rename) {
    const self = new Map();
    const edges = new Map();
    const merge = (map, key, seed) => {
        const existing = map.get(key) ?? seed;
        map.set(key, existing);
        return existing;
    };
    for (const [name, cost] of profile.self) {
        addCost(merge(self, rename(name), []), cost);
    }
    for (const edge of profile.edges.values()) {
        const caller = rename(edge.caller);
        const callee = rename(edge.callee);
        const target = merge(edges, `${caller}${EDGE_SEPARATOR}${callee}`, { caller, callee, calls: 0, cost: [] });
        target.calls += edge.calls;
        addCost(target.cost, edge.cost);
    }
    return { ...profile, self, edges };
}

/** The inclusive cost of the calls from `caller` to `callee`, or null. */
export function callEdge(profile, caller, callee) {
    return profile.edges.get(`${caller}${EDGE_SEPARATOR}${callee}`) ?? null;
}

/** Every recorded call into `callee`, whatever the caller. */
export function callersOf(profile, callee) {
    return [...profile.edges.values()].filter((edge) => edge.callee === callee);
}

/** Every recorded call out of `caller`. */
export function calleesOf(profile, caller) {
    return [...profile.edges.values()].filter((edge) => edge.caller === caller);
}

/** Name the declared events so a report never depends on their column order. */
export function costRecord(profile, cost) {
    const record = {};
    profile.events.forEach((event, index) => {
        record[event] = cost[index] ?? 0;
    });
    return record;
}
