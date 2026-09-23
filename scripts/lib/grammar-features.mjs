/** Concrete syntax productions for the feature corpus. These are fed to the
 * same product recognizer/inverse encoder as the historical corpus. A shared
 * feature uses the identical production, not an invented alternate spelling.
 * Native expectations are conformance witnesses, never proof admission rules.
 */
const f = (name, grammar = "word") => ({ name, grammar });
const b = f("body", "inline"),
    t = f("tail", "inline"),
    k = f("key"),
    v = f("value"),
    a = f("anchor"),
    target = f("target"),
    p = f("literal", "phrase");
const inline = (...parts) => ["probe ", ...parts, " end\n\n"];
const entries = [];
const ordinal = (encoding) => ({ name: "ordinal", grammar: "ordinal", encoding });
const state = (encoding) => ({ name: "state", grammar: "state", encoding });
function pair(id, feature, facets, dialect, common, kinds, options = {}) {
    if (feature === "attributes") {
        const restrict = (part) =>
            typeof part !== "string" && (part.name === "key" || (id === "attribute-order" && part.name === "target"))
                ? { ...part, grammar: "attribute-key" }
                : part;
        dialect = dialect.map(restrict);
        common = common.map(restrict);
    }
    entries.push({ id, feature, facets, dialect, common, kinds, ...options });
}
function shared(id, feature, facets, parts, kinds, options = {}) {
    pair(id, feature, facets, parts, parts, [kinds, kinds], { identity: true, ...options });
}

shared(
    "common-paragraph",
    "base",
    ["paragraphs", "blank-lines", "literal-punctuation"],
    [b, "\n\n", t, "... -- :emoji:\n\n"],
    ["Paragraph", "Text"]
);
shared("common-tabs", "base", ["tab-stops", "indented-content"], ["- ", k, "\n\n\t", b, "\n\n"], ["List", "ListItem"]);
shared(
    "common-escapes",
    "base",
    ["punctuation-escapes", "non-punctuation-backslashes"],
    inline("\\*", k, "\\* \\_", v, "\\_ \\a"),
    ["Text"]
);
shared(
    "common-entities",
    "base",
    ["named-entities", "decimal-entities", "hex-entities", "invalid-entities"],
    inline(k, " &amp; &#233; &#x5B57; &notanentity; &#0;", v),
    ["Text"]
);
for (const [name, eol] of [
    ["lf", "\n"],
    ["cr", "\r"],
    ["crlf", "\r\n"]
])
    shared(
        `common-eol-${name}`,
        "base",
        ["line-endings", "unicode"],
        [f("unicode", "unicode"), eol, b, eol, eol],
        ["Paragraph", "SoftBreak"]
    );
for (let level = 1; level <= 6; level++)
    shared(
        `common-atx-${level}`,
        "headings",
        ["atx-levels", "closing-hashes", "inline-content"],
        ["#".repeat(level), " ", b, " ##\n\n"],
        ["Heading"],
        { check: { kind: "Heading", fields: { level: String(level) } } }
    );
for (const [name, marker, level] of [
    ["one", "=", "1"],
    ["two", "-", "2"]
])
    shared(
        `common-setext-${name}`,
        "headings",
        ["setext", "multiline", "block-precedence"],
        [b, "\n", t, "\n", marker.repeat(3), "\n\n"],
        ["Heading", "SoftBreak"],
        { check: { kind: "Heading", fields: { level } } }
    );
for (const marker of ["*", "_"])
    shared(
        `common-emphasis-${marker === "*" ? "asterisk" : "underscore"}`,
        "emphasis",
        ["emphasis", "strong", "nesting", "flanking"],
        inline(
            marker,
            k,
            marker,
            " ",
            marker.repeat(2),
            v,
            marker.repeat(2),
            " ",
            marker.repeat(3),
            target,
            marker.repeat(3)
        ),
        ["Emphasis", "Strong"]
    );
