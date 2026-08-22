#!/usr/bin/env node
/**
 * Every byte an extension registers as a special inline character must be a
 * byte its own `match_inline` dispatches on.
 *
 * A registered byte ends the text run in front of it, and the parser then asks
 * the owning extension what to do with it. If the extension's `match` cannot
 * name that byte at all, the answer is always NULL and the split was pure cost:
 * the directive extension registered `}` and dispatched only `:` and `]`, which
 * bought nothing and cost about 181 arena bytes per `}` in the document,
 * because the release CLI never reclaims what text consolidation frees.
 *
 * There is no way to see this from output. `markdown_core_consolidate_text_nodes`
 * runs inside `markdown_core_parser_finish`, before any consumer sees the tree,
 * and merges the split run back carrying `end_column` forward — measured over
 * an exhaustive 37,448-case differential with zero differences. So this audit
 * reads the source, which is the only place the fact exists.
 *
 * THERE ARE NO SENTINELS ANY MORE, and that is now a law rather than a note.
 * Five bytes below 0x20 used to be declared here — `formula`'s 0x01–0x04 and
 * `directive`'s 0x08 — because a delimiter carried a byte and four kinds of
 * formula opener had to be told apart by it. They were exempt from the dispatch
 * requirement precisely because they were not characters at all. They were
 * still ordinary file bytes: a literal 0x01 in a document ended the text run in
 * front of it and was offered to `formula`'s inline hook. 3.3 gave a delimiter
 * a RULE instead of a byte and they are gone, so a byte below 0x20 in any set
 * is now a failure.
 *
 * SINCE 3.2 AN EXTENSION DECLARES THREE SETS, and two laws relate them, both
 * checked below:
 *
 *   terminates_text      ⊆ dispatch    — a byte that ends a text run and is
 *                                        dispatched to nobody is D2 exactly
 *   flanking_transparent ⊆ dispatch    — a byte an extension makes invisible to
 *                                        `scan_delims` without owning it is D1
 *
 * AND THE AUDIT MUST SEE SOMETHING. The declaration used to be a run of
 * `markdown_core_llist_append` calls; when 3.2 replaced it with one
 * `set_byte_sets` call this reader matched nothing, skipped all four
 * extensions, printed no report and exited 0. A source-scanning audit with no
 * saw-nothing assertion is one refactor away from being a gate that cannot
 * fail. Every `create_*_extension` in this directory must be found and must
 * declare its sets.
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const extensionsDir = path.join(root, "packages/markdown-core/extensions");

// The one declaration: three C string literals, or NULL, in one call.
const BYTE_SETS =
    /markdown_core_syntax_extension_set_byte_sets\(\s*\w+\s*,\s*(NULL|"(?:\\.|[^"])*")\s*,\s*(NULL|"(?:\\.|[^"])*")\s*,\s*(NULL|"(?:\\.|[^"])*")\s*\)/;
const CREATE = /markdown_core_syntax_extension \*create_(\w+)_extension\(void\)\s*\{/;

/** The bytes a C string literal denotes, with the escapes this repository uses. */
function bytesOf(literal) {
    if (literal === "NULL") return [];
    const body = literal.slice(1, -1);
    const bytes = [];
    for (let i = 0; i < body.length;) {
        if (body[i] !== "\\") {
            bytes.push(body.charCodeAt(i));
            i += 1;
            continue;
        }
        const kind = body[i + 1];
        if (kind === "x") {
            const hex = /^[0-9a-fA-F]+/.exec(body.slice(i + 2));
            if (hex === null) throw new Error(`bad hex escape in ${literal}`);
            bytes.push(parseInt(hex[0], 16));
            i += 2 + hex[0].length;
            continue;
        }
        const simple = { "\\": 0x5c, '"': 0x22, n: 0x0a, r: 0x0d, t: 0x09, 0: 0x00 };
        if (!(kind in simple)) throw new Error(`unsupported escape \\${kind} in ${literal}`);
        bytes.push(simple[kind]);
        i += 2;
    }
    return bytes;
}
// The one shape every extension's inline match uses: comparing its `character`
// (or `c`) parameter with a character literal.
const DISPATCH = /\b(?:character|c)\s*[!=]=\s*'((?:\\.|[^'])+)'/g;

