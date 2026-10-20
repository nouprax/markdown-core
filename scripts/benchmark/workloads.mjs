#!/usr/bin/env node
/**
 * THE INCREMENTAL WORKLOADS: every document and script the incremental gates
 * run (docs/plans/2026-09-29-incremental-gates.md, section 3).
 *
 * One generator, two outputs. The correctness set is small and tracked under
 * `specs/incremental/`, because the C test graph has no scripting-language
 * dependency; `--check` keeps it current. The benchmark workloads are large
 * and written when the benchmark runs, as the grammar corpus is.
 *
 * A workload is a document and a stream. Documents are generated here from
 * tracked sources: the dialect encoding of every grammar corpus certificate,
 * the canonical AST cases (correctness set), and the scale and adversarial
 * shapes built with the corpus's word generator in both alphabets. A stream
 * is a rule over the document (the chunkings of section 3.2), so a stream
 * never repeats the document's bytes. The scripted identity cases (4.5) are
 * scripts: a document a session opens on, and the appends that follow it.
 *
 *   node scripts/benchmark/workloads.mjs            write specs/incremental
 *   node scripts/benchmark/workloads.mjs --check    fail if it is stale
 */
import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { alphabets, buildGrammarCorpus, hash, word } from "./corpus.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
export const CORRECTNESS_SET = "specs/incremental";
const CANONICAL = "specs/canonical-ast";

export const WORKLOAD_VERSION = "incremental-workloads-v2";
export const SCRIPT_HEADER = "markdown-core stream script 1";
export const MANIFEST_HEADER = "markdown-core incremental manifest 2";
export const SIZES = Object.freeze([16384, 65536, 262144, 1048576]);
export const STREAM_FAMILIES = Object.freeze(["tokens", "scalars", "rows", "splits"]);
const SPLIT_LIMIT = 2048;

/* ---------------------------------------------------------------- text */

const bytes = (text) => Buffer.byteLength(text);

/** The byte offset of every scalar boundary of `buffer`, 0 and its length included. */
export function scalarBoundaries(buffer) {
    const result = [];
    for (let at = 0; at < buffer.length; at++) if ((buffer[at] & 0xc0) !== 0x80) result.push(at);
    result.push(buffer.length);
    return result;
}

const isBoundary = (buffer, at) => at === 0 || at === buffer.length || (buffer[at] & 0xc0) !== 0x80;

/** A deterministic 32-bit generator (mulberry32); nothing about it is shared with C. */
export function random(seed) {
    let state = seed >>> 0;
    const next = () => {
        state = (state + 0x6d2b79f5) >>> 0;
        let value = state;
        value = Math.imul(value ^ (value >>> 15), value | 1);
        value ^= value + Math.imul(value ^ (value >>> 7), value | 61);
        return ((value ^ (value >>> 14)) >>> 0) / 4294967296;
    };
    return { next, below: (count) => Math.floor(next() * count) };
}

/* ------------------------------------------------------------- streams */

/* The token chunk sizes: a fixed sequence of 4,096 sizes, each entry of a
 * table whose mean is four bytes appearing equally often, in a seeded
 * shuffled order, so the sequence's mean is exactly four. It is tracked
 * beside the correctness set, so every platform chunks by the same numbers
 * without sharing a generator. */
const TOKEN_TABLE = [1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 7, 12];
export const TOKEN_SIZES = (() => {
    const sizes = Array.from({ length: 4096 }, (_, index) => TOKEN_TABLE[index % TOKEN_TABLE.length]);
    const draw = random(0x746f6b65);
    for (let index = sizes.length - 1; index > 0; index--) {
        const other = draw.below(index + 1);
        [sizes[index], sizes[other]] = [sizes[other], sizes[index]];
    }
    return sizes;
})();