shared("common-softbreak", "line-breaks", ["soft-breaks"], [b, "\n", t, "\n\n"], ["SoftBreak"]);
for (const [name, marker] of [
    ["spaces", "  "],
    ["backslash", "\\"]
])
    shared(
        `common-hardbreak-${name}`,
        "line-breaks",
        ["hard-breaks", "source-ranges"],
        [b, marker, "\n", t, "\n\n"],
        ["LineBreak"]
    );
for (const [name, marker] of [
    ["star", "* * *"],
    ["dash", "- - -"],
    ["underscore", "___"]
])
    shared(
        `common-thematic-${name}`,
        "thematic-breaks",
        ["markers", "spacing", "block-precedence"],
        [b, "\n\n", marker, "\n\n", t, "\n\n"],
        ["ThematicBreak"]
    );
shared(
    "common-code-span",
    "code",
    ["code-span", "delimiter-length", "padding", "opaque-content"],
    inline("`` `", k, "` `` ` ", v, " `"),
    ["Code"]
);
shared("common-code-newline", "code", ["line-normalization"], inline("`", k, "\n", v, "`"), ["Code"]);
for (const [name, marker] of [
    ["backtick", "```"],
    ["tilde", "~~~"]
])
    shared(
        `common-code-fence-${name}`,
        "code",
        ["fenced-code", "info-string", "opaque-content"],
        [marker, "lang\n", p, "\n", marker, "\n\n"],
        ["CodeBlock"]
    );
shared(
    "common-indented-code",
    "code",
    ["indented-code", "blank-lines"],
    ["    ", k, "\n\n    ", v, "\n\n"],
    ["CodeBlock"]
);
shared(
    "common-inline-html",
    "html",
    ["tags", "attributes", "inline-boundaries"],
    inline('<span data-key="', k, '">', b, "</span>"),
    ["HTML"]
);
const htmlFrames = [
    ["raw", "<script>\n", "\n</script>\n\n"],
    ["instruction", "<?processing ", "?>\n\n"],
    ["declaration", "<!DOCTYPE ", ">\n\n"],
    ["cdata", "<![CDATA[", "]]>\n\n"],
    ["block-tag", "<div>\n", "\n</div>\n\n"],
    ["complete-tag", '<custom data-key="', '">\n\n']
];
for (const [name, open, close] of htmlFrames)
    shared(
        `common-html-${name}`,
        "html",
        ["html-block-types", "opaque-content", "termination"],
        [open, k, close],
        ["HTMLBlock"]
    );
shared("common-html-comment", "comments", ["html-comments", "block-comments"], ["<!-- ", p, " -->\n\n"], ["Comment"]);
shared("common-inline-comment", "comments", ["html-comments", "inline-comments"], inline("<!-- ", p, " -->"), [
    "Comment"
]);
shared(
    "common-link-direct",
    "links-and-images",
    ["direct-links", "destinations", "titles"],
    inline("[", b, "](/", target, ' "', v, '")'),
    ["Link"]
);
shared("common-image", "links-and-images", ["images", "formatted-alt"], inline("![", b, "](/", target, ' "', v, '")'), [
    "Embedded"
]);
for (const [name, label] of [
    ["full", ["[", b, "][", k, "]"]],
    ["collapsed", ["[", k, "][]"]],
    ["shortcut", ["[", k, "]"]]
])
    shared(
        `common-reference-${name}`,
        "links-and-images",
        ["reference-forms", "forward-resolution", "label-normalization"],
        [...inline(...label), "[", k, "]: /", target, ' "', v, '"\n\n'],
        ["Link"]
    );
