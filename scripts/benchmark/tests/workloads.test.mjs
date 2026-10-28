import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
    ADVERSARIAL_SHAPES,
    BENCHMARK_EDITS,
    EDIT_FAMILIES,
    EDIT_WINDOWS,
    RANDOM_SEEDS,
    benchmarkWorkloads,
    LOCAL_SHAPES,
    SCALE_SHAPES,
    SIZES,
    STREAM_FAMILIES,
    TOKEN_SIZES,
    correctnessSet,
    editScripts,
    formatScripts,
    hardScripts,
    identityScripts,
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
    assert.deepEqual(shapeSizes("short-lines"), [65536]);
    const lines = smallest("short-lines", "ascii").text.trimEnd().split("\n");
    assert.ok(lines.length > 1000);
    assert.ok(lines.every((line) => line.length > 0 && line.length < 80 && !/^\s/.test(line)));
});

test("the streaming shapes write the definition after its uses and leave the envelope and the grid table open", () => {
    const late = shapeDocument("late-definition", "ascii", SIZES.at(-1)).text;
    assert.ok(late.match(/\]\[t\]/g).length >= 10000);
    assert.equal(late.match(/^\[t\]: /gm).length, 1);
    assert.match(late, /\n\[t\]: \/[a-z]+\n$/);
    const envelope = smallest("unclosed-properties", "ascii").text.split("\n");
    assert.equal(envelope[0], "---");
    assert.ok(!envelope.slice(1).some((line) => /^(---|\.\.\.)\s*$/.test(line)));
    const grid = smallest("unclosed-grid", "ascii").text.trimEnd().split("\n");
    assert.match(grid[0], /^\+-+\+-+\+$/);
    assert.match(grid.at(-1), /^\| .*\|$/);
});

test("6.2 holds the streams of every scale shape, the long list, the deep quotes and the paragraph of short lines, and no other", () => {
    assert.deepEqual(LOCAL_SHAPES, [...Object.keys(SCALE_SHAPES), "long-list", "deep-quotes", "short-lines"]);
    const { workloads } = benchmarkWorkloads();
    for (const workload of workloads) {
        assert.equal(workload.local, LOCAL_SHAPES.includes(workload.document.shape), workload.name);
    }
    assert.ok(workloads.some((workload) => workload.local));
});

test("6.3 measures every step of each shape 6.2 leaves to it, at the shape's smallest size, and of no other stream", () => {
    const { workloads } = benchmarkWorkloads();
    for (const workload of workloads) {
        const { shape, size } = workload.document;
        const stepwise = Boolean(shape) && !LOCAL_SHAPES.includes(shape) && size === shapeSizes(shape)[0];
        assert.equal(workload.stepwise, stepwise, workload.name);
    }
    const names = workloads.filter((workload) => workload.stepwise).map((workload) => workload.name);
    assert.ok(names.includes("unclosed-grid-ascii-16k.rows"));
    assert.ok(names.includes("unclosed-strong-utf8-64k.tokens"));
    assert.ok(!names.some((name) => name.startsWith("short-lines-")));
    assert.ok(!names.some((name) => name.includes("-1024k.")));
});

test("scripts survive their text format unchanged", () => {
    const text = formatScripts([...identityScripts(), ...hardScripts()].map((entry) => entry.script));
    assert.equal(formatScripts(parseScripts(text)), text);
    const refused = `x\u{20000}\n`;
    const rejections = formatScripts([rejectionScript(refused)]);
    assert.equal(formatScripts(parseScripts(rejections)), rejections);
});

test("every identity case states what each append or edit keeps or changes", () => {
    for (const { name, text, script } of identityScripts()) {
        assert.equal(Buffer.from(text).toString("utf8"), text, name);
        const edits = script.steps[0].kind === "flush";
        assert.equal(script.family, edits ? "ids" : "identity", name);
        for (const step of script.steps.slice(edits ? 1 : 0)) {
            assert.equal(step.kind, edits ? "edit" : "append", name);
            if (!edits) assert.ok(step.text.length, `${name}: an empty append`);
            assert.ok(step.expect?.length, `${name}: a step without expectations`);
        }
    }
});

test("an edit script flushes before its first edit and edits inside the text", () => {
    const document = smallest("prose", "utf8");
    const scripts = editScripts(document);
    assert.deepEqual([...new Set(scripts.map((script) => script.family))].sort(), [...EDIT_FAMILIES].sort());
    assert.equal(scripts.filter((script) => script.family === "random").length, RANDOM_SEEDS);
    const cases = [...scripts.map((script) => ({ script, text: document.text })), ...hardScripts()];
    for (const { script, text: opened } of cases) {
        assert.equal(script.steps[0].kind, "flush", script.name);
        assert.ok(script.steps.length > 1, script.name);
        let text = Buffer.from(opened);
        for (const step of script.steps.slice(1)) {
            assert.equal(step.kind, "edit", script.name);
            assert.ok(step.start <= step.end && step.end <= text.length, script.name);
            const boundaries = scalarBoundaries(text);
            assert.ok(boundaries.includes(step.start) && boundaries.includes(step.end), script.name);
            text = Buffer.concat([text.subarray(0, step.start), Buffer.from(step.text), text.subarray(step.end)]);
        }
    }
});

