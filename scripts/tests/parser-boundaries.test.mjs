import fs from "node:fs";
import assert from "node:assert/strict";
import test from "node:test";
import { auditParserBoundaries } from "../lib/parser-boundaries.mjs";

const audit = (source) =>
    auditParserBoundaries([{ file: "engine.c", source }], {
        elementHeaders: ["heading.h", "footnote_scanners.h"],
        syntaxScanners: ["scan_atx_heading_start", "scan_footnote_definition"]
    });

test("engine boundaries allow shared model and descriptor operations", () => {
    assert.deepEqual(
        audit(`
        /* MARKDOWN_CORE_NODE_HEADING and scan_atx_heading() describe a caller. */
        if (node->kind == MARKDOWN_CORE_NODE_TEXT) source_map(node->as.literal);
        structure->scan_block_start(parser, context, candidate);
        structure->finalize_block(parser, node);
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
        '#include "../elements/heading.h"',
        '#include "heading.h"',
        '#include "footnote_scanners.h"'
    ]) {
        assert.ok(audit(source).length > 0, source);
    }
});

// Construction owns a disjoint subtree; arbitrary reparenting does not. Keep
// the latter API at its checked boundary so depth cannot multiply parser work.
test("parser construction does not invoke arbitrary tree reparenting", () => {
    const root = new URL("../../packages/markdown-core/", import.meta.url);
    const sources = [
        "core/blocks.c",
        "core/inlines.c",
        ...fs
            .readdirSync(new URL("elements/", root))
            .filter((name) => name.endsWith(".c"))
            .map((name) => `elements/${name}`)
    ];
    for (const file of sources) {
        const source = fs.readFileSync(new URL(file, root), "utf8").replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, "");
        assert.doesNotMatch(
            source,
            /\bmarkdown_core_node_(?:append_child|prepend_child|insert_before|insert_after|replace)\s*\(/,
            file
        );
    }
});
