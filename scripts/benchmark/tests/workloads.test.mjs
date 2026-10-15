import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
    ADVERSARIAL_SHAPES,
    SCALE_SHAPES,
    SIZES,
    STREAM_FAMILIES,
    TOKEN_SIZES,
    correctnessSet,
    formatScripts,
    identityScripts,
    parseScripts,
    scalarBoundaries,
    shapeDocument,
    shapeSizes,
    streamChunks,
    writeBenchmarkWorkloads
} from "../workloads.mjs";

const SHAPES = [...Object.keys(SCALE_SHAPES), ...Object.keys(ADVERSARIAL_SHAPES)];
const smallest = (shape, alphabet) => shapeDocument(shape, alphabet, shapeSizes(shape)[0]);

test("the scale sizes are the four of section 3.1", () => {
    assert.deepEqual(SIZES, [16384, 65536, 262144, 1048576]);
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

test("scripts survive their text format unchanged", () => {
    const text = formatScripts(identityScripts().map((entry) => entry.script));
    assert.equal(formatScripts(parseScripts(text)), text);
});

test("every identity case appends well-formed text and states what each step keeps or changes", () => {
    for (const { name, text, script } of identityScripts()) {
        assert.equal(Buffer.from(text).toString("utf8"), text, name);
        assert.ok(script.steps.length, name);
        for (const step of script.steps) {
            assert.ok(step.text.length, `${name}: an empty append`);
            assert.ok(step.expect?.length, `${name}: a step without expectations`);
        }
    }
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
        if (kind === "script") for (const script of parseScripts(files.get(family))) used.add(script.family);
    }
    assert.deepEqual([...used].sort(), [...declared].sort());
    assert.deepEqual(declared, ["documents", "identity", ...STREAM_FAMILIES]);
});

test("the correctness set streams every document, splits the short ones and runs every identity case", () => {
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
        }
    for (const [path, families] of streams) {
        if (!path.startsWith("../canonical-ast/")) continue;
        const length = fs.statSync(new URL(`../../../specs/incremental/${path}`, import.meta.url)).size;
        assert.deepEqual(families, ["tokens", "scalars", "rows", ...(length <= 2048 ? ["splits"] : [])], path);
    }
    assert.ok([...streams.values()].some((families) => families.includes("splits")));
    for (const entry of identityScripts()) {
        assert.equal(files.get(`documents/${entry.name}.md`), entry.text);
        assert.ok(manifest.includes(`script documents/${entry.name}.md scripts/${entry.name}.stream`), entry.name);
    }
});

test("a narrowed benchmark set writes and identifies only the named workloads", () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "workloads-"));
    try {
        const whole = writeBenchmarkWorkloads(directory, "corpus");
        const [first, second] = whole.workloads;
        assert.equal(first.document, second.document);
        assert.deepEqual([first.family, second.family], ["tokens", "scalars"]);
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
