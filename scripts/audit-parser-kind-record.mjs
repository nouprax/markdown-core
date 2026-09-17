#!/usr/bin/env node
/** `parser->kinds_created` decides which postprocess passes run. A production
 * site that produces a node kind without recording it does not fail a build or
 * a test: it makes the gate skip a pass some document needed, and the defect
 * surfaces as a missing rewrite far from the line that caused it.
 *
 * That used to be policed here, over twenty-one creation sites each paired
 * with a separate `markdown_core_parser_note_kind` call. It is not policed any
 * more, because recording is now part of producing a node:
 * `markdown_core_parser_new_node`, `markdown_core_parser_new_node_with_ext`
 * and `markdown_core_parser_set_node_kind` record the kind they are given.
 *
 * What is left to hold is that production code uses THOSE and not the
 * parser-less forms underneath them, which exist for callers that have no
 * parse -- the tests build trees by hand. That is one rule over a closed set
 * of names rather than a proximity check over call sites, and a new creation
 * site cannot quietly opt out of it.
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const pkg = path.join(root, "packages/markdown-core");

/** The parser-less forms. Production code reaches them only through the
 * recording wrappers in `parser.h`. */
const UNRECORDED = /\bmarkdown_core_node_(?:new(?:_with_ext)?|set_kind)\s*\(/g;
const RECORDING = /\bmarkdown_core_parser_(?:new_node(?:_with_ext)?|set_node_kind)\s*\(/g;

/** Where the parser-less forms are allowed to appear: the two headers that
 * DECLARE them, the translation unit that DEFINES them, and `parser.h`, where
 * the recording wrappers are the one thing in the library that calls them. */
const DEFINES_THEM = new Set([
    "core/markdown-core.h",
    "core/markdown-core-element-api.h",
    "core/node.c",
    "core/parser.h"
]);

const failures = [];
let recordingSites = 0;

function librarySources() {
    return ["core", "elements"].flatMap((dir) =>
        fs
            .readdirSync(path.join(pkg, dir))
            .filter((name) => name.endsWith(".c") || name.endsWith(".h"))
            .map((name) => `${dir}/${name}`)
    );
}

/** The name of the function a byte offset falls inside, by the last definition above it. */
function enclosingFunction(source, offset) {
    const definitions = [...source.slice(0, offset).matchAll(/^[A-Za-z_][\w *]*?\b(\w+)\([^;]*?\)\s*\{/gm)];
    return definitions.length ? definitions[definitions.length - 1][1] : "(file scope)";
}

for (const file of librarySources()) {
    const source = fs.readFileSync(path.join(pkg, file), "utf8");
    const stripped = source.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
    recordingSites += [...stripped.matchAll(RECORDING)].length;
    if (DEFINES_THEM.has(file)) continue;
    for (const match of stripped.matchAll(UNRECORDED)) {
        const line = stripped.slice(0, match.index).split("\n").length;
        const owner = enclosingFunction(stripped, match.index);
        failures.push(
            `${file}:${line}: ${owner} produces a node kind through ${match[0].replace(/\s*\($/, "")}, ` +
                `which does not record it; use the markdown_core_parser_ form`
        );
    }
}

if (!recordingSites) {
    failures.push("found no recording creation sites; the audit is not reaching the sources");
}

if (failures.length) {
    for (const failure of failures) {
        console.error(`audit-parser-kind-record: ${failure}`);
    }
    process.exit(1);
}
console.log(`audit-parser-kind-record: ${recordingSites} node-kind writes, every one through a recording operation`);