const ESCAPES = { "\\\\": 0x5c, "\\'": 0x27, "\\n": 0x0a, "\\r": 0x0d, "\\t": 0x09, "\\0": 0x00 };
const byteOf = (literal) => (literal in ESCAPES ? ESCAPES[literal] : literal.charCodeAt(0));
const spell = (byte) => (byte < 0x20 ? `0x${byte.toString(16).padStart(2, "0")}` : `'${String.fromCharCode(byte)}'`);

/** The body of the extension's `match_inline` hook, found through its registration. */
function matchBody(source, file) {
    const hook = /markdown_core_syntax_extension_set_match_inline_func\(\s*\w+\s*,\s*(\w+)\s*\)/.exec(source);
    if (hook === null) return null;
    const start = source.search(new RegExp(`^static markdown_core_node \\*${hook[1]}\\(`, "m"));
    if (start < 0) throw new Error(`${file}: match_inline hook \`${hook[1]}\` is registered but not defined here.`);
    const end = source.indexOf("\n}\n", start);
    return source.slice(start, end < 0 ? source.length : end);
}

const failures = [];
const report = [];
let declared = 0;
for (const entry of fs
    .readdirSync(extensionsDir)
    .filter((name) => name.endsWith(".c"))
    .sort()) {
    const source = fs.readFileSync(path.join(extensionsDir, entry), "utf8");
    if (!CREATE.test(source)) continue;

    const sets = BYTE_SETS.exec(source);
    if (sets === null) {
        // Every extension in this directory declares its sets, even the empty
        // ones, so that "no call" always means "the reader is broken".
        failures.push(`${entry}: defines a create_*_extension and no set_byte_sets call. This audit cannot see it.`);
        continue;
    }
    declared += 1;
    const terminates = bytesOf(sets[1]);
    const dispatch = bytesOf(sets[2]);
    const transparent = bytesOf(sets[3]);

    const body = matchBody(source, entry);
    if (body === null) {
        if (dispatch.length > 0) {
            failures.push(`${entry}: declares ${String(dispatch.length)} dispatch bytes and has no match_inline.`);
        }
        report.push(`  ${entry}: no inline hook`);
        continue;
    }
    const dispatched = new Set([...body.matchAll(DISPATCH)].map((match) => byteOf(match[1])));

    for (const byte of [...terminates, ...dispatch, ...transparent]) {
        if (byte < 0x20) {
            failures.push(
                `${entry}: declares ${spell(byte)}, a control byte. Delimiter tags are RULES since 3.3, and a byte ` +
                    "below 0x20 is a byte a document can contain."
            );
        }
    }
    for (const byte of dispatch) {
        if (byte >= 0x20 && !dispatched.has(byte)) {
            failures.push(
                `${entry}: declares ${spell(byte)} in its dispatch set, and its match_inline never dispatches on it.`
            );
        }
    }

    // The two laws that make D1 and D2 unexpressible rather than fixed.
    const owned = new Set(dispatch);
    for (const byte of terminates) {
        if (!owned.has(byte)) {
            failures.push(
                `${entry}: ${spell(byte)} ends a text run and is in no dispatch set. That split is pure cost (D2).`
            );
        }
    }
    for (const byte of transparent) {
        if (!owned.has(byte)) {
            failures.push(
                `${entry}: ${spell(byte)} is flanking-transparent and the extension does not own it. That is D1.`
            );
        }
    }

    const show = (label, bytes) => `${label}=${bytes.length ? bytes.map(spell).join(" ") : "(none)"}`;
    report.push(
        `  ${entry}: ${show("terminates", terminates)}  ${show("dispatch", dispatch)}  ` +
            `${show("transparent", transparent)}`
    );
}

if (declared === 0) {
    failures.push("no extension declared any byte set; this audit read nothing at all");
}

if (failures.length > 0) {
    process.stderr.write(`extension special characters FAILED:\n${failures.map((f) => `  ${f}`).join("\n")}\n`);
    process.stderr.write(
        "\nA registered byte splits the text run in front of it whether or not match_inline\n" +
            "claims it, and consolidation hides the split from every output gate. Either dispatch\n" +
            "the byte or do not register it.\n"
    );
    process.exit(1);
}
process.stdout.write(
    `Extension special characters: every declared byte is a character, and every one is dispatched.\n`
);
process.stdout.write(`${report.join("\n")}\n`);