/** The chunk ends of one stream family over a document, or, for `splits`, each split's point. */
export function streamChunks(buffer, family) {
    const ends = [];
    if (family === "tokens") {
        let at = 0;
        for (let index = 0; at < buffer.length; index++) {
            at = Math.min(buffer.length, at + TOKEN_SIZES[index % TOKEN_SIZES.length]);
            while (!isBoundary(buffer, at)) at++;
            ends.push(at);
        }
    } else if (family === "scalars") {
        ends.push(...scalarBoundaries(buffer).slice(1));
    } else if (family === "rows") {
        for (let at = 0; at < buffer.length; at++) {
            const byte = buffer[at];
            if (byte === 0x0a || (byte === 0x0d && buffer[at + 1] !== 0x0a)) ends.push(at + 1);
        }
        if (buffer.length && ends.at(-1) !== buffer.length) ends.push(buffer.length);
    } else if (family === "splits") {
        return [buffer.length ? scalarBoundaries(buffer).slice(1, -1) : []].map((points) =>
            points.map((point) => [point, buffer.length])
        )[0];
    } else {
        throw new Error(`unknown stream family ${family}`);
    }
    return ends;
}

/* ------------------------------------------------------------ documents */

/**
 * A document under construction: its text and the parts it joins. Byte
 * lengths are tracked as the text grows, so a megabyte document is built in
 * linear time.
 */
class Writer {
    constructor() {
        this.chunks = [];
        this.bytes = 0;
        this.parts = [];
        this.partStart = 0;
    }
    put(text) {
        this.chunks.push(text);
        this.bytes += bytes(text);
        return this;
    }
    part() {
        if (this.bytes > this.partStart) this.parts.push(this.bytes - this.partStart);
        this.partStart = this.bytes;
    }
    text() {
        return this.chunks.join("");
    }
}

/** The words of one section: distinct across sections, in one alphabet. */
const wordsOf =
    (section, letters) =>
    (index, width = 1) =>
        word(section * 61 + index * 7 + 5, width, letters);

/* Each shape writes one section at a time; `head` and `tail` frame the
 * sections where the shape needs them. A section is a part when the shape
 * joins independent blocks, and the whole document is one part when the
 * shape is one construct. */
function proseSection(out, i, letters, prefix = "") {
    const w = wordsOf(i, letters);
    const line = (text) => out.put(`${prefix}${text}\n`);
    const gap = () => out.put(`${prefix.trimEnd()}\n`);
    line(`## ${w(1)} ${w(2, 2)}`);
    gap();
    line(`${w(3)} *${w(4)}* **${w(5, 2)}** \`${w(6)}\` [${w(7)}][r${i}] ${w(8)}[^f${i}] ${w(9)}`);
    line(`${w(10, 3)} ${w(11)} [^m${i}] ${w(12)}.`);
    gap();
    line(`- ${w(13)} ${w(14)}`);
    line(`- ${w(15, 2)}`);
    gap();
    line("```");
    line(`${w(16)} ${w(17)}`);
    line("```");
    gap();
    line(`[r${i}]: /${w(18)}`);
    gap();
    line(`[^f${i}]: ${w(19)} ${w(20)}`);
    gap();
}

function listSection(out, i, letters) {
    const w = wordsOf(i, letters);
    out.put(`- ${w(1)} ${w(2)}\n`);
    out.put(`  ${w(3, 2)}\n\n`);
    out.put(`  - ${w(4)}\n`);
    out.put(`    - ${w(5)} ${w(6)}\n\n`);
    out.put(`  \`\`\`\n  ${w(7)} ${w(8)}\n  \`\`\`\n\n`);
}

const QUOTE_PREFIX = "> > > > ";

function tableRow(out, i, letters) {
    const w = wordsOf(i, letters);
    out.put(`| ${w(1)} *${w(2)}* | ${w(3)} | ${w(4)} |\n`);
}

/* A grid cell is a fixed number of characters: geometry counts scalars. */
const GRID = 12;
const gridCell = (text) => `${text}${" ".repeat(GRID - 1 - [...text].length)}`;
function gridRow(out, i, letters) {
    const w = wordsOf(i, letters);
    out.put(`| ${gridCell(w(1))}| ${gridCell(w(2))}|\n`);
    out.put(`+${"-".repeat(GRID)}+${"-".repeat(GRID)}+\n`);
}

function refsHead(out, letters) {
    const w = wordsOf(0, letters);
    for (let k = 0; k < 4; k++) out.put(`[d${k}]: /${w(k + 1)} "${w(k + 5)}"\n`);
    out.put("\n");
    for (let k = 0; k < 2; k++) out.put(`[^n${k}]: ${w(k + 9)} ${w(k + 11)}\n\n`);
}

