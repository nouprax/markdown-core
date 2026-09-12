#!/usr/bin/env node
/**
 * The product attaches core extensions through exactly one path, and that path
 * puts `table` last.
 *
 * This is D15's own statement, and until 0a.11 nothing in the repository could
 * see it. `core/main.c` attached `directive` FIRST and `extensions/ast.c` — the
 * path every binding goes through — attached it LAST, so the CLI's default
 * language was not the facade's. Measured over 2,744 ordered triples of 14
 * significant lines with D8 already fixed, the two orders still disagreed on 4;
 * no fixture contained any of them, and none ever could, because every fixture
 * runs through the facade and so can only see one of the two orders.
 *
 * `extensions-conflicts.txt` gates the ORDER — revert the table below and its
 * last two examples fail. Nothing there gates the number of attach SITES, and
 * two sites is how the defect was spelled. So this audit reads the source,
 * which is the only place that fact lives:
 *
 *   1. `markdown_core_parser_attach_extension` is called from exactly
 *      one function in the shipped library, and that function is
 *      `markdown_core_core_extensions_attach`.
 *   2. Its table names every registered core extension exactly once, so an
 *      extension cannot become attachable without being given a position.
 *   3. `table` is last (Q9): a table's row matcher claims any line inside an
 *      open table, so every narrower claim is offered the line first. D8
 *      answers the case where table DECLINES; only the order answers the case
 *      where its matcher succeeds.
 *
 * Tests may attach synthetic fault-injection or observation probes. The
 * engine still attaches the complete core table before any setup callback,
 * so those probes never select a subset of the dialect.
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { readExtensionInventory } from "./lib/extension-inventory.mjs";
import { auditParserBoundaries } from "./lib/parser-boundaries.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const pkg = path.join(root, "packages/markdown-core");
const read = (relative) => fs.readFileSync(path.join(pkg, relative), "utf8");

const failures = [];
const elementHeaders = fs
    .readdirSync(path.join(pkg, "extensions"))
    .filter((file) => file.endsWith(".h") && file !== "markdown-core-extensions.h");
const sharedScanners = new Set([...read("core/scanners.h").matchAll(/\b(_?scan_\w+)\s*\(/g)].map((match) => match[1]));
const syntaxScanners = elementHeaders
    .filter((file) => file.endsWith("_scanners.h"))
    .flatMap((file) => [...read(`extensions/${file}`).matchAll(/\b(_?scan_\w+)\s*\(/g)].map((match) => match[1]))
    .filter((name) => !sharedScanners.has(name));
failures.push(
    ...auditParserBoundaries(
        ["core/inlines.c", "core/blocks.c", "core/scanners.c"].map((file) => ({ file, source: read(file) })),
        { elementHeaders, syntaxScanners }
    )
);
if (fs.readdirSync(path.join(pkg, "core")).some((file) => file.endsWith(".re"))) {
    failures.push("generated lexical grammar belongs to element extensions, not core");
}
const ATTACH = "markdown_core_parser_attach_extension";

/** Every `*.c` under `core/` and `extensions/` — the shipped library, no tests. */
function librarySources() {
    return ["core", "extensions"].flatMap((dir) =>
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
const strays = sites.filter((site) => site.function !== "markdown_core_core_extensions_attach");
if (sites.length === 0) {
    failures.push(`no call to ${ATTACH} in the library at all — this audit is reading the wrong tree`);
}
for (const stray of strays) {
    failures.push(
        `${stray.file}: ${ATTACH} is called from \`${stray.function}\`. ` +
            "A second attach site is a second attach ORDER, which is D15."
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
    for (const match of source.matchAll(/\bmarkdown_core_core_extensions_attach\s*\(/g)) {
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
    failures.push("the sole engine parse transaction must be the sole complete-dialect attachment site");
}

// (2) The shared inventory proves every descriptor has exactly one position.
// (3) The table row matcher must follow every narrower block claim.
try {
    const { ordered: descriptors } = readExtensionInventory(path.join(pkg, "extensions"));
    const ordered = descriptors.map(({ symbol }) => symbol.replace("MARKDOWN_CORE_EXTENSION_", "").toLowerCase());
    if (ordered[ordered.length - 1] !== "table") {
        failures.push(
            `CORE_EXTENSIONS[] must end with \`table\` (Q9); it ends with \`${ordered[ordered.length - 1]}\``
        );
    }
    if (!failures.length) {
        process.stdout.write(`extension attach order: ${ordered.join(" -> ")}\n`);
    }
} catch (error) {
    failures.push(error.message);
}

if (failures.length) {
    process.stderr.write(`extension attach order audit FAILED\n  ${failures.join("\n  ")}\n`);
    process.exit(1);
}
process.stdout.write(`  one attach site, ${sites.length} call(s), in markdown_core_core_extensions_attach.\n`);
process.stdout.write("extension attach order audit passed.\n");
