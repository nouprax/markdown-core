import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
    ADVERSARIAL_SHAPES,
    EDIT_FAMILIES,
    RANDOM_SEEDS,
    SCALE_SHAPES,
    SIZES,
    TOKEN_SIZES,
    applyBatch,
    correctnessSet,
    editScripts,
    formatScripts,
    identityScripts,
    inverseBatch,
    parseScripts,
    rejectionScript,
    scalarBoundaries,
    shapeDocument,
    shapeSizes,
    streamChunks,
    writeBenchmarkWorkloads
} from "../workloads.mjs";

const SHAPES = [...Object.keys(SCALE_SHAPES), ...Object.keys(ADVERSARIAL_SHAPES)];
const smallest = (shape, alphabet) => shapeDocument(shape, alphabet, shapeSizes(shape)[0]);

/* Replays a script over its document and checks what 3.2 promises of every
 * valid step: offsets on scalar boundaries of the text before the step, well
 * formed texts and non-overlapping batches (applyBatch rejects the rest). */
function replay(text, script) {
    let buffer = Buffer.from(text);
    for (const step of script.steps) {
        if (step.kind !== "edit") continue;
        for (const edit of step.edits) {
            assert.ok(Buffer.from(edit.text).toString("utf8") === edit.text, `${script.name}: ill-formed text`);
        }
        buffer = applyBatch(buffer, step.edits);
    }
    return buffer;
}

test("the scale sizes are the four of section 3.1", () => {
    assert.deepEqual(SIZES, [16384, 65536, 262144, 1048576]);
});

test("a batch and its inverse restore the text, whatever order the batch is listed in", () => {
    const text = Buffer.from("alpha été \u{20000} omega\n");
    const edits = [
        { start: 12, end: 16, text: "x" },
        { start: 0, end: 5, text: "中文" },
        { start: 6, end: 6, text: "" }
    ];
    const edited = applyBatch(text, edits);
    assert.equal(edited.toString("utf8"), "中文 été x omega\n");
    assert.deepEqual(applyBatch(edited, inverseBatch(text, edits)), text);
});

test("a batch rejects overlapping edits and offsets inside a scalar", () => {
    const text = Buffer.from("aéb\n");
    assert.throws(() =>
        applyBatch(text, [
            { start: 0, end: 2, text: "" },
            { start: 1, end: 3, text: "" }
        ])
    );
    assert.throws(() => applyBatch(text, [{ start: 2, end: 2, text: "x" }]));
});

test("token chunk sizes run from 1 to 16 bytes with a mean of exactly four", () => {
    assert.ok(TOKEN_SIZES.every((size) => Number.isInteger(size) && size >= 1 && size <= 16));
    assert.equal(
        TOKEN_SIZES.reduce((sum, size) => sum + size, 0),
        4 * TOKEN_SIZES.length
    );
});

test("every stream chunks the whole document into whole scalars", () => {
    for (const text of ["", "a", "one\r\ntwo\rthree\nfour", "中文\u{20000}\né", smallest("prose", "utf8").text]) {
        const buffer = Buffer.from(text);
        const boundaries = new Set(scalarBoundaries(buffer));
        for (const family of ["tokens", "scalars", "rows"]) {
            const ends = streamChunks(buffer, family);
            let previous = 0;
            for (const end of ends) {
                assert.ok(end > previous && boundaries.has(end), `${family}: ${end} is not a scalar boundary`);
                previous = end;
            }
            assert.equal(previous, buffer.length, `${family} covers the document`);
        }
        assert.deepEqual(streamChunks(buffer, "scalars"), scalarBoundaries(buffer).slice(1));
        const rows = streamChunks(buffer, "rows");
        rows.forEach((end, index) => {
            const row = buffer.subarray(index ? rows[index - 1] : 0, end).toString("latin1");
            assert.ok(!/[\r\n]./s.test(row.replace(/\r\n$/, "")), "a row holds one physical line");
        });
        assert.deepEqual(
            streamChunks(buffer, "splits"),
            scalarBoundaries(buffer)
                .slice(1, -1)
                .map((point) => [point, buffer.length])
        );
    }
});

test("every shape reaches its size in both alphabets and records parts that cover it", () => {
    for (const shape of SHAPES)
        for (const alphabet of ["ascii", "utf8"]) {
            const document = smallest(shape, alphabet);
            const length = Buffer.byteLength(document.text);
            assert.ok(length >= document.size, `${document.name} is ${length} bytes`);
            assert.ok(length < document.size * 1.25, `${document.name} is ${length} bytes`);
            assert.equal(
                document.parts.reduce((sum, part) => sum + part, 0),
                length
            );
            if (alphabet === "utf8") assert.ok(length > document.text.length, `${document.name} has multi-byte words`);
        }
});