shared(
    "common-reference-duplicate",
    "links-and-images",
    ["duplicate-definitions", "first-definition"],
    ["[", k, "]\n\n[", k, "]: /", target, "\n[", k, "]: /", v, "\n\n"],
    ["Link"]
);
shared(
    "common-unresolved-reference",
    "links-and-images",
    ["unresolved-references", "bracket-fallback"],
    inline("[", b, "][missing]"),
    ["Text", "Strong"]
);
shared(
    "common-angle-autolink",
    "links-and-images",
    ["angle-url", "angle-email"],
    inline("<https://", k, ".example/> <", v, "@example.org>"),
    ["Link"]
);
shared(
    "gfm-bare-autolink",
    "links-and-images",
    ["bare-url", "bare-email", "punctuation-trimming"],
    inline("https://", k, ".example/", v, ". ", target, "@example.org"),
    ["Link"],
    { gfm: true }
);
for (const [name, marker] of [
    ["dash", "-"],
    ["plus", "+"],
    ["star", "*"]
])
    shared(
        `common-bullet-${name}`,
        "lists",
        ["bullet-markers", "tight-lists", "nesting"],
        [marker, " ", b, "\n  ", marker, " ", t, "\n\n"],
        ["List", "ListItem"]
    );
shared(
    "common-list-loose",
    "lists",
    ["loose-lists", "empty-items", "continuation"],
    ["1. ", b, "\n\n2. ", t, "\n\n3.\n\n"],
    ["List", "ListItem"]
);
shared(
    "common-list-parenthesis",
    "lists",
    ["decimal-delimiters", "decimal-start"],
    ["0) ", b, "\n1) ", t, "\n\n"],
    ["List"]
);
shared(
    "common-quote",
    "callouts",
    ["plain-quotes", "nested-quotes", "lazy-continuation"],
    ["> ", b, "\n", t, "\n>\n>> ", k, "\n\n"],
    ["Callout"]
);
for (const [name, marker] of [
    ["open", " "],
    ["lower", "x"],
    ["upper", "X"]
])
    shared(
        `gfm-task-${name}`,
        "task-lists",
        ["standard-markers", "opening-content"],
        ["- [", marker, "] ", b, "\n\n"],
        ["ListItem"],
        { gfm: true }
    );
shared("gfm-strike", "strikethrough", ["pairs", "inline-content"], inline("~~", b, "~~"), ["Strikethrough"], {
    gfm: true
});
shared(
    "gfm-pipe-table",
    "tables",
    ["pipe-tables", "alignment", "inline-cells"],
    ["| ", k, " | ", target, " |\n| :--- | ---: |\n| ", b, " | ", t, " |\n\n"],
    ["Table", "TableCell"],
    { gfm: true }
);
shared(
    "gfm-pipe-ragged",
    "tables",
    ["short-rows", "extra-cells", "escaped-pipes", "code-pipes"],
    ["| ", k, " | ", v, " |\n| --- | --- |\n| `a\\|b` |\n| ", target, " | c | dropped |\n\n"],
    ["Table", "Code"],
    { gfm: true }
);
shared(
    "gfm-footnote",
    "footnotes",
    ["reference-notes", "forward-resolution", "definition-bodies"],
    ["probe [^", k, "] end\n\n[^", k, "]: ", b, "\n\n"],
    ["Footnote", "Cite"],
    { gfm: true }
);

// Independent lexical fields are retained in the alternate grammar. These
// frames do not identify whole unrestricted features with CommonMark.
pair("formula-backtick", "formulas", ["backtick-dollar"], inline("$`", p, "`$"), inline("`", p, "`"), [
    ["Formula"],
    ["Code"]
]);
for (const [name, open, close] of [
    ["parenthesis", "\\\\(", "\\\\)"],
    ["bracket", "\\\\[", "\\\\]"]
])
    pair(
        `formula-${name}`,
        "formulas",
        ["backslash-brackets", "placement"],
        inline(open, p, close),
        inline("`", p, "`"),
        [["Formula"], ["Code"]]
    );