function refsSection(out, i, letters) {
    const w = wordsOf(i + 1, letters);
    out.put(`## ${w(1, 2)}\n\n`);
    out.put(`${w(2)} [${w(3)}][d0] ${w(4)} [${w(5)}][d1] [^n0] ${w(6)} [${w(7)}][d2] [${w(8)}][d3] ${w(9)}[^n1].\n\n`);
}

function flatSection(out, i, letters) {
    const w = wordsOf(i, letters);
    out.put(`${w(1)} ${w(2)}\n\n`);
    out.put(`# ${w(3)}\n\n`);
    out.put(`${w(4, 2)}\n\n`);
    out.put("***\n\n");
}

/** Sections until the document holds at least `size` bytes. */
function fill(out, size, write, from = 0) {
    let i = from;
    while (out.bytes < size) write(i++);
    return i;
}

/**
 * The scale shapes (section 3.1): one shape at four sizes, built by repeating
 * a fixed section with distinct words.
 */
export const SCALE_SHAPES = Object.freeze({
    prose(out, size, letters) {
        fill(out, size, (i) => {
            proseSection(out, i, letters);
            out.part();
        });
    },
    list(out, size, letters) {
        fill(out, size, (i) => listSection(out, i, letters));
        out.part();
    },
    quote(out, size, letters) {
        out.put(`> [!note] ${wordsOf(0, letters)(1)}\n`);
        fill(out, size, (i) => proseSection(out, i + 1, letters, QUOTE_PREFIX));
        out.part();
    },
    table(out, size, letters) {
        const w = wordsOf(0, letters);
        out.put(`| ${w(1)} | ${w(2)} | ${w(3)} |\n| --- | --- | --- |\n`);
        let i = fill(out, size / 2, (row) => tableRow(out, row + 1, letters));
        out.put("\n");
        out.part();
        out.put(`+${"-".repeat(GRID)}+${"-".repeat(GRID)}+\n`);
        out.put(`| ${gridCell(w(4))}| ${gridCell(w(5))}|\n`);
        out.put(`+${"=".repeat(GRID)}+${"=".repeat(GRID)}+\n`);
        fill(out, size, (row) => gridRow(out, row, letters), i + 1);
        out.put("\n");
        out.part();
    },
    refs(out, size, letters) {
        refsHead(out, letters);
        out.part();
        fill(out, size, (i) => {
            refsSection(out, i, letters);
            out.part();
        });
    },
    flat(out, size, letters) {
        fill(out, size, (i) => {
            flatSection(out, i, letters);
            out.part();
        });
    }
});

/* Paragraphs that use the label `t`, the definition of the definition
 * shapes, written with or without it. */
function definitionUses(out, size, letters) {
    const per = SIZES.at(-1) / 10000;
    let references = 0;
    fill(out, size, (i) => {
        const w = wordsOf(i + 1, letters);
        const start = out.bytes;
        let text = w(1);
        while (bytes(text) < 8 * per) {
            text += ` [${w(2 + (references % 5))}][t] ${w(8)}`;
            references++;
        }
        out.put(`${text}\n\n`);
        assert.ok(out.bytes > start);
        out.part();
    });
}

/* The stray early openers, one line each. */
const OPENERS = Object.freeze({
    fence: "```\n",
    html: "<pre>\n",
    comment: "%%\n",
    directive: ":::note\n"
});

/**
 * The adversarial shapes (section 3.1). A shape whose name counts items
 * (10,000 list items, 10,000 references) has that count at the largest size
 * and proportionally fewer at the smaller ones, because the smallest size
 * cannot hold ten thousand items; the nesting depth of the quotes is a depth,
 * not a size, and stays at 1,000. The two streamed paragraphs are built at
 * the 64 KB they are named with.
 */
