#!/usr/bin/env node
/**
 * Every byte an element's text constructs start at is a character, and every
 * paired run's default byte starts one of its element's constructs.
 *
 * THE BYTES THAT SPLIT TEXT ARE THE CONSTRUCTS' FIRST BYTES (plan
 * symbols-cleanroom.md 23). The dialect projects every element's
 * `markdown_core_text_construct` table onto one byte table as it seals, and
 * the tokenizer's data runs up to the next byte a construct may start at; a
 * construct that fails there gives the byte back to the data before it. A
 * byte named there is tried at every occurrence in every document, and
 * nothing in the output shows a byte that is tried and never taken, so this
 * audit reads the source, which is the only place the fact exists.
 *
 * THERE ARE NO SENTINELS. Bytes below 0x20 used to name delimiter kinds
 * (`formula`'s 0x01-0x04, `directive`'s 0x08); a literal 0x01 in a document
 * then was offered to `formula`. A run names a RULE since 3.3, so a control
 * byte in a construct table is a failure, except the line ends a construct
 * reads (a hard break).
 *
 * A PAIRED RUN'S BYTE (attention.h): an element whose delimiter rule names a
 * default `character` reads its runs with the core's sequence construct,
 * which it must list at that byte, or its runs are text however they flank.
 *
 * AND THE AUDIT MUST SEE SOMETHING. A source-scanning audit with no
 * saw-nothing assertion is one refactor away from being a gate that cannot
 * fail; the shared inventory pairs every descriptor definition with its
 * CORE_ELEMENTS[] entry, so every attached descriptor is audited.
 */

import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { readElementInventory } from "../shared/element-inventory.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const elementsDir = path.join(root, "packages/markdown-core/elements");

/** The bytes a C string literal denotes, with the escapes this repository uses. */
function bytesOf(literal) {
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

const spell = (byte) => (byte < 0x20 ? `0x${byte.toString(16).padStart(2, "0")}` : `'${String.fromCharCode(byte)}'`);

const failures = [];
const report = [];
const { descriptors } = readElementInventory(elementsDir);
let constructs = 0;
const startedBy = new Map();
const sources = new Map();
for (const { file: entry, source, tables } of descriptors) {
    const inlines = tables.inlines ?? "";
    const named = /\.constructs\s*=\s*(\w+)/.exec(inlines)?.[1];
    if (!named) {
        report.push(`  ${entry}: no text constructs`);
        continue;
    }
    const table = new RegExp(
        `\\bmarkdown_core_text_construct\\s+${named}\\s*\\[\\]\\s*=\\s*\\{([\\s\\S]*?)\\};`,
        "m"
    ).exec(source);
    if (table === null) {
        failures.push(`${entry}: its inline table names constructs \`${named}\`, which it does not define.`);
        continue;
    }
    const literals = [...table[1].matchAll(/\{\s*("(?:\\.|[^"\\])*")\s*,/g)].map((match) => match[1]);
    if (literals.length === 0) {
        failures.push(`${entry}: could not read a byte string of \`${named}\`.`);
        continue;
    }
    const bytes = new Set();
    for (const literal of literals) {
        constructs += 1;
        for (const byte of bytesOf(literal)) {
            if (byte < 0x20 && byte !== 0x0a && byte !== 0x0d) {
                failures.push(`${entry}: a construct starts at ${spell(byte)}, a control byte.`);
            }
            bytes.add(byte);
        }
    }
    // A source may define more than one descriptor (emphasis's `*` and `_`):
    // a rule's byte is checked against all of its source's constructs.
    const started = startedBy.get(entry) ?? new Set();
    for (const byte of bytes) started.add(byte);
    startedBy.set(entry, started);
    sources.set(entry, source);
    report.push(`  ${entry}: ${[...bytes].map(spell).join(" ")}`);
}
for (const [entry, source] of sources) {
    for (const [, character] of source.matchAll(/\.character\s*=\s*'([^'\\])'/g)) {
        if (!startedBy.get(entry).has(character.charCodeAt(0))) {
            failures.push(`${entry}: its paired runs of '${character}' start none of its constructs.`);
        }
    }
}

if (constructs === 0) failures.push("no element's text construct was found: the audit read nothing.");
if (failures.length > 0) {
    process.stderr.write(`element special characters FAILED:\n${failures.map((f) => `  ${f}`).join("\n")}\n`);
    process.exit(1);
}
process.stdout.write(
    `Element special characters: every construct starts at a character, and every paired run's byte starts one.\n`
);
process.stdout.write(`${report.join("\n")}\n`);