pair(
    "formula-bracket-block",
    "formulas",
    ["bracket-blocks", "block-trimming"],
    ["\\\\[\n", p, "\n\\\\]\n\n"],
    ["```\n", p, "\n```\n\n"],
    [["FormulaBlock"], ["CodeBlock"]]
);
pair(
    "script-formatting",
    "superscript-and-subscript",
    ["formatted-body"],
    inline("^*", k, "*^"),
    inline("***", k, "***"),
    [
        ["Superscript", "Emphasis"],
        ["Strong", "Emphasis"]
    ]
);
pair("script-empty", "superscript-and-subscript", ["empty-superscript"], inline("^^", k), inline("[](/empty)", k), [
    ["Superscript"],
    ["Link"]
]);
pair(
    "script-escaped-space",
    "superscript-and-subscript",
    ["escaped-space", "nbsp"],
    inline("^", k, "\\ ", v, "^"),
    inline("*", k, "&#160;", v, "*"),
    [["Superscript"], ["Emphasis"]]
);
pair("span-empty", "bracketed-spans", ["empty-spans", "classes"], inline("[]{.", v, "}"), inline("[](/", v, ")"), [
    ["Span"],
    ["Link"]
]);
pair(
    "span-nested",
    "bracketed-spans",
    ["nested-content", "attribute-precedence"],
    inline("[", b, "]{.", v, "}"),
    inline("[", b, "](/", v, ")"),
    [["Span"], ["Link"]]
);
for (const [name, open, close] of [
    ["unquoted", "=", ""],
    ["single", "='", "'"],
    ["double", '="', '"']
])
    pair(
        `attribute-${name}`,
        "attributes",
        ["record-values", "quote-forms"],
        inline("[", b, "]{", k, open, v, close, "}"),
        inline("[", b, "](/", k, ' "', v, '")'),
        [["Span"], ["Link"]]
    );