export const ADVERSARIAL_SHAPES = Object.freeze({
    ...Object.fromEntries(
        Object.entries(OPENERS).map(([name, opener]) => [
            `opener-${name}`,
            (out, size, letters) => {
                out.put(opener);
                fill(out, size, (i) => proseSection(out, i, letters));
                out.part();
            }
        ])
    ),
    "long-list"(out, size, letters) {
        const count = Math.ceil((10000 * size) / SIZES.at(-1));
        const per = size / count;
        for (let i = 0; i < count; i++) {
            const w = wordsOf(i, letters);
            const start = out.bytes;
            out.put(`- ${w(1)}`);
            while (out.bytes - start < per - 1) out.put(` ${w(2)}`);
            out.put("\n");
        }
        out.part();
    },
    "deep-quotes"(out, size, letters) {
        let i = fill(out, size / 2, (section) => {
            proseSection(out, section, letters);
            out.part();
        });
        out.put(`${"> ".repeat(1000)}${wordsOf(i, letters)(1)} ${wordsOf(i, letters)(2)}\n\n`);
        out.part();
        fill(
            out,
            size,
            (section) => {
                proseSection(out, section, letters);
                out.part();
            },
            i + 1
        );
    },
    "wide-definition"(out, size, letters) {
        out.put(`[t]: /${wordsOf(0, letters)(1)}\n\n`);
        out.part();
        definitionUses(out, size, letters);
    },
    "late-definition"(out, size, letters) {
        definitionUses(out, size, letters);
        out.put(`[t]: /${wordsOf(0, letters)(1)}\n`);
        out.part();
    },
    "repeated-heading"(out, size, letters) {
        const same = `${wordsOf(0, letters)(1)} ${wordsOf(0, letters)(2)}`;
        fill(out, size, (i) => {
            const w = wordsOf(i + 1, letters);
            out.put(`## ${same}\n\n`);
            out.put(`${w(1)} [${w(2)}](#${same.replaceAll(" ", "-")}) ${w(3)}\n\n`);
            out.part();
        });
    },
    "unclosed-strong"(out, size, letters) {
        out.put("**");
        fill(out, size, (i) => {
            const w = wordsOf(i, letters);
            out.put(`${w(1)} ${w(2)} ${w(3, 2)}\n`);
        });
        out.part();
    },
    "single-line"(out, size, letters) {
        fill(out, size - 1, (i) => out.put(`${wordsOf(i, letters)(1)} `));
        out.put("\n");
        out.part();
    },
    "unclosed-properties"(out, size, letters) {
        out.put("---\n");
        fill(out, size, (i) => {
            const w = wordsOf(i, letters);
            out.put(`${w(1)}: ${w(2)} ${w(3)}\n`);
        });
        out.part();
    },
    "unclosed-grid"(out, size, letters) {
        const w = wordsOf(0, letters);
        out.put(`+${"-".repeat(GRID)}+${"-".repeat(GRID)}+\n`);
        out.put(`| ${gridCell(w(1))}| ${gridCell(w(2))}|\n`);
        out.put(`+${"=".repeat(GRID)}+${"=".repeat(GRID)}+\n`);
        fill(out, size, (row) => gridRow(out, row + 1, letters));
        out.put(`| ${gridCell(w(3))}| ${gridCell(w(4))}|\n`);
        out.part();
    }
});
const FIXED_SIZES = Object.freeze({ "unclosed-strong": [65536], "single-line": [65536] });

/**
 * The shapes whose cost per chunk the language keeps local, which 6.2 holds
 * flat: every scale shape, the 10,000-item list and the 1,000 nested quotes.
 */
export const LOCAL_SHAPES = Object.freeze([...Object.keys(SCALE_SHAPES), "long-list", "deep-quotes"]);

/** A generated document: shape, alphabet and size. */
export function shapeDocument(shape, alphabet, size) {
    const build = SCALE_SHAPES[shape] ?? ADVERSARIAL_SHAPES[shape];
    assert.ok(build, `unknown shape ${shape}`);
    const out = new Writer();
    build(out, size, alphabets[alphabet]);
    out.part();
    const text = out.text();
    assert.equal(bytes(text), out.bytes);
    assert.equal(
        out.parts.reduce((sum, part) => sum + part, 0),
        out.bytes,
        "the parts cover the document"
    );
    return {
        name: `${shape}-${alphabet}-${size / 1024}k`,
        shape,
        alphabet,
        size,
        text,
        parts: out.parts
    };
}

