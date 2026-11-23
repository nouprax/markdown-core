import assert from "node:assert/strict";
import test from "node:test";
import { auditParserBoundaries } from "../parser-boundaries.mjs";

const audit = (source) =>
    auditParserBoundaries([{ file: "engine.c", source }], {
        elementHeaders: ["heading.h", "footnote_scanners.h"],
        syntaxScanners: ["scan_atx_heading_start", "scan_footnote_definition"],
        descriptors: ["MARKDOWN_CORE_ELEMENT_LINK"]
    });

test("engine boundaries allow shared model and descriptor operations", () => {
    assert.deepEqual(
        audit(`
        /* MARKDOWN_CORE_NODE_HEADING and scan_atx_heading() describe a caller. */
        if (node->kind == MARKDOWN_CORE_NODE_TEXT) source_map(node->as.literal);
        indexable = ((unsigned)kind & MARKDOWN_CORE_NODE_VALUE_MASK) < MARKDOWN_CORE_NODE_KIND_COUNT;
        structure->element->opens->scan(parser, context, candidate);
        structure->lines->close(parser, node);
        if (byte == ' ' || byte == '\\t' || byte == '\\n' || byte == '0') digits++;
        tail[0] = separator;
        node->as.data = NULL;
        #include "parser.h"
    `),
        []
    );
});

test("grammar cannot drift back into either engine", () => {
    for (const source of [
        "node->kind == MARKDOWN_CORE_NODE_HEADING",
        "node->as.code->fenced",
        "scan_atx_heading_start(input, length, offset)",
        "scan_footnote_definition(input, length, offset)",
        "switch (byte) { case '*': break; }",
        "if (byte == '[') return;",
        'const char *base = "section";',
        "attach(&MARKDOWN_CORE_ELEMENT_LINK);",
        '#include "../elements/heading.h"',
        '#include "heading.h"',
        '#include "footnote_scanners.h"'
    ]) {
        assert.ok(audit(source).length > 0, source);
    }
});
