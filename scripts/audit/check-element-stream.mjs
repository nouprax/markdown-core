#!/usr/bin/env node
/**
 * AN ELEMENT READS THE RUN THROUGH THE STREAM. The inline run's bytes and its
 * cursor are the engine's (inline_internal.h, THE INLINE STREAM): an element
 * reads them through `markdown_core_inline_offset`, `_seek`, `_has`,
 * `_char_at`, `_take`, `_window` and `_size`, so every read of the run's end
 * is one the engine sees. This audit reads the element sources, the only
 * place the fact exists, and fails on a field access of an inline state's
 * `input` or `pos`. The block phase's line has its stream already (reader.h:
 * `peek`, `rest`, `advance`, each noting how far the reading read); an
 * element reads the line's facts (`indent`, `nonspace`, ...) but never its
 * bytes (`line`) or how far it was read (`far`) by field.
 *
 * The services that still live beside the elements (ast, session, context,
 * parts) are not elements and are not judged here; they move into core.
 *
 *   node scripts/audit/check-element-stream.mjs
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const dir = path.join(root, "packages/markdown-core/elements");
const services = new Set(["ast.c", "session.c", "context.c", "parts.c"]);

const strip = (s) =>
    s
        .replace(/\/\*[\s\S]*?\*\//g, " ")
        .replace(/\/\/[^\n]*/g, " ")
        .replace(/"(?:[^"\\\n]|\\.)*"/g, '""');

const findings = [];
let judged = 0;
for (const file of fs.readdirSync(dir).sort()) {
    if (!/\.[ch]$/.test(file) || services.has(file) || /_scanners\.[ch]$/.test(file)) continue;
    judged++;
    const lines = strip(fs.readFileSync(path.join(dir, file), "utf8")).split("\n");
    lines.forEach((line, index) => {
        if (/->\s*(line\s*\.\s*(data|len)|far)\b/.test(line)) {
            findings.push(`${file}:${index + 1}: reads a line's bytes or reach by field, not through reader.h`);
        }
        if (/\binline_state\s*->\s*(input|pos)\b/.test(line)) {
            findings.push(
                `${file}:${index + 1}: reads the inline state's ${RegExp.lastMatch.split(/->\s*/)[1]} directly`
            );
        }
    });
}
if (judged === 0) throw new Error("no element source was judged");
if (findings.length) {
    for (const finding of findings) console.error(finding);
    console.error(`element stream: ${findings.length} finding(s), ${judged} element sources`);
    process.exit(1);
}
console.log(`element stream: ok, ${judged} element sources`);
