#!/usr/bin/env node
/** A NODE IS COMPLETE WHEN IT IS MADE (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.8), and an element takes part in that in two shapes
 * (core/element.h): a CLOSE step, `finalize_block`, which reads the block
 * that is closing and the blocks it holds, and a COMPLETION step,
 * `complete_step`, asked inside one inline root's completion at the events of
 * the kinds it declares. Neither walks: the close step reads one block and its
 * children, and the completion step is part of the one pass over its root.
 *
 * So the audit holds that on the source: a translation unit whose descriptor
 * declares either step opens no iterator, and frees a node only through the
 * parse, never with the parser-less `markdown_core_node_free`.
 *
 * AND THE AUDIT MUST SEE SOMETHING: the dialect's in-tree close steps (the
 * list's layout, the definition list's extent, the table's rows) and its
 * completion steps (autolink and formula) are declared by the elements it
 * reads, and a run that finds none is reaching the wrong sources.
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { readElementInventory } from "../shared/element-inventory.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const elementsDir = path.join(root, "packages/markdown-core/elements");

const CLOSE = /\.finalize_block\s*=\s*(\w+)/;
const COMPLETE = /\.complete_step\s*=\s*(\w+)/;
/** Opening an iterator is the one way to walk; `<iterator.h>` is where it is declared. */
const WALKS = /\bmarkdown_core_iter_new\s*\(|#include\s*[<"]iterator\.h[>"]/;
/** A step frees through the parse, which owns the node's slots (parser.h). */
const BARE_FREE = /\bmarkdown_core_node_free\s*\(/;

const failures = [];
let closes = 0;
let completions = 0;

const { ordered } = readElementInventory(elementsDir);
for (const { symbol, file, source, body } of ordered) {
    const close = CLOSE.exec(body)?.[1];
    const complete = COMPLETE.exec(body)?.[1];
    if (close) closes += 1;
    if (complete) completions += 1;
    if (!close && !complete) continue;
    const stripped = source.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
    const walk = WALKS.exec(stripped);
    if (walk) {
        const line = stripped.slice(0, walk.index).split("\n").length;
        failures.push(
            `${file}:${line}: ${symbol} declares a close or completion step and opens an iterator ` +
                `(${walk[0].trim()}); a step reads its node, it never walks`
        );
    }
    const bare = BARE_FREE.exec(stripped);
    if (bare) {
        const line = stripped.slice(0, bare.index).split("\n").length;
        failures.push(
            `${file}:${line}: ${symbol} declares a close or completion step and frees a node outside the parse; ` +
                "a step frees through markdown_core_parser_release_node"
        );
    }
}

if (!closes || !completions) {
    failures.push(
        `found ${closes} close step${closes === 1 ? "" : "s"} and ${completions} completion ` +
            `step${completions === 1 ? "" : "s"}; the audit is not reaching the sources`
    );
}

if (failures.length) {
    for (const failure of failures) {
        console.error(`audit-completion-hooks: ${failure}`);
    }
    process.exit(1);
}
console.log(
    `audit-completion-hooks: ${closes} close step${closes === 1 ? "" : "s"} and ${completions} completion ` +
        `step${completions === 1 ? "" : "s"}, none opens an iterator or frees outside the parse`
);
