#!/usr/bin/env node
/** THE ELEMENT CONTRACT (docs/plans/2026-09-29-incremental-parsing.md, 5.4).
 * The engine takes only what elements make deterministic, so element code
 * holds three rules on the source:
 *
 * - E1 Reads go through the index. An element reads the source through the
 *   parser's line accessors and lookahead, which raise the high-water mark,
 *   and never touches the input index's own fields.
 * - E2 Retroactive writes go through one service. An element that changes a
 *   closed block calls `markdown_core_parser_write_closed`.
 * - E3 Carried state is a word. A container's carried state is the word its
 *   `carry_save` returns; the parse record built from it (a block's entry,
 *   its reach and whether it holds the next block) is the engine's, so no
 *   element computes or writes it.
 *
 * Element code is every source under `elements/` except the publication:
 * `ast_internal.h` and the units that include it settle, number and keep
 * nodes, and are the engine's.
 *
 * AND THE AUDIT MUST SEE SOMETHING: the dialect's separate-line block
 * identifier and a table's trailing caption write into closed blocks, and the
 * list, the callout, the definition list and the directive save a carried
 * word, so a run that finds no write_closed call or no carry_save hook is
 * reaching the wrong sources.
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const elementsDir = path.join(root, "packages/markdown-core/elements");

const RULES = [
    {
        rule: "E1",
        pattern: /\bparser->input_\w+|\bmarkdown_core_parser_extend_source_lines\s*\(/,
        message: "reads the input index's fields; an element reads lines through the parser's accessors"
    },
    {
        rule: "E3",
        pattern: /->(?:entry|reach)\s*=[^=]|\bMARKDOWN_CORE_NODE__HOLDS_NEXT\b|\bmarkdown_core_parser_carry\s*\(/,
        message: "writes a parse record; entries, reaches and held blocks are the engine's"
    }
];
const WRITE_CLOSED = /\bmarkdown_core_parser_write_closed\s*\(/g;
const CARRY_SAVE = /\.carry_save\s*=\s*\w+/g;

const failures = [];
let writes = 0;
let carries = 0;
let units = 0;

for (const name of fs.readdirSync(elementsDir).sort()) {
    if (!/\.[ch]$/.test(name) || name === "ast_internal.h") continue;
    const source = fs.readFileSync(path.join(elementsDir, name), "utf8");
    if (/#include\s*"ast_internal\.h"/.test(source)) continue;
    units += 1;
    const code = source.replace(/\/\*[\s\S]*?\*\//g, (comment) => comment.replace(/[^\n]/g, " "));
    const stripped = code.replace(/\/\/[^\n]*/g, "");
    for (const { rule, pattern, message } of RULES) {
        const match = pattern.exec(stripped);
        if (match) {
            const line = stripped.slice(0, match.index).split("\n").length;
            failures.push(`${name}:${line}: ${rule}: ${match[0].trim()} ${message}`);
        }
    }
    writes += [...stripped.matchAll(WRITE_CLOSED)].length;
    carries += [...stripped.matchAll(CARRY_SAVE)].length;
}

if (!units || writes < 2 || carries < 4) {
    failures.push(
        `found ${units} element sources, ${writes} write_closed calls and ${carries} carry_save hooks; ` +
            "the audit is not reaching the sources"
    );
}

if (failures.length) {
    for (const failure of failures) {
        console.error(`audit-element-contract: ${failure}`);
    }
    process.exit(1);
}
console.log(
    `audit-element-contract: ${units} element sources read lines through the index, write closed blocks ` +
        `through write_closed (${writes} calls) and leave parse records to the engine (${carries} carry_save hooks)`
);