pair("attribute-empty", "attributes", ["empty-values"], inline("[]{", k, '=""}'), inline("[](/", k, ' "")'), [
    ["Span"],
    ["Link"]
]);
pair(
    "attribute-order",
    "attributes",
    ["duplicate-records", "ordered-classes", "adjacent-members"],
    inline("[]{.", k, " .", v, " ", target, "=first ", target, "=second}"),
    inline("[", k, " ", v, "](/", target, ' "first second")'),
    [["Span"], ["Link"]]
);
pair(
    "cross-block-anchor",
    "cross-links",
    ["block-target", "raw-anchor"],
    inline("[[", target, "#^", a, "]]"),
    inline("[](/", target, "#^", a, ")"),
    [["CrossLink"], ["Link"]]
);
pair(
    "cross-raw-label",
    "cross-links",
    ["opaque-label", "empty-path"],
    inline("[[#", a, "|**", k, "**]]"),
    inline("[](#", a, ' "**', k, '**")'),
    [["CrossLink"], ["Link"]]
);
pair(
    "inline-footnote",
    "footnotes",
    ["inline-notes", "inline-body"],
    inline("^[", b, "]"),
    inline("[", b, "](/note)"),
    [["Footnote", "Cite"], ["Link"]]
);
pair(
    "directive-attribute-only",
    "directives",
    ["attribute-only", "part-anchor"],
    inline(":", k, "{.", v, "}"),
    inline("[](/", k, ' "', v, '")'),
    [["Directive"], ["Link"]]
);
pair(
    "directive-both-parts",
    "directives",
    ["label-and-attributes"],
    inline(":", k, "[", b, "]{.", v, "}"),
    inline("[", b, "](/", k, ' "', v, '")'),
    [["Directive"], ["Link"]]
);
pair(
    "directive-unicode",
    "directives",
    ["unicode-names", "raw-name"],
    inline(":", f("unicode", "unicode"), "[", b, "]"),
    inline("[", b, "](/", f("unicode", "unicode"), ")"),
    [["Directive"], ["Link"]]
);
pair(
    "directive-leaf-bare",
    "directives",
    ["leaf-without-parts"],
    ["::", k, "\n\n"],
    ["[](/", k, ")\n\n"],
    [["DirectiveBlock"], ["Link"]]
);
pair(
    "directive-nested",
    "directives",
    ["nested-containers", "minimum-closer", "longer-closer"],
    ["::::", k, "\n:::", v, "\n", b, "\n:::\n:::::\n\n"],
    ["> ", k, "\n>\n>> ", v, "\n>>\n>> ", b, "\n\n"],
    [["DirectiveBlock"], ["Callout"]]
);
pair(
    "directive-unbraced",
    "directives",
    ["nameless-unbraced", "trailing-colons"],
    ["::: ", k, " :::\n", b, "\n:::\n\n"],
    ["> ", k, "\n>\n> ", b, "\n\n"],
    [["DirectiveBlock"], ["Callout"]]
);
pair(
    "block-id-paragraph",
    "block-identifiers",
    ["paragraph-suffix", "anchor-declaration"],
    [b, " #", k, "#\n\n"],
    [b, "\n\n[", k, "]: /target\n\n"],
    [["Paragraph"], ["Paragraph"]]
);
pair(
    "heading-explicit-id",
    "anchors",
    ["explicit-anchor", "attribute-override"],
    ["# ", b, " {#", k, "}\n\n"],
    ["# ", b, "\n\n[", k, "]: /target\n\n"],
    [["Heading"], ["Heading"]]
);
// Repeated labels in these products are equality constraints on source fields.
pair(
    "heading-reference",
    "anchors",
    ["implicit-references", "forward-resolution"],
    ["[", k, "]\n\n# ", k, "\n\n"],
    ["[", k, "]\n\n[", k, "]: #", k, "\n\n"],
    [["Link", "Heading"], ["Link"]]
);
pair(
    "citation-group",
    "citations",
    ["groups", "modes", "semicolons"],
    inline("[@", k, "; -@", target, "]"),
    inline("[", k, "](/normal) [", target, "](/suppressed)"),
    [["Citation", "Cite"], ["Link"]]
);
pair(
    "task-unicode",
    "task-lists",
    ["unicode-marker", "authored-marker"],
    ["- [✓] ", b, "\n\n"],
    ["- [x] ", b, "\n\n"],
    [["ListItem"], ["ListItem"]],
    { gfm: true }
);
pair(
    "task-heading",
    "task-lists",
    ["heading-body", "first-block"],
    ["- [!] # ", b, "\n\n"],
    ["- # ", b, "\n\n"],
    [
        ["ListItem", "Heading"],
        ["ListItem", "Heading"]
    ]
);

// Replacements for incorrect historical counterparts. The first marker fixes
// the variant's context; later i/I cannot accidentally start another variant.
for (const [id, encoding, first, left, right] of [
    ["alpha-list", "alpha", "a)", "", ")"],
    ["upper-list", "upper-alpha", "(A)", "(", ")"],
    ["roman-list", "roman", "i.", "", "."],
    ["upper-roman-list", "upper-roman", "I)", "", ")"]
])
    pair(
        id,
        "lists",
        ["extended-ordered-markers", "variant-context", "canonical-numerals"],
        [first, " ", b, "\n", left, ordinal(encoding), right, " ", t, "\n\n"],
        ["1. ", b, "\n", ordinal("decimal"), ". ", t, "\n\n"],
        [["List"], ["List"]]
    );
for (const [id, marker, common] of [
    ["default-list", "#.", "1."],
    ["enclosed-default-list", "(#)", "1)"]
])
    pair(
        id,
        "lists",
        ["automatic-markers", "fixed-start"],
        [marker, " ", b, "\n", marker, " ", t, "\n\n"],
        [common, " ", b, "\n", common, " ", t, "\n\n"],
        [["List"], ["List"]]
    );
