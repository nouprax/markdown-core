import assert from "node:assert/strict";
import { test } from "node:test";
import { digest, fromPandoc, fromCanonical, validatePolicy, verifyComparison } from "../lib/pandoc-oracle.mjs";
import { parseCanonicalDump } from "../lib/upstream-cmark.mjs";

test("Pandoc projection keeps attribute ordering, duplicates, content and owners", () => {
    const doc = (attr) => ({ blocks: [{ t: "Para", c: [{ t: "Code", c: [attr, "x"] }] }] });
    const expected = fromPandoc(
        doc([
            "id",
            ["a", "a"],
            [
                ["k", "1"],
                ["k", "2"]
            ]
        ])
    );
    const actual = fromCanonical(
        parseCanonicalDump(
            'Document scope=1:1..1:1 anchor=null attributes={} children=1\n└── Paragraph scope=1:1..1:1 anchor=null attributes={} children=1\n    └── Code scope=1:1..1:1 anchor="id" attributes={.a .a k="1" k="2"} literal="x" children=0\n'
        )
    );
    assert.equal(digest(actual), digest(expected));
    for (const attr of [
        [
            "other",
            ["a", "a"],
            [
                ["k", "1"],
                ["k", "2"]
            ]
        ],
        [
            "id",
            ["a"],
            [
                ["k", "1"],
                ["k", "2"]
            ]
        ],
        [
            "id",
            ["a", "a"],
            [
                ["k", "2"],
                ["k", "1"]
            ]
        ]
    ])
        assert.notEqual(digest(fromPandoc(doc(attr))), digest(expected));
    assert.throws(() => fromPandoc({ blocks: [{ t: "Unknown" }] }), /unmapped/);
});

test("Pandoc registry fails closed for new, changed, duplicate and stale evidence", () => {
    const testCase = { id: "case", from: "markdown_strict", input: "x" };
    const entry = {
        id: "case",
        item: "P5",
        status: "gap",
        reason: "bracketed spans are not yet implemented",
        inputDigest: digest([testCase.from, testCase.input]),
        oracleDigest: digest({ kind: "Span" }),
        coreDigest: digest({ kind: "Text" })
    };
    assert.equal(validatePolicy({ schemaVersion: 1, entries: [entry] }, [testCase]).size, 1);
    assert.equal(verifyComparison(testCase, { kind: "Span" }, { kind: "Text" }, entry), true);
    assert.throws(() => verifyComparison(testCase, {}, {}, entry), /stale/);
    assert.throws(() => verifyComparison(testCase, {}, { changed: true }), /unregistered/);
    assert.throws(
        () => verifyComparison({ ...testCase, input: "changed" }, { kind: "Span" }, { kind: "Text" }, entry),
        /changed input/
    );
    assert.throws(() => verifyComparison(testCase, {}, { kind: "Text" }, entry), /changed oracle/);
    assert.throws(() => verifyComparison(testCase, { kind: "Span" }, {}, entry), /changed core/);
    for (const entries of [
        [entry, entry],
        [{ ...entry, id: "unknown" }],
        [{ ...entry, coreDigest: "bad" }],
        [{ ...entry, status: "ignore" }]
    ])
        assert.throws(() => validatePolicy({ schemaVersion: 1, entries }, [testCase]));
});

test("Pandoc projection distinguishes absent anchors from the authored string null", () => {
    const parse = (anchor) =>
        fromCanonical(
            parseCanonicalDump(
                `Document scope=1:1..1:1 anchor=null attributes={} children=1\n└── Code scope=1:1..1:1 anchor=${anchor} attributes={} literal="x" children=0\n`
            )
        ).children[0].anchor;
    assert.equal(parse("null"), null);
    assert.equal(parse('"null"'), "null");
});

test("list variants and table columns compare as values rather than wire spellings", () => {
    const list = fromPandoc({ blocks: [{ t: "OrderedList", c: [[9, { t: "LowerRoman" }, { t: "OneParen" }], []] }] });
    const canonical = fromCanonical(
        parseCanonicalDump(
            "Document scope=1:1..1:1 anchor=null attributes={} children=1\n" +
                "└── List scope=1:1..1:1 anchor=null attributes={} flavor=ordered start=9 variant=roman(lowercased=true) delimiter=parenthesis(closed=false) tight=true children=0\n"
        )
    );
    assert.deepEqual(canonical, list);
    const table = fromCanonical(
        parseCanonicalDump(
            "Document scope=1:1..1:1 anchor=null attributes={} children=1\n└── Table scope=1:1..1:1 anchor=null attributes={} columns=[left:null,right:0.25] children=0\n"
        )
    );
    assert.deepEqual(table.children[0].columns, [
        { alignment: "left", relative: null },
        { alignment: "right", relative: 0.25 }
    ]);
});
