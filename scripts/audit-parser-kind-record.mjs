#!/usr/bin/env node
/** The postprocess gate reads `parser->kinds_created`, and that record is only
 * as complete as the writes that fill it. A creation site added later that
 * forgets to record its kind does not fail a build or a test: it makes the
 * gate skip a pass the document needed, and the defect surfaces as a missing
 * rewrite in one document, far from the line that caused it.
 *
 * So the record is held here instead. Every production site that creates a
 * node or converts one's kind records it, either on the spot or -- when the
 * creating helper has no parser to record through -- at every caller that
 * does. The second form cannot be checked textually, so each such helper is
 * named below with the reason it is one. */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const pkg = path.join(root, "packages/markdown-core");

const NOTE = "markdown_core_parser_note_kind";
/** A write and its recording must sit within this many lines of each other. */
const WINDOW = 4;
const WRITES = /\bmarkdown_core_node_(?:new(?:_with_ext)?|set_kind)\s*\(/g;

/** `file` -> `enclosing function` -> why its kind is recorded somewhere else.
 * A `*` covers the whole file. */
const RECORDED_ELSEWHERE = {
    "core/node.c": { "*": "defines the node operations this audit is about" },
    "core/inlines.c": {
        markdown_core_inline_make_simple:
            "the mem-only form, for callers with no parse; parse-time callers use the _noted form"
    },
    "elements/directive.c": {
        make_label_node: "takes a mem, not a parser; both callers record DIRECTIVE_LABEL"
    },
    "elements/formula.c": {
        new_formula_block_from_literal: "runs inside a postprocess pass, after the gate has read the record"
    }
};

/** Every `*.c` under `core/` and `elements/` -- the shipped library, no tests. */
function librarySources() {
    return ["core", "elements"].flatMap((dir) =>
        fs
            .readdirSync(path.join(pkg, dir))
            .filter((name) => name.endsWith(".c"))
            .map((name) => `${dir}/${name}`)
    );
}

/** The name of the function a byte offset falls inside, by the last definition above it. */
function enclosingFunction(source, offset) {
    const definitions = [...source.slice(0, offset).matchAll(/^[A-Za-z_][\w *]*?\b(\w+)\([^;]*?\)\s*\{/gm)];
    return definitions.length ? definitions[definitions.length - 1][1] : "(file scope)";
}

const failures = [];
let checked = 0;
const unused = new Set();
for (const [file, functions] of Object.entries(RECORDED_ELSEWHERE)) {
    for (const name of Object.keys(functions)) {
        unused.add(`${file}:${name}`);
    }
}

for (const file of librarySources()) {
    const source = fs.readFileSync(path.join(pkg, file), "utf8");
    const lines = source.split("\n");
    const excused = RECORDED_ELSEWHERE[file] ?? {};
    for (const match of source.matchAll(WRITES)) {
        const line = source.slice(0, match.index).split("\n").length - 1;
        const owner = enclosingFunction(source, match.index);
        if (excused["*"] !== undefined || excused[owner] !== undefined) {
            unused.delete(`${file}:*`);
            unused.delete(`${file}:${owner}`);
            continue;
        }
        checked += 1;
        const from = Math.max(0, line - WINDOW);
        const to = Math.min(lines.length, line + WINDOW + 1);
        if (!lines.slice(from, to).some((text) => text.includes(NOTE))) {
            failures.push(`${file}:${line + 1}: ${owner} writes a node kind without recording it in kinds_created`);
        }
    }
}
for (const entry of unused) {
    failures.push(`${entry}: listed as recording its kind elsewhere, but writes no node kind`);
}
if (!checked) {
    failures.push("found no node-kind writes to check; the audit is not reaching the sources");
}

if (failures.length) {
    for (const failure of failures) {
        console.error(`audit-parser-kind-record: ${failure}`);
    }
    process.exit(1);
}
console.log(`audit-parser-kind-record: ${checked} node-kind writes, every one recorded`);
