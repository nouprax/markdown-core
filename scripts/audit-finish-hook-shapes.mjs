#!/usr/bin/env node
/** THE FINISH STAGE WALKS EACH OWNED ROOT ONCE, and an element takes part in
 * that walk in one of two shapes (core/markdown-core-element-api.h): a LOCAL
 * finish step, asked from inside the walk at the events of the kinds it
 * declares, or a GLOBAL postprocess pass, handed each whole root after the
 * root's walk. The engine refuses a descriptor that declares both (and a
 * step asked at no kind, or at a kind its table cannot index), and the api
 * tests count the walk's events to hold the "once" -- but they can only
 * count walks that report themselves. A step that opened a private iterator
 * over its node's subtree would walk unseen by every counter, and its cost
 * would grow with the subtree it is asked at, which is the shape #341 removed.
 *
 * So the second half of the invariant is held here, on the source: a
 * translation unit whose descriptor declares a finish step does not open an
 * iterator at all. It has no walk to make; the walk it is part of is the one
 * traversal the finish stage makes.
 *
 * AND THE AUDIT MUST SEE SOMETHING: the dialect's two in-tree hooks, autolink
 * and formula, are steps, and a run that finds no step-declaring descriptor
 * is reaching the wrong sources.
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { readElementInventory } from "./lib/element-inventory.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const elementsDir = path.join(root, "packages/markdown-core/elements");

const STEP = /\.finish_step\s*=\s*(\w+)/;
const PASS = /\.postprocess_func\s*=\s*(\w+)/;
/** Opening an iterator is the one way to walk; `<iterator.h>` is where it is declared. */
const WALKS = /\bmarkdown_core_iter_new\s*\(|#include\s*[<"]iterator\.h[>"]/;
/** A step frees through the parse, which counts what it released (parser.h). */
const BARE_FREE = /\bmarkdown_core_node_free\s*\(/;

const failures = [];
let steps = 0;
let passes = 0;

const { ordered } = readElementInventory(elementsDir);
for (const { symbol, file, source, body } of ordered) {
    const step = STEP.exec(body)?.[1];
    const pass = PASS.exec(body)?.[1];
    if (step && pass) {
        failures.push(`${file}: ${symbol} declares both a finish step (${step}) and a postprocess pass (${pass})`);
    }
    if (pass) passes += 1;
    if (!step) continue;
    steps += 1;
    const stripped = source.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
    const walk = WALKS.exec(stripped);
    if (walk) {
        const line = stripped.slice(0, walk.index).split("\n").length;
        failures.push(
            `${file}:${line}: ${symbol} declares a finish step and opens an iterator (${walk[0].trim()}); ` +
                "a step never walks, the finish walk it is part of is the one traversal"
        );
    }
    const bare = BARE_FREE.exec(stripped);
    if (bare) {
        const line = stripped.slice(0, bare.index).split("\n").length;
        failures.push(
            `${file}:${line}: ${symbol} declares a finish step and frees a node outside the parse; ` +
                "a step frees through markdown_core_parser_free_node, which counts the release"
        );
    }
}

if (!steps) {
    failures.push("found no descriptor declaring a finish step; the audit is not reaching the sources");
}

if (failures.length) {
    for (const failure of failures) {
        console.error(`audit-finish-hook-shapes: ${failure}`);
    }
    process.exit(1);
}
console.log(
    `audit-finish-hook-shapes: ${steps} finish step${steps === 1 ? "" : "s"} and ${passes} postprocess pass` +
        `${passes === 1 ? "" : "es"}, no descriptor declares both, no step opens an iterator or frees outside the parse`
);
