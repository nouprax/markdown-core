// The coverage closure of the fixtures: grow the fixture inputs by single
// edits until no edit reaches a transition the pool has not reached.
//
// The edits of an input are: insert one token at any scalar boundary, or
// delete one scalar. Tokens are up to two members of every class of the
// alphabet (`alphabet.mjs`) and its keywords. The pool starts as the
// fixture inputs and takes, in a fixed order, every edit that reaches a new
// transition; it is closed when a round takes none. Then every transition
// the pool reaches (cover.mjs: a step after a step) is reached by an input
// that no single edit of a pool
// input improves on.
//
// `node scripts/correctness/closure.mjs [--rounds N] [--seeds N] [--out FILE]`
// writes the pool, each input with the transitions only it reached when taken.
import { mkdirSync, writeFileSync } from "node:fs";
import { availableParallelism } from "node:os";
import { dirname } from "node:path";
import { representatives, words } from "./alphabet.mjs";
import { build, fixtures, start, transitions } from "./cover.mjs";

const flag = (name, fallback) => {
    const at = process.argv.indexOf(name);
    return at < 0 ? fallback : process.argv[at + 1];
};
const rounds = Number(flag("--rounds", "1000"));
const out = flag("--out", "build/closure/pool.json");

// Whole tokens: the members of each class, the keywords, and the names the
// parser reads from tables, one of each kind.
const tokens = [];
for (const members of representatives().values()) {
    for (const c of members) tokens.push(String.fromCodePoint(c));
}
tokens.push(...words, "&amp;", "div", "pre", "CDATA[");

function edits(input) {
    const scalars = [...input];
    const found = [];
    for (let at = 0; at <= scalars.length; at += 1) {
        const before = scalars.slice(0, at).join("");
        const after = scalars.slice(at).join("");
        for (const token of tokens) found.push(before + token + after);
        if (at < scalars.length) found.push(before + scalars.slice(at + 1).join(""));
    }
    return found;
}

const binary = build();
const workers = availableParallelism();
const chunk = 4000;

/// Run `inputs` in chunks over all workers; call `take` with each chunk's
/// results in input order.
async function each(inputs, take) {
    const chunks = [];
    for (let at = 0; at < inputs.length; at += chunk) chunks.push(inputs.slice(at, at + chunk));
    const pending = new Map();
    let next = 0;
    for (let index = 0; index < chunks.length; index += 1) {
        while (next < chunks.length && next < index + workers) {
            pending.set(next, start(binary, chunks[next]));
            next += 1;
        }
        const done = await pending.get(index);
        pending.delete(index);
        take(done);
    }
}

const reached = new Set();
const pool = [];
const known = new Set();
const crashes = [];
function offer({ input, edges, crash }, parent) {
    if (crash !== undefined) {
        if (!crashes.some((found) => found.input === input)) crashes.push({ input, crash });
        console.log(`  crash: ${JSON.stringify(input)}: ${crash.split("\n")[1] ?? crash}`);
        return;
    }
    if (known.has(input)) return;
    const fresh = [...edges].filter((edge) => !reached.has(edge));
    if (parent !== undefined && fresh.length === 0) return;
    known.add(input);
    for (const edge of fresh) reached.add(edge);
    pool.push({ input, parent, fresh: fresh.map((edge) => transitions[edge]) });
}

function save(closed) {
    mkdirSync(dirname(out), { recursive: true });
    writeFileSync(out, JSON.stringify({ closed, transitions: reached.size, crashes, pool }, null, 1));
}

const seeds = fixtures().slice(0, Number(flag("--seeds", "Infinity")));
await each(seeds, (done) => done.forEach((result) => offer(result, undefined)));
console.log(`seeds: ${pool.length} inputs, ${reached.size} transitions`);

let frontier = pool.map((_, index) => index);
let ran = 0;
for (let round = 1; round <= rounds && frontier.length; round += 1) {
    const before = pool.length;
    // Run the edits of many inputs at once, then take them in order.
    let inputs = [];
    let parents = [];
    const flush = async () => {
        let at = 0;
        await each(inputs, (done) => done.forEach((result) => offer(result, parents[at++])));
        ran += inputs.length;
        console.log(`  ran ${ran}: ${pool.length} inputs, ${reached.size} transitions`);
        inputs = [];
        parents = [];
    };
    const queued = new Set();
    for (const index of frontier) {
        for (const input of edits(pool[index].input)) {
            if (known.has(input) || queued.has(input)) continue;
            queued.add(input);
            inputs.push(input);
            parents.push(index);
        }
        if (inputs.length >= workers * chunk) {
            await flush();
            queued.clear();
        }
    }
    await flush();
    frontier = pool.slice(before).map((_, at) => before + at);
    console.log(`round ${round}: took ${frontier.length}, ${pool.length} inputs, ${reached.size} transitions`);
    save(false);
}

save(frontier.length === 0);
console.log(frontier.length === 0 ? "closed" : "not closed: rounds ran out");