test("the adversarial shapes hold the counts and depths section 3.1 names", () => {
    const largest = SIZES.at(-1);
    const items = shapeDocument("long-list", "ascii", largest).text.match(/^- /gm).length;
    assert.equal(items, 10000);
    const references = shapeDocument("wide-definition", "ascii", largest).text.match(/\]\[t\]/g).length;
    assert.ok(references >= 10000, `${references} references`);
    for (const size of shapeSizes("deep-quotes"))
        assert.ok(shapeDocument("deep-quotes", "ascii", size).text.includes(`${"> ".repeat(1000)}`));
    for (const [shape, opener] of [
        ["opener-fence", "```\n"],
        ["opener-html", "<pre>\n"],
        ["opener-comment", "%%\n"],
        ["opener-directive", ":::note\n"]
    ])
        assert.ok(smallest(shape, "ascii").text.startsWith(opener), shape);
    assert.deepEqual(shapeSizes("unclosed-strong"), [65536]);
    assert.deepEqual(shapeSizes("single-line"), [65536]);
    assert.ok(smallest("unclosed-strong", "ascii").text.startsWith("**"));
    assert.equal(smallest("single-line", "ascii").text.trimEnd().split("\n").length, 1);
});

test("the same edit lands at the same relative position at every size", () => {
    for (const shape of ["prose", "list", "table"]) {
        const places = SIZES.slice(0, 2).map((size) => {
            const document = shapeDocument(shape, "ascii", size);
            const script = editScripts(document, ["typing"]).find((item) => item.name === "typing-paragraph");
            return script ? script.steps[0].edits[0].start / Buffer.byteLength(document.text) : null;
        });
        if (places[0] === null) continue;
        assert.ok(Math.abs(places[0] - places[1]) < 0.05, `${shape}: ${places.join(" vs ")}`);
    }
});

test("every shape runs every edit family, with valid steps", () => {
    for (const shape of SHAPES) {
        const document = smallest(shape, "utf8");
        const scripts = editScripts(document);
        const families = new Set(scripts.map((script) => script.family));
        for (const family of EDIT_FAMILIES.filter((item) => item !== "declarations"))
            assert.ok(families.has(family), `${document.name} has no ${family} script`);
        assert.equal(scripts.filter((script) => script.family === "random").length, RANDOM_SEEDS);
        for (const script of scripts) replay(document.text, script);
    }
    for (const shape of ["prose", "refs"])
        assert.ok(
            editScripts(smallest(shape, "ascii")).some((script) => script.family === "declarations"),
            shape
        );
});

test("a batch step lists sixteen disjoint edits", () => {
    const [script] = editScripts(smallest("prose", "ascii"), ["batch"]);
    assert.ok(script.steps.every((step) => step.edits.length === 16));
});

test("an undo script returns to the text before each step", () => {
    const document = smallest("prose", "ascii");
    for (const script of editScripts(document, ["undo"])) {
        let buffer = Buffer.from(document.text);
        const before = [];
        for (const step of script.steps) {
            before.push(buffer);
            buffer = applyBatch(buffer, step.edits);
        }
        const texts = before.map((item) => item.toString("utf8"));
        assert.ok(
            texts.some((text, index) => index > 0 && text === document.text),
            `${script.name} never undoes`
        );
    }
});

test("scripts survive their text format unchanged", () => {
    const document = smallest("quote", "utf8");
    const scripts = [...editScripts(document), ...identityScripts().map((entry) => entry.script)];
    const text = formatScripts(scripts);
    assert.equal(formatScripts(parseScripts(text)), text);
});