export function shapeSizes(shape) {
    return FIXED_SIZES[shape] ?? SIZES;
}

/* ------------------------------------------------------------- scripts */

/** A script: the appends a session takes after it opens on its document. */
class Script {
    constructor(name, family) {
        this.name = name;
        this.family = family;
        this.steps = [];
    }
    append(text) {
        this.steps.push({ text });
        return this;
    }
    expect(...values) {
        this.steps.at(-1).expect = [...(this.steps.at(-1).expect ?? []), values.join(" ")];
        return this;
    }
}

/**
 * The appends whose outcome the plan states exactly (sections 5.9 and 8),
 * written as the ids kept, made and retired by position. `kept KIND OLD NEW`
 * names a node that keeps its id; `new KIND AT` a node with a fresh id;
 * `retired KIND AT` an old node nothing continues; `changed KIND AT` a node
 * whose value changed, and `only` that no node outside the `changed` lines
 * changed. Positions are the byte offset where a node's source starts, in the
 * text before the step for `retired` and after it otherwise.
 */
export function identityScripts() {
    const cases = [];
    const add = (name, text, build) => {
        const script = new Script(`identity-${name}`, "identity");
        build(script);
        cases.push({ name: `identity-${name}`, text, script });
    };
    add("type-at-paragraph-end", "alpha beta\n\n", (s) =>
        s
            .append("gamma")
            .expect("kept", "Paragraph", 0, 0)
            .expect("new", "Paragraph", 12)
            .append(" delta")
            .expect("kept", "Paragraph", 0, 0)
            .expect("kept", "Paragraph", 12, 12)
            .expect("changed", "Document", 0)
            .expect("changed", "Paragraph", 12)
            .expect("changed", "Text", 12)
            .expect("only")
    );
    add("paragraph-to-setext-and-back", "title\n", (s) =>
        s
            .append("==")
            .expect("new", "Heading", 0)
            .expect("retired", "Paragraph", 0)
            .append("x")
            .expect("new", "Paragraph", 0)
            .expect("retired", "Heading", 0)
    );
    {
        const text = "| a | b |\n| - | - |\n| c | d |\n";
        const at = bytes(text);
        add("row-appended-to-table", text, (s) =>
            s
                .append("| e | f |\n")
                .expect("changed", "Document", 0)
                .expect("changed", "Table", 0)
                .expect("changed", "TableRow", at)
                .expect("changed", "TableCell", at + 1)
                .expect("changed", "Text", at + 2)
                .expect("changed", "TableCell", at + 5)
                .expect("changed", "Text", at + 6)
                .expect("only")
        );
    }
    {
        const text = Array.from({ length: 1000 }, (_, i) => `- ${word(i * 3 + 1)} ${word(i * 3 + 2)}\n`).join("");
        const at = bytes(text);
        add("item-appended-to-long-list", text, (s) =>
            s
                .append("- last\n")
                .expect("changed", "Document", 0)
                .expect("changed", "List", 0)
                .expect("changed", "ListItem", at)
                .expect("changed", "Paragraph", at + 2)
                .expect("changed", "Text", at + 2)
                .expect("only")
        );
    }
    return cases;
}

/* ------------------------------------------------------------ encoding */

const hex = (text) => (text === "" ? "-" : Buffer.from(text).toString("hex"));

/** Scripts in the tracked text format: a `script NAME FAMILY` line, then one step per line. */
export function formatScripts(scripts) {
    const lines = [SCRIPT_HEADER];
    for (const script of scripts) {
        lines.push(`script ${script.name} ${script.family}`);
        for (const step of script.steps) {
            lines.push(`append ${hex(step.text)}`);
            for (const expectation of step.expect ?? []) lines.push(`expect ${expectation}`);
        }
    }
    return `${lines.join("\n")}\n`;
}

