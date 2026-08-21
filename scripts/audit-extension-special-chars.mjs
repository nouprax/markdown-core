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
 * Bytes below 0x20 are delimiter-tag SENTINELS, not characters the extension
 * expects to meet in the input: they are pushed onto the delimiter stack to
 * label an opener, and `match` never sees them. They are exempt from the
 * dispatch requirement and required to stay below 0x20 — a sentinel that grew
 * into printable range would be an ordinary byte a user can type.
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const extensionsDir = path.join(root, "packages/markdown-core/extensions");

// `(void *)'x'` or `(void *)SENTINEL_NAME`, in the argument to the llist append
// that builds the list handed to `set_special_inline_chars`.
const REGISTRATION = /special_chars\s*=\s*markdown_core_llist_append\([^;]*?\(void \*\)(?:'((?:\\.|[^'])+)'|(\w+))\)/g;
const DEFINE = (name) => new RegExp(`^#define\\s+${name}\\s+(\\d+)\\s*$`, "m");
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
for (const entry of fs
    .readdirSync(extensionsDir)
    .filter((name) => name.endsWith(".c"))
    .sort()) {
    const source = fs.readFileSync(path.join(extensionsDir, entry), "utf8");
    const registered = [...source.matchAll(REGISTRATION)].map((match) => {
        if (match[1] !== undefined) return { byte: byteOf(match[1]), spelling: `'${match[1]}'` };
        const define = DEFINE(match[2]).exec(source);
        if (define === null)
            throw new Error(`${entry}: registers \`${match[2]}\`, which this file does not #define to a number.`);
        return { byte: Number(define[1]), spelling: match[2] };
    });
    if (registered.length === 0) continue;

    const body = matchBody(source, entry);
    if (body === null) {
        failures.push(`${entry}: registers ${String(registered.length)} special inline chars and has no match_inline.`);
        continue;
    }
    const dispatched = new Set([...body.matchAll(DISPATCH)].map((match) => byteOf(match[1])));
    for (const { byte, spelling } of registered) {
        if (byte < 0x20) continue;
        if (!dispatched.has(byte))
            failures.push(
                `${entry}: registers ${spelling} as a special inline char, and its match_inline never dispatches on it.`
            );
    }
    for (const { byte, spelling } of registered)
        if (byte < 0x20 && byte === 0)
            failures.push(`${entry}: registers ${spelling} as NUL, which the feed replaces before inlines run.`);
    report.push(
        `  ${entry}: ${registered.map(({ byte }) => spell(byte)).join(" ")} ` +
            `(${String(registered.filter(({ byte }) => byte < 0x20).length)} sentinel)`
    );
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
process.stdout.write(`Extension special characters: every registered byte is dispatched or is a sentinel.\n`);
process.stdout.write(`${report.join("\n")}\n`);
