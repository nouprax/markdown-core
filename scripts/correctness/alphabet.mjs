// Byte classes of the Rust parser: the partition of ASCII bytes that its
// decisions can tell apart, read from the literals, ranges and predicates in
// its source.
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const root = new URL("../../packages/markdown-core-rs/src/", import.meta.url).pathname;
// Tables hold data (character references, Unicode classes, read below); the
// dump writes output; coverage records what runs. None decides how input
// parses.
const skip = new Set(["util/tables.rs", "util/classes.rs", "util/constant.rs", "dump.rs", "cover.rs"]);

function files(dir) {
    return readdirSync(dir).flatMap((name) => {
        const path = join(dir, name);
        if (statSync(path).isDirectory()) return files(path);
        return path.endsWith(".rs") ? [path] : [];
    });
}

const escapes = { n: 10, t: 9, r: 13, 0: 0, "\\": 92, "'": 39, '"': 34 };
function unescape(text) {
    const out = [];
    for (let at = 0; at < text.length; at += 1) {
        if (text[at] !== "\\") {
            out.push(text.charCodeAt(at));
            continue;
        }
        const next = text[at + 1];
        if (next === "x") {
            out.push(parseInt(text.slice(at + 2, at + 4), 16));
            at += 3;
        } else {
            out.push(escapes[next]);
            at += 1;
        }
    }
    return out;
}

function value(token) {
    if (token.startsWith("0x")) return parseInt(token, 16);
    const scalar = /^'\\u\{([0-9A-Fa-f]+)\}'$/.exec(token);
    if (scalar) return parseInt(scalar[1], 16);
    const body = token.replace(/^b?'/, "").replace(/'$/, "");
    return unescape(body)[0];
}

// The front matter's properties are a language of their own, with their
// own alphabet.
const regions = { markdown: new Map(), properties: new Map() };
const keywords = new Map();
const unread = [];
let predicates;
function add(name, test, site) {
    if (!predicates.has(name)) predicates.set(name, { test, sites: [] });
    predicates.get(name).sites.push(site);
}

const named = {
    is_ascii_alphanumeric: (b) => /[0-9A-Za-z]/.test(String.fromCharCode(b)),
    is_ascii_alphabetic: (b) => /[A-Za-z]/.test(String.fromCharCode(b)),
    is_ascii_digit: (b) => b >= 48 && b <= 57,
    is_ascii_hexdigit: (b) => /[0-9A-Fa-f]/.test(String.fromCharCode(b)),
    is_ascii_punctuation: (b) =>
        (b >= 33 && b <= 47) || (b >= 58 && b <= 64) || (b >= 91 && b <= 96) || (b >= 123 && b <= 126),
    is_ascii_control: (b) => b < 32 || b === 127,
    is_ascii_whitespace: (b) => [9, 10, 12, 13, 32].includes(b),
    is_whitespace: (b) => [9, 10, 11, 12, 13, 32].includes(b)
};

const literal = String.raw`(?:'\\u\{[0-9A-Fa-f]+\}'|b'(?:\\x[0-9A-Fa-f]{2}|\\.|[^'\\])'|'(?:\\x[0-9A-Fa-f]{2}|\\.|[^'\\])'|0x[0-9A-Fa-f]{2})`;
const range = new RegExp(String.raw`(${literal})\s*\.\.(=?)\s*(${literal})`, "g");
const single = new RegExp(literal, "g");
const bytes = /b"((?:\\.|[^"\\])*)"/g;
// A whole word an input slice is compared with is a keyword: its letters are
// read together, as one token.
const keyword =
    /(?:Some\("([A-Za-z][A-Za-z]+)"\)|==\s*b?"([A-Za-z][A-Za-z]+)"|b?"([A-Za-z][A-Za-z]+)"\s*(?:=>|==|\|))/g;

