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
        { flow: "left", relative: null },
        { flow: "right", relative: 0.25 }
    ]);
});

test("citation projection compares keys, modes and ordered affixes without fallback rendering", () => {
    const make = (key = "key", mode = "SuppressAuthor", prefix = "pre", suffix = "tail") => ({
        blocks: [
            {
                t: "Para",
                c: [
                    {
                        t: "Cite",
                        c: [
                            [
                                {
                                    citationId: key,
                                    citationMode: { t: mode },
                                    citationPrefix: [{ t: "Str", c: prefix }],
                                    citationSuffix: [{ t: "Emph", c: [{ t: "Str", c: suffix }] }]
                                }
                            ],
                            [{ t: "Str", c: "[pre -@key tail]" }]
                        ]
                    }
                ]
            }
        ]
    });
    const actual = fromCanonical(
        parseCanonicalDump(
            "Document scope=1:1..1:16 anchor=null attributes={} children=1\n" +
                "└── Paragraph scope=1:1..1:16 anchor=null attributes={} children=1\n" +
                "    └── Cite scope=1:1..1:16 anchor=null attributes={} children=1\n" +
                '        └── Citation scope=1:2..1:15 referent=bib(key="key",mode=suppressAuthor) children=0\n' +
                "            ├── CitationPrefix children=1\n" +
                '            │   └── Text scope=1:2..1:4 anchor=null attributes={} literal="pre" children=0\n' +
                "            └── CitationSuffix children=1\n" +
                "                └── Emphasis scope=1:12..1:15 anchor=null attributes={} children=1\n" +
                '                    └── Text scope=1:12..1:15 anchor=null attributes={} literal="tail" children=0\n'
        )
    );
    assert.deepEqual(actual, fromPandoc(make()));
    for (const args of [
        ["other"],
        ["key", "NormalCitation"],
        ["key", "AuthorInText"],
        ["key", "SuppressAuthor", "different"],
        ["key", "SuppressAuthor", "pre", "different"]
    ]) {
        assert.notDeepEqual(actual, fromPandoc(make(...args)));
    }
});

test("definition projection preserves compactness, terms, body boundaries and nameless names", () => {
    const make = (first = "Plain", term = "T", bodies = [[{ t: first, c: [{ t: "Str", c: "body" }] }], []]) => ({
        blocks: [
            { t: "Div", c: [["", ["box"], []], [{ t: "DefinitionList", c: [[[{ t: "Str", c: term }], bodies]] }]] }
        ]
    });
    const dump =
        "Document scope=1:1..4:3 anchor=null attributes={} children=1\n" +
        "└── DirectiveBlock scope=1:1..4:3 anchor=null attributes={.box} name=null children=1\n" +
        "    └── DefinitionList scope=2:1..3:6 anchor=null attributes={} children=1\n" +
        "        └── Definition scope=2:1..3:6 anchor=null attributes={} compact=true children=2\n" +
        "            ├── DefinitionTerm children=1\n" +
        '            │   └── Text scope=2:1..2:1 anchor=null attributes={} literal="T" children=0\n' +
        "            ├── DefinitionBody children=1\n" +
        "            │   └── Paragraph scope=3:3..3:6 anchor=null attributes={} children=1\n" +
        '            │       └── Text scope=3:3..3:6 anchor=null attributes={} literal="body" children=0\n' +
        "            └── DefinitionBody children=0\n";
    const actual = fromCanonical(parseCanonicalDump(dump));
    assert.deepEqual(actual, fromPandoc(make()));
    assert.notDeepEqual(actual, fromPandoc(make("Para")));
    assert.notDeepEqual(actual, fromPandoc(make("Plain", "other")));
    assert.notDeepEqual(actual, fromPandoc(make("Plain", "T", [[], [{ t: "Plain", c: [{ t: "Str", c: "body" }] }]])));
    assert.notDeepEqual(actual, fromCanonical(parseCanonicalDump(dump.replace("compact=true", "compact=false"))));
    assert.notDeepEqual(actual, fromCanonical(parseCanonicalDump(dump.replace("name=null", 'name="box"'))));
    for (const bodies of [[[]], [[{ t: "CodeBlock", c: [["", [], []], "code"] }]]]) {
        const definition = fromPandoc(make("Plain", "T", bodies)).children[0].children[0].children[0];
        assert.equal(definition.compact, null);
        assert.equal(definition.children.length, bodies.length + 1);
    }
});
