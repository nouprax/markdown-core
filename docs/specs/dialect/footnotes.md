# Footnotes

Status: normative module of the [Markdown Core dialect](../dialect.md).
Sources: cmark-gfm's footnote
extension for the referenced form; Obsidian and Pandoc for the inline form.
Executable oracles: cmark-gfm and remark for the referenced form; the inline
form is product fixtures. Landing: the citation model landed with `M4`, and
the inline form lands with `O4`; until then only the referenced form is
recognized. The [example format](../dialect.md#examples) is defined by the
index.

## The citation model

```text
BibMode = normal | authorInText | suppressAuthor

Cite(citations: [Citation])

Citation(referent: CitationReferent, prefix: [Markup], suffix: [Markup],
         scope: Scope)

CitationReferent =
  bib(key: String, mode: BibMode)
  | footnote(id: String)

Footnote(id: String, content: [Markup], scope: Scope)

Document(content: [Markup], metadata: Metadata?, footnotes: [Footnote])
```

`Cite` is an inline `Markup` kind whose non-empty `citations` array keeps
source order. `Citation` and `Footnote` are scoped values, not `Markup`: they
are written, so they carry a scope, and the walking visitor gives each a value
callback and descends into their markup arrays in declared field order.
`Citation.prefix` and `suffix` are non-null inline-content arrays, empty when
absent. Every item has exactly one referent; one `Cite` cannot mix referent
families; a `bib` referent is produced only by the [citations](citations.md)
module. `CitationReferent` has no scope; the owning `Citation.scope` covers
the occurrence. A Markdown reference link is parser indirection and is never a
citation; the model has no `link` branch and `Document` has no link registry.

`Cite` is the cluster on which a consumer sorts, punctuates, and renders. A
`footnote` referent is an edge to the `Footnote` in `Document.footnotes`
whose `id` is equal; display numbering is derived from cite order by the
consumer and is never the id. `Document.footnotes` is a named owned field,
visited after `Document.content`, ordered by `Footnote.scope.start`, and
holds referenced definitions and inline values in one sequence.
`Footnote.content` is inline-or-block content: a referenced definition holds
its parsed block content, and an inline footnote holds its parsed inline body
directly, with no synthesized `Paragraph`. In the dump, each `Citation` is
nested under its `Cite` with its affixes as `CitationPrefix` and
`CitationSuffix` groups, and each `Footnote` is nested under `Document` after
the content lines:

```````````````````````````````` example
Text[^1] and more[^note].

[^1]: The first note.
[^note]: The second note.
.
Document scope=1:1..4:25 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:25 anchor=null attributes={} children=5
│   ├── Text scope=1:1..1:4 anchor=null attributes={} literal="Text" children=0
│   ├── Cite scope=1:5..1:8 anchor=null attributes={} children=1
│   │   └── Citation scope=1:6..1:7 referent=footnote(id="1") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   ├── Text scope=1:9..1:17 anchor=null attributes={} literal=" and more" children=0
│   ├── Cite scope=1:18..1:24 anchor=null attributes={} children=1
│   │   └── Citation scope=1:19..1:23 referent=footnote(id="note") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   └── Text scope=1:25..1:25 anchor=null attributes={} literal="." children=0
├── Footnote scope=3:1..3:21 id="1" children=1
│   └── Paragraph scope=3:7..3:21 anchor=null attributes={} children=1
│       └── Text scope=3:7..3:21 anchor=null attributes={} literal="The first note." children=0
└── Footnote scope=4:1..4:25 id="note" children=1
    └── Paragraph scope=4:10..4:25 anchor=null attributes={} children=1
        └── Text scope=4:10..4:25 anchor=null attributes={} literal="The second note." children=0
````````````````````````````````

## Referenced footnotes

A footnote definition is step 12 of the block-start order:

```text
definition = *3SP "[^" label "]:" [ inline-content ] EOL continuation*
label      = 1*( any scalar except "]", "[", SP, and TAB )
```

A definition is recognized only when the label is at most 1000 bytes and
only at footnote container depth below 100. Its
key is the label under the inherited reference-label normalization; the
stored `Footnote.id` is that key and never contains the caret. A footnote call
is the second alternative of the bracket procedure, tested after a direct
tail: an unescaped `[^label]` whose label is defined produces a one-item
`Cite` whose `Citation` has referent `footnote(id)` and empty affixes.
Repeated calls share one `Footnote`; the body is never duplicated:

```````````````````````````````` example
[^a] [^a]

[^a]: once
.
Document scope=1:1..3:10 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:9 anchor=null attributes={} children=3
│   ├── Cite scope=1:1..1:4 anchor=null attributes={} children=1
│   │   └── Citation scope=1:2..1:3 referent=footnote(id="a") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   ├── Text scope=1:5..1:5 anchor=null attributes={} literal=" " children=0
│   └── Cite scope=1:6..1:9 anchor=null attributes={} children=1
│       └── Citation scope=1:7..1:8 referent=footnote(id="a") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
└── Footnote scope=3:1..3:10 id="a" children=1
    └── Paragraph scope=3:7..3:10 anchor=null attributes={} children=1
        └── Text scope=3:7..3:10 anchor=null attributes={} literal="once" children=0
````````````````````````````````

Continuation lines are indented at least four columns, and the content is
block content parsed by the ordinary block parser:

```````````````````````````````` example
[^a]

[^a]: first paragraph
    continued

    second paragraph
.
Document scope=1:1..6:20 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Cite scope=1:1..1:4 anchor=null attributes={} children=1
│       └── Citation scope=1:2..1:3 referent=footnote(id="a") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
└── Footnote scope=3:1..6:20 id="a" children=2
    ├── Paragraph scope=3:7..4:13 anchor=null attributes={} children=3
    │   ├── Text scope=3:7..3:21 anchor=null attributes={} literal="first paragraph" children=0
    │   ├── SoftBreak scope=3:22..3:22 anchor=null attributes={} children=0
    │   └── Text scope=4:5..4:13 anchor=null attributes={} literal="continued" children=0
    └── Paragraph scope=6:5..6:20 anchor=null attributes={} children=1
        └── Text scope=6:5..6:20 anchor=null attributes={} literal="second paragraph" children=0
````````````````````````````````

A valid definition that no call references is still a `Footnote`:

```````````````````````````````` example
[^a]: kept

text
.
Document scope=1:1..3:4 anchor=null attributes={} children=1
├── Paragraph scope=3:1..3:4 anchor=null attributes={} children=1
│   └── Text scope=3:1..3:4 anchor=null attributes={} literal="text" children=0
└── Footnote scope=1:1..2:0 id="a" children=1
    └── Paragraph scope=1:7..1:10 anchor=null attributes={} children=1
        └── Text scope=1:7..1:10 anchor=null attributes={} literal="kept" children=0
````````````````````````````````

When two definitions share a key, the first in source order wins: every call
resolves to it. Each later one is still parsed as a definition, as cmark-gfm's
block grammar parses it, and remains a `Footnote` after the winner in
`Document.footnotes` with the same id, so nothing authored is lost; a consumer
keying footnotes by id takes the first. cmark-gfm destroys the later
definition instead, which is a registered delta of that oracle:

```````````````````````````````` example
[^a]

[^a]: first

[^a]: second
.
Document scope=1:1..5:12 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Cite scope=1:1..1:4 anchor=null attributes={} children=1
│       └── Citation scope=1:2..1:3 referent=footnote(id="a") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
├── Footnote scope=3:1..4:0 id="a" children=1
│   └── Paragraph scope=3:7..3:11 anchor=null attributes={} children=1
│       └── Text scope=3:7..3:11 anchor=null attributes={} literal="first" children=0
└── Footnote scope=5:1..5:12 id="a" children=1
    └── Paragraph scope=5:7..5:12 anchor=null attributes={} children=1
        └── Text scope=5:7..5:12 anchor=null attributes={} literal="second" children=0
````````````````````````````````

A call whose label no definition defines is not a call: the brackets are
inherited bracket text and never create a `Footnote`, allocate an id, or
affect numbering. Only a literal source caret opens a call: `[\^a]`,
`[&#94;a]`, and `[&Hat;a]` are text:

```````````````````````````````` example
[^x] and [\^a] and [&#94;a]

[^a]: note
.
Document scope=1:1..3:10 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:27 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:27 anchor=null attributes={} literal="[^x] and [^a] and [^a]" children=0
└── Footnote scope=3:1..3:10 id="a" children=1
    └── Paragraph scope=3:7..3:10 anchor=null attributes={} children=1
        └── Text scope=3:7..3:10 anchor=null attributes={} literal="note" children=0
````````````````````````````````

The key is case-folded, so a call and a definition that differ in case share
one footnote, whose id is the normalized form:

```````````````````````````````` example
[^Note]

[^note]: x
.
Document scope=1:1..3:10 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:7 anchor=null attributes={} children=1
│   └── Cite scope=1:1..1:7 anchor=null attributes={} children=1
│       └── Citation scope=1:2..1:6 referent=footnote(id="note") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
└── Footnote scope=3:1..3:10 id="note" children=1
    └── Paragraph scope=3:10..3:10 anchor=null attributes={} children=1
        └── Text scope=3:10..3:10 anchor=null attributes={} literal="x" children=0
````````````````````````````````

A valid direct tail `(...)` is tested before the call, so `[^a](u)` is an
inherited link whose text is `^a`; every other tail and every container after
a defined call is text, because the call is complete at its `]`:

```````````````````````````````` example
[^a](u) [^a]{.x}

[^a]: note
.
Document scope=1:1..3:10 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:16 anchor=null attributes={} children=4
│   ├── Link scope=1:1..1:7 anchor=null attributes={} dest=url("u") title=null children=1
│   │   └── Text scope=1:2..1:3 anchor=null attributes={} literal="^a" children=0
│   ├── Text scope=1:8..1:8 anchor=null attributes={} literal=" " children=0
│   ├── Cite scope=1:9..1:12 anchor=null attributes={} children=1
│   │   └── Citation scope=1:10..1:11 referent=footnote(id="a") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   └── Text scope=1:13..1:16 anchor=null attributes={} literal="{.x}" children=0
└── Footnote scope=3:1..3:10 id="a" children=1
    └── Paragraph scope=3:7..3:10 anchor=null attributes={} children=1
        └── Text scope=3:7..3:10 anchor=null attributes={} literal="note" children=0
````````````````````````````````

A call inside a footnote's own content is an id edge, not an object cycle; a
consumer that renders bodies recursively detects semantic cycles itself. A
`[^label]:` line is never a link reference definition.

```````````````````````````````` example
[^a]

[^a]: see [^b]
[^b]: end
.
Document scope=1:1..4:9 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Cite scope=1:1..1:4 anchor=null attributes={} children=1
│       └── Citation scope=1:2..1:3 referent=footnote(id="a") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
├── Footnote scope=3:1..3:14 id="a" children=1
│   └── Paragraph scope=3:7..3:14 anchor=null attributes={} children=2
│       ├── Text scope=3:7..3:10 anchor=null attributes={} literal="see " children=0
│       └── Cite scope=3:11..3:14 anchor=null attributes={} children=1
│           └── Citation scope=3:12..3:13 referent=footnote(id="b") children=0
│               ├── CitationPrefix children=0
│               └── CitationSuffix children=0
└── Footnote scope=4:1..4:9 id="b" children=1
    └── Paragraph scope=4:7..4:9 anchor=null attributes={} children=1
        └── Text scope=4:7..4:9 anchor=null attributes={} literal="end" children=0
````````````````````````````````

## Inline footnotes

An unescaped `^` immediately followed by `[` pushes an inline-footnote opener onto the shared
bracket stack at inline step A7. Every recognized inline footnote creates one
`Footnote` whose content is the parsed inline body and one one-item `Cite`
with referent `footnote(id)` and empty affixes:

```````````````````````````````` example
text^[an inline note]
.
Document scope=1:1..1:21 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:21 anchor=null attributes={} children=2
│   ├── Text scope=1:1..1:4 anchor=null attributes={} literal="text" children=0
│   └── Cite scope=1:5..1:21 anchor=null attributes={} children=1
│       └── Citation scope=1:7..1:20 referent=footnote(id="inline-1") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
└── Footnote scope=1:5..1:21 id="inline-1" children=1
    └── Text scope=1:7..1:20 anchor=null attributes={} literal="an inline note" children=0
````````````````````````````````

```````````````````````````````` example
^[a *b*]
.
Document scope=1:1..1:8 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:8 anchor=null attributes={} children=1
│   └── Cite scope=1:1..1:8 anchor=null attributes={} children=1
│       └── Citation scope=1:3..1:7 referent=footnote(id="inline-1") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
└── Footnote scope=1:1..1:8 id="inline-1" children=2
    ├── Text scope=1:3..1:4 anchor=null attributes={} literal="a " children=0
    └── Emphasis scope=1:5..1:7 anchor=null attributes={} children=1
        └── Text scope=1:6..1:6 anchor=null attributes={} literal="b" children=0
````````````````````````````````

The `]` that matches the opener closes the footnote without attempting any
link, reference, span, cite, or attribute tail, so `^[a](b)` is a `Cite`
followed by text `(b)`. At one `^`, the inline footnote wins over a `[^label]`
call and over superscript, so `^[^1]` is a footnote whose body is text `^1`:

```````````````````````````````` example
^[a](b) ^[^1]
.
Document scope=1:1..1:13 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:13 anchor=null attributes={} children=3
│   ├── Cite scope=1:1..1:4 anchor=null attributes={} children=1
│   │   └── Citation scope=1:3..1:3 referent=footnote(id="inline-1") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   ├── Text scope=1:5..1:8 anchor=null attributes={} literal="(b) " children=0
│   └── Cite scope=1:9..1:13 anchor=null attributes={} children=1
│       └── Citation scope=1:11..1:12 referent=footnote(id="inline-2") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
├── Footnote scope=1:1..1:4 id="inline-1" children=1
│   └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
└── Footnote scope=1:9..1:13 id="inline-2" children=1
    └── Text scope=1:11..1:12 anchor=null attributes={} literal="^1" children=0
````````````````````````````````

Inside the body, a link opener is closed by its own `]` and tail, and a
footnote opener inside a link label is closed by the first `]`. Ids are
assigned once, during document finalization, after every authored id is
known. Let `A` be the ids of every `Footnote` produced from a winning or
unreferenced definition. Inline footnotes are numbered in ascending order of
the start position of their `^[`, an outer footnote before one nested in its
body; the `N`-th receives `inline-N` when that string is not in `A` and not
already assigned, otherwise `inline-N-K` for the smallest `K` of at least 1
in neither set. The value carries no authored meaning; consumers compare and
copy ids and never display them:

```````````````````````````````` example
^[a ^[b] c]
.
Document scope=1:1..1:11 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:11 anchor=null attributes={} children=1
│   └── Cite scope=1:1..1:11 anchor=null attributes={} children=1
│       └── Citation scope=1:3..1:10 referent=footnote(id="inline-1") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
├── Footnote scope=1:1..1:11 id="inline-1" children=3
│   ├── Text scope=1:3..1:4 anchor=null attributes={} literal="a " children=0
│   ├── Cite scope=1:5..1:8 anchor=null attributes={} children=1
│   │   └── Citation scope=1:7..1:7 referent=footnote(id="inline-2") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   └── Text scope=1:9..1:10 anchor=null attributes={} literal=" c" children=0
└── Footnote scope=1:5..1:8 id="inline-2" children=1
    └── Text scope=1:7..1:7 anchor=null attributes={} literal="b" children=0
````````````````````````````````

```````````````````````````````` example
[^inline-1] ^[b]

[^inline-1]: authored
.
Document scope=1:1..3:21 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:16 anchor=null attributes={} children=3
│   ├── Cite scope=1:1..1:11 anchor=null attributes={} children=1
│   │   └── Citation scope=1:2..1:10 referent=footnote(id="inline-1") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   ├── Text scope=1:12..1:12 anchor=null attributes={} literal=" " children=0
│   └── Cite scope=1:13..1:16 anchor=null attributes={} children=1
│       └── Citation scope=1:15..1:15 referent=footnote(id="inline-1-1") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
├── Footnote scope=1:13..1:16 id="inline-1-1" children=1
│   └── Text scope=1:15..1:15 anchor=null attributes={} literal="b" children=0
└── Footnote scope=3:1..3:21 id="inline-1" children=1
    └── Paragraph scope=3:14..3:21 anchor=null attributes={} children=1
        └── Text scope=3:14..3:21 anchor=null attributes={} literal="authored" children=0
````````````````````````````````

A body that is empty or consists only of spaces and tabs is invalid; the
opener is text. `\^[` never opens:

```````````````````````````````` example
^[] ^[ ] \^[a]
.
Document scope=1:1..1:14 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:14 anchor=null attributes={} children=1
    └── Text scope=1:1..1:14 anchor=null attributes={} literal="^[] ^[ ] ^[a]" children=0
````````````````````````````````

## Fallback

Failed recognition consumes nothing. Inline code, HTML tokens, comments,
formulas, and cross links are opaque to both forms.

## Scopes

For a referenced call, `Cite.scope` covers `[^label]` and `Citation.scope`
covers `^label`; `Footnote.scope` covers the complete definition through its
last continuation line. For an inline footnote,
`Cite.scope` and `Footnote.scope` cover `^[content]` and `Citation.scope`
covers `content`; the body's descendants cover only their own bytes.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover numeric
and named labels, definitions before and after calls, nested duplicate
definitions, the label length and container depth limits, `&Hat;`, an
undefined call with a matching link definition, a footnote inside a link
label, mixed referenced and inline source order in `Document.footnotes`,
code, comments, HTML, and formulas, exact scopes, allocation failure, and size-doubling runs of `^`, `[`, and `]`.