/// Read the byte tests of one line of source, without its comment: call
/// `put(name, test)` for each, and return whether the line reads a byte in
/// a way these forms do not model.
function scan(line, put) {
    line = line.replace(range, (_, lo, inclusive, hi) => {
        const low = value(lo);
        const high = value(hi) - (inclusive ? 0 : 1);
        put(`[${low}-${high}]`, (b) => b >= low && b <= high);
        return " ";
    });
    // `byte < 0x80` is whether a byte is ASCII.
    line = line.replace(/<\s*0x80\b/g, () => {
        put("[0-127]", (b) => b < 128);
        return " ";
    });
    line = line.replace(bytes, (_, body) => {
        for (const b of unescape(body)) put(`=${b}`, (x) => x === b);
        return " ";
    });
    const unmodeled = /(?<![=-])[<>]=?\s*(?:b'|0x[0-9A-Fa-f])|(?:b'(?:\\.|[^'\\])'|0x[0-9A-Fa-f]+)\s*[<>]/.test(line);
    for (const token of line.match(single) ?? []) {
        const b = value(token);
        if (b !== undefined) put(`=${b}`, (x) => x === b);
    }
    for (const [name, test] of Object.entries(named)) {
        if (new RegExp(String.raw`\b${name}\b`).test(line)) put(name, test);
    }
    return unmodeled;
}

for (const path of files(root)) {
    const file = relative(root, path);
    if (skip.has(file)) continue;
    predicates = file.startsWith("util/properties/") ? regions.properties : regions.markdown;
    let lines = readFileSync(path, "utf8").split("\n");
    const tests = lines.findIndex((line) => line.includes("#[cfg(test)]"));
    if (tests >= 0) lines = lines.slice(0, tests);
    lines.forEach((raw, index) => {
        const site = `${file}:${index + 1}`;
        let line = raw.replace(/\/\/.*$/, "");
        if (!line.trim()) return;
        line = line.replace(keyword, (_, a, b, c) => {
            const word = a ?? b ?? c;
            if (!keywords.has(word)) keywords.set(word, []);
            keywords.get(word).push(site);
            return " ";
        });
        // A byte read any other way than by the forms scan reads would
        // escape the partition: refuse it, so the partition stays complete.
        if (scan(line, (name, test) => add(name, test, site))) unread.push(site);
    });
}

/// The classes of bytes that the source `text` (a step's function) tells
/// apart: for a byte, or 256 for the end, the key of its class, which is
/// the set of the byte tests of `text` it passes.
export function local(text) {
    const found = new Map();
    for (const raw of text.split("\n")) {
        const line = raw.replace(/\/\/.*$/, "").replace(keyword, " ");
        if (line.trim()) scan(line, (name, test) => found.set(name, test));
    }
    const tests = [...found.entries()];
    return (byte) =>
        byte === 256
            ? "end"
            : tests
                  .filter(([, test]) => test(byte))
                  .map(([name]) => name)
                  .join(" ") || "other";
}

// ASCII classes: bytes that every predicate of the region answers alike.
function partition(found) {
    const groups = new Map();
    for (let b = 0; b < 128; b += 1) {
        const key = [...found.values()].map((p) => (p.test(b) ? 1 : 0)).join("");
        if (!groups.has(key)) groups.set(key, []);
        groups.get(key).push(b);
    }
    return [...groups.values()];
}

// Outside ASCII, a scalar is read by its Unicode classes, whether it case
// folds, its UTF-8 width, whether it is whitespace, and the scalars and
// ranges the source names.
const tables = readFileSync(join(root, "util/tables.rs"), "utf8");
const table = (name) =>
    new RegExp(String.raw`pub static ${name}: \[\w+; \d+\] = \[([^\]]*)\]`)
        .exec(tables)[1]
        .split(",")
        .map((n) => n.trim())
        .filter(Boolean)
        .map(Number);
const pages = table("PAGES");
const blocks = table("BLOCKS");
const foldPages = table("FOLD_PAGES");
const foldLeaves = table("FOLD_LEAVES");
const wide = [...regions.markdown].filter(([name]) => /^[=[]/.test(name)).map(([, p]) => p.test);

function unicodeKey(c) {
    const mask = blocks[pages[c >> 8] + (c & 255)];
    const folds = c < 1 << 17 && foldLeaves[foldPages[c >> 6] * 64 + (c & 63)] !== 0;
    const width = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    const space = /\s/u.test(String.fromCodePoint(c)) ? 1 : 0;
    return `u${mask}.${folds ? 1 : 0}.${width}.${space}.${wide.map((test) => (test(c) ? 1 : 0)).join("")}`;
}

const ascii = partition(regions.markdown);
const byteClass = new Array(128);
ascii.forEach((members, index) => members.forEach((b) => (byteClass[b] = `a${index}`)));
const unicode = new Map();
for (let c = 128; c <= 0x10ffff; c += 1) {
    if (c >= 0xd800 && c <= 0xdfff) continue;
    const key = unicodeKey(c);
    if (!unicode.has(key)) unicode.set(key, []);
    const members = unicode.get(key);
    if (members.length < 2) members.push(c);
    else members.count = (members.count ?? 2) + 1;
}

/// The end of the input, as the parser's coverage records it.
export const END = 0x110000;

/// The class of scalar `c`.
const classes = new Map();
export function classify(c) {
    if (c === END) return "end";
    if (c < 128) return byteClass[c];
    let found = classes.get(c);
    if (found === undefined) {
        found = unicodeKey(c);
        classes.set(c, found);
    }
    return found;
}

/// Up to two members of each class, so that equality between input bytes
/// can be told from inequality.
export function representatives() {
    const out = new Map();
    ascii.forEach((members, index) => out.set(`a${index}`, members.slice(0, 2)));
    for (const [key, members] of unicode) out.set(key, members.slice(0, 2));
    return out;
}

/// The keywords: words the source compares whole slices with.
export const words = [...keywords.keys()].sort();

if (unread.length) {
    throw new Error(`bytes read by a comparison the partition does not model: ${unread.join(", ")}`);
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
    const show = (b) => (b > 32 && b < 127 ? String.fromCharCode(b) : `\\x${b.toString(16).padStart(2, "0")}`);
    for (const [region, found] of Object.entries(regions)) {
        const groups = partition(found);
        console.log(`${region}: ${found.size} predicates, ${groups.size ?? groups.length} classes of 128 ASCII bytes`);
        console.log("  " + groups.map((m) => m.map(show).join("")).join("  "));
        if (process.argv.includes("--sites")) {
            for (const [name, p] of found) console.log("   ", name, p.sites.slice(0, 6).join(" "));
        }
    }
    console.log(`unicode: ${unicode.size} classes of non-ASCII scalars`);
    if (process.argv.includes("--unicode")) {
        for (const [key, members] of unicode) {
            console.log(
                `  ${key} n=${members.count ?? members.length} e.g. U+${members[0].toString(16).toUpperCase()}`
            );
        }
    }
    console.log(`keywords: ${words.join(" ")}`);
}
