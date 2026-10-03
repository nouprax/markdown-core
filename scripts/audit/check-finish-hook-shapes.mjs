#!/usr/bin/env node
/** A FINISH STEP RUNS WHEN A NODE IS COMPLETE (core/markdown-core-element-api.h):
 * a block when it closes, an inline node when its inline root's pass reaches
 * its EXIT. The step is handed that one node and its parent, and is asked at
 * the events of the kinds it declares. A step that opened a private iterator
 * over its node's subtree would walk again what is already complete, and its
 * cost would grow with the subtree it is asked at, which is the shape #341
 * removed.
 *
 * So the invariant is held here, on the source: a translation unit whose
 * descriptor declares a finish step does not open an iterator at all, and
 * frees what it removes through the parse.
 *
 * AND THE AUDIT MUST SEE SOMETHING: the dialect's in-tree hooks -- the
 * list's layout, the definition list's extents, autolink and formula -- are
 * steps, and a run that finds no step-declaring descriptor is reaching the
 * wrong sources.
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { readElementInventory } from "../shared/element-inventory.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const elementsDir = path.join(root, "packages/markdown-core/elements");

const STEP = /\.finish_step\s*=\s*(\w+)/;
/** Opening an iterator is the one way to walk; `<iterator.h>` is where it is declared. */
const WALKS = /\bmarkdown_core_iter_new\s*\(|#include\s*[<"]iterator\.h[>"]/;
/** A step frees through the parse, which counts what it released (parser.h). */
const BARE_FREE = /\bmarkdown_core_node_free\s*\(/;

const failures = [];
let steps = 0;

const { ordered } = readElementInventory(elementsDir);
for (const { symbol, file, source, body } of ordered) {
    const step = STEP.exec(body)?.[1];
    if (!step) continue;
    steps += 1;
    const stripped = source.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
    const walk = WALKS.exec(stripped);
    if (walk) {
        const line = stripped.slice(0, walk.index).split("\n").length;
        failures.push(
            `${file}:${line}: ${symbol} declares a finish step and opens an iterator (${walk[0].trim()}); ` +
                "a step is handed one complete node and never walks its subtree"
        );
    }
    const bare = BARE_FREE.exec(stripped);
    if (bare) {
        const line = stripped.slice(0, bare.index).split("\n").length;
        failures.push(
            `${file}:${line}: ${symbol} declares a finish step and frees a node outside the parse; ` +
                "a step frees through markdown_core_parser_release_node, which counts the release"
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
    `audit-finish-hook-shapes: ${steps} finish step${steps === 1 ? "" : "s"}, ` +
        "no step opens an iterator or frees outside the parse"
);
