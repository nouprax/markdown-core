import assert from "node:assert/strict";
import { test } from "node:test";
import { unified } from "unified";
import remarkParse from "remark-parse";
import { fromMdast, projectMdastComparison } from "../lib/mdast-oracle.mjs";
import { parseCanonicalDump } from "../lib/upstream-cmark.mjs";

test("the mdast intersection omits only the unavailable heading anchor", () => {
    const markdown = "# Title\n";
    const expected = projectMdastComparison(fromMdast(unified().use(remarkParse).parse(markdown)));
    const dump =
        "Document scope=1:1..1:7 anchor=null attributes={} children=1\n" +
        '└── Heading scope=1:1..1:7 anchor="title" attributes={} level=1 children=1\n' +
        '    └── Text scope=1:3..1:7 anchor=null attributes={} literal="Title" children=0\n';
    const project = (value) => projectMdastComparison(parseCanonicalDump(value));
    assert.deepEqual(project(dump), expected);
    assert.deepEqual(project(dump.replace('anchor="title"', 'anchor="other"')), expected);
    for (const changed of [
        dump.replace("level=1", "level=2"),
        dump.replace('literal="Title"', 'literal="Changed"'),
        dump.replace('anchor="title" attributes={}', 'anchor="title" attributes={.heading}'),
        dump.replace("Text scope=1:3..1:7 anchor=null", 'Text scope=1:3..1:7 anchor="local"')
    ]) {
        assert.notDeepEqual(project(changed), expected, "shared facts must remain visible");
    }
});

test("the mdast intersection still compares explicit directive anchors", () => {
    const dump = (anchor) => `Directive scope=1:1..1:8 anchor=${anchor} attributes={.c} name="n" children=0\n`;
    assert.notDeepEqual(
        projectMdastComparison(parseCanonicalDump(dump('"one"'))),
        projectMdastComparison(parseCanonicalDump(dump('"two"')))
    );
});
