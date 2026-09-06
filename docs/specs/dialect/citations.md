# Citations

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `citations` (default `false`). Source: Pandoc's citation syntax.
Executable oracle: the Pandoc 3.11 CLI under `specs/oracles/pandoc/`.
Landing: `P7`, the first producer of `CitationReferent.bib`. The `Cite`,
`Citation`, and `CitationReferent` values are defined by the
[footnotes](footnotes.md) module; this module produces the `bib` branch and
never touches footnote recognition. Every example in this module runs with
`citations` on unless its fence says otherwise; the
[example format](../dialect.md#examples) is defined by the index.

```````````````````````````````` example citations
[see @doe99, pp. 3]
.
Document scope=1:1..1:19 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:19 anchor=null attributes={} children=1
    └── Cite scope=1:1..1:19 anchor=null attributes={} children=1
        └── Citation scope=1:2..1:18 referent=bib(key="doe99",mode=normal) children=0
            ├── CitationPrefix children=1
            │   └── Text scope=1:2..1:4 anchor=null attributes={} literal="see" children=0
            └── CitationSuffix children=1
                └── Text scope=1:12..1:18 anchor=null attributes={} literal=", pp. 3" children=0
````````````````````````````````

## Keys

```text
key-char        = unicode-letter / unicode-number / "_"
key-punctuation = ":" / "." / "#" / "$" / "%" / "&" / "-" / "+" / "?" /
                  "<" / ">" / "~" / "/"
bare-key        = key-char *( [ key-punctuation ] key-char )
braced-key      = "{" 1*( braced-key-character / braced-key ) "}"
braced-key-character
                = any non-whitespace scalar except "{" and "}"
citation-key    = "@" ( bare-key / braced-key )
```

In a bare key a punctuation scalar must be followed by a key character, so
a terminal `.` or a `,` ends the key. A braced key is non-empty,
whitespace-free, and balanced before the end of the same inline container;
braces owned by code spans or HTML tokens do not count. The outer braces are
excluded from the stored key. Keys are stored exactly after delimiter removal
and are neither case-folded nor resolved:

```````````````````````````````` example citations
@Foo_bar.baz. @Foo_bar,baz @{https://example.com/x}.
.
Document scope=1:1..1:52 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:52 anchor=null attributes={} children=6
    ├── Cite scope=1:1..1:12 anchor=null attributes={} children=1
    │   └── Citation scope=1:1..1:12 referent=bib(key="Foo_bar.baz",mode=authorInText) children=0
    │       ├── CitationPrefix children=0
    │       └── CitationSuffix children=0
    ├── Text scope=1:13..1:14 anchor=null attributes={} literal=". " children=0
    ├── Cite scope=1:15..1:22 anchor=null attributes={} children=1
    │   └── Citation scope=1:15..1:22 referent=bib(key="Foo_bar",mode=authorInText) children=0
    │       ├── CitationPrefix children=0
    │       └── CitationSuffix children=0
    ├── Text scope=1:23..1:27 anchor=null attributes={} literal=",baz " children=0
    ├── Cite scope=1:28..1:51 anchor=null attributes={} children=1
    │   └── Citation scope=1:28..1:51 referent=bib(key="https://example.com/x",mode=authorInText) children=0
    │       ├── CitationPrefix children=0
    │       └── CitationSuffix children=0
    └── Text scope=1:52..1:52 anchor=null attributes={} literal="." children=0
````````````````````````````````

A `@`, or the `-` of `-@`, opens a candidate only at the start of the inline
container or when the preceding scalar is not a letter, number, or `_`, so
`foo@bar`, `1@bar`, and `x_@bar` open nothing, while `(@bar)` does; this
holds independently of `autolinks`:

```````````````````````````````` example citations
foo@bar 1@bar x_@bar (@bar)
.
Document scope=1:1..1:27 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:27 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:22 anchor=null attributes={} literal="foo@bar 1@bar x_@bar (" children=0
    ├── Cite scope=1:23..1:26 anchor=null attributes={} children=1
    │   └── Citation scope=1:23..1:26 referent=bib(key="bar",mode=authorInText) children=0
    │       ├── CitationPrefix children=0
    │       └── CitationSuffix children=0
    └── Text scope=1:27..1:27 anchor=null attributes={} literal=")" children=0
````````````````````````````````

An escaped `\@` is text, and a `@` inside an autolink token is that token's
byte; the email post-pass of the [links and images](links-and-images.md)
module still runs over the text a failed candidate leaves behind:

```````````````````````````````` example citations
\@bar <x@y.z> x@y.z
.
Document scope=1:1..1:19 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:19 anchor=null attributes={} children=4
    ├── Text scope=1:1..1:6 anchor=null attributes={} literal="@bar " children=0
    ├── Link scope=1:7..1:13 anchor=null attributes={} dest=url("mailto:x@y.z") title=null children=1
    │   └── Text scope=1:8..1:12 anchor=null attributes={} literal="x@y.z" children=0
    ├── Text scope=1:14..1:14 anchor=null attributes={} literal=" " children=0
    └── Link scope=1:15..1:19 anchor=null attributes={} dest=url("mailto:x@y.z") title=null children=1
        └── Text scope=1:15..1:19 anchor=null attributes={} literal="x@y.z" children=0
````````````````````````````````

## Bracketed groups

Recognition of a group is alternative 5 of the bracket procedure of the
[links and images](links-and-images.md) module: it is tested after a direct
tail, a footnote call, a resolving reference tail, and an enabled valid span
container have failed, and before the shortcut-reference alternative.

```text
bracketed-group = "[" spacing citation-item *( ";" spacing citation-item )
                  spacing "]"
citation-item   = prefix [ "-" ] citation-key suffix
spacing         = optional whitespace with at most one line ending
```

A group produces one `Cite` with one `Citation` per item in source order.
Semicolons separate items and belong to neither affix. `prefix` is the inline
content before the item's key and optional mode marker; `suffix` is the inline
content after the key up to the next item boundary. Both exclude leading and
trailing whitespace, may be empty, and may contain nested inline markup; a
suffix of only whitespace is empty:

```````````````````````````````` example citations
[@a; @b, p. 1; see @c]
.
Document scope=1:1..1:22 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:22 anchor=null attributes={} children=1
    └── Cite scope=1:1..1:22 anchor=null attributes={} children=3
        ├── Citation scope=1:2..1:3 referent=bib(key="a",mode=normal) children=0
        │   ├── CitationPrefix children=0
        │   └── CitationSuffix children=0
        ├── Citation scope=1:6..1:13 referent=bib(key="b",mode=normal) children=0
        │   ├── CitationPrefix children=0
        │   └── CitationSuffix children=1
        │       └── Text scope=1:8..1:13 anchor=null attributes={} literal=", p. 1" children=0
        └── Citation scope=1:16..1:21 referent=bib(key="c",mode=normal) children=0
            ├── CitationPrefix children=1
            │   └── Text scope=1:16..1:18 anchor=null attributes={} literal="see" children=0
            └── CitationSuffix children=0
````````````````````````````````

```````````````````````````````` example citations
[@a, *emphasis*]
.
Document scope=1:1..1:16 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:16 anchor=null attributes={} children=1
    └── Cite scope=1:1..1:16 anchor=null attributes={} children=1
        └── Citation scope=1:2..1:15 referent=bib(key="a",mode=normal) children=0
            ├── CitationPrefix children=0
            └── CitationSuffix children=2
                ├── Text scope=1:4..1:5 anchor=null attributes={} literal=", " children=0
                └── Emphasis scope=1:6..1:15 anchor=null attributes={} children=1
                    └── Text scope=1:7..1:14 anchor=null attributes={} literal="emphasis" children=0
````````````````````````````````

An unescaped `-` immediately before `@` is the mode marker when the opener
precondition of the key grammar holds at the `-`: it then selects
`suppressAuthor` and is excluded from the affixes and the key. When the
precondition fails at the `-`, because a letter, number, or `_` precedes it,
the `-` is prefix text and the precondition is evaluated at the `@` instead,
so `[Smith-@1990]` has the prefix `Smith-` and mode `normal`. Every other
item has mode `normal`:

```````````````````````````````` example citations
[-@doe99] [Smith -@1990] [Smith-@1990]
.
Document scope=1:1..1:38 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:38 anchor=null attributes={} children=5
    ├── Cite scope=1:1..1:9 anchor=null attributes={} children=1
    │   └── Citation scope=1:2..1:8 referent=bib(key="doe99",mode=suppressAuthor) children=0
    │       ├── CitationPrefix children=0
    │       └── CitationSuffix children=0
    ├── Text scope=1:10..1:10 anchor=null attributes={} literal=" " children=0
    ├── Cite scope=1:11..1:24 anchor=null attributes={} children=1
    │   └── Citation scope=1:12..1:23 referent=bib(key="1990",mode=suppressAuthor) children=0
    │       ├── CitationPrefix children=1
    │       │   └── Text scope=1:12..1:16 anchor=null attributes={} literal="Smith" children=0
    │       └── CitationSuffix children=0
    ├── Text scope=1:25..1:25 anchor=null attributes={} literal=" " children=0
    └── Cite scope=1:26..1:38 anchor=null attributes={} children=1
        └── Citation scope=1:27..1:37 referent=bib(key="1990",mode=normal) children=0
            ├── CitationPrefix children=1
            │   └── Text scope=1:27..1:32 anchor=null attributes={} literal="Smith-" children=0
            └── CitationSuffix children=0
````````````````````````````````

`spacing` admits at most one line ending, so a group may span two lines; the
line ending belongs to neither affix:

```````````````````````````````` example citations
[@a;
@b]
.
Document scope=1:1..2:3 anchor=null attributes={} children=1
└── Paragraph scope=1:1..2:3 anchor=null attributes={} children=1
    └── Cite scope=1:1..2:3 anchor=null attributes={} children=2
        ├── Citation scope=1:2..1:3 referent=bib(key="a",mode=normal) children=0
        │   ├── CitationPrefix children=0
        │   └── CitationSuffix children=0
        └── Citation scope=2:1..2:2 referent=bib(key="b",mode=normal) children=0
            ├── CitationPrefix children=0
            └── CitationSuffix children=0
````````````````````````````````

Curly braces inside a suffix are suffix text; their locator meaning belongs
to a CSL-aware consumer and is not represented:

```````````````````````````````` example citations
[@smith{ii, A, D-Z}, with a suffix]
.
Document scope=1:1..1:35 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:35 anchor=null attributes={} children=1
    └── Cite scope=1:1..1:35 anchor=null attributes={} children=1
        └── Citation scope=1:2..1:34 referent=bib(key="smith",mode=normal) children=0
            ├── CitationPrefix children=0
            └── CitationSuffix children=1
                └── Text scope=1:8..1:34 anchor=null attributes={} literal="{ii, A, D-Z}, with a suffix" children=0
````````````````````````````````

A group in which any item lacks a key is not a citation, and the bracket pair
continues at the shortcut-reference alternative:

```````````````````````````````` example citations
[see p. 3] [@foo; no key]
.
Document scope=1:1..1:25 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:25 anchor=null attributes={} children=1
    └── Text scope=1:1..1:25 anchor=null attributes={} literal="[see p. 3] [@foo; no key]" children=0
````````````````````````````````

A non-resolving reference tail does not block a group, and a direct tail
wins over it, with the group's bytes then parsed as ordinary link content:

```````````````````````````````` example citations
[@foo][nope] [@foo](u)
.
Document scope=1:1..1:22 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:22 anchor=null attributes={} children=3
    ├── Cite scope=1:1..1:6 anchor=null attributes={} children=1
    │   └── Citation scope=1:2..1:5 referent=bib(key="foo",mode=normal) children=0
    │       ├── CitationPrefix children=0
    │       └── CitationSuffix children=0
    ├── Text scope=1:7..1:13 anchor=null attributes={} literal="[nope] " children=0
    └── Link scope=1:14..1:22 anchor=null attributes={} dest=url("u") title=null children=1
        └── Cite scope=1:15..1:18 anchor=null attributes={} children=1
            └── Citation scope=1:15..1:18 referent=bib(key="foo",mode=authorInText) children=0
                ├── CitationPrefix children=0
                └── CitationSuffix children=0
````````````````````````````````

With `bracketedSpans` on, a span container after the group is tested first,
so `[@foo]{.key}` is a `Span` containing an author-in-text `Cite`; with it
off, the container is literal text after the bracketed `Cite`:

```````````````````````````````` example citations bracketed_spans
[@foo]{.key}
.
Document scope=1:1..1:12 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:12 anchor=null attributes={} children=1
    └── Span scope=1:1..1:12 anchor=null attributes={.key} children=1
        └── Cite scope=1:2..1:5 anchor=null attributes={} children=1
            └── Citation scope=1:2..1:5 referent=bib(key="foo",mode=authorInText) children=0
                ├── CitationPrefix children=0
                └── CitationSuffix children=0
````````````````````````````````

```````````````````````````````` example citations
[@foo]{.key}
.
Document scope=1:1..1:12 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:12 anchor=null attributes={} children=2
    ├── Cite scope=1:1..1:6 anchor=null attributes={} children=1
    │   └── Citation scope=1:2..1:5 referent=bib(key="foo",mode=normal) children=0
    │       ├── CitationPrefix children=0
    │       └── CitationSuffix children=0
    └── Text scope=1:7..1:12 anchor=null attributes={} literal="{.key}" children=0
````````````````````````````````

## Author-in-text keys

An unbracketed citation key is inline step A8 and produces a one-item `Cite`
whose referent mode is `authorInText`; `-@key` outside brackets produces
`suppressAuthor`:

```````````````````````````````` example citations
@smith04 says blah.

-@jones says blah.
.
Document scope=1:1..3:18 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:19 anchor=null attributes={} children=2
│   ├── Cite scope=1:1..1:8 anchor=null attributes={} children=1
│   │   └── Citation scope=1:1..1:8 referent=bib(key="smith04",mode=authorInText) children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   └── Text scope=1:9..1:19 anchor=null attributes={} literal=" says blah." children=0
└── Paragraph scope=3:1..3:18 anchor=null attributes={} children=2
    ├── Cite scope=3:1..3:7 anchor=null attributes={} children=1
    │   └── Citation scope=3:1..3:7 referent=bib(key="jones",mode=suppressAuthor) children=0
    │       ├── CitationPrefix children=0
    │       └── CitationSuffix children=0
    └── Text scope=3:8..3:18 anchor=null attributes={} literal=" says blah." children=0
````````````````````````````````

An immediately following bracketed tail belongs to the sole item's suffix
without its brackets. Optional spaces or tabs and at most one line ending may
separate the key from the `[`. A tail that itself contains items produces one
`Cite` whose first item is author-in-text with the first suffix, followed by
the further items:

```````````````````````````````` example citations
@smith04 [p. 33] says blah.

@k [s1; @k2, s2]
.
Document scope=1:1..3:16 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:27 anchor=null attributes={} children=2
│   ├── Cite scope=1:1..1:16 anchor=null attributes={} children=1
│   │   └── Citation scope=1:1..1:16 referent=bib(key="smith04",mode=authorInText) children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=1
│   │           └── Text scope=1:11..1:15 anchor=null attributes={} literal="p. 33" children=0
│   └── Text scope=1:17..1:27 anchor=null attributes={} literal=" says blah." children=0
└── Paragraph scope=3:1..3:16 anchor=null attributes={} children=1
    └── Cite scope=3:1..3:16 anchor=null attributes={} children=2
        ├── Citation scope=3:1..3:6 referent=bib(key="k",mode=authorInText) children=0
        │   ├── CitationPrefix children=0
        │   └── CitationSuffix children=1
        │       └── Text scope=3:5..3:6 anchor=null attributes={} literal="s1" children=0
        └── Citation scope=3:9..3:15 referent=bib(key="k2",mode=normal) children=0
            ├── CitationPrefix children=0
            └── CitationSuffix children=1
                └── Text scope=3:12..3:15 anchor=null attributes={} literal=", s2" children=0
````````````````````````````````

The tail is not claimed when it begins with `^`, or when its `]` is
immediately followed by `(`, `[`, or a valid attribute container, in which
case the bracket pair is decided by the bracket procedure on its own.

A bare `@label` with no bracketed tail whose label is registered as an example
label anywhere in the document under `exampleLists` is an `ExampleReference`
rather than a `Cite`; the [lists](lists.md) module states that rule, and the
choice is finalized document-wide so parser order cannot change it.

## Non-normative notes

Locator recognition, the default page locator, and the meaning of braces in
suffixes are behaviors of a citation processor. The exact heading class
`reset-citation-positions` on a heading whose parent is `Document` asks such a
processor to reset position-sensitive state; the parser stores the class as
written through the [attributes](attributes.md) module and does nothing else.

## Option behavior and fallback

With `citations=false`, `@` has no meaning and every bracket pair follows the
other alternatives; footnote `Cite` nodes are unaffected:

```````````````````````````````` example
[@a] @b
.
Document scope=1:1..1:7 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:7 anchor=null attributes={} children=1
    └── Text scope=1:1..1:7 anchor=null attributes={} literal="[@a] @b" children=0
````````````````````````````````

A failed candidate releases its opener and consumes nothing. Inline code,
comment bodies, HTML tokens, formulas, and cross links are opaque; a
semicolon inside an opaque child is not an item separator. A line the
inherited grammar accepts as a link reference definition is one regardless of
a leading `@`.

## Scopes

A bracketed `Cite.scope` covers its outer brackets and contents. Each
`Citation.scope` runs from the first non-whitespace byte after `[` or `;` to
the last non-whitespace byte before `;` or `]`. An author-in-text `Cite` and
its item both run from the mode marker or `@` through the tail's closing `]`,
or through the key when no tail is claimed. Affix child scopes cover visible
authored content only.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover every
key punctuation scalar, repeated punctuation, braced keys with nesting,
spacing after `[` and `;`, author-in-text tails with a `^` start and with a
following `(`, `[`, or container, example labels before and after their
definitions, code, comments, HTML, and formulas, definitions with a leading
`@`, exact group, item, and affix scopes, allocation failure, and
adversarial runs of `@`, punctuation, braces, brackets, and semicolons.
