#!/usr/bin/env node
/**
 * An element reaches another element only through the peers its descriptor
 * declares (`.peers`, element.h), and the engine reaches elements only
 * through their descriptors. This audit reads the sources, which is the only
 * place the fact exists: a call from one element into another's functions
 * compiles, links and produces correct output whether or not the dialect
 * attaches the callee, so no output-level oracle can see it.
 *
 * OWNERSHIP. A header belongs to element E when it declares E's descriptor
 * (`extern const markdown_core_element MARKDOWN_CORE_ELEMENT_E`), and so does
 * every header and source named `<stem>_<anything>` beside a `<stem>.h` that
 * E owns (its scanners, its state). A source belongs to the elements whose
 * descriptors it defines. Files owned by no element (the composition root,
 * the AST and context services) are not elements and are not judged here.
 *
 * THE GENERATED SCANNERS ARE A LEXICON, NOT AN ELEMENT. A `*_scanners.h`
 * declares pure functions from bytes to a match length, generated from its
 * `.re`: no instance, no state, no hook. Two elements that read the same
 * lexical rule (HTML tags for inline and block HTML, a URI for an autolink
 * and a citation) share its scanner without either asking the other
 * anything, so these headers belong to no element.
 *
 * WHAT AN ELEMENT MAY NAME. In a source owned by element E, every symbol
 * declared by a header owned by element F ≠ E — a function, a macro, a type,
 * an enum constant or F's descriptor itself — is a finding unless F is in
 * E's `_PEERS[]` initializer, and so is an `#include` of F's header. Naming
 * F's descriptor is allowed only inside that initializer.
 *
 * AND THE AUDIT MUST SEE SOMETHING: every element header must contribute
 * symbols and every attached descriptor must be read, or it fails closed.
 *
 *   node scripts/audit/check-element-peers.mjs [--verbose]
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
    const peerBlocks = [...text.matchAll(/_PEERS\[\]\s*=\s*\{([^}]*)\}/g)];
    const peers = new Set(peerBlocks.flatMap((m) => [...m[1].matchAll(/MARKDOWN_CORE_ELEMENT_\w+/g)].map((x) => x[0])));
    const mine = (owner) => owner.some((symbol) => self.includes(symbol));
    const allowed = (owner) => mine(owner) || owner.some((symbol) => peers.has(symbol));
    for (const [, include] of raw.matchAll(/^\s*#\s*include\s*"([^"]+)"/gm)) {
        const owner = owners.has(include) ? owners.get(include) : stemOwner(include);
        if (owner && headers.includes(include) && !allowed(owner)) {
            findings.push(`${file}: includes ${include} of ${owner.join(", ")}, not a peer`);
        }
    }
    const outside = text.replace(/_PEERS\[\]\s*=\s*\{[^}]*\}/g, " ");
    const seen = new Set();
    for (const [name] of outside.matchAll(/\b[A-Za-z_]\w*\b/g)) {
        const hit = owned.get(name);
        if (!hit || seen.has(name) || mine(hit.owner)) continue;
        seen.add(name);
        if (/^MARKDOWN_CORE_ELEMENT_/.test(name) && hit.owner.includes(name)) {
            findings.push(`${file}: names ${name} outside its peers`);
        } else if (!allowed(hit.owner)) {
            findings.push(`${file}: uses ${name} of ${hit.file} (${hit.owner.join(", ")}), not a peer`);
        }
    }
}
if (judged === 0) throw new Error("no element source was judged");

if (verbose) console.log(`element headers: ${owners.size}, owned symbols: ${owned.size}, sources judged: ${judged}`);
for (const finding of findings) console.log(finding);
console.log(
    `element peers: ${findings.length === 0 ? "ok" : `${findings.length} finding(s)`}, ${judged} element sources`
);
process.exit(findings.length === 0 ? 0 : 1);
