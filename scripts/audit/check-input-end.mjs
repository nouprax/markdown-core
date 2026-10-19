#!/usr/bin/env node
/** A streaming session commits the lines whose decisions hold whatever text
 * follows (docs/plans/2026-09-29-incremental-parsing.md, 5.2). A line whose
 * reading reached the end of the document's input decided something only
 * while no more text follows, so the parser records it: every reading that
 * asks whether the input goes on asks `markdown_core_parser_input_ends`, or
 * asks the line index for a line. The entry records the line that asked when
 * the input ends where it asked, and the line index when the line asked for
 * is not complete (parser.h).
 *
 * A reading that compares a position with the input's end itself decides
 * from the end without the record, and the commit then keeps a decision the
 * next append can change. Nothing fails at once: the stream diverges from a
 * fresh parse only when text arrives after that line. So the rule is held
 * here, over a closed set of names: the parser's view of the end -- the
 * input's length, where its last complete line ends, how far its scanner has
 * read, and the record -- is read only by the entry, the line index, the
 * driver that installs an input, the snapshot's mark, which keeps the
 * commit's view while the snapshot reads on and puts it back, and the commit
 * point; and for the candidates (blocks.c, THE CANDIDATES) by the reading
 * that sets the end aside, which keeps the record and tells whether it read
 * the end, by where the lines the text has finished end, which a reading
 * compares a line with without reading the end, by the reading that waits at
 * the end, and by the candidates' feed, which feeds them to the end and
 * decides one that waited again once the end moves.
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const pkg = path.join(root, "packages/markdown-core");

const END_VIEW = /\bparser->(input\.(size|complete|scanned)|end_seen_at|end_seen)\b/g;

/** The functions that own the end: the entry and its record, the input's
 * text, which a reading reads to its end asking the entry, the reading
 * that sets the end aside, where the finished lines end, the reading that
 * waits at the end (parser.h), the line index's frontier, the physical-line
 * scanner, the line index's entry for readers, the driver that installs an
 * input, the snapshot's mark and the put-back a rollback and a candidate's
 * undo share, the commit point the record leaves, and the candidates' feed
 * and the snapshot's feed of them to the end (blocks.c). */
const OWNERS = new Set([
    "core/parser.h:markdown_core_parser_see_end",
    "core/parser.h:markdown_core_parser_input_ends",
    "core/parser.h:markdown_core_parser_input_text",
    "core/parser.h:markdown_core_parser_set_end_aside",
    "core/parser.h:markdown_core_parser_end_read",
    "core/parser.h:markdown_core_parser_put_end_back",
    "core/parser.h:markdown_core_parser_complete_end",
    "core/parser.h:markdown_core_parser_lookahead_wait",
    "core/parser.h:markdown_core_input_line_next",
    "core/blocks.c:S_scan_line",
    "core/blocks.c:S_extend_source_lines",
    "core/blocks.c:markdown_core_parser_extend_source_lines",
    "core/blocks.c:S_parse_source",
    "core/blocks.c:S_mark",
    "core/blocks.c:S_restore",
    "core/blocks.c:markdown_core_parser_commit_point",
    "core/blocks.c:S_feed",
    "core/blocks.c:S_candidates_see_end"
]);

function librarySources() {
    return ["core", "elements"].flatMap((dir) =>
        fs
            .readdirSync(path.join(pkg, dir))
            .filter((name) => name.endsWith(".c") || name.endsWith(".h"))
            .map((name) => `${dir}/${name}`)
    );
}

/** The top-level blocks of a translation unit, each named by the function
 * it defines, or "(declaration)" for a struct, an enum or an initializer.
 * `source` has its comments, literals and directives blanked. */
function topLevelBlocks(source) {
    const blocks = [];
    let depth = 0;
    let headerStart = 0;
    let open = 0;
    let name = "";
    let linkage = 0;
    for (let i = 0; i < source.length; i++) {
        const c = source[i];
        if (c === "{" && depth === 0 && /\bextern\s*""\s*$/.test(source.slice(headerStart, i))) {
            /* A linkage block holds top-level definitions. */
            linkage++;
            headerStart = i + 1;
        } else if (c === "}" && depth === 0 && linkage) {
            linkage--;
            headerStart = i + 1;
        } else if (c === "{") {
            if (depth === 0) {
                const header = source.slice(headerStart, i).replace(/MARKDOWN_CORE_ATTRIBUTE\(\([^)]*\)\)/g, "");
                const call = header.match(/(\w+)\s*\(/);
                name = call ? call[1] : "(declaration)";
                open = i;
            }
            depth++;
        } else if (c === "}") {
            depth--;
            if (depth === 0) {
                blocks.push({ name, start: open, end: i });
                headerStart = i + 1;
            }
        } else if (c === ";" && depth === 0) {
            headerStart = i + 1;
        }
    }
    return blocks;
}

const failures = [];
const seen = new Set();
let reads = 0;

for (const file of librarySources()) {
    const source = fs.readFileSync(path.join(pkg, file), "utf8");
    const stripped = source
        .replace(/\/\*[\s\S]*?\*\//g, (comment) => comment.replace(/[^\n]/g, " "))
        .replace(/\/\/[^\n]*/g, "")
        .replace(/"(?:[^"\\\n]|\\.)*"/g, '""')
        .replace(/'(?:[^'\\\n]|\\.)*'/g, "' '")
        .replace(/^[ \t]*#(?:[^\n]*\\\n)*[^\n]*/gm, (directive) => directive.replace(/[^\n]/g, " "));
    const blocks = topLevelBlocks(stripped);
    for (const match of stripped.matchAll(END_VIEW)) {
        const block = blocks.find((entry) => entry.start < match.index && match.index < entry.end);
        const owner = block ? block.name : "(file scope)";
        const key = `${file}:${owner}`;
        reads++;
        if (OWNERS.has(key)) {
            seen.add(key);
            continue;
        }
        const line = stripped.slice(0, match.index).split("\n").length;
        failures.push(
            `${file}:${line}: ${owner} reads parser->${match[1]}; ask markdown_core_parser_input_ends ` +
                `or markdown_core_parser_source_line whether the input goes on`
        );
    }
}

for (const owner of OWNERS) {
    if (!seen.has(owner)) {
        failures.push(`${owner} no longer reads the input's end; remove it from the owners`);
    }
}
if (!reads) {
    failures.push("found no reading of the input's end; the audit is not reaching the sources");
}

if (failures.length) {
    for (const failure of failures) {
        console.error(`audit-input-end: ${failure}`);
    }
    process.exit(1);
}
console.log(`audit-input-end: ${reads} readings of the input's end, every one in its ${OWNERS.size} owners`);
