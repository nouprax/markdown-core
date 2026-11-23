#!/usr/bin/env node
/**
 * An element reaches another element only through the services it needs
 * (`.needs`, element.h): it asks a service by name, and whichever element
 * provides it answers through the interface elements/services.h states. So
 * no element source names a symbol another element's header declares -- a
 * function, a macro, a type, an enum constant or a descriptor -- nor
 * includes such a header. This audit reads the sources, which is the only
 * place the fact exists: a call from one element into another's functions
 * compiles, links and produces correct output whether or not the dialect
 * attaches the callee, so no output-level oracle can see it.
 *
 * OWNERSHIP. A header belongs to element E when it declares E's descriptor
 * (`extern const markdown_core_element MARKDOWN_CORE_ELEMENT_E`), and so does
 * every header and source named `<stem>_<anything>` beside a `<stem>.h` that
 * E owns (its state). A source belongs to the elements whose descriptors it
 * defines. Files owned by no element (services.h, the composition root, the
 * AST and context services) are not elements and are not judged here.
 *
 * THE GENERATED SCANNERS BELONG TO NO ELEMENT. A `*_scanners.h` declares
 * pure functions from bytes to a match length, generated from its `.re`: no
 * instance, no state, no hook.
 *
 * WHAT IS STILL READ OUTSIDE A PARSE. A context field's end runs with no
 * dialect, so no service can answer it: the attribute grammar's own
 * functions are what it calls (`OUTSIDE`). It goes when context reads only
 * what the parse recorded (the decoupling plan, 8.5); the list must shrink,
 * never grow.
 *
 * AND THE AUDIT MUST SEE SOMETHING: every element header must contribute
 * symbols and an element source must be judged, or it fails closed.
 *
 *   node scripts/audit/check-element-services.mjs [--verbose]
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const dir = path.join(root, "packages/markdown-core/elements");
const verbose = process.argv.includes("--verbose");

const strip = (s) =>
    s
        .replace(/\/\*[\s\S]*?\*\//g, " ")
        .replace(/\/\/[^\n]*/g, " ")
        .replace(/"(?:[^"\\\n]|\\.)*"/g, '""')
        .replace(/'(?:[^'\\\n]|\\.)*'/g, "''");
const read = (file) => fs.readFileSync(path.join(dir, file), "utf8");

const files = fs.readdirSync(dir).sort();
const headers = files.filter((f) => f.endsWith(".h"));
const sources = files.filter((f) => f.endsWith(".c"));

/* Headers to the descriptors they declare. */
const owners = new Map();
for (const file of headers) {
    const symbols = [
        ...strip(read(file)).matchAll(/\bextern\s+const\s+markdown_core_element\s+(MARKDOWN_CORE_ELEMENT_\w+)/g)
    ].map((m) => m[1]);
    if (symbols.length) owners.set(file, symbols);
}
if (owners.size === 0) throw new Error("no element header declares a descriptor");
const lexicon = (file) => /_scanners\.[ch]$/.test(file);
const stemOwner = (file) => {
    if (lexicon(file)) return null;
    const base = file.replace(/\.[ch]$/, "");
    if (owners.has(`${base}.h`)) return owners.get(`${base}.h`);
    for (let cut = base.lastIndexOf("_"); cut > 0; cut = base.lastIndexOf("_", cut - 1)) {
        const stem = `${base.slice(0, cut)}.h`;
        if (owners.has(stem)) return owners.get(stem);
    }
    return null;
};

/* What a header declares: macros, functions, types, enum constants. */
function declared(source) {
    const s = strip(source);
    const names = new Set();
    for (const [, name] of s.matchAll(/^\s*#\s*define\s+(\w+)/gm)) names.add(name);
    const top = s.replace(/^\s*#[^\n]*$/gm, " ");
    for (const [, name] of top.matchAll(/^[A-Za-z_][\w\s*]*?\b(\w+)\s*\(/gm)) names.add(name);
    for (const [, name] of top.matchAll(/\}\s*(\w+)\s*;/g)) names.add(name);
    for (const [, name] of top.matchAll(/\btypedef\b[^;{}]*?\b(\w+)\s*;/g)) names.add(name);
    for (const [, body] of top.matchAll(/\benum\b[^{;]*\{([^}]*)\}/g)) {
        for (const [, name] of body.matchAll(/(?:^|,)\s*(\w+)/g)) names.add(name);
    }
    for (const [, name] of top.matchAll(/\bextern\b[^;(]*?\b(\w+)\s*;/g)) names.add(name);
    for (const word of ["static", "inline", "const", "struct", "enum", "typedef", "extern", "void"]) names.delete(word);
    return names;
}

/* Every symbol to the element that owns it. */
const owned = new Map();
for (const file of headers) {
    const owner = stemOwner(file);
    if (!owner) continue;
    const names = declared(read(file));
    if (names.size === 0) throw new Error(`${file}: an element header declares nothing this audit can read`);
    for (const name of names) {
        if (/_H$/.test(name)) continue;
        owned.set(name, { owner, file });
    }
}

/* (WHAT IS STILL READ OUTSIDE A PARSE) source -> the other elements'
 * symbols and headers it may name. */
const OUTSIDE = new Map([
    ["code_block.c", ["attributes.h", "markdown_core_attributes_tail", "markdown_core_attribute_parser_free"]]
]);

const findings = [];
let judged = 0;
for (const file of sources) {
    const raw = read(file);
    const text = strip(raw);
    let self = [...text.matchAll(/^const\s+markdown_core_element\s+(MARKDOWN_CORE_ELEMENT_\w+)\s*=/gm)].map(
        (m) => m[1]
    );
    if (self.length === 0) self = stemOwner(file) ?? [];
    if (self.length === 0) continue;
    judged++;
    const outside = new Set(OUTSIDE.get(file) ?? []);
    const mine = (owner) => owner.some((symbol) => self.includes(symbol));
    if (/\b\w+_PEERS\[\]/.test(text)) findings.push(`${file}: declares peers; an element asks services`);
    for (const [, include] of raw.matchAll(/^\s*#\s*include\s*"([^"]+)"/gm)) {
        const owner = owners.has(include) ? owners.get(include) : stemOwner(include);
        if (owner && headers.includes(include) && !mine(owner) && !outside.has(include)) {
            findings.push(`${file}: includes ${include} of ${owner.join(", ")}`);
        }
    }
    const seen = new Set();
    for (const [name] of text.matchAll(/\b[A-Za-z_]\w*\b/g)) {
        const hit = owned.get(name);
        if (!hit || seen.has(name) || mine(hit.owner) || outside.has(name)) continue;
        seen.add(name);
        findings.push(`${file}: uses ${name} of ${hit.file} (${hit.owner.join(", ")})`);
    }
}
if (judged === 0) throw new Error("no element source was judged");

if (verbose) console.log(`element headers: ${owners.size}, owned symbols: ${owned.size}, sources judged: ${judged}`);
for (const finding of findings) console.log(finding);
console.log(
    `element services: ${findings.length === 0 ? "ok" : `${findings.length} finding(s)`}, ${judged} element sources`
);
process.exit(findings.length === 0 ? 0 : 1);
