import assert from "node:assert/strict";
import test from "node:test";
import { auditParserBoundaries } from "../lib/parser-boundaries.mjs";

const audit = (source) =>
    auditParserBoundaries([{ file: "engine.c", source }], {
        elementHeaders: ["heading.h", "footnote_scanners.h"],
        syntaxScanners: ["scan_atx_heading_start", "_scan_footnote_definition"]
    });

test("engine boundaries allow shared model and descriptor operations", () => {
    assert.deepEqual(
        audit(`
        /* MARKDOWN_CORE_NODE_HEADING and scan_atx_heading() describe a caller. */
        if (node->kind == MARKDOWN_CORE_NODE_TEXT) source_map(node->as.literal);
        syntax->scan_block_start(parser, context, candidate);
        syntax->finalize_block(parser, node);
    `),
        []
    );
});

test("grammar cannot drift back into either engine", () => {
    for (const source of [
        "node->kind == MARKDOWN_CORE_NODE_HEADING",
        "node->as.code->fenced",
        "scan_atx_heading_start(input, offset)",
        "_scan_footnote_definition(cursor, end)",
        "switch (byte) { case '*': break; }",
        '#include "../extensions/heading.h"',
        '#include "heading.h"',
        '#include "footnote_scanners.h"'
    ]) {
        assert.ok(audit(source).length > 0, source);
    }
});
