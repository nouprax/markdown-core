#!/usr/bin/env node
/** A NODE THE PARSE MAKES LIVES IN THE PARSE'S POOL, and a block whose kind
 * changes reads the old node of its new kind again (5.9):
 * `markdown_core_parser_make_node`, `markdown_core_parser_make_node_with_ext`
 * and `markdown_core_parser_set_node_kind` are the operations that do both.
 *
 * What is left to hold is that production code uses THOSE and not the
 * parser-less forms underneath them, which exist for callers that have no
 * parse -- the tests build trees by hand. That is one rule over a closed set
 * of names rather than a proximity check over call sites, and a new creation
 * site cannot quietly opt out of it.
 *
 * THE SAME RULE HOLDS FOR A RELEASE. A node the parse made lives in the
 * parse's pool, and a node it discards gives its slots back to that pool.
 * Production code releases a node through `markdown_core_parser_release_node`;
 * the parser-less `markdown_core_node_free` is for a caller with no parse,
 * which in the library is the two teardowns -- a document's, and a parser's
 * own root.
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const pkg = path.join(root, "packages/markdown-core");

/** The parser-less forms. Production code reaches them only through the
 * parse's operations in `parser.h`. */
const UNRECORDED = /\bmarkdown_core_node_(?:new(?:_with_ext)?|set_kind)\s*\(/g;
const RECORDING = /\bmarkdown_core_parser_(?:make_node(?:_with_ext)?|set_node_kind)\s*\(/g;
const UNCOUNTED_FREE = /\bmarkdown_core_node_free\s*\(/g;
const COUNTED_FREE = /\bmarkdown_core_parser_release_node\s*\(/g;
/** The two releases outside any parse: the document's teardown and the
 * parser's teardown of a root it never handed out. */
const TEARDOWNS = new Set(["elements/ast.c:markdown_core_document_free", "core/blocks.c:S_parser_dispose"]);

/** Where the parser-less forms are allowed to appear: the two headers that
 * DECLARE them, the translation unit that DEFINES them, and `parser.h`, where
 * the parse's operations are the one thing in the library that calls them. */
const DEFINES_THEM = new Set(["core/node.h", "core/markdown-core-element-api.h", "core/node.c", "core/parser.h"]);

const failures = [];
let recordingSites = 0;
let countedReleases = 0;

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
    countedReleases += [...stripped.matchAll(COUNTED_FREE)].length;
    if (DEFINES_THEM.has(file)) continue;
    for (const match of stripped.matchAll(UNRECORDED)) {
        const line = stripped.slice(0, match.index).split("\n").length;
        const owner = enclosingFunction(stripped, match.index);
        failures.push(
            `${file}:${line}: ${owner} produces a node kind through ${match[0].replace(/\s*\($/, "")}, ` +
                `outside the parse; use the markdown_core_parser_ form`
        );
    }
    for (const match of stripped.matchAll(UNCOUNTED_FREE)) {
        const owner = enclosingFunction(stripped, match.index);
        if (TEARDOWNS.has(`${file}:${owner}`)) continue;
        const line = stripped.slice(0, match.index).split("\n").length;
        failures.push(
            `${file}:${line}: ${owner} releases a node through markdown_core_node_free, outside the parse's pool; ` +
                `use markdown_core_parser_release_node`
        );
    }
}

if (!recordingSites) {
    failures.push("found no creation sites through the parse; the audit is not reaching the sources");
}
if (!countedReleases) {
    failures.push("found no release sites through the parse; the audit is not reaching the sources");
}

if (failures.length) {
    for (const failure of failures) {
        console.error(`audit-parser-kind-record: ${failure}`);
    }
    process.exit(1);
}
console.log(
    `audit-parser-kind-record: ${recordingSites} node-kind writes, every one through the parse; ` +
        `${countedReleases} releases, every one through the parse`
);
