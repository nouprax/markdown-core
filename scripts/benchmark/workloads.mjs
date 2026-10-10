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
 * A workload is a document and a script. Documents are generated here from
 * tracked sources: the dialect encoding of every grammar corpus certificate,
 * the canonical AST cases (correctness set), and the scale and adversarial
 * shapes built with the corpus's word generator in both alphabets. A script
 * is a sequence of steps whose offsets are UTF-8 bytes of the text before the
 * step; every offset falls on a scalar boundary and every text is well formed,
 * except in the invalid-argument cases. Stream scripts are rules over the
 * document (the chunkings of section 3.3), so a stream never repeats the
 * document's bytes.
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

export const WORKLOAD_VERSION = "incremental-workloads-v1";
export const SCRIPT_HEADER = "markdown-core edit script 1";
export const MANIFEST_HEADER = "markdown-core incremental manifest 1";
export const SIZES = Object.freeze([16384, 65536, 262144, 1048576]);
export const STREAM_FAMILIES = Object.freeze(["tokens", "scalars", "rows", "splits"]);
export const EDIT_FAMILIES = Object.freeze([
    "typing",
    "lines",
    "markers",
    "ranges",
    "far",
    "batch",
    "declarations",
    "undo",
    "random"
]);
/* The families whose effect the language itself keeps local (section 3.2). */
export const LOCAL_FAMILIES = Object.freeze(["typing", "lines", "ranges", "far", "batch"]);
export const RANDOM_SEEDS = 16;
const RANDOM_STEPS = 64;
const TYPING_STEPS = 64;
const FAR_STEPS = 64;
const BATCH_CURSORS = 16;
const BATCH_STEPS = 16;
const PASTE_BYTES = 2048;
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

/** Apply a batch of non-overlapping edits, each against the text before the batch. */
export function applyBatch(buffer, edits) {
    const sorted = [...edits].sort((left, right) => left.start - right.start);
    const pieces = [];
    let at = 0;
    for (const edit of sorted) {
        assert.ok(edit.start >= at && edit.end >= edit.start && edit.end <= buffer.length, "overlapping edits");
        assert.ok(isBoundary(buffer, edit.start) && isBoundary(buffer, edit.end), "an edit inside a scalar");
        pieces.push(buffer.subarray(at, edit.start), Buffer.from(edit.text));
        at = edit.end;
    }
    pieces.push(buffer.subarray(at));
    return Buffer.concat(pieces);
}

/** The batch that undoes `edits` on the text they produce. */
export function inverseBatch(buffer, edits) {
    let delta = 0;
    return [...edits]
        .sort((left, right) => left.start - right.start)
        .map((edit) => {
            const start = edit.start + delta;
            const length = bytes(edit.text);
            delta += length - (edit.end - edit.start);
            return { start, end: start + length, text: buffer.subarray(edit.start, edit.end).toString("utf8") };
        });
}

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
 * a fixed section with distinct words. The same script applies the same edit
 * at the same relative position at every size.
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