pair(
    "callout",
    "callouts",
    ["custom-type", "case", "three-collapse-states", "title"],
    ["> [!", k, "]", state("callout"), " ", b, "\n\n"],
    ["> [", b, "](/", k, ' "', state("title"), '")\n\n'],
    [["Callout"], ["Callout", "Link"]]
);
pair(
    "citation-affixes",
    "citations",
    ["prefix", "suffix", "key"],
    inline("[", p, " @", k, ", ", v, "]"),
    inline("[", p, "](/", k, ' "', v, '")'),
    [["Citation", "CitationPrefix", "CitationSuffix"], ["Link"]]
);
for (const id of ["leading-caption", "trailing-caption"]) {
    const table = ["| ", k, " |\n| --- |\n| ", v, " |\n\n"];
    pair(
        id,
        "tables",
        ["captions", id],
        id === "leading-caption" ? ["Table: ", p, "\n", ...table] : [...table, ": ", p, "\n\n"],
        id === "leading-caption" ? [p, "\n\n", ...table] : [...table, p, "\n\n"],
        [
            ["Table", "TableCaption"],
            ["Table", "Paragraph"]
        ],
        { gfm: true }
    );
}
pair(
    "mixed-definitions",
    "definition-lists",
    ["multiple-definitions", "loose-definitions", "empty-definitions"],
    [k, "\n: ", b, "\n: ", t, "\n\n", a, "\n\n: ", p, "\n\n", target, "\n:", f("empty", "empty"), "\n\n"],
    [
        "- ",
        k,
        "\n  - ",
        b,
        "\n  - ",
        t,
        "\n\n- ",
        a,
        "\n\n  ",
        p,
        "\n\n- ",
        target,
        "\n  [",
        f("empty", "empty"),
        "](/empty)\n\n"
    ],
    [["DefinitionList", "DefinitionBody"], ["List"]]
);
pair(
    "specimen-graph",
    "specimens",
    ["labelled-definitions", "references", "binding"],
    ["As (@", k, ") shows.\n\n(@", k, ") ", b, "\n\n"],
    ["As [^", k, "] shows.\n\n[^", k, "]: ", b, "\n\n"],
    [
        ["Specimen", "Citation"],
        ["Footnote", "Cite"]
    ],
    { gfm: true }
);

pair(
    "attribute-code",
    "attributes",
    ["code-site"],
    inline("`", p, "`{", k, "=", v, "}"),
    inline("[`", p, "`](/", k, ' "', v, '")'),
    [["Code"], ["Code", "Link"]]
);
pair(
    "attribute-heading",
    "attributes",
    ["heading-site", "closing-hashes"],
    ["## ", b, " ## {#", k, " .", v, "}\n\n"],
    ["## [", b, "](/", k, ' "', v, '")\n\n'],
    [["Heading"], ["Heading", "Link"]]
);
pair(
    "attribute-fence",
    "attributes",
    ["fence-site"],
    ["```lang {", k, "=", v, "}\n", p, "\n```\n\n"],
    ["```lang\n", p, "\n```\n\n[](/", k, ' "', v, '")\n\n'],
    [["CodeBlock"], ["CodeBlock", "Link"]]
);
for (const [name, marker, kind] of [
    ["link", "", "Link"],
    ["image", "!", "Embedded"]
])
    pair(
        `attribute-${name}`,
        "attributes",
        ["occurrence-site", `${name}-site`],
        inline(marker, "[", b, "](/", target, "){", k, "=", v, "}"),
        inline(marker, "[", b, "](/", target, ") [](/", k, ' "', v, '")'),
        [[kind], [kind, "Link"]]
    );