/** Read scripts back; the inverse of `formatScripts`. */
export function parseScripts(text) {
    const lines = text.split("\n");
    assert.equal(lines[0], SCRIPT_HEADER);
    assert.equal(lines.at(-1), "");
    const scripts = [];
    for (const line of lines.slice(1, -1)) {
        const [kind, ...fields] = line.split(" ");
        const steps = scripts.at(-1)?.steps;
        if (kind === "script") {
            assert.equal(fields.length, 2, `malformed script line: ${line}`);
            scripts.push({ name: fields[0], family: fields[1], steps: [] });
        } else if (kind === "append") {
            assert.ok(steps && fields.length === 1, `malformed append: ${line}`);
            steps.push({ text: fields[0] === "-" ? "" : Buffer.from(fields[0], "hex").toString("utf8") });
        } else if (kind === "expect") {
            assert.ok(steps?.length, `an expectation before any append: ${line}`);
            steps.at(-1).expect = [...(steps.at(-1).expect ?? []), fields.join(" ")];
        } else {
            throw new Error(`unknown script line: ${line}`);
        }
    }
    return scripts;
}

/* ------------------------------------------------------------- outputs */

const canonicalNames = () =>
    JSON.parse(fs.readFileSync(path.join(root, CANONICAL, "manifest.json"), "utf8")).cases.map((entry) => entry.name);

/**
 * The correctness set (section 9): the canonical AST cases and the scale and
 * adversarial shapes at their smallest size, with every stream family,
 * `splits` on every document up to 2 KB, and the scripted identity cases.
 * Returns the files to write, keyed by their path under `specs/incremental/`.
 */
export function correctnessSet() {
    const files = new Map();
    const manifest = [];
    const documents = [];
    for (const name of canonicalNames()) {
        documents.push({
            name,
            path: `../canonical-ast/${name}.md`,
            text: fs.readFileSync(path.join(root, CANONICAL, `${name}.md`), "utf8"),
            parts: null
        });
    }
    for (const shape of [...Object.keys(SCALE_SHAPES), ...Object.keys(ADVERSARIAL_SHAPES)])
        for (const alphabet of Object.keys(alphabets)) {
            const document = shapeDocument(shape, alphabet, shapeSizes(shape)[0]);
            documents.push({ ...document, path: `documents/${document.name}.md` });
            files.set(`documents/${document.name}.md`, document.text);
        }
    /* The targeted cases run first, then the sweeps. */
    for (const { name, text, script } of identityScripts()) {
        files.set(`documents/${name}.md`, text);
        files.set(`scripts/${name}.stream`, formatScripts([script]));
        manifest.push(`document documents/${name}.md`, `script documents/${name}.md scripts/${name}.stream`);
    }
    for (const document of documents) {
        manifest.push(`document ${document.path}${document.parts ? ` parts ${document.parts.join(" ")}` : ""}`);
        const buffer = Buffer.from(document.text);
        const streams = document.shape
            ? ["tokens", "rows"]
            : ["tokens", "scalars", "rows", ...(buffer.length <= SPLIT_LIMIT ? ["splits"] : [])];
        for (const family of streams) manifest.push(`stream ${document.path} ${family}`);
    }
    files.set("token-sizes.txt", `${TOKEN_SIZES.join(" ")}\n`);
    /* The manifest declares the families in the order of sections 3.1 to
     * 3.2 and 4.5, and the C tests run one family each. */
    const declared = ["documents", "identity", ...STREAM_FAMILIES].map((family) => `family ${family}`);
    files.set("manifest.txt", `${[MANIFEST_HEADER, ...declared, ...manifest].join("\n")}\n`);
    return files;
}

/**
 * The benchmark workloads (section 9): every grammar corpus document with
 * `tokens` and `scalars`, and every scale and adversarial shape at all four
 * sizes with `tokens` and `rows`. Each workload is one document and one
 * stream family, is `local` when 6.2 holds its stream flat, and is
 * `stepwise` when 6.3 measures it at every step however long it is: a shape
 * 6.2 leaves to 6.3, at the shape's smallest size (decision G17).
 *
 * `set` is `all`, or `corpus` for the grammar corpus's workloads alone: the
 * shapes are measured at their four sizes together or not at all.
 */
export const BENCHMARK_SETS = Object.freeze(["all", "corpus"]);