test("each undo edit is followed by its inverse and by itself again", () => {
    const document = smallest("prose", "ascii");
    for (const script of editScripts(document, ["undo"])) {
        const edits = script.steps.slice(1);
        assert.equal(edits.length % 3, 0, script.name);
        let text = Buffer.from(document.text);
        for (let index = 0; index < edits.length; index += 3) {
            const [edit, inverse, again] = edits.slice(index, index + 3);
            const before = text;
            const apply = (step) =>
                (text = Buffer.concat([text.subarray(0, step.start), Buffer.from(step.text), text.subarray(step.end)]));
            apply(edit);
            apply(inverse);
            assert.deepEqual(text, before, script.name);
            assert.deepEqual(again, edit);
            apply(again);
        }
    }
});

test("the refused edits stream first, then fall inside the wide scalar in both units", () => {
    const text = `ab\u{20000}\n`;
    const script = rejectionScript(text);
    const kinds = script.steps.map((step) => step.kind);
    assert.deepEqual(kinds.slice(0, 3), ["reject", "reject", "flush"]);
    assert.deepEqual(
        script.steps.slice(0, 2).map((step) => step.status),
        ["streaming", "streaming"]
    );
    assert.ok(script.steps.some((step) => step.unit === "utf8" && step.start === 3));
    assert.ok(script.steps.some((step) => step.unit === "utf16" && step.start === 3));
    assert.deepEqual(kinds.slice(-2), ["edit", "edit"]);
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
    assert.deepEqual(declared, [
        "documents",
        "identity",
        "ids",
        "hard",
        "rejections",
        ...EDIT_FAMILIES,
        ...STREAM_FAMILIES
    ]);
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
        assert.ok(manifest.includes(`script documents/${entry.name}.md scripts/${entry.name}.script`), entry.name);
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
        assert.deepEqual(narrowed.edits, []);
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

test("the edit workloads run every family but undo with one seed, and a shape's larger sizes typing and far alone", () => {
    assert.deepEqual(
        BENCHMARK_EDITS,
        EDIT_FAMILIES.filter((family) => family !== "undo")
    );
    assert.ok(EDIT_WINDOWS < 16);
    const { documents, edits } = benchmarkWorkloads();
    const corpus = documents.filter((document) => !document.shape);
    assert.deepEqual(
        edits.filter((workload) => !workload.document.shape).map((workload) => workload.document),
        corpus
    );
    for (const workload of edits) {
        const { shape, size } = workload.document;
        const stepwise = !shape || size === shapeSizes(shape)[0];
        assert.equal(workload.stepwise, stepwise, workload.name);
        assert.equal(workload.name, `${workload.document.name}.edits`);
        const families = new Set(workload.scripts.map((script) => script.family));
        for (const family of families)
            assert.ok((stepwise ? BENCHMARK_EDITS : ["typing", "far"]).includes(family), `${workload.name} ${family}`);
        assert.deepEqual(
            workload.scripts.filter((script) => script.family === "random").map((script) => script.name),
            stepwise ? ["random-1"] : []
        );
        for (const script of workload.scripts) {
            assert.equal(script.steps[0].kind, "flush", `${workload.name} ${script.name}`);
            assert.ok(
                script.steps.slice(1).every((step) => step.kind === "edit"),
                `${workload.name} ${script.name}`
            );
        }
    }
    assert.ok(edits.some((workload) => workload.name === "prose-ascii-1024k.edits"));
});

test("an edit workload writes its scripts beside its document, and its scripts enter the digest", () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "workloads-"));
    try {
        const whole = writeBenchmarkWorkloads(directory, "corpus");
        const [workload] = whole.edits;
        const narrowed = writeBenchmarkWorkloads(directory, "corpus", [workload.name]);
        assert.deepEqual(narrowed.workloads, []);
        assert.deepEqual(
            narrowed.edits.map((entry) => entry.name),
            [workload.name]
        );
        assert.equal(narrowed.edits[0].path, `scripts/${workload.document.name}.script`);
        const text = fs.readFileSync(path.join(directory, narrowed.edits[0].path), "utf8");
        assert.equal(text, formatScripts(workload.scripts));
        assert.deepEqual(
            parseScripts(text).map((script) => script.name),
            workload.scripts.map((script) => script.name)
        );
        assert.deepEqual(fs.readdirSync(path.join(directory, "documents")), [`${workload.document.name}.md`]);
        assert.notEqual(
            narrowed.digest,
            writeBenchmarkWorkloads(directory, "corpus", [whole.workloads[0].name]).digest
        );
    } finally {
        fs.rmSync(directory, { recursive: true, force: true });
    }
});

test("a marker that belongs after a block's first line goes after it, also for a block at a line start", () => {
    const document = {
        text: "one\n\ntwo\n",
        sites: [{ kind: "start:paragraph", start: 5, end: 5 }],
        alphabet: "ascii"
    };
    const [script] = editScripts(document, ["markers"]);
    const after = script.steps.filter(
        (step) => step.kind === "edit" && ["===\n", "| --- |\n", ": x\n"].includes(step.text)
    );
    assert.deepEqual(
        after.map((step) => step.start),
        [9, 9, 9]
    );
});