pair(
    "attribute-autolink",
    "attributes",
    ["angle-autolink-site"],
    inline("<https://", target, ">{", k, "=", v, "}"),
    inline("<https://", target, "> [](/", k, ' "', v, '")'),
    [["Link"], ["Link"]]
);
pair(
    "attribute-reference",
    "attributes",
    ["definition-site", "inheritance", "occurrence-order", "anchor-override"],
    ["[", b, "][", k, "]{#", a, " .", v, "}\n\n[", k, "]: /", target, " {.base}\n\n"],
    ["[", b, "][", k, "] [](#", a, ' "', v, '")\n\n[', k, "]: /", target, "\n\n"],
    [["Link"], ["Link"]]
);
pair(
    "attribute-escaped-value",
    "attributes",
    ["double-quote-escapes", "entities", "single-quote-literals"],
    inline("[]{", k, '="', v, '&amp;\\*"}'),
    inline("[](/", k, ' "', v, '&amp;\\*")'),
    [["Span"], ["Link"]]
);
pair(
    "attribute-newline",
    "attributes",
    ["single-line-ending", "class-splitting"],
    inline('[]{class="', k, " ", v, '"\n}'),
    inline("[", k, " ", v, "](/class)"),
    [["Span"], ["Link"]]
);
pair(
    "anchor-last-empty",
    "anchors",
    ["last-id", "empty-id-clears"],
    inline("[]{#", k, " id=", v, ' id=""}'),
    inline("[](/", k, ' "', v, '")'),
    [["Span"], ["Link"]]
);
shared(
    "common-anchor-generation",
    "anchors",
    ["unicode-lowercase", "collisions", "empty-base"],
    ["# É字", k, "\n\n# É字", k, "\n\n# !!!\n\n"],
    ["Heading"]
);
pair(
    "block-id-list",
    "block-identifiers",
    ["list-item-suffix", "standalone-list-id"],
    ["- ", b, " #", k, "#\n\n#", v, "#\n\n"],
    ["- ", b, "\n\n[", k, "]: /item\n[", v, "]: /list\n\n"],
    [
        ["ListItem", "List"],
        ["ListItem", "List"]
    ]
);
pair(
    "footnote-nested-inline",
    "footnotes",
    ["nested-inline-notes", "generated-identities"],
    inline("^[", k, " ^[", v, "] ", target, "]"),
    inline("*", k, " **", v, "** ", target, "*"),
    [
        ["Footnote", "Cite"],
        ["Emphasis", "Strong"]
    ]
);
shared(
    "gfm-footnote-cycle",
    "footnotes",
    ["self-reference", "semantic-cycles"],
    ["probe [^", k, "]\n\n[^", k, "]: ", b, " [^", k, "]\n\n"],
    ["Footnote", "Cite"],
    { gfm: true }
);
shared(
    "footnote-retention",
    "footnotes",
    ["unused-definitions", "duplicate-definitions", "repeated-calls"],
    ["probe [^", k, "] [^", k, "] end\n\n[^", k, "]: ", b, "\n\n[^", k, "]: ", t, "\n\n[^", target, "]: ", p, "\n\n"],
    ["Footnote", "Cite"],
    {
        gfm: true,
        outputDifference:
            "Core retains all authored definitions; GFM emits only the first used definition of each normalized ID. Grammar identity does not identify their output work."
    }
);
pair(
    "comment-empty",
    "comments",
    ["empty-percent-comment", "adjacent-delimiters"],
    inline("%%%%", k),
    inline("<!-- -->", k),
    [["Comment"], ["Comment"]]
);
pair(
    "definition-blocks",
    "definition-lists",
    ["multiple-blocks", "nested-content"],
    [k, "\n\n: ", b, "\n\n    ", t, "\n\n"],
    ["- ", k, "\n\n  ", b, "\n\n  ", t, "\n\n"],
    [["DefinitionList", "DefinitionBody"], ["List"]]
);
pair(
    "composition-directive-callout",
    "conflicts",
    ["block-containers", "inline-composition"],
    ["> :::", k, "\n> ", b, "\n> :::\n\n"],
    ["> > ", k, "\n> >\n> > ", b, "\n\n"],
    [["Callout", "DirectiveBlock"], ["Callout"]]
);
pair(
    "composition-literal-islands",
    "conflicts",
    ["literal-ownership", "inline-precedence"],
    inline("++", k, " `++", v, "++` tail++"),
    inline("**", k, " `++", v, "++` tail**"),
    [
        ["Insertion", "Code"],
        ["Strong", "Code"]
    ]
);