export function benchmarkWorkloads(set = "all") {
    assert.ok(BENCHMARK_SETS.includes(set), `unknown workload set ${set}`);
    const documents = [];
    const workloads = [];
    for (const entry of buildGrammarCorpus().cases.filter((item) => item.side === "dialect")) {
        const document = { name: entry.name, text: entry.text, alphabet: entry.alphabet, parts: null };
        documents.push(document);
        for (const family of ["tokens", "scalars"])
            workloads.push({ document, family, name: `${document.name}.${family}`, local: false, stepwise: false });
    }
    for (const shape of set === "all" ? [...Object.keys(SCALE_SHAPES), ...Object.keys(ADVERSARIAL_SHAPES)] : [])
        for (const alphabet of Object.keys(alphabets))
            for (const size of shapeSizes(shape)) {
                const document = shapeDocument(shape, alphabet, size);
                documents.push(document);
                for (const family of ["tokens", "rows"])
                    workloads.push({
                        document,
                        family,
                        name: `${document.name}.${family}`,
                        local: LOCAL_SHAPES.includes(shape),
                        stepwise: !LOCAL_SHAPES.includes(shape) && size === shapeSizes(shape)[0]
                    });
            }
    return { documents, workloads };
}

/**
 * Write the benchmark workloads of `set` under `directory`, or only those
 * `names` lists: each document and the token sizes. Returns the index of what
 * it wrote, whose digest identifies the workload the way the corpus digest
 * identifies the one-shot benchmark's.
 */
export function writeBenchmarkWorkloads(directory, set = "all", names = []) {
    const generated = benchmarkWorkloads(set);
    const wanted = new Set(names);
    const unknown = names.filter((name) => !generated.workloads.some((workload) => workload.name === name));
    if (unknown.length) throw new Error(`the ${set} set has no workload named ${unknown.join(", ")}`);
    const workloads = wanted.size
        ? generated.workloads.filter((workload) => wanted.has(workload.name))
        : generated.workloads;
    const used = new Set(workloads.map((workload) => workload.document));
    const documents = generated.documents.filter((document) => used.has(document));
    fs.rmSync(directory, { recursive: true, force: true });
    fs.mkdirSync(path.join(directory, "documents"), { recursive: true });
    for (const document of documents) {
        document.path = `documents/${document.name}.md`;
        document.sha256 = hash(document.text);
        document.bytes = bytes(document.text);
        fs.writeFileSync(path.join(directory, document.path), document.text);
    }
    fs.writeFileSync(path.join(directory, "token-sizes.txt"), `${TOKEN_SIZES.join(" ")}\n`);
    return {
        version: WORKLOAD_VERSION,
        set,
        digest: hash(
            JSON.stringify([
                TOKEN_SIZES,
                workloads.map((workload) => [workload.name, workload.document.sha256, workload.family])
            ])
        ),
        documents,
        workloads
    };
}

function main() {
    const check = process.argv.includes("--check");
    const directory = path.join(root, CORRECTNESS_SET);
    const files = correctnessSet();
    const stale = [];
    const expected = new Set([...files.keys(), "README.md"]);
    for (const [relative, text] of files) {
        const absolute = path.join(directory, relative);
        const current = fs.existsSync(absolute) ? fs.readFileSync(absolute, "utf8") : null;
        if (current === text) continue;
        if (check) stale.push(relative);
        else {
            fs.mkdirSync(path.dirname(absolute), { recursive: true });
            fs.writeFileSync(absolute, text);
        }
    }
    const walk = (at) =>
        fs.existsSync(at)
            ? fs
                  .readdirSync(at, { withFileTypes: true })
                  .flatMap((entry) =>
                      entry.isDirectory() ? walk(path.join(at, entry.name)) : [path.join(at, entry.name)]
                  )
            : [];
    for (const absolute of walk(directory)) {
        const relative = path.relative(directory, absolute);
        if (expected.has(relative)) continue;
        if (check) stale.push(relative);
        else fs.rmSync(absolute);
    }
    if (stale.length) {
        console.error(
            `Stale incremental correctness set; run node scripts/benchmark/workloads.mjs:\n${stale
                .slice(0, 20)
                .map((file) => `  ${CORRECTNESS_SET}/${file}`)
                .join("\n")}`
        );
        process.exit(1);
    }
    console.log(
        check
            ? `The incremental correctness set is current: ${files.size} files.`
            : `Wrote ${files.size} files to ${CORRECTNESS_SET}.`
    );
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) main();