/* The stray early openers and their closers, one line each. */
const OPENERS = Object.freeze({
    fence: ["```\n", "```\n"],
    html: ["<pre>\n", "</pre>\n"],
    comment: ["%%\n", "%%\n"],
    directive: [":::note\n", ":::\n"]
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
        Object.entries(OPENERS).map(([name, [opener]]) => [
            `opener-${name}`,
            (out, size, letters) => {
                out.at("opener").put(opener);
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
            out.put("- ").at("start:item").site("item", w(1));
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
        out.put("> ".repeat(1000))
            .at("start:quote")
            .site("quote", `${wordsOf(i, letters)(1)} ${wordsOf(i, letters)(2)}`)
            .put("\n\n");
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
        const w0 = wordsOf(0, letters);
        out.put("[t]: /").site("destination", w0(1)).put("\n\n");
        out.part();
        const per = SIZES.at(-1) / 10000;
        let references = 0;
        fill(out, size, (i) => {
            const w = wordsOf(i + 1, letters);
            out.at("start:paragraph");
            const start = out.bytes;
            let text = w(1);
            while (bytes(text) < 8 * per) {
                text += ` [${w(2 + (references % 5))}][t] ${w(8)}`;
                references++;
            }
            out.site("paragraph", text).put("\n\n");
            assert.ok(out.bytes > start);
            out.part();
        });
    },
    "repeated-heading"(out, size, letters) {
        const same = `${wordsOf(0, letters)(1)} ${wordsOf(0, letters)(2)}`;
        fill(out, size, (i) => {
            const w = wordsOf(i + 1, letters);
            out.put("## ").at("start:heading").site("heading", same).put("\n\n");
            out.at("start:paragraph").site("paragraph", `${w(1)} [${w(2)}](#${same.replaceAll(" ", "-")}) ${w(3)}`);
            out.put("\n\n");
            out.part();
        });
    },
    "unclosed-strong"(out, size, letters) {
        out.at("start:paragraph").put("**");
        fill(out, size, (i) => {
            const w = wordsOf(i, letters);
            out.site("paragraph", `${w(1)} ${w(2)} ${w(3, 2)}`).put("\n");
        });
        out.part();
    },
    "single-line"(out, size, letters) {
        out.at("start:paragraph");
        fill(out, size - 1, (i) => out.site("paragraph", `${wordsOf(i, letters)(1)} `));
        out.put("\n");
        out.part();
    }
});
const FIXED_SIZES = Object.freeze({ "unclosed-strong": [65536], "single-line": [65536] });

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
        sites: out.sites,
        adversarial: Object.hasOwn(ADVERSARIAL_SHAPES, shape),
        opener: shape.startsWith("opener-") ? OPENERS[shape.slice("opener-".length)] : null
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

/* ------------------------------------------------------------- scripts */

/**
 * A script under construction. It keeps the text every step applies to, so
 * each step's offsets are computed against the text before that step.
 */
class Script {
    constructor(name, family, text) {
        this.name = name;
        this.family = family;
        this.buffer = Buffer.from(text);
        this.steps = [];
    }
    edit(edits) {
        this.steps.push({ kind: "edit", edits });
        this.buffer = applyBatch(this.buffer, edits);
        return this;
    }
    insert(at, text) {
        return this.edit([{ start: at, end: at, text }]);
    }
    remove(start, end) {
        return this.edit([{ start, end, text: "" }]);
    }
    /** Insert `text` at `at`, then remove it again. */
    toggle(at, text) {
        return this.insert(at, text).remove(at, at + bytes(text));
    }
    /** An edit batch the session refuses with `status`, offsets in `unit`. */
    reject(unit, status, ...values) {
        this.steps.push({ kind: "reject", unit, status, values });
        return this;
    }
    expect(...values) {
        this.steps.at(-1).expect = [...(this.steps.at(-1).expect ?? []), values.join(" ")];
        return this;
    }
}

/** The scalars a script types, spelled in the document's alphabet. */
const typed = (letters, index) => letters[(index * 7 + 3) % letters.length];

function typingScript(name, text, at, letters) {
    const script = new Script(name, "typing", text);
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

/** The sites of `kind`, falling back to generic positions when a document has none. */
const sitesOf = (document, kind) => document.sites.filter((site) => site.kind === kind);

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

/** Snap a byte offset inside the site back to the nearest scalar boundary. */
function snap(buffer, at) {
    while (!isBoundary(buffer, at)) at--;
    return at;
}

function linesScripts(document) {
    const scripts = [];
    for (const [where, fraction] of Object.entries(POSITIONS)) {
        const script = new Script(`lines-${where}`, "lines", document.text);
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
            script.edit([{ start: line.end, end: line.next, text: " " }]);
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
        if (script.steps.length) scripts.push(script);
    }
    return scripts;
}

/* Section 3.2's markers, each added at the start of a block and removed. A
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
        const script = new Script(`markers-${kind}`, "markers", document.text);
        const line = linesOf(script.buffer).find((candidate) => candidate.start <= at && at <= candidate.next);
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
    const script = new Script("ranges", "ranges", document.text);
    const blocks = () => {
        const lines = linesOf(script.buffer);
        const result = [];
        let open = null;
        for (const line of lines) {
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
    if (middle) script.insert(middle.start, `${pasteText(letters)}`);
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
            script.edit([{ start: cursor, end: index ? cursor : target.end, text: scalar }]);
            cursor += bytes(scalar);
        }
    }
    return script;
}

function farScript(document, letters) {
    const script = new Script("far", "far", document.text);
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

function batchScript(document, letters, seed) {
    const script = new Script("batch", "batch", document.text);
    const buffer = Buffer.from(document.text);
    const cursors = [];
    const joints = letterJoints(buffer);
    for (let index = 0; index < BATCH_CURSORS; index++) {
        const at = wordPosition(buffer, (index + 0.5) / BATCH_CURSORS, joints);
        if (at !== null && !cursors.includes(at)) cursors.push(at);
    }
    if (cursors.length < 2) return null;
    cursors.sort((left, right) => left - right);
    const draw = random(seed);
    for (let step = 0; step < BATCH_STEPS; step++) {
        const scalar = typed(letters, step);
        const edits = cursors.map((at) => ({ start: at, end: at, text: scalar }));
        /* Listed in a seeded shuffled order: the batch is a set. */
        for (let index = edits.length - 1; index > 0; index--) {
            const other = draw.below(index + 1);
            [edits[index], edits[other]] = [edits[other], edits[index]];
        }
        script.edit(edits);
        cursors.forEach((at, index) => (cursors[index] = at + (index + 1) * bytes(scalar)));
    }
    return script;
}

function declarationsScript(document, letters) {
    const destinations = sitesOf(document, "destination");
    if (!destinations.length) return null;
    const script = new Script("declarations", "declarations", document.text);
    const w = wordsOf(999983, letters);
    /* Change a reference destination. */
    const destination = pick(destinations, 0.5);
    script.edit([{ start: destination.start, end: destination.end, text: w(1) }]);
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

/** Each step of a script followed by its inverse, then by itself again so the next step's offsets hold. */
function undoScript(script, text) {
    const undo = new Script(`undo-${script.name}`, "undo", text);
    for (const step of script.steps) {
        if (step.kind !== "edit") continue;
        const before = undo.buffer;
        undo.edit(step.edits);
        undo.edit(inverseBatch(before, step.edits));
        undo.edit(step.edits);
    }
    return undo;
}

/* What random steps insert: letters, the characters that open and close
 * constructs, and the line endings, including a bare CR, a CR LF split across
 * steps, and NUL. */
const RANDOM_PIECES = [
    ..."*_`[]()#>-|:!^{}<~$%=+\\",
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
    const script = new Script(`random-${seed}`, "random", document.text);
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
        script.edit([{ start, end, text }]);
    }
    return script;
}

/** The families whose steps `undo` inverts. */
export const UNDONE_FAMILIES = Object.freeze(["typing", "lines", "markers", "ranges", "far", "batch", "declarations"]);

/**
 * Every edit script of one document. `families` narrows the set, and `undone`
 * the families `undo` inverts; the benchmark's grammar corpus documents run a
 * subset (section 9).
 */
export function editScripts(document, families = EDIT_FAMILIES, undone = UNDONE_FAMILIES) {
    const letters = alphabets[document.alphabet ?? "ascii"];
    const wanted = new Set(families);
    const inverted = new Set(wanted.has("undo") ? undone : []);
    const base = [];
    if (wanted.has("typing") || inverted.has("typing"))
        for (const site of typingSites(document))
            base.push(
                typingScript(`typing-${site.kind}`, document.text, snap(Buffer.from(document.text), site.at), letters)
            );
    if (wanted.has("lines") || inverted.has("lines")) base.push(...linesScripts(document));
    if (wanted.has("markers") || inverted.has("markers")) base.push(...markerScripts(document));
    if (wanted.has("ranges") || inverted.has("ranges")) base.push(rangesScript(document, letters));
    if (wanted.has("far") || inverted.has("far")) base.push(farScript(document, letters));
    if (wanted.has("batch") || inverted.has("batch")) base.push(batchScript(document, letters, 0x62617463));
    if (wanted.has("declarations") || inverted.has("declarations")) base.push(declarationsScript(document, letters));
    const scripts = base.filter((script) => script && script.steps.length);
    const result = scripts.filter((script) => wanted.has(script.family));
    for (const script of scripts) if (inverted.has(script.family)) result.push(undoScript(script, document.text));
    if (wanted.has("random"))
        for (let seed = 1; seed <= RANDOM_SEEDS; seed++) result.push(randomScript(document, letters, seed));
    /* A script is its steps; the text it built them against is not kept. */
    return result.map(({ name, family, steps }) => ({ name, family, steps }));
}

/* ------------------------------------------ scripted identity (4.5), 4.8 */

/**
 * The edits whose outcome the plan states exactly (sections 5.9 and 8),
 * written as the ids kept, made and retired by position. `kept KIND OLD NEW`
 * names a node that keeps its id; `new KIND AT` a node with a fresh id;
 * `retired KIND AT` an old node nothing continues; `changed KIND AT` a node
 * whose value changed, and `only` that no node outside the `changed` lines
 * changed. Positions are the byte offset where a node's scope starts, in the
 * text before the step for `retired` and after it otherwise.
 */
export function identityScripts() {
    const cases = [];
    const add = (name, text, build) => {
        const script = new Script(name, "identity", text);
        build(script);
        cases.push({ name: `identity-${name}`, text, script });
    };
    add("type-at-paragraph-start", "alpha beta\n\ngamma delta\n", (s) =>
        s.insert(0, "x").expect("kept", "Paragraph", 0, 0).expect("kept", "Paragraph", 12, 13)
    );
    add("insert-before-paragraph", "alpha\n", (s) =>
        s.insert(0, "new\n\n").expect("new", "Paragraph", 0).expect("kept", "Paragraph", 0, 5)
    );
    add("delete-first-word", "alpha beta\n", (s) => s.remove(0, 6).expect("kept", "Paragraph", 0, 0));
    add("delete-sibling", "one\n\ntwo\n\nthree\n", (s) =>
        s
            .remove(5, 10)
            .expect("kept", "Paragraph", 0, 0)
            .expect("retired", "Paragraph", 5)
            .expect("kept", "Paragraph", 10, 5)
    );
    add("merge-paragraphs", "one\n\ntwo\n", (s) =>
        s.remove(4, 5).expect("kept", "Paragraph", 0, 0).expect("retired", "Paragraph", 5)
    );
    add("paragraph-to-setext", "title\n", (s) =>
        s.insert(6, "===\n").expect("new", "Heading", 0).expect("retired", "Paragraph", 0)
    );
    add("paragraph-into-quote", "text\n", (s) =>
        s.insert(0, "> ").expect("new", "Callout", 0).expect("new", "Paragraph", 2).expect("retired", "Paragraph", 0)
    );
    add("unwrap-nested-note", "a^[b ^[c] d] e\n", (s) =>
        s
            .edit([
                { start: 1, end: 3, text: "" },
                { start: 11, end: 12, text: "" }
            ])
            .expect("new", "Footnote", 3)
    );
    {
        const text = Array.from({ length: 200 }, (_, i) => `${word(i * 3 + 1)} ${word(i * 3 + 2)}\n\n`).join("");
        add("line-at-top", text, (s) =>
            s
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
        s
            .edit([{ start: 11, end: 14, text: "two" }])
            .expect("changed", "Document", 0)
            .expect("changed", "Heading", 0)
            .expect("only")
    );
    /* A code span's closer search reads what earlier spans' searches
     * learned of the runs after them; the answer must not depend on which
     * of those spans the edit took whole. */
    add("code-closer-after-span", "``x`8`@`\n", (s) => s.insert(8, "D`").expect("new", "Code", 7));
    /* A delimiter row tried against the paragraph so far, which refused it,
     * keeps the paragraph from becoming a table on a later delimiter row:
     * the line it was tried on is read again when the paragraph is (E5). */
    add("table-header-refused", "a| b | c |\n| - | - |\n| p | d |\n| - | - |\n", (s) =>
        s.insert(23, "x").expect("changed", "Paragraph", 0)
    );
    /* A leaf block edited on a later line takes its untouched lines (E5);
     * an edit that ends it early, or makes its lines a heading, is read. */
    {
        const lines = Array.from({ length: 40 }, (_, i) => `${word(i * 2 + 1)} ${word(i * 2 + 2)}\n`);
        const code = `\`\`\`\n${lines.join("")}\`\`\`\n`;
        const line = (n) => 4 + bytes(lines.slice(0, n).join(""));
        add("code-line-near-end", code, (s) => s.insert(line(30) + 1, "x").expect("kept", "CodeBlock", 0, 0));
        add("fence-inside-code", code, (s) =>
            s
                .insert(line(20), "```\n")
                .expect("kept", "CodeBlock", 0, 0)
                .expect("new", "Paragraph", line(20) + 4)
        );
        const text = lines.join("");
        const at = bytes(lines.slice(0, 20).join(""));
        add("paragraph-line-near-end", text, (s) => s.insert(at + 1, "x").expect("kept", "Paragraph", 0, 0));
        add("underline-inside-paragraph", text, (s) =>
            s.insert(at, "===\n").expect("new", "Heading", 0).expect("retired", "Paragraph", 0)
        );
        const item = `- ${lines.join("  ")}`;
        const third = 2 + bytes(lines.slice(0, 30).join("  "));
        add("item-line-near-end", item, (s) => s.insert(third + 3, "x").expect("kept", "Paragraph", 2, 2));
    }
    {
        const paragraphs = Array.from({ length: 1000 }, (_, i) => `${word(i * 3 + 1)} ${word(i * 3 + 2)}\n\n`);
        const text = paragraphs.join("");
        const at = bytes(paragraphs.slice(0, 4).join(""));
        add("typing-paragraph-5-of-1000", text, (s) =>
            s
                .insert(at + 1, "x")
                .expect("changed", "Document", 0)
                .expect("changed", "Paragraph", at)
                .expect("changed", "Text", at)
                .expect("only")
        );
    }
    /* An edit inside an inline root changes the nodes it touches; the
     * nodes beside it are taken whole (5.6), and a closer that pairs with an
     * opener read before it makes a new node there. */
    add("inline-siblings-kept", "one *two* `three` four\n", (s) =>
        s
            .insert(22, "x")
            .expect("changed", "Document", 0)
            .expect("changed", "Paragraph", 0)
            .expect("changed", "Text", 17)
            .expect("only")
    );
    add("closer-pairs-earlier-opener", "*a* b *c d\n", (s) =>
        s.insert(10, "*").expect("kept", "Emphasis", 0, 0).expect("new", "Emphasis", 6)
    );
    /* A quote that interrupts a paragraph of only definitions closes it
     * into its References; the quote after an edited definition is taken. */
    add("quote-after-definition", "[d]:g\n>\n", (s) =>
        s.edit([{ start: 4, end: 5, text: "q" }]).expect("kept", "Callout", 6, 6)
    );
    return cases;
}

/**
 * The refused batches of 4.8, against a document with a four-byte scalar: out
 * of bounds past the end, reversed and overlapping, and inside that scalar at
 * one of its continuation bytes in UTF-8 and between its two units in UTF-16.
 */
export function rejectionScript(text) {
    const script = new Script("rejections", "rejections", text);
    const buffer = Buffer.from(text);
    const wide = scalarBoundaries(buffer).find((at, index, all) => all[index + 1] - at === 4);
    for (const [unit, length] of [
        ["utf8", buffer.length],
        ["utf16", text.length]
    ]) {
        script.reject(unit, "out-of-bounds", length + 1, length + 1, "61");
        script.reject(unit, "out-of-bounds", 2, 1, "61");
        script.reject(unit, "out-of-bounds", 0, length, "61", 0, length, "61");
    }
    script.reject("utf8", "inside-scalar", wide + 1, wide + 1, "61");
    script.reject("utf8", "inside-scalar", 0, wide + 3, "61");
    const units = buffer.subarray(0, wide).toString("utf8").length;
    script.reject("utf16", "inside-scalar", units + 1, units + 1, "61");
    script.reject("utf16", "inside-scalar", 0, units + 1, "61");
    return script;
}

/* ------------------------------------------------------------ encoding */

const hex = (text) => (text === "" ? "-" : Buffer.from(text).toString("hex"));

/** Scripts in the tracked text format: a `script NAME FAMILY` line, then one step per line. */
export function formatScripts(scripts) {
    const lines = [SCRIPT_HEADER];
    for (const script of scripts) {
        lines.push(`script ${script.name} ${script.family}`);
        for (const step of script.steps) {
            if (step.kind === "edit") {
                lines.push(`edit ${step.edits.map((edit) => `${edit.start} ${edit.end} ${hex(edit.text)}`).join(" ")}`);
            } else if (step.kind === "reject") {
                lines.push(`reject ${step.unit} ${step.status} edit ${step.values.join(" ")}`);
            }
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
    const unhex = (value) => (value === "-" ? "" : Buffer.from(value, "hex").toString("utf8"));
    for (const line of lines.slice(1, -1)) {
        const [kind, ...fields] = line.split(" ");
        const steps = scripts.at(-1)?.steps;
        if (kind === "script") {
            assert.equal(fields.length, 2, `malformed script line: ${line}`);
            scripts.push({ name: fields[0], family: fields[1], steps: [] });
        } else if (kind === "edit") {
            assert.ok(steps && fields.length && fields.length % 3 === 0, `malformed edit: ${line}`);
            const edits = [];
            for (let index = 0; index < fields.length; index += 3)
                edits.push({
                    start: Number(fields[index]),
                    end: Number(fields[index + 1]),
                    text: unhex(fields[index + 2])
                });
            steps.push({ kind, edits });
        } else if (kind === "reject" && fields[2] === "edit") {
            steps.push({ kind, unit: fields[0], status: fields[1], values: fields.slice(3) });
        } else if (kind === "expect") {
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
 * adversarial shapes at their smallest size, with every edit and stream
 * family, `splits` on every document up to 2 KB, `random` with 16 seeds, the
 * scripted identity cases and the invalid arguments. Returns the files to
 * write, keyed by their path under `specs/incremental/`.
 */
export function correctnessSet() {
    const files = new Map();
    const manifest = [];
    /* Every family a case belongs to. The manifest declares them in the
     * order of sections 3.1 to 3.3, and the C tests run one family each. */
    const families = new Set();
    const addScripts = (file, scripts) => {
        files.set(file, formatScripts(scripts));
        for (const script of scripts) families.add(script.family);
    };
    const documents = [];
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
    for (const { name, text, script } of identityScripts()) {
        files.set(`documents/${name}.md`, text);
        addScripts(`scripts/${name}.edits`, [script]);
        manifest.push(`document documents/${name}.md`, `edits documents/${name}.md scripts/${name}.edits`);
    }
    /* The invalid arguments run against a document that ends in a four-byte
     * scalar, so an offset can fall inside one in either unit. */
    const rejected = `${fs.readFileSync(path.join(root, CANONICAL, "inlines.md"), "utf8")}\u{20000}\n`;
    files.set("documents/rejections.md", rejected);
    addScripts("scripts/rejections.edits", [rejectionScript(rejected)]);
    manifest.push("document documents/rejections.md", "edits documents/rejections.md scripts/rejections.edits");
    for (const document of documents) {
        manifest.push(`document ${document.path}${document.parts ? ` parts ${document.parts.join(" ")}` : ""}`);
        const file = `scripts/${document.name}.edits`;
        addScripts(file, editScripts(document));
        manifest.push(`edits ${document.path} ${file}`);
        const buffer = Buffer.from(document.text);
        const streams = document.shape
            ? ["tokens", "rows"]
            : ["tokens", "scalars", "rows", ...(buffer.length <= SPLIT_LIMIT ? ["splits"] : [])];
        for (const family of streams) {
            manifest.push(`stream ${document.path} ${family}`);
            families.add(family);
        }
    }
    files.set("token-sizes.txt", `${TOKEN_SIZES.join(" ")}\n`);
    const declared = ["documents", "identity", "rejections", ...EDIT_FAMILIES, ...STREAM_FAMILIES]
        .filter((family) => family === "documents" || families.has(family))
        .map((family) => `family ${family}`);
    files.set("manifest.txt", `${[MANIFEST_HEADER, ...declared, ...manifest].join("\n")}\n`);
    return files;
}

/**
 * The benchmark workloads (section 9): every grammar corpus document with
 * `typing`, `lines`, `markers`, `undo` of every family but `batch`, `random`,
 * `tokens` and `scalars`, and every scale and adversarial shape at all four
 * sizes with every family but `undo` and `batch`. Each workload is one document and one script or stream family.
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
        const document = { name: entry.name, text: entry.text, alphabet: entry.alphabet, sites: [], parts: null };
        documents.push(document);
        for (const script of editScripts(
            document,
            ["typing", "lines", "markers", "undo", "random"],
            UNDONE_FAMILIES.filter((family) => family !== "batch")
        ))
            workloads.push({ document, family: script.family, name: `${document.name}.${script.name}`, script });
        for (const family of ["tokens", "scalars"])
            workloads.push({ document, family, name: `${document.name}.${family}`, stream: family });
    }
    for (const shape of set === "all" ? [...Object.keys(SCALE_SHAPES), ...Object.keys(ADVERSARIAL_SHAPES)] : [])
        for (const alphabet of Object.keys(alphabets))
            for (const size of shapeSizes(shape)) {
                /* The sites only place the scripts: the set keeps the rest. */
                const { sites, ...document } = shapeDocument(shape, alphabet, size);
                documents.push(document);
                for (const script of editScripts(
                    { ...document, sites },
                    EDIT_FAMILIES.filter((family) => family !== "undo" && family !== "batch")
                ))
                    workloads.push({
                        document,
                        family: script.family,
                        name: `${document.name}.${script.name}`,
                        script
                    });
                for (const family of ["tokens", "rows"])
                    workloads.push({ document, family, name: `${document.name}.${family}`, stream: family });
            }
    return { documents, workloads };
}

/**
 * Write the benchmark workloads of `set` under `directory`, or only those
 * `names` lists: each document, each document's scripts in one file, and the
 * token sizes. Returns the index of what it wrote, whose digest identifies
 * the workload the way the corpus digest identifies the one-shot benchmark's.
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
    fs.mkdirSync(path.join(directory, "scripts"), { recursive: true });
    const scripts = new Map();
    for (const document of documents) {
        document.path = `documents/${document.name}.md`;
        document.sha256 = hash(document.text);
        document.bytes = bytes(document.text);
        fs.writeFileSync(path.join(directory, document.path), document.text);
    }
    for (const workload of workloads) {
        if (!workload.script) continue;
        workload.file = `scripts/${workload.document.name}.edits`;
        scripts.set(workload.file, [...(scripts.get(workload.file) ?? []), workload.script]);
    }
    const digests = new Map();
    for (const [file, list] of scripts) {
        const text = formatScripts(list);
        fs.writeFileSync(path.join(directory, file), text);
        digests.set(file, hash(text));
    }
    fs.writeFileSync(path.join(directory, "token-sizes.txt"), `${TOKEN_SIZES.join(" ")}\n`);
    return {
        version: WORKLOAD_VERSION,
        set,
        digest: hash(
            JSON.stringify([
                TOKEN_SIZES,
                workloads.map((workload) => [
                    workload.name,
                    workload.document.sha256,
                    workload.script ? [digests.get(workload.file), workload.script.name] : workload.stream
                ])
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