// Negative productions have infinite variable fields too. The owning rule's
// rejected prefix is fixed; fallback is positively checked with both parsers.
for (const [id, feature, parts, forbidden] of [
    ["insertion", "insertion", inline("++", b), "Insertion"],
    ["mark", "marks", inline("==", b), "Mark"],
    ["strike", "strikethrough", inline("~~", b), "Strikethrough"],
    ["formula", "formulas", inline("$", b), "Formula"],
    ["comment", "comments", inline("%%", b), "Comment"],
    ["cross-link", "cross-links", inline("[[", k), "CrossLink"],
    ["directive", "directives", inline(":", k), "Directive"],
    ["attributes", "attributes", inline("[", b, "]{=broken}"), "Span"],
    ["script", "superscript-and-subscript", inline("^", k, " ", v, "^"), "Superscript"],
    ["list-limit", "lists", ["1234567890. ", b, "\n\n"], "List"],
    ["footnote", "footnotes", inline("[^", k, "]"), "Footnote"],
    ["grid", "tables", ["+---+\n|", k, "\n+---+\n\n"], "Table"],
    ["citation", "citations", inline("@-", k), "Citation"],
    ["span", "bracketed-spans", inline("[", b, "]{"), "Span"]
])
    shared(`fallback-${id}`, feature, ["fallback", "rejected-prefix"], parts, ["Text"], { forbidden });
shared(
    "fallback-image-dimensions",
    "links-and-images",
    ["dimension-fallback", "leading-zero"],
    inline("![", k, "|01x20](/", target, ")"),
    ["Embedded"]
);
shared(
    "fallback-noninitial-metadata",
    "properties",
    ["document-initial-only", "envelope-fallback"],
    ["probe\n\n---\n", k, ": ", v, "\n---\n\n"],
    ["Heading", "ThematicBreak"]
);
pair(
    "attribute-bare",
    "attributes",
    ["bare-names", "boolean-record", "unnumbered"],
    inline("[]{", k, " -}"),
    inline("[](/", k, ' "true unnumbered")'),
    [["Span"], ["Link"]]
);

export const featureGrammars = new Map(entries.map((entry) => [entry.id, Object.freeze(entry)]));
if (featureGrammars.size !== entries.length) throw new Error("duplicate feature grammar");

/** Grammar slots, not a list of accepted sample strings. Each independent
 * name receives a separately generated value of its declared lexical type. */
export function featureValues(parameters) {
    return { unicode: `é字${parameters.value}`, empty: "", ordinal: parameters.start, state: parameters.mode };
}

// Finite lexical substitutions are explicit bijections, not numeric-value
// quotients of noncanonical Roman spellings. The domain is 1..26 in both
// grammars. Outside it the historical numeral boundary remains unproved.
const roman = [
    "i",
    "ii",
    "iii",
    "iv",
    "v",
    "vi",
    "vii",
    "viii",
    "ix",
    "x",
    "xi",
    "xii",
    "xiii",
    "xiv",
    "xv",
    "xvi",
    "xvii",
    "xviii",
    "xix",
    "xx",
    "xxi",
    "xxii",
    "xxiii",
    "xxiv",
    "xxv",
    "xxvi"
];
export const finiteLexicons = {
    ordinal: {
        decimal: roman.map((_, i) => String(i + 1)),
        alpha: roman.map((_, i) => String.fromCharCode(97 + i)),
        "upper-alpha": roman.map((_, i) => String.fromCharCode(65 + i)),
        roman,
        "upper-roman": roman.map((s) => s.toUpperCase())
    },
    state: { callout: ["", "-", "+"], title: ["plain", "closed", "open"] }
};
