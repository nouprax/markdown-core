import assert from "node:assert/strict";
import test from "node:test";
import { caseRow, formatRow, parseCollected, parseInstructionsLine } from "../benchmark-instructions.mjs";

test("callgrind's collected total is read from its stderr summary", () => {
    assert.equal(parseCollected("==12== Events    : Ir\n==12== Collected : 235750\n==12== \n"), 235750);
    assert.throws(() => parseCollected("==12== no summary"), /no instruction count/u);
});

test("the runner's instructions line names the case, the implementation, the bytes and the input", () => {
    const line = `benchmark noise\ninstructions case=empty_document implementation=core bytes=0 parses=1 sha256=${"e".repeat(64)}\n`;
    assert.deepEqual(parseInstructionsLine(line), {
        name: "empty_document",
        implementation: "core",
        bytes: 0,
        parses: 1,
        sha256: "e".repeat(64)
    });
    assert.throws(() => parseInstructionsLine("nothing"), /no instructions line/u);
});

test("a row is the difference of the counted run and its dry run, per implementation", () => {
    const row = caseRow("lorem1.md", 3789, "a".repeat(64), {
        core: 540000,
        coreDry: 210000,
        cmark: 500000,
        cmarkDry: 190000
    });
    assert.deepEqual(row, {
        name: "lorem1.md",
        bytes: 3789,
        inputSha256: "a".repeat(64),
        coreIr: 330000,
        cmarkIr: 310000,
        ratio: 330000 / 310000
    });
    assert.equal(formatRow(row), "instructions case=lorem1.md bytes=3789 core_ir=330000 cmark_ir=310000 ratio=1.065");
    const alone = caseRow("empty_document", 0, "b".repeat(64), { core: 235750, coreDry: 205196 });
    assert.equal(formatRow(alone), "instructions case=empty_document bytes=0 core_ir=30554");
    assert.equal(caseRow("x", 1, "c".repeat(64), { core: 5, coreDry: 1, cmark: 3, cmarkDry: 3 }).ratio, null);
});
