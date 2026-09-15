import assert from "node:assert/strict";
import test from "node:test";
import { caseRow, formatRow, parseCollected, parseInstructionsLine } from "../benchmark-instructions.mjs";

test("callgrind's collected total is read from its stderr summary", () => {
    assert.equal(parseCollected("==12== Events    : Ir\n==12== Collected : 235750\n==12== \n"), 235750);
    assert.throws(() => parseCollected("==12== no summary"), /no instruction count/u);
});

test("the runner's instructions line names the case, the implementation, the stage, the bytes and the input", () => {
    const line = `benchmark noise\ninstructions case=empty_document implementation=core stage=parse bytes=0 parses=1 sha256=${"e".repeat(64)}\n`;
    assert.deepEqual(parseInstructionsLine(line), {
        name: "empty_document",
        implementation: "core",
        stage: "parse",
        bytes: 0,
        parses: 1,
        sha256: "e".repeat(64)
    });
    assert.throws(() => parseInstructionsLine("nothing"), /no instructions line/u);
});

test("a row is the counted parse window itself, with the free window and the unreplicated read beside it", () => {
    /* Nothing is subtracted: callgrind collects only the window the runner
     * opens around the parse, so the count is the parse. */
    const row = caseRow("lorem1.md", 758200, "a".repeat(64), {
        core: 330000,
        coreFree: 12000,
        coreRaw: 96953,
        rawBytes: 3789,
        cmark: 310000
    });
    assert.deepEqual(row, {
        name: "lorem1.md",
        bytes: 758200,
        inputSha256: "a".repeat(64),
        coreIr: 330000,
        coreFreeIr: 12000,
        rawBytes: 3789,
        coreRawIr: 96953,
        cmarkIr: 310000,
        ratio: 330000 / 310000
    });
    assert.equal(
        formatRow(row),
        "instructions case=lorem1.md bytes=758200 core_ir=330000 core_free_ir=12000 " +
            "raw_bytes=3789 core_raw_ir=96953 cmark_ir=310000 ratio=1.065"
    );
});

test("a case that does not replicate reports no second read, and a zero reference has no ratio", () => {
    /* Only the sample workload replicates a file on disk; a generated case
     * is its own size, so the driver leaves the raw fields off. */
    const alone = caseRow("empty_document", 0, "b".repeat(64), { core: 4961, coreFree: 409 });
    assert.equal(formatRow(alone), "instructions case=empty_document bytes=0 core_ir=4961 core_free_ir=409");
    assert.equal(alone.coreRawIr, undefined);
    assert.equal(caseRow("x", 1, "c".repeat(64), { core: 5, cmark: 0 }).ratio, null);
});
