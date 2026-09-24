/** Concrete syntax productions for the feature corpus. These are fed to the
 * same product recognizer/inverse encoder as the historical corpus. A shared
 * feature uses the identical production, not an invented alternate spelling.
 * Native parser correctness belongs to parity and regression pipelines.
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
// Pinned cmark/Core link labels admit at most 1000 source bytes. A GFM
// footnote call includes its leading caret in that counter, leaving 999 for
// this ASCII key. These are grammar constraints on every occurrence of the
// binding, including an unrestricted dialect-side key paired with a label.
const labelBounds = {
    "common-reference-full": { key: 1000 },
    "common-reference-collapsed": { key: 1000 },
    "common-reference-shortcut": { key: 1000 },
    "common-reference-duplicate": { key: 1000 },
    "heading-reference": { key: 1000 },
    "heading-explicit-id": { key: 1000 },
    "block-id-paragraph": { key: 1000 },
    "block-id-list": { key: 1000, value: 1000 },
    "block-id-container-list": { key: 1000 },
    "block-id-container-table": { key: 1000 },
    "attribute-reference": { key: 1000 },
    "gfm-footnote": { key: 999 },
    "gfm-footnote-cycle": { key: 999 },
    "gfm-footnote-blocks": { key: 999 },
    "footnote-retention": { key: 999, target: 999 },
    "specimen-graph": { key: 999 },
    "specimen-reset": { key: 1000 },
    "specimen-groups": { key: 1000 }
};
// The constructs rejection certificates are built around, each with the pinned
// references that implement it. A reference ratio compares two implementations
// of the same work only when the reference has the construct the certificate
// exercises; an empty list marks a construct only Core has.
export const rejectedConstructs = Object.freeze({
    "ordered-list-marker": Object.freeze(["cmark", "cmark-gfm"]),
    "reference-link": Object.freeze(["cmark", "cmark-gfm"]),
    strikethrough: Object.freeze(["cmark-gfm"]),
    "footnote-reference": Object.freeze(["cmark-gfm"]),
    "task-list-marker": Object.freeze(["cmark-gfm"]),
    insertion: Object.freeze([]),
    mark: Object.freeze([]),
    "inline-formula": Object.freeze([]),
    "percent-comment": Object.freeze([]),
    "cross-link": Object.freeze([]),
    "inline-directive": Object.freeze([]),
    "attribute-block": Object.freeze([]),
    superscript: Object.freeze([]),
    "grid-table": Object.freeze([]),
    citation: Object.freeze([]),
    "bracketed-span": Object.freeze([]),
    "image-dimensions": Object.freeze([]),
    "metadata-envelope": Object.freeze([])
});

// A control, when declared, is a third production of the same fields.
function pair(id, feature, facets, dialect, common, options = {}) {
    const productions = { dialect, common, ...(options.control ? { control: options.control } : {}) };
    for (const [name, parts] of Object.entries(productions)) {
        productions[name] = parts.map((part) => {
            if (typeof part === "string") return part;
            if (
                feature === "attributes" &&
                (part.name === "key" || (id === "attribute-order" && part.name === "target"))
            )
                part = { ...part, grammar: "attribute-key" };
            if (labelBounds[id]?.[part.name]) part = { ...part, maxBytes: labelBounds[id][part.name] };
            return part;
        });
    }
    entries.push({ id, feature, facets, ...options, ...productions });
}
function shared(id, feature, facets, parts, options = {}) {
    pair(id, feature, facets, parts, parts, { identity: true, ...options });
}

shared(
    "common-paragraph",
    "base",
    ["paragraphs", "blank-lines", "literal-punctuation"],
    [b, "\n\n", t, "... -- :emoji:\n\n"]
);
shared("common-tabs", "base", ["tab-stops", "indented-content"], ["- ", k, "\n\n\t", b, "\n\n"]);
shared(
    "common-escapes",
    "base",
    ["punctuation-escapes", "non-punctuation-backslashes"],
    inline("\\*", k, "\\* \\_", v, "\\_ \\a")
);
shared(
    "common-entities",
    "base",
    ["named-entities", "decimal-entities", "hex-entities", "invalid-entities"],
    inline(k, " &amp; &#233; &#x5B57; &notanentity; &#0;", v)
);
for (const [name, eol] of [
    ["lf", "\n"],
    ["cr", "\r"],
    ["crlf", "\r\n"]
])
    shared(`common-eol-${name}`, "base", ["line-endings", "unicode"], [f("unicode", "unicode"), eol, b, eol, eol]);
for (let level = 1; level <= 6; level++)
    shared(
        `common-atx-${level}`,
        "headings",
        ["atx-levels", "closing-hashes", "inline-content"],
        ["#".repeat(level), " ", b, " ##\n\n"]
    );
for (const [name, marker] of [
    ["one", "="],
    ["two", "-"]
])
    shared(
        `common-setext-${name}`,
        "headings",
        ["setext", "multiline", "block-precedence"],
        [b, "\n", t, "\n", marker.repeat(3), "\n\n"]
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
        )
    );
shared("common-softbreak", "line-breaks", ["soft-breaks"], [b, "\n", t, "\n\n"]);
for (const [name, marker] of [
    ["spaces", "  "],
    ["backslash", "\\"]
])
    shared(`common-hardbreak-${name}`, "line-breaks", ["hard-breaks", "source-ranges"], [b, marker, "\n", t, "\n\n"]);
for (const [name, marker] of [
    ["star", "* * *"],
    ["dash", "- - -"],
    ["underscore", "___"]
])
    shared(
        `common-thematic-${name}`,
        "thematic-breaks",
        ["markers", "spacing", "block-precedence"],
        [b, "\n\n", marker, "\n\n", t, "\n\n"]
    );
shared(
    "common-code-span",
    "code",
    ["code-span", "delimiter-length", "padding", "opaque-content"],
    inline("`` `", k, "` `` ` ", v, " `")
);
shared("common-code-newline", "code", ["line-normalization"], inline("`", k, "\n", v, "`"));
for (const [name, marker] of [
    ["backtick", "```"],
    ["tilde", "~~~"]
])
    shared(
        `common-code-fence-${name}`,
        "code",
        ["fenced-code", "info-string", "opaque-content"],
        [marker, "lang\n", p, "\n", marker, "\n\n"]
    );
shared("common-indented-code", "code", ["indented-code", "blank-lines"], ["    ", k, "\n\n    ", v, "\n\n"]);
shared(
    "common-inline-html",
    "html",
    ["tags", "attributes", "inline-boundaries"],
    inline('<span data-key="', k, '">', b, "</span>")
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
    shared(`common-html-${name}`, "html", ["html-block-types", "opaque-content", "termination"], [open, k, close]);
shared("common-html-comment", "comments", ["html-comments", "block-comments"], ["<!-- ", p, " -->\n\n"]);
shared("common-inline-comment", "comments", ["html-comments", "inline-comments"], inline("<!-- ", p, " -->"));
shared(
    "common-link-direct",
    "links-and-images",
    ["direct-links", "destinations", "titles"],
    inline("[", b, "](/", target, ' "', v, '")')
);
shared("common-image", "links-and-images", ["images", "formatted-alt"], inline("![", b, "](/", target, ' "', v, '")'));
for (const [name, label] of [
    ["full", ["[", b, "][", k, "]"]],
    ["collapsed", ["[", k, "][]"]],
    ["shortcut", ["[", k, "]"]]
])
    shared(
        `common-reference-${name}`,
        "links-and-images",
        ["reference-forms", "forward-resolution", "label-normalization"],
        [...inline(...label), "[", k, "]: /", target, ' "', v, '"\n\n']
    );
shared(
    "common-reference-duplicate",
    "links-and-images",
    ["duplicate-definitions", "first-definition"],
    ["[", k, "]\n\n[", k, "]: /", target, "\n[", k, "]: /", v, "\n\n"]
);
shared(
    "common-unresolved-reference",
    "links-and-images",
    ["unresolved-references", "bracket-fallback"],
    inline("[", b, "][missing]"),
    { rejects: "reference-link" }
);
shared(
    "common-angle-autolink",
    "links-and-images",
    ["angle-url", "angle-email"],
    inline("<https://", k, ".example/> <", v, "@example.org>")
);
shared(
    "gfm-bare-autolink",
    "links-and-images",
    ["bare-url", "bare-email", "punctuation-trimming"],
    inline("https://", k, ".example/", v, ". ", target, "@example.org"),
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
        [marker, " ", b, "\n  ", marker, " ", t, "\n\n"]
    );
shared(
    "common-list-loose",
    "lists",
    ["loose-lists", "empty-items", "continuation"],
    ["1. ", b, "\n\n2. ", t, "\n\n3.\n\n"]
);
shared("common-list-parenthesis", "lists", ["decimal-delimiters", "decimal-start"], ["0) ", b, "\n1) ", t, "\n\n"]);
shared(
    "common-quote",
    "callouts",
    ["plain-quotes", "nested-quotes", "lazy-continuation"],
    ["> ", b, "\n", t, "\n>\n>> ", k, "\n\n"]
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
        { gfm: true }
    );
shared("gfm-strike", "strikethrough", ["pairs", "inline-content"], inline("~~", b, "~~"), {
    gfm: true
});
shared(
    "gfm-pipe-table",
    "tables",
    ["pipe-tables", "alignment", "inline-cells"],
    ["| ", k, " | ", target, " |\n| :--- | ---: |\n| ", b, " | ", t, " |\n\n"],
    { gfm: true }
);
shared(
    "gfm-pipe-ragged",
    "tables",
    ["short-rows", "extra-cells", "escaped-pipes", "code-pipes"],
    ["| ", k, " | ", v, " |\n| --- | --- |\n| `a\\|b` |\n| ", target, " | c | dropped |\n\n"],
    { gfm: true }
);
shared(
    "gfm-footnote",
    "footnotes",
    ["reference-notes", "forward-resolution", "definition-bodies"],
    ["probe [^", k, "] end\n\n[^", k, "]: ", b, "\n\n"],
    { gfm: true }
);

// Independent lexical fields are retained in the alternate grammar. These
// frames do not identify whole unrestricted features with CommonMark.
pair("formula-backtick", "formulas", ["backtick-dollar"], inline("$`", p, "`$"), inline("`", p, "`"));
for (const [name, open, close] of [
    ["parenthesis", "\\\\(", "\\\\)"],
    ["bracket", "\\\\[", "\\\\]"]
])
    pair(
        `formula-${name}`,
        "formulas",
        ["backslash-brackets", "placement"],
        inline(open, p, close),
        inline("`", p, "`")
    );
pair(
    "formula-bracket-block",
    "formulas",
    ["bracket-blocks", "block-trimming"],
    ["\\\\[\n", p, "\n\\\\]\n\n"],
    ["```\n", p, "\n```\n\n"]
);
pair(
    "script-formatting",
    "superscript-and-subscript",
    ["formatted-body"],
    inline("^*", k, "*^"),
    inline("***", k, "***")
);
pair("script-empty", "superscript-and-subscript", ["empty-superscript"], inline("^^", k), inline("[](/empty)", k));
pair(
    "script-escaped-space",
    "superscript-and-subscript",
    ["escaped-space", "nbsp"],
    inline("^", k, "\\ ", v, "^"),
    inline("*", k, "&#160;", v, "*")
);
pair("span-empty", "bracketed-spans", ["empty-spans", "classes"], inline("[]{.", v, "}"), inline("[](/", v, ")"));
pair(
    "span-nested",
    "bracketed-spans",
    ["nested-content", "attribute-precedence"],
    inline("[", b, "]{.", v, "}"),
    inline("[", b, "](/", v, ")")
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
        inline("[", b, "](/", k, ' "', v, '")')
    );
pair("attribute-empty", "attributes", ["empty-values"], inline("[]{", k, '=""}'), inline("[](/", k, ' "")'));
pair(
    "attribute-order",
    "attributes",
    ["duplicate-records", "ordered-classes", "adjacent-members"],
    inline("[]{.", k, " .", v, " ", target, "=first ", target, "=second}"),
    inline("[", k, " ", v, "](/", target, ' "first second")')
);
pair(
    "cross-block-anchor",
    "cross-links",
    ["block-target", "raw-anchor"],
    inline("[[", target, "#^", a, "]]"),
    inline("[](/", target, "#^", a, ")")
);
pair(
    "cross-raw-label",
    "cross-links",
    ["opaque-label", "empty-path"],
    inline("[[#", a, "|**", k, "**]]"),
    inline("[](#", a, ' "**', k, '**")')
);
pair("inline-footnote", "footnotes", ["inline-notes", "inline-body"], inline("^[", b, "]"), inline("[", b, "](/note)"));
pair(
    "directive-attribute-only",
    "directives",
    ["attribute-only", "part-anchor"],
    inline(":", k, "{.", v, "}"),
    inline("[](/", k, ' "', v, '")')
);
pair(
    "directive-both-parts",
    "directives",
    ["label-and-attributes"],
    inline(":", k, "[", b, "]{.", v, "}"),
    inline("[", b, "](/", k, ' "', v, '")')
);
pair(
    "directive-unicode",
    "directives",
    ["unicode-names", "raw-name"],
    inline(":", f("unicode", "unicode"), "[", b, "]"),
    inline("[", b, "](/", f("unicode", "unicode"), ")")
);
pair("directive-leaf-bare", "directives", ["leaf-without-parts"], ["::", k, "\n\n"], ["[](/", k, ")\n\n"]);
pair(
    "directive-nested",
    "directives",
    ["nested-containers", "minimum-closer", "longer-closer"],
    ["::::", k, "\n:::", v, "\n", b, "\n:::\n:::::\n\n"],
    ["> ", k, "\n>\n>> ", v, "\n>>\n>> ", b, "\n\n"]
);
pair(
    "directive-unbraced",
    "directives",
    ["nameless-unbraced", "trailing-colons"],
    ["::: ", k, " :::\n", b, "\n:::\n\n"],
    ["> ", k, "\n>\n> ", b, "\n\n"]
);
pair(
    "block-id-paragraph",
    "block-identifiers",
    ["paragraph-suffix", "anchor-declaration"],
    [b, " #", k, "#\n\n"],
    [b, "\n\n[", k, "]: /target\n\n"]
);
pair(
    "heading-explicit-id",
    "anchors",
    ["explicit-anchor", "attribute-override"],
    ["# ", b, " {#", k, "}\n\n"],
    ["# ", b, "\n\n[", k, "]: /target\n\n"]
);
// Repeated labels in these products are equality constraints on source fields.
pair(
    "heading-reference",
    "anchors",
    ["implicit-references", "forward-resolution"],
    ["[", k, "]\n\n# ", k, "\n\n"],
    ["[", k, "]\n\n[", k, "]: #", k, "\n\n"]
);
pair(
    "citation-group",
    "citations",
    ["groups", "modes", "semicolons"],
    inline("[@", k, "; -@", target, "]"),
    inline("[", k, "](/normal) [", target, "](/suppressed)")
);
pair(
    "task-unicode",
    "task-lists",
    ["unicode-marker", "authored-marker"],
    ["- [✓] ", b, "\n\n"],
    ["- [x] ", b, "\n\n"],
    { gfm: true }
);
pair("task-heading", "task-lists", ["heading-body", "first-block"], ["- [!] # ", b, "\n\n"], ["- # ", b, "\n\n"]);

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
        ["1. ", b, "\n", ordinal("decimal"), ". ", t, "\n\n"]
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
        [common, " ", b, "\n", common, " ", t, "\n\n"]
    );
pair(
    "callout",
    "callouts",
    ["custom-type", "case", "three-collapse-states", "title"],
    ["> [!", k, "]", state("callout"), " ", b, "\n\n"],
    ["> [", b, "](/", k, ' "', state("title"), '")\n\n']
);
pair(
    "citation-affixes",
    "citations",
    ["prefix", "suffix", "key"],
    inline("[", p, " @", k, ", ", v, "]"),
    inline("[", p, "](/", k, ' "', v, '")')
);
for (const id of ["leading-caption", "trailing-caption"]) {
    const table = ["| ", k, " |\n| --- |\n| ", v, " |\n\n"];
    pair(
        id,
        "tables",
        ["captions", id],
        id === "leading-caption" ? ["Table: ", p, "\n", ...table] : [...table, ": ", p, "\n\n"],
        id === "leading-caption" ? [p, "\n\n", ...table] : [...table, p, "\n\n"],
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
    ]
);
pair(
    "specimen-graph",
    "specimens",
    ["labelled-definitions", "references", "binding"],
    ["As (@", k, ") shows.\n\n(@", k, ") ", b, "\n\n"],
    ["As [^", k, "] shows.\n\n[^", k, "]: ", b, "\n\n"],
    { gfm: true }
);

pair(
    "attribute-code",
    "attributes",
    ["code-site"],
    inline("`", p, "`{", k, "=", v, "}"),
    inline("[`", p, "`](/", k, ' "', v, '")')
);
pair(
    "attribute-heading",
    "attributes",
    ["heading-site", "closing-hashes"],
    ["## ", b, " ## {#", k, " .", v, "}\n\n"],
    ["## [", b, "](/", k, ' "', v, '")\n\n']
);
pair(
    "attribute-fence",
    "attributes",
    ["fence-site"],
    ["```lang {", k, "=", v, "}\n", p, "\n```\n\n"],
    ["```lang\n", p, "\n```\n\n[](/", k, ' "', v, '")\n\n']
);
for (const [name, marker] of [
    ["link", ""],
    ["image", "!"]
])
    pair(
        `attribute-${name}`,
        "attributes",
        ["occurrence-site", `${name}-site`],
        inline(marker, "[", b, "](/", target, "){", k, "=", v, "}"),
        inline(marker, "[", b, "](/", target, ") [](/", k, ' "', v, '")')
    );
pair(
    "attribute-autolink",
    "attributes",
    ["angle-autolink-site"],
    inline("<https://", target, ">{", k, "=", v, "}"),
    inline("<https://", target, "> [](/", k, ' "', v, '")')
);
pair(
    "attribute-reference",
    "attributes",
    ["definition-site", "inheritance", "occurrence-order", "anchor-override"],
    ["[", b, "][", k, "]{#", a, " .", v, "}\n\n[", k, "]: /", target, " {.base}\n\n"],
    ["[", b, "][", k, "] [](#", a, ' "', v, '")\n\n[', k, "]: /", target, "\n\n"]
);
pair(
    "attribute-escaped-value",
    "attributes",
    ["double-quote-escapes", "entities", "single-quote-literals"],
    inline("[]{", k, '="', v, '&amp;\\*"}'),
    inline("[](/", k, ' "', v, '&amp;\\*")')
);
pair(
    "attribute-newline",
    "attributes",
    ["single-line-ending", "class-splitting"],
    inline('[]{class="', k, " ", v, '"\n}'),
    inline("[", k, " ", v, "](/class)")
);
pair(
    "anchor-last-empty",
    "anchors",
    ["last-id", "empty-id-clears"],
    inline("[]{#", k, " id=", v, ' id=""}'),
    inline("[](/", k, ' "', v, '")')
);
shared(
    "common-anchor-generation",
    "anchors",
    ["unicode-lowercase", "collisions", "empty-base"],
    ["# É字", k, "\n\n# É字", k, "\n\n# !!!\n\n"]
);
pair(
    "block-id-list",
    "block-identifiers",
    ["list-item-suffix", "standalone-list-id"],
    ["- ", b, " #", k, "#\n\n#", v, "#\n\n"],
    ["- ", b, "\n\n[", k, "]: /item\n[", v, "]: /list\n\n"]
);
pair(
    "footnote-nested-inline",
    "footnotes",
    ["nested-inline-notes", "generated-identities"],
    inline("^[", k, " ^[", v, "] ", target, "]"),
    inline("*", k, " **", v, "** ", target, "*")
);
shared(
    "gfm-footnote-cycle",
    "footnotes",
    ["self-reference", "semantic-cycles"],
    ["probe [^", k, "]\n\n[^", k, "]: ", b, " [^", k, "]\n\n"],
    { gfm: true }
);
shared(
    "footnote-retention",
    "footnotes",
    ["unused-definitions", "duplicate-definitions", "repeated-calls"],
    ["probe [^", k, "] [^", k, "] end\n\n[^", k, "]: ", b, "\n\n[^", k, "]: ", t, "\n\n[^", target, "]: ", p, "\n\n"],
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
    inline("<!-->", k)
);
pair(
    "definition-blocks",
    "definition-lists",
    ["multiple-blocks", "nested-content"],
    [k, "\n\n: ", b, "\n\n    ", t, "\n\n"],
    ["- ", k, "\n\n  ", b, "\n\n  ", t, "\n\n"]
);
pair(
    "composition-directive-callout",
    "conflicts",
    ["block-containers", "inline-composition"],
    ["> :::", k, "\n> ", b, "\n> :::\n\n"],
    ["> > ", k, "\n> >\n> > ", b, "\n\n"]
);
pair(
    "composition-literal-islands",
    "conflicts",
    ["literal-ownership", "inline-precedence"],
    inline("++", k, " `++", v, "++` tail++"),
    inline("**", k, " `++", v, "++` tail**")
);

// Negative productions have infinite variable fields too. The owning rule's
// rejected prefix is fixed; the same source language measures fallback work.
// Each names the construct it rejects. When no reference implements it, the
// certificate declares a control: the same production with the trigger bytes
// replaced by letters of the same width, so Core's rejection work is measured
// against Core rather than against a parser with nothing to reject.
for (const [id, feature, rejects, parts, options] of [
    ["insertion", "insertion", "insertion", inline("++", b), { control: inline("qq", b) }],
    ["mark", "marks", "mark", inline("==", b), { control: inline("qq", b) }],
    ["strike", "strikethrough", "strikethrough", inline("~~", b), { gfm: true }],
    ["formula", "formulas", "inline-formula", inline("$", b), { control: inline("q", b) }],
    ["comment", "comments", "percent-comment", inline("%%", b), { control: inline("qq", b) }],
    ["cross-link", "cross-links", "cross-link", inline("[[", k), { control: inline("[q", k) }],
    ["directive", "directives", "inline-directive", inline(":", k), { control: inline("q", k) }],
    [
        "attributes",
        "attributes",
        "attribute-block",
        inline("[", b, "]{=broken}"),
        { control: inline("[", b, "]q=broken}") }
    ],
    [
        "script",
        "superscript-and-subscript",
        "superscript",
        inline("^", k, " ", v, "^"),
        { control: inline("q", k, " ", v, "q") }
    ],
    ["list-limit", "lists", "ordered-list-marker", ["1234567890. ", b, "\n\n"], {}],
    ["footnote", "footnotes", "footnote-reference", inline("[^", k, "]"), { gfm: true }],
    ["grid", "tables", "grid-table", ["+---+\n|", k, "\n+---+\n\n"], { control: ["q---q\n|", k, "\nq---q\n\n"] }],
    ["citation", "citations", "citation", inline("@-", k), { control: inline("q-", k) }],
    ["span", "bracketed-spans", "bracketed-span", inline("[", b, "]{"), { control: inline("[", b, "]q") }]
])
    shared(`fallback-${id}`, feature, ["fallback", "rejected-prefix"], parts, { rejects, ...options });
shared(
    "fallback-image-dimensions",
    "links-and-images",
    ["dimension-fallback", "leading-zero"],
    inline("![", k, "|01x20](/", target, ")"),
    { rejects: "image-dimensions", control: inline("![", k, "q01x20](/", target, ")") }
);
shared(
    "fallback-noninitial-metadata",
    "properties",
    ["document-initial-only", "envelope-fallback"],
    ["probe\n\n---\n", k, ": ", v, "\n---\n\n"],
    {
        rejects: "metadata-envelope",
        uncontrolled:
            "The rejected envelope delimiter --- is itself shared syntax, a thematic break or setext underline in both parsers; no same-width letter substitution removes the envelope attempt without also removing that shared work."
    }
);
pair(
    "attribute-bare",
    "attributes",
    ["bare-names", "boolean-record", "unnumbered"],
    inline("[]{", k, " -}"),
    inline("[](/", k, ' "true unnumbered")')
);

pair(
    "specimen-reset",
    "specimens",
    ["explicit-reset", "positive-nine-digit-start", "binding"],
    ["As (@", k, ") shows.\n\n(", f("reset", "positive9"), "@", k, ") ", b, "\n\n"],
    ["As [", k, "] shows.\n\n[", k, "]: /", k, "\n\n", f("reset", "positive9"), ". ", b, "\n\n"]
);
pair(
    "specimen-groups",
    "specimens",
    ["anonymous-definitions", "duplicate-definitions", "group-reset-suppression"],
    ["(5@) ", b, "\n\n(7@", k, ") ", t, "\n\n(@", k, ") ", b, "\n\nAs (@", k, ") shows.\n\n"],
    ["5. ", b, "\n\n7. ", t, "\n\n1. ", b, "\n\nAs [", k, "] shows.\n\n[", k, "]: /", k, "\n\n"]
);

// Source forms identified by the section-level coverage review. These reuse
// the same identity/product proofs; no native output expectations are added.
pair(
    "block-id-container-list",
    "block-identifiers",
    ["standalone-id", "list-container"],
    ["- ", b, "\n\n#", k, "#\n\n"],
    ["- ", b, "\n\n[", k, "]: /block\n\n"]
);
pair(
    "block-id-container-table",
    "block-identifiers",
    ["standalone-id", "table-container"],
    ["| ", v, " |\n| --- |\n| ", target, " |\n\n#", k, "#\n\n"],
    ["| ", v, " |\n| --- |\n| ", target, " |\n\n[", k, "]: /block\n\n"],
    { gfm: true }
);
shared("block-id-escaped", "block-identifiers", ["escaped-id", "fallback"], inline("\\#", k, "#"));
for (const [name, prefix] of [
    ["link", ""],
    ["embed", "!"]
])
    pair(
        `cross-${name}-table`,
        "cross-links",
        ["pipe-table", "escaped-label-separator"],
        ["| ", prefix, "[[", target, "\\|", p, "]] |\n| --- |\n| ", b, " |\n\n"],
        ["| ", prefix, "[", p, "](/", target, ") |\n| --- |\n| ", b, " |\n\n"],
        { gfm: true }
    );
shared(
    "fallback-task-separator",
    "task-lists",
    ["required-separator", "invalid-marker"],
    ["- [x]\n- [x]", k, "\n- [] ", v, "\n- [ab] ", target, "\n\n"],
    { gfm: true, rejects: "task-list-marker" }
);
shared("task-nested", "task-lists", ["nested-tasks", "ordered-task"], ["1. [ ] ", b, "\n   - [x] ", t, "\n\n"], {
    gfm: true
});

pair("mark-formatting", "marks", ["formatted-content"], inline("==", b, "=="), inline("**", b, "**"));
shared(
    "gfm-footnote-blocks",
    "footnotes",
    ["definition-blocks", "continuation-indentation"],
    ["probe [^", k, "] end\n\n[^", k, "]: ", b, "\n\n    ", t, "\n\n"],
    { gfm: true }
);

export const featureGrammars = new Map(entries.map((entry) => [entry.id, Object.freeze(entry)]));
if (featureGrammars.size !== entries.length) throw new Error("duplicate feature grammar");

/** Grammar slots, not a list of accepted sample strings. Each independent
 * name receives a separately generated value of its declared lexical type. */
export function featureValues(parameters) {
    return {
        unicode: `é字${parameters.value}`,
        empty: "",
        ordinal: parameters.start,
        state: parameters.mode,
        reset: parameters.reset ?? String(parameters.start)
    };
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
