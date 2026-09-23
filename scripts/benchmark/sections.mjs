/** Reviewed links from specification sections to source-grammar certificates.
 * Titles are literal keys: a new/renamed section requires an explicit decision.
 * Context-only sections state why they introduce no new source grammar. Output
 * correctness remains outside this registry and the benchmark pipeline.
 */
const grammar = (...certificates) => ({ kind: "grammar", certificates });
const context = (reason) => ({ kind: "context", reason });
export const sectionDispositions = {
    anchors: {
        Anchors: grammar("heading-explicit-id", "anchor-last-empty", "common-anchor-generation"),
        "Explicit anchors": grammar("heading-explicit-id", "anchor-last-empty"),
        "Automatic anchors": context(
            "Automatic IDs are derived output, with no additional authored grammar. The heading source workload is common-anchor-generation; correctness of normalization/collision allocation belongs to regression and parity."
        ),
        "Implicit heading references": grammar("heading-reference"),
        "Anchor ownership": context(
            "Anchor ownership, inheritance and application matching are output/resolution contracts. Authored forms are covered by explicit/implicit anchor sections and attribute-reference."
        )
    },
    attributes: {
        Attributes: grammar("record-span", "class-span", "attribute-bare"),
        "Write attribute members": grammar("record-span", "class-span", "attribute-bare", "attribute-order"),
        "Values and spacing": grammar(
            "attribute-double",
            "attribute-single",
            "attribute-unquoted",
            "attribute-empty",
            "attribute-newline",
            "attribute-escaped-value"
        ),
        "Attachment sites": grammar(
            "attribute-code",
            "attribute-heading",
            "attribute-fence",
            "attribute-link",
            "attribute-image",
            "attribute-autolink",
            "attribute-reference"
        ),
        "Inline code": grammar("attribute-code"),
        Headings: grammar("attribute-heading"),
        "Fenced code": grammar("attribute-fence"),
        "Links, images, and inherited attributes": grammar(
            "attribute-link",
            "attribute-image",
            "attribute-autolink",
            "attribute-reference"
        ),
        "Invalid containers": grammar("fallback-attributes")
    },
    base: {
        Basics: grammar("common-paragraph", "common-eol-lf", "common-eol-cr", "common-eol-crlf"),
        Indentation: grammar("common-tabs", "common-indented-code", "common-list-loose"),
        "Escape punctuation": grammar("common-escapes"),
        "Character references": grammar("common-entities"),
        "Literal punctuation": grammar("common-paragraph"),
        "Learn the block and inline forms": context(
            "Navigation to the dedicated syntax pages; this section introduces no source production."
        )
    },
    "block-identifiers": {
        "Block identifiers": grammar("block-id-paragraph"),
        "Paragraphs and list items": grammar("block-id-paragraph", "block-id-list"),
        "Lists, quotes, and tables": grammar("anchor", "block-id-container-list", "block-id-container-table"),
        Boundaries: grammar("block-id-escaped", "attribute-heading")
    },
    "bracketed-spans": {
        "Bracketed spans": grammar("record-span", "class-span"),
        "Empty spans and nested content": grammar("span-empty", "span-nested"),
        "Links and spans": grammar("record-span", "common-link-direct", "common-reference-shortcut", "fallback-span")
    },
    callouts: {
        "Quotes and callouts": grammar("common-quote", "callout"),
        "Add a type and title": grammar("callout"),
        "Fold state": grammar("callout"),
        "Metadata boundaries": grammar("callout")
    },
    citations: {
        Citations: grammar("cite-normal", "citation-group"),
        "Author and suppression modes": grammar("cite-author", "cite-suppress"),
        "Citation keys": grammar("cite-normal", "fallback-citation"),
        "Affixes and tails": grammar("citation-affixes", "citation-group"),
        "Boundaries and fallback": grammar("fallback-citation")
    },
    code: {
        Code: grammar("common-code-span", "common-code-newline"),
        "Include a backtick": grammar("common-code-span"),
        "Fenced code": grammar("common-code-fence-backtick", "common-code-fence-tilde"),
        "Indented code": grammar("common-indented-code")
    },
    comments: {
        Comments: grammar("opaque-comment", "comment-empty"),
        "Block comments": grammar("leaf-comment"),
        "HTML comments": grammar("common-html-comment", "common-inline-comment"),
        Boundaries: grammar("fallback-comment", "comment-empty")
    },
    conflicts: {
        "Compatibility and precedence": context(
            "Scope and navigation for the compatibility guide; the following sections enumerate the source-language interactions."
        ),
        "Differences to know": grammar(
            "run-sub",
            "run-strike",
            "script-empty",
            "callout",
            "block-id-paragraph",
            "cross-raw-label",
            "metadata-types",
            "attribute-bare",
            "named-container",
            "formula-parenthesis",
            "task-unicode",
            "common-html-comment",
            "common-inline-html",
            "decimal-list",
            "footnote-retention",
            "fallback-grid",
            "common-paragraph"
        ),
        "Block recognition": grammar(
            "composition-directive-callout",
            "common-setext-two",
            "metadata-types",
            "leaf-promotion",
            "leading-caption"
        ),
        "Inline recognition": grammar(
            "composition-literal-islands",
            "record-span",
            "common-link-direct",
            "common-escapes",
            "formula-parenthesis"
        ),
        "Literal content and incomplete syntax": grammar(
            "composition-literal-islands",
            "fallback-insertion",
            "fallback-cross-link",
            "fallback-directive"
        )
    },
    "cross-links": {
        "Cross links and embeds": grammar("cross-link", "cross-link-absent", "cross-link-empty", "cross-raw-label"),
        "Headings and blocks": grammar("cross-anchor", "cross-local", "cross-block-anchor"),
        Embeds: grammar("cross-embed", "cross-embed-absent", "cross-embed-empty"),
        Dimensions: grammar("embed-dimensions"),
        "Pipes and tables": grammar("cross-link-table", "cross-embed-table"),
        "Incomplete targets": grammar("fallback-cross-link")
    },
    "definition-lists": {
        "Definition lists": grammar("loose-definition", "mixed-definitions"),
        "Multiple definitions and terms": grammar("mixed-definitions"),
        "Block content": grammar("definition-blocks"),
        "Recognition boundaries": grammar("loose-definition", "mixed-definitions")
    },
    directives: {
        Directives: grammar("inline-directive", "leaf-directive", "named-container", "anonymous-container"),
        "Inline directives": grammar(
            "inline-directive",
            "empty-directive",
            "directive-attribute-only",
            "directive-both-parts",
            "directive-unicode"
        ),
        "Leaf directives": grammar("leaf-directive", "directive-leaf-bare"),
        "Container directives": grammar("named-container", "directive-nested"),
        "Nameless containers": grammar("anonymous-container", "directive-unbraced"),
        "Container boundaries": grammar("directive-nested", "fallback-directive")
    },
    emphasis: {
        Emphasis: grammar("common-emphasis-asterisk", "common-emphasis-underscore"),
        "Nest formatting": grammar("common-emphasis-asterisk", "common-emphasis-underscore"),
        "Spaces and word boundaries": grammar("common-emphasis-asterisk", "common-emphasis-underscore")
    },
    footnotes: {
        Footnotes: grammar("gfm-footnote"),
        "Definition bodies": grammar("gfm-footnote", "gfm-footnote-blocks"),
        "Undefined calls": grammar("fallback-footnote"),
        "Inline footnotes": grammar("inline-footnote", "footnote-nested-inline"),
        "Walking and resolution": grammar("footnote-retention", "gfm-footnote-cycle")
    },
    formulas: {
        Formulas: grammar("opaque-formula", "opaque-display"),
        "Inline forms": grammar("opaque-formula", "formula-backtick", "formula-parenthesis", "formula-bracket"),
        "Dollar boundaries": grammar("opaque-formula", "opaque-display", "fallback-formula"),
        "Formula blocks": grammar("leaf-formula", "leaf-promotion", "formula-bracket-block"),
        "Code fences": grammar("leaf-fence")
    },
    headings: {
        Headings: grammar(
            "common-atx-1",
            "common-atx-2",
            "common-atx-3",
            "common-atx-4",
            "common-atx-5",
            "common-atx-6"
        ),
        "Underlined headings": grammar("common-setext-one", "common-setext-two"),
        "Format a heading": grammar("common-atx-1", "attribute-heading")
    },
    html: {
        HTML: grammar("common-inline-html"),
        "HTML blocks": grammar(
            "common-html-raw",
            "common-html-instruction",
            "common-html-declaration",
            "common-html-cdata",
            "common-html-block-tag",
            "common-html-complete-tag"
        ),
        Comments: grammar("common-html-comment", "common-inline-comment")
    },
    insertion: {
        Insertions: grammar("insertion-strong", "run-insertion"),
        "Nest and combine": grammar("insertion-strong", "composition-literal-islands"),
        "Matching rules": grammar("run-insertion", "fallback-insertion")
    },
    "line-breaks": {
        "Line breaks": grammar("common-softbreak"),
        "Hard breaks": grammar("common-hardbreak-backslash", "common-hardbreak-spaces"),
        "Source positions": {
            ...grammar("common-eol-lf", "common-eol-cr", "common-eol-crlf"),
            note: "Line-ending spellings are source grammar; native coordinate attribution is an output contract owned by parity/regression."
        }
    },
    "links-and-images": {
        "Links and images": grammar("common-link-direct", "common-image"),
        "Reference links": grammar(
            "common-reference-full",
            "common-reference-collapsed",
            "common-reference-shortcut",
            "common-reference-duplicate",
            "common-unresolved-reference"
        ),
        "Automatic links": grammar("common-angle-autolink", "gfm-bare-autolink"),
        Images: grammar("common-image"),
        "Image dimensions": grammar("image-dimensions", "embed-dimensions", "fallback-image-dimensions"),
        Attributes: grammar("attribute-link", "attribute-image", "attribute-reference"),
        "Bracket precedence": grammar(
            "record-span",
            "common-link-direct",
            "common-reference-shortcut",
            "common-unresolved-reference"
        )
    },
    lists: {
        Lists: grammar("common-bullet-dash", "common-bullet-plus", "common-bullet-star"),
        "Numbered lists": grammar("decimal-list", "common-list-parenthesis", "fallback-list-limit"),
        "Alphabetic and Roman markers": grammar("alpha-list", "upper-list", "roman-list", "upper-roman-list"),
        "Automatic markers": grammar("default-list", "enclosed-default-list"),
        "Blank lines and starts": grammar("common-list-loose", "decimal-list")
    },
    marks: {
        Highlights: grammar("run-mark"),
        "Format highlighted text": grammar("mark-formatting"),
        "When signs stay literal": grammar("fallback-mark")
    },
    properties: {
        Properties: grammar("metadata", "metadataempty"),
        "Supported fields": grammar("metadata-types"),
        Values: grammar("metadata-types"),
        "Literal prose": grammar("metadata-literal"),
        "Envelope and recovery": grammar("metadataempty", "metadata-types", "fallback-noninitial-metadata")
    },
    specimens: {
        "Specimens (numbered examples)": grammar("specimen-graph"),
        "Anonymous definitions and resets": grammar("specimen-reset", "specimen-groups"),
        "Bodies and labels": grammar("specimen-graph", "specimen-groups"),
        "Reference forms": grammar("specimen-graph")
    },
    strikethrough: {
        Strikethrough: grammar("gfm-strike", "run-strike"),
        "Combine formatting": grammar("gfm-strike"),
        "Literal tildes": grammar("fallback-strike", "run-sub")
    },
    "superscript-and-subscript": {
        "Superscript and subscript": grammar("run-super", "run-sub", "script-empty"),
        "Include formatting or spaces": grammar("script-formatting", "script-escaped-space"),
        "Delimiter boundaries": grammar("fallback-script", "run-super", "run-sub")
    },
    tables: {
        Tables: grammar("gfm-pipe-table", "simple-matrix", "multiline-matrix", "grid-cell"),
        "Pipe tables": grammar("gfm-pipe-table", "gfm-pipe-ragged"),
        "Pipes inside cells": grammar("gfm-pipe-ragged", "cross-link-table", "cross-embed-table"),
        Captions: grammar("leading-caption", "trailing-caption", "sparse-grid"),
        "Simple tables": grammar("simple-matrix", "headless-matrix"),
        "Multiline tables": grammar("multiline-matrix", "headless-multiline"),
        "Grid tables": grammar("grid-cell", "sparse-grid"),
        "Merged cells": grammar("sparse-grid"),
        "Foot rows": grammar("sparse-grid"),
        "Boundaries and row groups": grammar("sparse-grid", "fallback-grid"),
        "Column widths and source positions": {
            ...grammar("grid-cell", "multiline-matrix", "headless-multiline", "block-id-container-table"),
            note: "Authored geometry has explicit local boundary proofs; numeric width output and original-coordinate contracts remain parity/regression responsibilities."
        },
        "Competing block syntax": grammar(
            "common-setext-one",
            "common-setext-two",
            "simple-matrix",
            "multiline-matrix",
            "grid-cell",
            "fallback-grid"
        )
    },
    "task-lists": {
        "Task lists": grammar("gfm-task-open", "gfm-task-lower", "gfm-task-upper"),
        "Custom markers and nesting": grammar("task-value", "task-unicode", "task-nested"),
        "Required separator": grammar("fallback-task-separator", "task-heading")
    },
    "thematic-breaks": {
        "Thematic breaks": grammar("common-thematic-dash", "common-thematic-star", "common-thematic-underscore")
    }
};
