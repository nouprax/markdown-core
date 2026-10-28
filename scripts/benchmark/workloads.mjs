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
 * A workload is a document and a stream or a script. Documents are generated
 * here from tracked sources: the dialect encoding of every grammar corpus
 * certificate, the canonical AST cases (correctness set), and the scale and
 * adversarial shapes built with the corpus's word generator in both
 * alphabets. A stream is a rule over the document (the chunkings of section
 * 3.2), so a stream never repeats the document's bytes. A script is a
 * document a session opens on and the steps that follow it: the edit
 * families of section 3.3, which flush and then edit, the scripted
 * identity cases (4.5), the hard cases of plan 12.7 and the refused edits
 * (4.13).
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

export const WORKLOAD_VERSION = "incremental-workloads-v4";
export const SCRIPT_HEADER = "markdown-core incremental script 2";
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
 * A document under construction: its text, the parts it joins and the named
 * sites its scripts edit. A site is a byte range; a position is a range of
 * length zero. Byte lengths are tracked as the text grows, so a megabyte
 * document is built in linear time.
 */
class Writer {
    constructor() {
        this.chunks = [];
        this.bytes = 0;
        this.sites = [];
        this.parts = [];
        this.partStart = 0;
    }
    put(text) {
        this.chunks.push(text);
        this.bytes += bytes(text);
        return this;
    }
    /** Write `text` as the range of site `kind`. */
    site(kind, text) {
        const start = this.bytes;
        this.put(text);
        this.sites.push({ kind, start, end: this.bytes });
        return this;
    }
    at(kind) {
        this.sites.push({ kind, start: this.bytes, end: this.bytes });
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
function proseSection(out, i, letters, prefix = "", paragraph = "paragraph") {
    const w = wordsOf(i, letters);
    const line = (write) => {
        out.put(prefix);
        write();
        out.put("\n");
    };
    const gap = () => out.put(`${prefix.trimEnd()}\n`);
    line(() =>
        out
            .put("## ")
            .at("start:heading")
            .site("heading", `${w(1)} ${w(2, 2)}`)
    );
    gap();
    line(() =>
        out
            .at("start:paragraph")
            .site(paragraph, `${w(3)} *${w(4)}* **${w(5, 2)}** \`${w(6)}\` [${w(7)}][r${i}] ${w(8)}[^f${i}] ${w(9)}`)
    );
    line(() => out.site(paragraph, `${w(10, 3)} ${w(11)} [^m${i}] ${w(12)}.`));
    gap();
    line(() =>
        out
            .put("- ")
            .at("start:item")
            .site("item", `${w(13)} ${w(14)}`)
    );
    line(() => out.put("- ").site("item", w(15, 2)));
    gap();
    line(() => out.at("start:code").put("```"));
    line(() => out.site("code", `${w(16)} ${w(17)}`));
    line(() => out.put("```"));
    gap();
    line(() => out.put(`[r${i}]: /`).site("destination", w(18)));
    gap();
    line(() => out.put(`[^f${i}]: `).site("footnote", `${w(19)} ${w(20)}`));
    gap();
}

function listSection(out, i, letters) {
    const w = wordsOf(i, letters);
    out.put("- ")
        .at("start:item")
        .site("paragraph", `${w(1)} ${w(2)}`)
        .put("\n");
    out.put("  ").site("paragraph", w(3, 2)).put("\n\n");
    out.put("  - ").site("item", w(4)).put("\n");
    out.put("    - ")
        .site("item3", `${w(5)} ${w(6)}`)
        .put("\n\n");
    out.put("  ```\n  ")
        .site("code", `${w(7)} ${w(8)}`)
        .put("\n  ```\n\n");
}

const QUOTE_PREFIX = "> > > > ";

function tableRow(out, i, letters) {
    const w = wordsOf(i, letters);
    out.put("| ")
        .site("cell", `${w(1)} *${w(2)}*`)
        .put(" | ")
        .site("cell", w(3))
        .put(" | ")
        .site("cell", w(4))
        .put(" |\n");
}

/* A grid cell is a fixed number of characters: geometry counts scalars. */
const GRID = 12;
const gridCell = (text) => `${text}${" ".repeat(GRID - 1 - [...text].length)}`;
function gridRow(out, i, letters) {
    const w = wordsOf(i, letters);
    out.put("| ")
        .site("cell", gridCell(w(1)))
        .put("| ")
        .site("cell", gridCell(w(2)))
        .put("|\n");
    out.put(`+${"-".repeat(GRID)}+${"-".repeat(GRID)}+\n`);
}

function refsHead(out, letters) {
    const w = wordsOf(0, letters);
    for (let k = 0; k < 4; k++)
        out.put(`[d${k}]: /`)
            .site("destination", w(k + 1))
            .put(` "${w(k + 5)}"\n`);
    out.put("\n");
    for (let k = 0; k < 2; k++)
        out.put(`[^n${k}]: `)
            .site("footnote", `${w(k + 9)} ${w(k + 11)}`)
            .put("\n\n");
}

function refsSection(out, i, letters) {
    const w = wordsOf(i + 1, letters);
    out.put("## ").at("start:heading").site("heading", w(1, 2)).put("\n\n");
    out.at("start:paragraph")
        .site(
            "paragraph",
            `${w(2)} [${w(3)}][d0] ${w(4)} [${w(5)}][d1] [^n0] ${w(6)} [${w(7)}][d2] [${w(8)}][d3] ${w(9)}[^n1].`
        )
        .put("\n\n");
}

function flatSection(out, i, letters) {
    const w = wordsOf(i, letters);
    out.at("start:paragraph")
        .site("paragraph", `${w(1)} ${w(2)}`)
        .put("\n\n");
    out.put("# ").at("start:heading").site("heading", w(3)).put("\n\n");
    out.at("start:paragraph").site("paragraph", w(4, 2)).put("\n\n");
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
        out.put("> [!note] ").site("heading", wordsOf(0, letters)(1)).put("\n");
        fill(out, size, (i) => proseSection(out, i + 1, letters, QUOTE_PREFIX, "quote"));
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
 * not a size, and stays at 1,000. The three streamed paragraphs are built at
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
    "short-lines"(out, size, letters) {
        fill(out, size, (i) => {
            const w = wordsOf(i, letters);
            out.put(`${w(1)} ${w(2)} ${w(3, 2)}\n`);
        });
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
const FIXED_SIZES = Object.freeze({ "unclosed-strong": [65536], "single-line": [65536], "short-lines": [65536] });

/**
 * The shapes whose cost per chunk the language keeps local, which 6.2 holds
 * flat: every scale shape, the 10,000-item list, the 1,000 nested quotes and,
 * from step 8, the paragraph of short lines.
 */
export const LOCAL_SHAPES = Object.freeze([...Object.keys(SCALE_SHAPES), "long-list", "deep-quotes", "short-lines"]);

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
        parts: out.parts,
        sites: out.sites
    };
}

export function shapeSizes(shape) {
    return FIXED_SIZES[shape] ?? SIZES;
}

/* ----------------------------------------------------- generic analysis */

/** Lines with their byte ranges; `end` excludes and `next` includes the line ending. */
function linesOf(buffer) {
    const lines = [];
    let start = 0;
    for (let at = 0; at < buffer.length; at++) {
        const byte = buffer[at];
        if (byte === 0x0a || byte === 0x0d) {
            const next = byte === 0x0d && buffer[at + 1] === 0x0a ? at + 2 : at + 1;
            lines.push({ start, end: at, next });
            start = next;
            at = next - 1;
        }
    }
    if (start < buffer.length) lines.push({ start, end: buffer.length, next: buffer.length });
    return lines;
}

const ending = (byte) => byte === 0x0a || byte === 0x0d;

/** The line holding byte `at` of a non-empty text, found without reading the rest of the text. */
function lineAt(buffer, at) {
    /* The LF of a CR LF ends the line its CR ends. */
    if (buffer[at] === 0x0a && at > 0 && buffer[at - 1] === 0x0d) at--;
    let start = at;
    while (start > 0 && !ending(buffer[start - 1])) start--;
    let end = at;
    while (end < buffer.length && !ending(buffer[end])) end++;
    const next = end === buffer.length ? end : buffer[end] === 0x0d && buffer[end + 1] === 0x0a ? end + 2 : end + 1;
    return { start, end, next };
}

const blank = (buffer, line) => /^[ \t]*$/u.test(buffer.subarray(line.start, line.end).toString("latin1"));
const letter = /\p{L}/u;

/** Every position between two letters. */
function letterJoints(buffer) {
    const joints = [];
    let previous = false;
    let offset = 0;
    for (const scalar of buffer.toString("utf8")) {
        const current = letter.test(scalar);
        if (previous && current) joints.push(offset);
        previous = current;
        offset += bytes(scalar);
    }
    return joints;
}

/** The position between two letters closest to `fraction` of the document. */
function wordPosition(buffer, fraction, joints = letterJoints(buffer)) {
    if (!joints.length) return null;
    const target = fraction * buffer.length;
    let low = 0;
    let high = joints.length - 1;
    while (low < high) {
        const middle = (low + high) >> 1;
        if (joints[middle] < target) low = middle + 1;
        else high = middle;
    }
    const before = joints[Math.max(0, low - 1)];
    return Math.abs(before - target) <= Math.abs(joints[low] - target) ? before : joints[low];
}

const pick = (items, fraction) => items[Math.min(items.length - 1, Math.floor(fraction * items.length))];
const POSITIONS = Object.freeze({ beginning: 0, middle: 0.5, end: 0.999 });

/** Snap a byte offset back to the nearest scalar boundary. */
function snap(buffer, at) {
    while (!isBoundary(buffer, at)) at--;
    return at;
}

/* ------------------------------------------------------------- scripts */

/**
 * A script: the steps a subject takes after it opens on its document. It
 * keeps the text each step applies to, so each edit's offsets are computed
 * against the text before that edit. A session streams until it flushes
 * and takes edits only after (plan 12.3), so an edit script flushes first.
 */
class Script {
    constructor(name, family, text) {
        this.name = name;
        this.family = family;
        this.buffer = Buffer.from(text);
        this.steps = [];
    }
    append(text) {
        this.steps.push({ kind: "append", text });
        this.buffer = Buffer.concat([this.buffer, Buffer.from(text)]);
        return this;
    }
    flush() {
        this.steps.push({ kind: "flush" });
        return this;
    }
    edit(start, end, text) {
        assert.ok(start <= end && end <= this.buffer.length, `${this.name}: an edit past the text`);
        assert.ok(isBoundary(this.buffer, start) && isBoundary(this.buffer, end), `${this.name}: an edit in a scalar`);
        this.steps.push({ kind: "edit", start, end, text });
        this.buffer = Buffer.concat([this.buffer.subarray(0, start), Buffer.from(text), this.buffer.subarray(end)]);
        return this;
    }
    insert(at, text) {
        return this.edit(at, at, text);
    }
    remove(start, end) {
        return this.edit(start, end, "");
    }
    /** Insert `text` at `at`, then remove it again. */
    toggle(at, text) {
        return this.insert(at, text).remove(at, at + bytes(text));
    }
    /** An edit the subject refuses with `status`, its offsets in `unit`. */
    reject(unit, status, start, end, text) {
        this.steps.push({ kind: "reject", unit, status, start, end, text });
        return this;
    }
    expect(...values) {
        this.steps.at(-1).expect = [...(this.steps.at(-1).expect ?? []), values.join(" ")];
        return this;
    }
    /** The bytes [from, to) of the new text the edit above reads again (gates 4.12). */
    reread(from, to) {
        assert.equal(this.steps.at(-1).kind, "edit");
        return this.expect("reread", from, to);
    }
    /** The bytes of the new text the edit above reads again lie in [from, to) (gates 4.12). */
    within(from, to) {
        assert.equal(this.steps.at(-1).kind, "edit");
        return this.expect("within", from, to);
    }
    /** The step as the text before it: where an undo puts the text back. */
    inverse(step, before) {
        return {
            start: step.start,
            end: step.start + bytes(step.text),
            text: before.subarray(step.start, step.end).toString("utf8")
        };
    }
}

/* -------------------------------------------------- edit families (3.3) */

export const EDIT_FAMILIES = Object.freeze([
    "typing",
    "lines",
    "markers",
    "ranges",
    "far",
    "declarations",
    "undo",
    "random"
]);
export const RANDOM_SEEDS = 16;
const RANDOM_STEPS = 64;
const TYPING_STEPS = 64;
const FAR_STEPS = 64;
const PASTE_BYTES = 2048;

/** A script of `family` on `text` that has flushed, ready for its edits. */
const editing = (name, family, text) => new Script(name, family, text).flush();

/** The scalars a script types, spelled in the document's alphabet. */
const typed = (letters, index) => letters[(index * 7 + 3) % letters.length];

function typingScript(name, text, at, letters) {
    const script = editing(name, "typing", text);
    let cursor = at;
    let typedCount = 0;
    for (let step = 0; step < TYPING_STEPS; step++) {
        /* Eight scalars forward, then a run of three backspaces. */
        if (step % 11 >= 8 && typedCount > 0) {
            const start = snap(script.buffer, cursor - 1);
            script.remove(start, cursor);
            cursor = start;
            typedCount--;
        } else {
            const scalar = typed(letters, step);
            script.insert(cursor, scalar);
            cursor += bytes(scalar);
            typedCount++;
        }
    }
    return script;
}

/** The sites of `kind`. */
const sitesOf = (document, kind) => document.sites.filter((site) => site.kind === kind);

/** Where `typing` types: the middle of one site of each kind, or a word in the middle of the text. */
function typingSites(document) {
    const kinds = ["paragraph", "item3", "item", "cell", "heading", "code", "quote", "footnote"];
    const found = kinds.filter((kind) => sitesOf(document, kind).length);
    if (found.length)
        return found.map((kind) => {
            const site = pick(sitesOf(document, kind), 0.5);
            return { kind, at: site.start + Math.floor((site.end - site.start) / 2) };
        });
    const buffer = Buffer.from(document.text);
    const at = wordPosition(buffer, 0.5);
    return at === null ? [] : [{ kind: "word", at }];
}

function linesScripts(document) {
    const scripts = [];
    for (const [where, fraction] of Object.entries(POSITIONS)) {
        const script = editing(`lines-${where}`, "lines", document.text);
        const lines = () => linesOf(script.buffer);
        /* Enter inside a paragraph, and back. */
        const inside = wordPosition(script.buffer, fraction);
        if (inside !== null) script.toggle(inside, "\n");
        /* A blank line between two blocks, inserted and deleted. */
        const blanks = lines().filter(
            (line, index, all) => blank(script.buffer, line) && index > 0 && !blank(script.buffer, all[index - 1])
        );
        if (blanks.length) script.toggle(pick(blanks, fraction).start, "\n");
        /* Join two lines of one paragraph. */
        const joins = lines().filter(
            (line, index, all) =>
                index + 1 < all.length && !blank(script.buffer, line) && !blank(script.buffer, all[index + 1])
        );
        if (joins.length) {
            const line = pick(joins, fraction);
            script.edit(line.end, line.next, " ");
        }
        /* Merge two paragraphs by deleting the blank line between them. */
        const merges = lines().filter(
            (line, index, all) =>
                index > 0 &&
                index + 1 < all.length &&
                blank(script.buffer, line) &&
                !blank(script.buffer, all[index - 1]) &&
                !blank(script.buffer, all[index + 1])
        );
        if (merges.length) {
            const line = pick(merges, fraction);
            script.remove(line.start, line.next);
        }
        if (script.steps.length > 1) scripts.push(script);
    }
    return scripts;
}

/* Section 3.3's markers, each added at the start of a block and removed. A
 * marker that belongs on its own line goes on the line after the block's
 * first line. */
const MARKERS = Object.freeze([
    ["> ", "prefix"],
    ["- ", "prefix"],
    ["1. ", "prefix"],
    ["# ", "prefix"],
    ["    ", "prefix"],
    ["```\n", "prefix"],
    ["===\n", "after"],
    ["| --- |\n", "after"],
    [": x\n", "after"]
]);

function markerScripts(document) {
    const kinds = [...new Set(document.sites.map((site) => site.kind).filter((kind) => kind.startsWith("start:")))];
    const buffer = Buffer.from(document.text);
    const starts = kinds.length
        ? kinds.map((kind) => [kind.slice("start:".length), pick(sitesOf(document, kind), 0.5).start])
        : (() => {
              const lines = linesOf(buffer).filter((line) => !blank(buffer, line));
              return lines.length ? [["line", pick(lines, 0.5).start]] : [];
          })();
    return starts.map(([kind, at]) => {
        const script = editing(`markers-${kind}`, "markers", document.text);
        /* The line that holds `at`; a start of line belongs to the line it starts. */
        const line = linesOf(script.buffer).find(
            (candidate) => candidate.start <= at && (at < candidate.next || candidate.next === script.buffer.length)
        );
        for (const [marker, placement] of MARKERS) script.toggle(placement === "prefix" ? at : line.next, marker);
        return script;
    });
}

/** A section to paste: prose in the document's alphabet, at least 2 KB. */
function pasteText(letters) {
    const out = new Writer();
    fill(out, PASTE_BYTES, (i) => proseSection(out, 100000 + i, letters));
    return out.text();
}

function rangesScript(document, letters) {
    const script = editing("ranges", "ranges", document.text);
    const blocks = () => {
        const result = [];
        let open = null;
        for (const line of linesOf(script.buffer)) {
            if (blank(script.buffer, line)) {
                if (open) result.push(open);
                open = null;
            } else if (open) open.next = line.next;
            else open = { start: line.start, next: line.next };
        }
        if (open) result.push(open);
        return result;
    };
    /* Paste a 2 KB section at a block boundary in the middle. */
    const middle = pick(blocks(), 0.5);
    if (middle) script.insert(middle.start, pasteText(letters));
    /* Delete a section. */
    const doomed = pick(blocks(), 0.5);
    if (doomed) script.remove(doomed.start, doomed.next);
    /* Select a paragraph's whole text and type a replacement. The two edits
     * above moved it, so its text is found again. */
    const paragraph = pick(sitesOf(document, "paragraph"), 0.5);
    const line = pick(
        linesOf(script.buffer).filter((candidate) => !blank(script.buffer, candidate)),
        0.5
    );
    const found = paragraph
        ? script.buffer.indexOf(Buffer.from(document.text).subarray(paragraph.start, paragraph.end))
        : -1;
    const target =
        found >= 0
            ? { start: found, end: found + paragraph.end - paragraph.start }
            : line && { start: line.start, end: line.end };
    if (target && target.end > target.start) {
        let cursor = target.start;
        for (let index = 0; index < 8; index++) {
            const scalar = typed(letters, index);
            script.edit(cursor, index ? cursor : target.end, scalar);
            cursor += bytes(scalar);
        }
    }
    return script;
}

function farScript(document, letters) {
    const script = editing("far", "far", document.text);
    for (let step = 0; step < FAR_STEPS; step++) {
        const buffer = script.buffer;
        /* The first or the last line that is not blank, read from its end of the text. */
        let line = buffer.length ? lineAt(buffer, step % 2 ? buffer.length - 1 : 0) : null;
        while (line && blank(buffer, line))
            line =
                step % 2
                    ? line.start > 0
                        ? lineAt(buffer, line.start - 1)
                        : null
                    : line.next < buffer.length
                      ? lineAt(buffer, line.next)
                      : null;
        if (!line) break;
        script.insert(line.end, typed(letters, step));
    }
    return script;
}

function declarationsScript(document, letters) {
    const destinations = sitesOf(document, "destination");
    if (!destinations.length) return null;
    const script = editing("declarations", "declarations", document.text);
    const w = wordsOf(999983, letters);
    /* Change a reference destination. */
    const destination = pick(destinations, 0.5);
    script.edit(destination.start, destination.end, w(1));
    /* A duplicate reference label, ahead of the original so that it wins. */
    const label = /\[([^\]]+)\]: \/$/u.exec(
        script.buffer.subarray(Math.max(0, destination.start - 64), destination.start).toString("utf8")
    )?.[1];
    if (label) script.toggle(0, `[${label}]: /${w(2)}\n\n`);
    /* A heading whose label collides with an existing one. */
    const heading = pick(sitesOf(document, "heading"), 0.5);
    if (heading) {
        const text = Buffer.from(document.text).subarray(heading.start, heading.end);
        const at = script.buffer.indexOf(text);
        const line = linesOf(script.buffer).find((candidate) => candidate.start <= at && at <= candidate.end);
        if (line) script.toggle(line.start, `## ${text.toString("utf8")}\n\n`);
    }
    /* A footnote definition for a label the text references but never defines. */
    const undefinedLabel = /\[\^(m[0-9]+)\]/u.exec(script.buffer.toString("utf8"));
    if (undefinedLabel) script.toggle(script.buffer.length, `\n[^${undefinedLabel[1]}]: ${w(3)}\n`);
    /* An inline note in a paragraph. */
    const paragraph = pick(sitesOf(document, "paragraph"), 0.5) ?? pick(sitesOf(document, "quote"), 0.5);
    if (paragraph) {
        const text = Buffer.from(document.text).subarray(paragraph.start, paragraph.end);
        const at = script.buffer.indexOf(text);
        if (at >= 0) script.toggle(at + text.length, `^[${w(4)} ${w(5)}]`);
    }
    return script;
}

/** Each edit of a script followed by its inverse, then by itself again so the next edit's offsets hold. */
function undoScript(script, text) {
    const undo = editing(`undo-${script.name}`, "undo", text);
    for (const step of script.steps) {
        if (step.kind !== "edit") continue;
        const before = undo.buffer;
        undo.edit(step.start, step.end, step.text);
        const inverse = undo.inverse(step, before);
        undo.edit(inverse.start, inverse.end, inverse.text);
        undo.edit(step.start, step.end, step.text);
    }
    return undo;
}

/* What random steps insert: letters, the characters that open and close
 * constructs, and the line endings, including a bare CR, a CR LF split across
 * steps, and NUL. */
const RANDOM_PIECES = [
    ..."*_`[]()#>-|:!^{}<~$%=+\\@",
    " ",
    "  ",
    "\n",
    "\n\n",
    "\r",
    "\r\n",
    "\0",
    "1. ",
    "- ",
    "> ",
    "```",
    "    "
];

function randomScript(document, letters, seed) {
    const script = editing(`random-${seed}`, "random", document.text);
    const draw = random(0x72616e64 + seed);
    const piece = () =>
        draw.below(3) ? typed(letters, draw.below(letters.length)) : RANDOM_PIECES[draw.below(RANDOM_PIECES.length)];
    const insertion = () => Array.from({ length: 1 + draw.below(8) }, piece).join("");
    for (let step = 0; step < RANDOM_STEPS; step++) {
        const buffer = script.buffer;
        const operation = buffer.length ? draw.below(3) : 0;
        let start;
        let end;
        if (draw.below(2)) {
            /* Line granularity: whole lines, from the line holding a uniformly
             * drawn byte through up to two more. */
            let line = buffer.length ? lineAt(buffer, draw.below(buffer.length)) : { start: 0, next: 0 };
            start = line.start;
            for (let more = draw.below(3); more > 0 && line.next < buffer.length; more--)
                line = lineAt(buffer, line.next);
            end = operation ? line.next : start;
        } else {
            /* Byte granularity, moved back to a scalar boundary. */
            start = snap(buffer, draw.below(buffer.length + 1));
            end = operation ? snap(buffer, Math.min(buffer.length, start + 1 + draw.below(16))) : start;
            if (end < start) end = start;
        }
        const text = operation === 1 ? "" : insertion();
        if (start === end && text === "") continue;
        script.edit(start, end, text);
    }
    return script;
}

/**
 * Every edit script of one document, each flushing before its first edit.
 * `families` narrows the set, and `random` runs `seeds` seeds.
 */
export function editScripts(document, families = EDIT_FAMILIES, seeds = RANDOM_SEEDS) {
    const letters = alphabets[document.alphabet ?? "ascii"];
    const wanted = new Set(families);
    const base = [];
    const all = wanted.has("undo");
    if (wanted.has("typing") || all)
        for (const site of typingSites(document))
            base.push(
                typingScript(`typing-${site.kind}`, document.text, snap(Buffer.from(document.text), site.at), letters)
            );
    if (wanted.has("lines") || all) base.push(...linesScripts(document));
    if (wanted.has("markers") || all) base.push(...markerScripts(document));
    if (wanted.has("ranges") || all) base.push(rangesScript(document, letters));
    if (wanted.has("far") || all) base.push(farScript(document, letters));
    if (wanted.has("declarations") || all) base.push(declarationsScript(document, letters));
    const scripts = base.filter((script) => script && script.steps.length > 1);
    const result = scripts.filter((script) => wanted.has(script.family));
    if (all) result.push(...scripts.map((script) => undoScript(script, document.text)));
    if (wanted.has("random"))
        for (let seed = 1; seed <= seeds; seed++) result.push(randomScript(document, letters, seed));
    return result;
}

/* ------------------------------- scripted identity (4.5) and 4.12, 4.13 */

/**
 * The steps whose outcome the plan states exactly (sections 5.9, 8 and
 * 12.5), written as the ids kept, made and retired by position. `kept KIND
 * OLD NEW` names a node that keeps its id; `new KIND AT` a node with a fresh
 * id; `retired KIND AT` an old node nothing continues; `changed KIND AT` a
 * node whose value changed, and `only` that no node outside the `changed`
 * lines changed. Positions are the byte offset where a node's source starts,
 * in the text before the step for `retired` and after it otherwise. The
 * append cases stream; the edit cases flush first.
 */
export function identityScripts() {
    const cases = [];
    /* The edit cases are the `ids` family: they run once the session takes
     * edits (plan 12.9), and the append cases run now. */
    const add = (name, text, build) => {
        const script = new Script(`identity-${name}`, "identity", text);
        build(script);
        if (script.steps[0].kind === "flush") script.family = "ids";
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
    add("type-at-paragraph-start", "alpha beta\n\ngamma delta\n", (s) =>
        s.flush().insert(0, "x").expect("kept", "Paragraph", 0, 0).expect("kept", "Paragraph", 12, 13)
    );
    add("insert-before-paragraph", "alpha\n", (s) =>
        s.flush().insert(0, "new\n\n").expect("new", "Paragraph", 0).expect("kept", "Paragraph", 0, 5)
    );
    add("delete-first-word", "alpha beta\n", (s) => s.flush().remove(0, 6).expect("kept", "Paragraph", 0, 0));
    add("delete-sibling", "one\n\ntwo\n\nthree\n", (s) =>
        s
            .flush()
            .remove(5, 10)
            .expect("kept", "Paragraph", 0, 0)
            .expect("retired", "Paragraph", 5)
            .expect("kept", "Paragraph", 10, 5)
    );
    add("merge-paragraphs", "one\n\ntwo\n", (s) =>
        s.flush().remove(4, 5).expect("kept", "Paragraph", 0, 0).expect("retired", "Paragraph", 5)
    );
    add("paragraph-to-setext", "title\n", (s) =>
        s.flush().insert(6, "===\n").expect("new", "Heading", 0).expect("retired", "Paragraph", 0)
    );
    add("paragraph-into-quote", "text\n", (s) =>
        s
            .flush()
            .insert(0, "> ")
            .expect("new", "Callout", 0)
            .expect("new", "Paragraph", 2)
            .expect("retired", "Paragraph", 0)
    );
    /* Deleting the outer note's closer moves the inner note to the
     * paragraph, a new owner; deleting its opener then moves the inner note
     * within the paragraph. */
    add("unwrap-nested-note", "a^[b ^[c] d] e\n", (s) =>
        s.flush().remove(11, 12).expect("new", "Footnote", 5).remove(1, 3).expect("kept", "Footnote", 5, 3)
    );
    {
        const text = Array.from({ length: 200 }, (_, i) => `${word(i * 3 + 1)} ${word(i * 3 + 2)}\n\n`).join("");
        add("line-at-top", text, (s) =>
            s
                .flush()
                .insert(0, "new line\n")
                .expect("changed", "Document", 0)
                .expect("changed", "Paragraph", 0)
                .expect("changed", "Text", 0)
                .expect("changed", "SoftBreak", 8)
                .expect("only")
        );
    }
    /* The links name the heading's label, which the document resolves to
     * the heading, so a new anchor changes only the heading and the
     * Document. */
    add("heading-anchor-targets", "# Target {#one}\n\nsee [a][Target] and [b][Target]\n", (s) =>
        s.flush().edit(11, 14, "two").expect("changed", "Document", 0).expect("changed", "Heading", 0).expect("only")
    );
    {
        const paragraphs = Array.from({ length: 1000 }, (_, i) => `${word(i * 3 + 1)} ${word(i * 3 + 2)}\n\n`);
        const text = paragraphs.join("");
        const at = bytes(paragraphs.slice(0, 4).join(""));
        add("typing-paragraph-5-of-1000", text, (s) =>
            s
                .flush()
                .insert(at + 1, "x")
                .expect("changed", "Document", 0)
                .expect("changed", "Paragraph", at)
                .expect("changed", "Text", at)
                .expect("only")
        );
    }
    return cases;
}

/**
 * The hard cases of plan 12.7, one script each, in the `hard` family. Every
 * one runs every oracle; the ones whose re-read the plan fixes state it with
 * `reread` (exact) or `within` (an upper bound), as 4.12 reads them.
 */
export function hardScripts() {
    const cases = [];
    const add = (name, text, build) => {
        const script = editing(`hard-${name}`, "hard", text);
        build(script);
        cases.push({ name: `hard-${name}`, text, script });
    };
    const items = (count) => Array.from({ length: count }, (_, i) => `- i${String(i).padStart(2, "0")}\n`).join("");
    /* Typing in a paragraph re-reads it and the paragraph before it, and
     * resyncs at the next. */
    add("type-in-middle-paragraph", "one\n\ntwo\n\nthree\n", (s) =>
        s.insert(6, "x").reread(0, 11).remove(6, 7).reread(0, 10)
    );
    /* Fences, comments and HTML blocks opened before unchanged text and
     * closed again. */
    for (const [kind, opener, closer] of [
        ["fence", "```\n", "```\n"],
        ["comment", "%%\n", "%%\n"],
        ["html", "<pre>\n", "</pre>\n"],
        ["directive", ":::note\n", ":::\n"]
    ])
        add(`open-${kind}`, "one\n\ntwo\n\nthree\n\nfour\n", (s) =>
            s
                .insert(5, opener)
                .insert(5 + bytes(opener) + 5, closer)
                .remove(5, 5 + bytes(opener))
        );
    /* An edit that opens a candidate before unchanged children: deleting `x`
     * makes `%%` a comment opener that the last line closes (12.3). */
    add("candidate-before-children", "%%x\n\nbody\n\n%%\n", (s) => s.remove(2, 3).insert(2, "x"));
    /* Container syntax: `>`, a list marker and indentation, typed and
     * deleted, at the top level and inside a list item. */
    add("container-markers", "one\n\n- two\n\n  three\n\nfour\n", (s) =>
        s
            .toggle(0, "> ")
            .toggle(0, "- ")
            .toggle(0, "    ")
            .toggle(7, "> ")
            .toggle(7, "- ")
            .toggle(12, "  ")
            .remove(12, 14)
            .insert(12, "  ")
    );
    /* Definitions: a reference, a footnote, a heading label and a specimen
     * added, renamed and removed, with their uses before and after them. */
    add("reference-definition", "see [a][r] and [r]\n\n[r]: /one\n\nafter [r]\n", (s) =>
        s.edit(26, 29, "two").edit(21, 22, "q").edit(21, 22, "r").remove(20, 31).insert(20, "[r]: /three\n\n")
    );
    add("footnote-definition", "note[^n] here\n\nmore[^n]\n", (s) =>
        s.insert(24, "\n[^n]: body\n").edit(27, 28, "m").edit(27, 28, "n").remove(24, 36)
    );
    add("heading-label", "see [Title] and [x][Title]\n\n# Title\n\n# Other\n", (s) =>
        s.edit(30, 35, "Other").edit(30, 35, "Title").toggle(28, "# Title\n\n")
    );
    add("specimen-definition", "see @spec and (@spec)\n\nbody\n", (s) =>
        s
            .insert(0, "(@spec) Defined.\n\n")
            .edit(2, 6, "spek")
            .edit(2, 6, "spec")
            .insert(bytes("(@spec) Defined.\n\nsee @spec and (@spec)\n\nbody\n"), "\n(@spec) Again.\n")
            .remove(0, 18)
    );
    /* The text of a heading with an automatic anchor that a link targets. */
    add("automatic-anchor", "# Title\n\nsee [x](#title)\n\n# Title\n", (s) =>
        s.insert(7, "s").remove(7, 8).edit(2, 7, "Other")
    );
    /* Splitting and joining paragraphs and list items. */
    add("split-and-join", "alpha beta\n\n- one two\n- three\n", (s) =>
        s.insert(5, "\n\n").remove(5, 7).insert(18, "\n- ").remove(18, 21).remove(21, 23)
    );
    /* The rules that write backwards, each reaching the block before. */
    add("setext-underline", "one\n\ntitle\n\ntwo\n", (s) => s.insert(11, "===\n").remove(11, 15).insert(11, "---\n"));
    add("definition-term", "one\n\nTerm\n\ntwo\n", (s) => s.insert(10, ": def\n").remove(10, 16));
    add("table-caption", "one\n\n| h |\n|---|\n| v |\n\ntwo\n", (s) => s.insert(5, "Table: cap\n\n").remove(5, 17));
    add("block-identifier", "one\n\nA para\n\ntwo\n", (s) =>
        s.insert(11, " #id#").remove(11, 16).insert(12, "#id#\n\n")
    );
    add("table-header", "one\n\n| h |\n| v |\n\ntwo\n", (s) =>
        s.insert(11, "|---|\n").remove(11, 17).edit(5, 10, "| j |")
    );
    add("lazy-continuation", "> quote\n\nlazy\n\ntwo\n", (s) => s.remove(8, 9).insert(8, "\n").insert(9, "> "));
    /* The Properties envelope, opened and closed at the top. */
    add("properties", "name: x\n\ntext\n", (s) => s.insert(0, "---\n").insert(12, "---\n").remove(12, 16).remove(0, 4));
    /* A loose list turning tight and back; an early item made loose in a
     * long list re-reads only the items around it. */
    add("loose-and-tight", "- a\n- b\n\n- c\n", (s) => s.remove(8, 9).insert(8, "\n").remove(4, 8).insert(4, "- b\n"));
    add("early-item-loose", items(50), (s) => s.insert(12, "\n").within(0, 19).remove(12, 13).within(0, 18));
    add("type-in-early-item", items(50), (s) => s.insert(15, "x").within(6, 19).remove(15, 16).within(6, 18));
    return cases;
}

/**
 * The refused edits of 4.13, against a document that ends in a four-byte
 * scalar, so an offset can fall inside one in either unit: an edit while the
 * subject streams, and after the flush a range past the end, a reversed
 * range, and an offset inside that scalar at one of its continuation bytes
 * in UTF-8 and between its two units in UTF-16. An edit that holds follows
 * them all.
 */
export function rejectionScript(text) {
    const script = new Script("rejections", "rejections", text);
    const buffer = Buffer.from(text);
    const wide = scalarBoundaries(buffer).find((at, index, all) => all[index + 1] - at === 4);
    const lengths = { utf8: buffer.length, utf16: text.length };
    for (const unit of Object.keys(lengths)) script.reject(unit, "streaming", 0, 0, "a");
    script.flush();
    for (const [unit, length] of Object.entries(lengths)) {
        script.reject(unit, "out-of-bounds", length + 1, length + 1, "a");
        script.reject(unit, "out-of-bounds", 0, length + 1, "a");
        script.reject(unit, "out-of-bounds", 2, 1, "a");
    }
    script.reject("utf8", "out-of-bounds", wide + 1, wide + 1, "a");
    script.reject("utf8", "out-of-bounds", 0, wide + 3, "a");
    const units = buffer.subarray(0, wide).toString("utf8").length;
    script.reject("utf16", "out-of-bounds", units + 1, units + 1, "a");
    script.reject("utf16", "out-of-bounds", 0, units + 1, "a");
    return script.insert(0, "a").remove(wide + 1, wide + 5);
}

/* ------------------------------------------------------------ encoding */

const hex = (text) => (text === "" ? "-" : Buffer.from(text).toString("hex"));
const unhex = (value) => (value === "-" ? "" : Buffer.from(value, "hex").toString("utf8"));

/** Scripts in the tracked text format: a `script NAME FAMILY` line, then one step per line. */
export function formatScripts(scripts) {
    const lines = [SCRIPT_HEADER];
    for (const script of scripts) {
        lines.push(`script ${script.name} ${script.family}`);
        for (const step of script.steps) {
            if (step.kind === "append") lines.push(`append ${hex(step.text)}`);
            else if (step.kind === "flush") lines.push("flush");
            else if (step.kind === "edit") lines.push(`edit ${step.start} ${step.end} ${hex(step.text)}`);
            else lines.push(`reject ${step.unit} ${step.status} ${step.start} ${step.end} ${hex(step.text)}`);
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
            steps.push({ kind, text: unhex(fields[0]) });
        } else if (kind === "flush") {
            assert.ok(steps && !fields.length, `malformed flush: ${line}`);
            steps.push({ kind });
        } else if (kind === "edit") {
            assert.ok(steps && fields.length === 3, `malformed edit: ${line}`);
            steps.push({ kind, start: Number(fields[0]), end: Number(fields[1]), text: unhex(fields[2]) });
        } else if (kind === "reject") {
            assert.ok(steps && fields.length === 5, `malformed reject: ${line}`);
            steps.push({
                kind,
                unit: fields[0],
                status: fields[1],
                start: Number(fields[2]),
                end: Number(fields[3]),
                text: unhex(fields[4])
            });
        } else if (kind === "expect") {
            assert.ok(steps?.length, `an expectation before any step: ${line}`);
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
 * adversarial shapes at their smallest size, with every edit family, every
 * stream family, `splits` on every document up to 2 KB, the scripted
 * identity cases, the hard cases and the refused edits. Returns the files to
 * write, keyed by their path under `specs/incremental/`.
 */
export function correctnessSet() {
    const files = new Map();
    const manifest = [];
    const documents = [];
    const script = (name, text, scripts) => {
        files.set(`documents/${name}.md`, text);
        files.set(`scripts/${name}.script`, formatScripts(scripts));
        manifest.push(`document documents/${name}.md`, `script documents/${name}.md scripts/${name}.script`);
    };
    for (const name of canonicalNames()) {
        documents.push({
            name,
            path: `../canonical-ast/${name}.md`,
            text: fs.readFileSync(path.join(root, CANONICAL, `${name}.md`), "utf8"),
            alphabet: "ascii",
            sites: [],
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
    for (const { name, text, script: steps } of [...identityScripts(), ...hardScripts()]) script(name, text, [steps]);
    /* The refused edits run against a document that ends in a four-byte
     * scalar, so an offset can fall inside one in either unit. */
    const refused = `${fs.readFileSync(path.join(root, CANONICAL, "inlines.md"), "utf8")}\u{20000}\n`;
    script("rejections", refused, [rejectionScript(refused)]);
    for (const document of documents) {
        manifest.push(`document ${document.path}${document.parts ? ` parts ${document.parts.join(" ")}` : ""}`);
        const file = `scripts/${document.name}.script`;
        files.set(file, formatScripts(editScripts(document)));
        manifest.push(`script ${document.path} ${file}`);
        const buffer = Buffer.from(document.text);
        const streams = document.shape
            ? ["tokens", "rows"]
            : ["tokens", "scalars", "rows", ...(buffer.length <= SPLIT_LIMIT ? ["splits"] : [])];
        for (const family of streams) manifest.push(`stream ${document.path} ${family}`);
    }
    files.set("token-sizes.txt", `${TOKEN_SIZES.join(" ")}\n`);
    /* The manifest declares the families in the order of sections 3.1 to
     * 3.3, 4.5 and 4.13, and the C tests run one family each. */
    const declared = ["documents", "identity", "ids", "hard", "rejections", ...EDIT_FAMILIES, ...STREAM_FAMILIES].map(
        (family) => `family ${family}`
    );
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
 * The edit workloads (5.1) are one document each and its edit scripts:
 * every grammar corpus document and every shape at its smallest size with
 * the families of `BENCHMARK_EDITS`, one random seed, measured at every
 * edit, and the larger sizes with `typing` and `far` alone, measured in at
 * most `EDIT_WINDOWS` windows a script, because each `reparse` edit there
 * parses up to a megabyte. `undo` replays the other families' edits
 * backwards, and is left to the correctness set.
 *
 * `set` is `all`, or `corpus` for the grammar corpus's workloads alone: the
 * shapes are measured at their four sizes together or not at all.
 */
export const BENCHMARK_SETS = Object.freeze(["all", "corpus"]);
export const BENCHMARK_EDITS = Object.freeze(EDIT_FAMILIES.filter((family) => family !== "undo"));
const LARGE_EDITS = Object.freeze(["typing", "far"]);
export const EDIT_WINDOWS = 4;

export function benchmarkWorkloads(set = "all") {
    assert.ok(BENCHMARK_SETS.includes(set), `unknown workload set ${set}`);
    const documents = [];
    const workloads = [];
    const edits = [];
    const edited = (document, stepwise) =>
        edits.push({
            document,
            name: `${document.name}.edits`,
            scripts: editScripts(document, stepwise ? BENCHMARK_EDITS : LARGE_EDITS, 1),
            stepwise
        });
    for (const entry of buildGrammarCorpus().cases.filter((item) => item.side === "dialect")) {
        const document = { name: entry.name, text: entry.text, alphabet: entry.alphabet, parts: null, sites: [] };
        documents.push(document);
        for (const family of ["tokens", "scalars"])
            workloads.push({ document, family, name: `${document.name}.${family}`, local: false, stepwise: false });
        edited(document, true);
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
                edited(document, size === shapeSizes(shape)[0]);
            }
    return { documents, workloads, edits: edits.filter((entry) => entry.scripts.length) };
}

/**
 * Write the benchmark workloads of `set` under `directory`, or only those
 * `names` lists, stream and edit workloads alike: each document, each edit
 * workload's scripts and the token sizes. Returns the index of what it
 * wrote, whose digest identifies the workload the way the corpus digest
 * identifies the one-shot benchmark's.
 */
export function writeBenchmarkWorkloads(directory, set = "all", names = []) {
    const generated = benchmarkWorkloads(set);
    const wanted = new Set(names);
    const known = new Set([...generated.workloads, ...generated.edits].map((workload) => workload.name));
    const unknown = names.filter((name) => !known.has(name));
    if (unknown.length) throw new Error(`the ${set} set has no workload named ${unknown.join(", ")}`);
    const chosen = (list) => (wanted.size ? list.filter((workload) => wanted.has(workload.name)) : list);
    const workloads = chosen(generated.workloads);
    const edits = chosen(generated.edits);
    const used = new Set([...workloads, ...edits].map((workload) => workload.document));
    const documents = generated.documents.filter((document) => used.has(document));
    fs.rmSync(directory, { recursive: true, force: true });
    fs.mkdirSync(path.join(directory, "documents"), { recursive: true });
    fs.mkdirSync(path.join(directory, "scripts"), { recursive: true });
    for (const document of documents) {
        document.path = `documents/${document.name}.md`;
        document.sha256 = hash(document.text);
        document.bytes = bytes(document.text);
        fs.writeFileSync(path.join(directory, document.path), document.text);
    }
    for (const workload of edits) {
        const text = formatScripts(workload.scripts);
        workload.path = `scripts/${workload.document.name}.script`;
        workload.sha256 = hash(text);
        fs.writeFileSync(path.join(directory, workload.path), text);
    }
    fs.writeFileSync(path.join(directory, "token-sizes.txt"), `${TOKEN_SIZES.join(" ")}\n`);
    return {
        version: WORKLOAD_VERSION,
        set,
        digest: hash(
            JSON.stringify([
                TOKEN_SIZES,
                workloads.map((workload) => [workload.name, workload.document.sha256, workload.family]),
                edits.map((workload) => [workload.name, workload.document.sha256, workload.sha256])
            ])
        ),
        documents,
        workloads,
        edits
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
