#!/usr/bin/env node
/** The parser selects the complete immutable core registry at exactly one
 * transaction boundary. Every descriptor occurs once and table is last.
 * Private setup probes can extend a snapshot; production parsing must not
 * rebuild a per-call registration list or choose a different dialect. */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { readElementInventory } from "./lib/element-inventory.mjs";
import { auditParserBoundaries } from "./lib/parser-boundaries.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const pkg = path.join(root, "packages/markdown-core");
const read = (relative) => fs.readFileSync(path.join(pkg, relative), "utf8");

const failures = [];
const elementHeaders = fs
    .readdirSync(path.join(pkg, "elements"))
    .filter((file) => file.endsWith(".h") && file !== "markdown-core-elements.h");
const syntaxScanners = elementHeaders
    .filter((file) => file.endsWith("_scanners.h"))
    .flatMap((file) => [...read(`elements/${file}`).matchAll(/\b(_?scan_\w+)\s*\(/g)].map((match) => match[1]));
failures.push(
    ...auditParserBoundaries(
        ["core/inlines.c", "core/blocks.c"].map((file) => ({ file, source: read(file) })),
        { elementHeaders, syntaxScanners }
    )
);
if (fs.readdirSync(path.join(pkg, "core")).some((file) => file.endsWith(".re"))) {
    failures.push("generated lexical grammar belongs to elements, not core");
}
const ATTACH = "markdown_core_parser_attach_element";

/** Every `*.c` under `core/` and `elements/` — the shipped library, no tests. */
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

/**
 * The index just past the `)` that closes the argument list opened at `open`,
 * or -1 if the parentheses do not balance. Reading the parentheses rather than
 * scanning for the next `;` is what makes this immune to where the braces sit:
 * Step 2's `InsertBraces` turned `if (ATTACH(...))\n return 0;` into
 * `if (ATTACH(...)) {`, and the old "a call has no `{` before its `;`" rule
 * then classified the one real call site as a definition and reported that the
 * library contains no attach call at all.
 */
function endOfArguments(source, open) {
    let depth = 0;
    for (let i = open; i < source.length; i += 1) {
        if (source[i] === "(") depth += 1;
        else if (source[i] === ")") {
            depth -= 1;
            if (depth === 0) return i + 1;
        }
    }
    return -1;
}

// (1) One attach site. The function's own definition is not a call: its
// argument list is followed by the body's `{`. Prototypes live in headers,
// which this audit does not read.
const sites = [];
for (const file of librarySources()) {
    const source = read(file);
    for (const match of source.matchAll(new RegExp(`\\b${ATTACH}\\s*\\(`, "g"))) {
        const end = endOfArguments(source, source.indexOf("(", match.index));
        if (end < 0) continue;
        if (/^\s*\{/.test(source.slice(end))) continue;
        sites.push({ file, function: enclosingFunction(source, match.index) });
    }
}
for (const site of sites) {
    failures.push(
        `${site.file}: ${ATTACH} is called from ${site.function}; the fixed dialect must borrow its registry`
    );
}

// The one transaction attaches the whole table; neither facade, tests, nor
// fuzzers own a configurable engine entry. Old option words cannot return.
const dialectAttachSites = [];
function cSources(dir) {
    return fs.readdirSync(dir, { withFileTypes: true }).flatMap((entry) => {
        const full = path.join(dir, entry.name);
        return entry.isDirectory() ? cSources(full) : /\.(?:c|h|cpp)$/.test(entry.name) ? [full] : [];
    });
}
for (const file of cSources(pkg)) {
    const source = fs.readFileSync(file, "utf8");
    if (/\bMARKDOWN_CORE_(?:OPT_\w+|DIALECT_OPTIONS)\b/.test(source)) {
        failures.push(`${path.relative(pkg, file)} retains a parse option`);
    }
    if (!file.endsWith(".c")) continue;
    for (const match of source.matchAll(/\bmarkdown_core_core_elements\s*\(/g)) {
        const end = endOfArguments(source, source.indexOf("(", match.index));
        if (end < 0 || /^\s*\{/.test(source.slice(end))) continue;
        dialectAttachSites.push({ file: path.relative(pkg, file), function: enclosingFunction(source, match.index) });
    }
}
if (
    dialectAttachSites.length !== 1 ||
    dialectAttachSites[0].file !== "core/blocks.c" ||
    dialectAttachSites[0].function !== "markdown_core_parse_document_with_mem"
) {
    failures.push("the sole engine parse transaction must select the complete immutable dialect registry");
}

// (2) The shared inventory proves every descriptor has exactly one position.
// (3) The table row matcher must follow every narrower block claim.
try {
    const { ordered: descriptors } = readElementInventory(path.join(pkg, "elements"));
    const ordered = descriptors.map(({ symbol }) => symbol.replace("MARKDOWN_CORE_ELEMENT_", "").toLowerCase());
    if (ordered[ordered.length - 1] !== "table") {
        failures.push(`CORE_ELEMENTS[] must end with \`table\` (Q9); it ends with \`${ordered[ordered.length - 1]}\``);
    }
    if (!failures.length) {
        process.stdout.write(`element attach order: ${ordered.join(" -> ")}\n`);
    }
} catch (error) {
    failures.push(error.message);
}

if (failures.length) {
    process.stderr.write(`element attach order audit FAILED\n  ${failures.join("\n  ")}\n`);
    process.exit(1);
}
process.stdout.write("  one immutable registry selection site; no production attachment calls.\n");
process.stdout.write("element attach order audit passed.\n");
