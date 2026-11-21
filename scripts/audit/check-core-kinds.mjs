#!/usr/bin/env node
/**
 * The engine names no node kind. What a kind holds, its record's lifecycle,
 * its name and its grammar belong to the elements and the kinds' schema
 * (packages/markdown-core/node-types.json): the engine asks the generated
 * per-kind tables and the elements' descriptors, and knows two kinds only
 * by role -- the root and plain text, which the dialect resolves from the
 * elements that declare them (dialect.h, `root` and `plain`).
 *
 * So no source of the engine (packages/markdown-core/core/*.c) may name a
 * concrete kind: a native type (`MARKDOWN_CORE_NODE_<TYPE>` for a type of
 * the schema) or a public kind (`MARKDOWN_CORE_KIND_<KIND>` for a kind of
 * docs/specs/canonical-ast.json). The sentinel NONE is not a kind. Comments
 * and strings are read too: a kind named in a comment is a kind the engine
 * claims to know.
 *
 * AND THE AUDIT MUST SEE SOMETHING: the schemas must give types and kinds and
 * the directory must hold sources, or it fails closed.
 *
 *   node scripts/audit/check-core-kinds.mjs
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const read = (relative) => fs.readFileSync(path.join(root, relative), "utf8");

/** `HTMLBlock` -> `HTML_BLOCK`, as the generator spells a public kind. */
const screaming = (name) =>
    name
        .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
        .replace(/([A-Z]+)([A-Z][a-z])/g, "$1_$2")
        .toUpperCase();

const native = JSON.parse(read("packages/markdown-core/node-types.json"));
const contract = JSON.parse(read("docs/specs/canonical-ast.json"));
const types = [...(native.block ?? []), ...(native.inline ?? [])].map((type) => type.name);
const kinds = (contract.kinds ?? []).map((kind) => screaming(kind.name));

const failures = [];
if (types.length === 0 || kinds.length === 0) failures.push("the schemas list no types or no kinds");

const names = new Set([
    ...types.map((type) => `MARKDOWN_CORE_NODE_${type}`),
    ...kinds.map((kind) => `MARKDOWN_CORE_KIND_${kind}`)
]);
const directory = "packages/markdown-core/core";
const sources = fs
    .readdirSync(path.join(root, directory))
    .filter((file) => file.endsWith(".c"))
    .sort();
if (sources.length === 0) failures.push(`${directory} holds no sources`);

for (const file of sources) {
    const lines = read(`${directory}/${file}`).split("\n");
    lines.forEach((line, index) => {
        for (const match of line.matchAll(/\bMARKDOWN_CORE_(?:NODE|KIND)_[A-Z0-9_]+\b/g)) {
            if (names.has(match[0])) failures.push(`${directory}/${file}:${String(index + 1)}: names ${match[0]}`);
        }
    });
}

if (failures.length) {
    for (const failure of failures) console.error(failure);
    console.error(`core kinds: ${String(failures.length)} finding(s)`);
    process.exit(1);
}
console.log(`core kinds: ok, ${String(sources.length)} engine sources name no kind`);