test("every rejected batch is refused with the status it declares, in the unit it is written in", () => {
    const text = "a\u00e9\u{20000}\n";
    const buffer = Buffer.from(text);
    const script = rejectionScript(text);
    const statuses = new Set();
    for (const step of script.steps) {
        assert.equal(step.kind, "reject");
        const length = step.unit === "utf8" ? buffer.length : text.length;
        /* No scalar begins at a continuation byte or between two units of one scalar. */
        const inside = (at) =>
            at < length &&
            (step.unit === "utf8" ? (buffer[at] & 0xc0) === 0x80 : at > 0 && /[\ud800-\udbff]/u.test(text[at - 1]));
        const ranges = [];
        for (let index = 0; index < step.values.length; index += 3) {
            ranges.push([Number(step.values[index]), Number(step.values[index + 1])]);
        }
        /* The session checks each edit in order, its start before its end, then the overlaps. */
        let status = null;
        for (const [start, end] of ranges) {
            if (start > end) status = "out-of-bounds";
            else if (start > length) status = "out-of-bounds";
            else if (inside(start)) status = "inside-scalar";
            else if (end > length) status = "out-of-bounds";
            else if (inside(end)) status = "inside-scalar";
            if (status) break;
        }
        const sorted = ranges.toSorted((left, right) => left[0] - right[0] || left[1] - right[1]);
        if (!status && sorted.some(([start], index) => index > 0 && start < sorted[index - 1][1])) {
            status = "out-of-bounds";
        }
        assert.equal(status, step.status, `${step.unit} ${step.values.join(" ")}`);
        statuses.add(`${step.unit} ${step.status}`);
    }
    assert.deepEqual([...statuses].sort(), [
        "utf16 inside-scalar",
        "utf16 out-of-bounds",
        "utf8 inside-scalar",
        "utf8 out-of-bounds"
    ]);
});

test("the correctness set declares exactly the families its cases belong to", () => {
    const files = correctnessSet();
    const lines = files.get("manifest.txt").trimEnd().split("\n").slice(1);
    const declared = lines.filter((line) => line.startsWith("family ")).map((line) => line.split(" ")[1]);
    assert.equal(new Set(declared).size, declared.length);
    const used = new Set();
    for (const line of lines) {
        const [kind, , family] = line.split(" ");
        if (kind === "document") used.add("documents");
        if (kind === "stream") used.add(family);
        if (kind === "edits") for (const script of parseScripts(files.get(family))) used.add(script.family);
    }
    assert.deepEqual([...used].sort(), [...declared].sort());
    for (const family of [...EDIT_FAMILIES, "identity", "rejections", "tokens", "scalars", "rows", "splits"])
        assert.ok(declared.includes(family), `${family} is not declared`);
});

test("the correctness set runs every family on every document and splits the short ones", () => {
    const files = correctnessSet();
    const manifest = files
        .get("manifest.txt")
        .trimEnd()
        .split("\n")
        .slice(1)
        .filter((line) => !line.startsWith("family "));
    const streams = new Map();
    for (const line of manifest) {
        const [kind, path, family] = line.split(" ");
        if (kind === "stream") streams.set(path, [...(streams.get(path) ?? []), family]);
    }
    for (const shape of SHAPES)
        for (const alphabet of ["ascii", "utf8"]) {
            const name = smallest(shape, alphabet).name;
            assert.deepEqual(streams.get(`documents/${name}.md`), ["tokens", "rows"]);
            assert.ok(files.has(`scripts/${name}.edits`));
        }
    for (const [path, families] of streams) {
        if (!path.startsWith("../canonical-ast/")) continue;
        const length = fs.statSync(new URL(`../../../specs/incremental/${path}`, import.meta.url)).size;
        assert.deepEqual(families, ["tokens", "scalars", "rows", ...(length <= 2048 ? ["splits"] : [])], path);
    }
    assert.ok([...streams.values()].some((families) => families.includes("splits")));
    assert.ok(files.has("scripts/rejections.edits"));
    assert.equal(
        identityScripts().filter((entry) => files.has(`scripts/${entry.name}.edits`)).length,
        identityScripts().length
    );
});

test("a narrowed benchmark set writes and identifies only the named workloads", () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "workloads-"));
    try {
        const whole = writeBenchmarkWorkloads(directory, "corpus");
        const [first, second] = whole.workloads.filter((workload) => workload.script);
        const narrowed = writeBenchmarkWorkloads(directory, "corpus", [first.name, second.name]);
        assert.deepEqual(
            narrowed.workloads.map((workload) => workload.name),
            [first.name, second.name]
        );
        assert.deepEqual(
            narrowed.documents.map((document) => document.name),
            [...new Set([first.document.name, second.document.name])]
        );
        assert.notEqual(narrowed.digest, whole.digest);
        assert.deepEqual(fs.readdirSync(path.join(directory, "documents")), [`${first.document.name}.md`]);
        assert.throws(() => writeBenchmarkWorkloads(directory, "corpus", ["no-such-workload"]), /no workload named/);
    } finally {
        fs.rmSync(directory, { recursive: true, force: true });
    }
});
