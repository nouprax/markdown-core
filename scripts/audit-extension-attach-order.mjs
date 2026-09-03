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
 * Tests are exempt from (1) on purpose. `extension_decline_yields_turn` in
 * `tests/api/main.c` attaches `table` and then `directive` by hand precisely so
 * that it keeps failing under any order. The strict OOM runner similarly
 * supplies its own setup callback. A test that could not build a parser the
 * product cannot build would be unable to gate the product's choice.
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const pkg = path.join(root, "packages/markdown-core");
const read = (relative) => fs.readFileSync(path.join(pkg, relative), "utf8");

const failures = [];
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

// (2) and (3): the ordered table.
//
// Since 3.4 the table names DESCRIPTORS, not strings: there is nothing to look
// up by name and nothing to register. The pairing it has to check is therefore
// the other way round -- every descriptor DEFINED in `extensions/` must have a
// place in the table, so an extension cannot become attachable without being
// given a position.
const extensionsSource = read("extensions/core-extensions.c");
const table = /CORE_EXTENSIONS\[\]\s*=\s*\{([\s\S]*?)\};/.exec(extensionsSource);
if (!table) {
    failures.push("extensions/core-extensions.c: no CORE_EXTENSIONS[] table");
} else {
    const ordered = [...table[1].matchAll(/&MARKDOWN_CORE_EXTENSION_(\w+)/g)].map((match) => match[1].toLowerCase());
    const defined = fs
        .readdirSync(path.join(pkg, "extensions"))
        .filter((name) => name.endsWith(".c"))
        .flatMap((name) => [
            ...read(`extensions/${name}`).matchAll(/^const markdown_core_extension MARKDOWN_CORE_EXTENSION_(\w+) =/gm)
        ])
        .map((match) => match[1].toLowerCase());

    if (defined.length === 0) {
        failures.push("no extension descriptor was found at all — this audit is reading the wrong tree");
    }
    for (const name of defined) {
        if (!ordered.includes(name)) {
            failures.push(
                `\`${name}\` defines a descriptor and has no place in CORE_EXTENSIONS[]. ` +
                    "An extension the product cannot attach in a stated order is one it will attach in an unstated one."
            );
        }
    }
    for (const name of ordered) {
        if (!defined.includes(name)) failures.push(`CORE_EXTENSIONS[] names \`${name}\`, which no source defines`);
        if (ordered.indexOf(name) !== ordered.lastIndexOf(name)) {
            failures.push(`CORE_EXTENSIONS[] names \`${name}\` twice`);
        }
    }
    if (ordered[ordered.length - 1] !== "table") {
        failures.push(
            `CORE_EXTENSIONS[] must end with \`table\` (Q9); it ends with \`${ordered[ordered.length - 1]}\``
        );
    }
    if (!failures.length) {
        process.stdout.write(`extension attach order: ${ordered.join(" -> ")}\n`);
    }
}

if (failures.length) {
    process.stderr.write(`extension attach order audit FAILED\n  ${failures.join("\n  ")}\n`);
    process.exit(1);
}
process.stdout.write(`  one attach site, ${sites.length} call(s), in markdown_core_core_extensions_attach.\n`);
process.stdout.write("extension attach order audit passed.\n");
