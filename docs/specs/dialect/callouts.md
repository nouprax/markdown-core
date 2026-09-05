# Callouts

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the `Callout` kind, which every `>` container produces, and the option
`callouts` (default `false`) that recognizes `[!type]` metadata. Source:
Obsidian's callouts. Executable oracle: none; the Obsidian package does not
parse callouts, so product fixtures are the oracle of record. Landing: the
kind with `M3`, metadata with `O8`; until `M3` the current contract's
`BlockQuote` stands. Every example in this module runs with `callouts` on
unless its fence says otherwise; the [example format](../dialect.md#examples)
is defined by the index.

## Model

```text
CalloutFold = none | expanded | collapsed

Callout(variant: String?, fold: CalloutFold, title: [Markup]?,
        content: [Markup])
```

Every source container introduced by `>` produces a `Callout`. A plain quoted
block is a callout without metadata; the AST has no `BlockQuote` kind, alias,
or wrapper. The inherited algorithm owns the `>` prefix, laziness,
continuation, blank-line, and nesting rules; this module changes the kind and
adds optional metadata without a second container parser. `title` is a
node-valued field visited before `content` and is not counted in `children`;
the dump prints a non-null title as a `CalloutTitle` group before the content
lines and prints nothing for a null title.

- `variant == null` means no valid metadata line; then `title == null`,
  `fold == none`, and every quoted byte is represented by `content`.
- A non-null `variant` is the authored type as written, case preserved;
  matching against a type list is consumer policy.
- `fold == none` means no `+` or `-` was authored; `+` is `expanded` and `-`
  is `collapsed`.
- `title == null` means no title bytes were authored; otherwise `title` is
  the parsed inline content of the title, which may consist of one `Comment`.

```````````````````````````````` example callouts
> quote
.
Document scope=1:1..1:7 anchor=null attributes={} children=1
└── Callout scope=1:1..1:7 anchor=null attributes={} variant=null fold=none children=1
    └── Paragraph scope=1:3..1:7 anchor=null attributes={} children=1
        └── Text scope=1:3..1:7 anchor=null attributes={} literal="quote" children=0
````````````````````````````````

```````````````````````````````` example callouts
> [!info]

> [!TIP] Title
.
Document scope=1:1..3:14 anchor=null attributes={} children=2
├── Callout scope=1:1..1:9 anchor=null attributes={} variant="info" fold=none children=0
└── Callout scope=3:1..3:14 anchor=null attributes={} variant="TIP" fold=none children=0
    └── CalloutTitle children=1
        └── Text scope=3:10..3:14 anchor=null attributes={} literal="Title" children=0
````````````````````````````````

Default titles, built-in aliases, icons, colors, custom CSS types, and the
current fold state are renderer data. An unknown type stays its `variant`,
and so does a built-in type in any spelling; the parser substitutes nothing:

```````````````````````````````` example callouts
> [!custom-type]

> [!Note]
.
Document scope=1:1..3:9 anchor=null attributes={} children=2
├── Callout scope=1:1..1:16 anchor=null attributes={} variant="custom-type" fold=none children=0
└── Callout scope=3:1..3:9 anchor=null attributes={} variant="Note" fold=none children=0
````````````````````````````````

## Metadata grammar

With `callouts=true`, the candidate is the first line of the container after
the `>` prefix and its optional space are removed; up to three further spaces
may precede `[!`. A blank first line, four or more spaces, or any other
leading byte means no metadata.

```text
metadata-line = "[!" type "]" [ fold-marker ] ( 1*sep title / *WSP EOL )
type          = 1*( ASCII-letter / DIGIT / "_" / "-" )
fold-marker   = "+" / "-"
sep           = SP / TAB
```

```````````````````````````````` example callouts
> [!faq]+ Are callouts foldable?

> [!faq]- Are callouts foldable?
.
Document scope=1:1..3:32 anchor=null attributes={} children=2
├── Callout scope=1:1..1:32 anchor=null attributes={} variant="faq" fold=expanded children=0
│   └── CalloutTitle children=1
│       └── Text scope=1:11..1:32 anchor=null attributes={} literal="Are callouts foldable?" children=0
└── Callout scope=3:1..3:32 anchor=null attributes={} variant="faq" fold=collapsed children=0
    └── CalloutTitle children=1
        └── Text scope=3:11..3:32 anchor=null attributes={} literal="Are callouts foldable?" children=0
````````````````````````````````

The `]`, or the fold marker when present, must be followed by a space, a tab,
or the end of the line; otherwise the line is not a metadata line and the
callout is metadata-free, so `[!note]Title` and `[!faq]+Title` are content, as
is a marker that is not at the first position:

```````````````````````````````` example callouts
> [!note]Title

> [!faq]+Title

> x [!note]
.
Document scope=1:1..5:11 anchor=null attributes={} children=3
├── Callout scope=1:1..1:14 anchor=null attributes={} variant=null fold=none children=1
│   └── Paragraph scope=1:3..1:14 anchor=null attributes={} children=1
│       └── Text scope=1:3..1:14 anchor=null attributes={} literal="[!note]Title" children=0
├── Callout scope=3:1..3:14 anchor=null attributes={} variant=null fold=none children=1
│   └── Paragraph scope=3:3..3:14 anchor=null attributes={} children=1
│       └── Text scope=3:3..3:14 anchor=null attributes={} literal="[!faq]+Title" children=0
└── Callout scope=5:1..5:11 anchor=null attributes={} variant=null fold=none children=1
    └── Paragraph scope=5:3..5:11 anchor=null attributes={} children=1
        └── Text scope=5:3..5:11 anchor=null attributes={} literal="x [!note]" children=0
````````````````````````````````

Up to three spaces may precede `[!`; four make the line an indented code
block under the inherited grammar, so the callout is metadata-free:

```````````````````````````````` example callouts
>    [!note] x

>     [!note]
.
Document scope=1:1..3:13 anchor=null attributes={} children=2
├── Callout scope=1:1..1:14 anchor=null attributes={} variant="note" fold=none children=0
│   └── CalloutTitle children=1
│       └── Text scope=1:14..1:14 anchor=null attributes={} literal="x" children=0
└── Callout scope=3:1..3:13 anchor=null attributes={} variant=null fold=none children=1
    └── CodeBlock scope=3:7..3:13 anchor=null attributes={} info=null language=null literal="[!note]\n" fenced=false closed=true children=0
````````````````````````````````

A blank first line means no metadata:

```````````````````````````````` example callouts
>
> [!note] x
.
Document scope=1:1..2:11 anchor=null attributes={} children=1
└── Callout scope=1:1..2:11 anchor=null attributes={} variant=null fold=none children=1
    └── Paragraph scope=2:3..2:11 anchor=null attributes={} children=1
        └── Text scope=2:3..2:11 anchor=null attributes={} literal="[!note] x" children=0
````````````````````````````````

Trailing spaces and tabs are removed before the title is parsed, and a title
never contains `SoftBreak` or `LineBreak`. The title is inline content:

```````````````````````````````` example callouts
> [!note] **bold** title
.
Document scope=1:1..1:24 anchor=null attributes={} children=1
└── Callout scope=1:1..1:24 anchor=null attributes={} variant="note" fold=none children=0
    └── CalloutTitle children=2
        ├── Strong scope=1:11..1:18 anchor=null attributes={} children=1
        │   └── Text scope=1:13..1:16 anchor=null attributes={} literal="bold" children=0
        └── Text scope=1:19..1:24 anchor=null attributes={} literal=" title" children=0
````````````````````````````````

The paragraph that began on the metadata line is split: its remaining lines,
lazy lines included, form a `Paragraph` whose scope starts at the first byte
of the second line, and with no remaining lines the body is empty:

```````````````````````````````` example callouts
> [!note] Title
> body
> more
.
Document scope=1:1..3:6 anchor=null attributes={} children=1
└── Callout scope=1:1..3:6 anchor=null attributes={} variant="note" fold=none children=1
    ├── CalloutTitle children=1
    │   └── Text scope=1:11..1:15 anchor=null attributes={} literal="Title" children=0
    └── Paragraph scope=2:3..3:6 anchor=null attributes={} children=3
        ├── Text scope=2:3..2:6 anchor=null attributes={} literal="body" children=0
        ├── SoftBreak scope=2:7..2:7 anchor=null attributes={} children=0
        └── Text scope=3:3..3:6 anchor=null attributes={} literal="more" children=0
````````````````````````````````

```````````````````````````````` example callouts
> [!note] T
> body
lazy
.
Document scope=1:1..3:4 anchor=null attributes={} children=1
└── Callout scope=1:1..3:4 anchor=null attributes={} variant="note" fold=none children=1
    ├── CalloutTitle children=1
    │   └── Text scope=1:11..1:11 anchor=null attributes={} literal="T" children=0
    └── Paragraph scope=2:3..3:4 anchor=null attributes={} children=3
        ├── Text scope=2:3..2:6 anchor=null attributes={} literal="body" children=0
        ├── SoftBreak scope=2:7..2:7 anchor=null attributes={} children=0
        └── Text scope=3:1..3:4 anchor=null attributes={} literal="lazy" children=0
````````````````````````````````

Metadata is decided when the first line is consumed, before Setext
resolution, so a following underline belongs to the body:

```````````````````````````````` example callouts
> [!note] T
> ===
.
Document scope=1:1..2:5 anchor=null attributes={} children=1
└── Callout scope=1:1..2:5 anchor=null attributes={} variant="note" fold=none children=1
    ├── CalloutTitle children=1
    │   └── Text scope=1:11..1:11 anchor=null attributes={} literal="T" children=0
    └── Paragraph scope=2:3..2:5 anchor=null attributes={} children=1
        └── Text scope=2:3..2:5 anchor=null attributes={} literal="===" children=0
````````````````````````````````

Metadata is evaluated independently for every nested container:

```````````````````````````````` example callouts
> [!outer]
> > [!inner] x
.
Document scope=1:1..2:14 anchor=null attributes={} children=1
└── Callout scope=1:1..2:14 anchor=null attributes={} variant="outer" fold=none children=1
    └── Callout scope=2:3..2:14 anchor=null attributes={} variant="inner" fold=none children=0
        └── CalloutTitle children=1
            └── Text scope=2:14..2:14 anchor=null attributes={} literal="x" children=0
````````````````````````````````

A comment is an earlier scanner step, so a title may consist of one
`Comment`:

```````````````````````````````` example callouts comments
> [!note] %%t%%
.
Document scope=1:1..1:15 anchor=null attributes={} children=1
└── Callout scope=1:1..1:15 anchor=null attributes={} variant="note" fold=none children=0
    └── CalloutTitle children=1
        └── Comment scope=1:11..1:15 anchor=null attributes={} literal="t" children=0
````````````````````````````````

A block identifier candidate on a metadata line is title text; the
[block identifiers](block-identifiers.md) module attaches after metadata is
extracted, so a candidate in the body attaches to the body paragraph:

```````````````````````````````` example callouts block_identifiers
> [!note] Title ^t
> body ^p
.
Document scope=1:1..2:9 anchor=null attributes={} children=1
└── Callout scope=1:1..2:9 anchor=null attributes={} variant="note" fold=none children=1
    ├── CalloutTitle children=1
    │   └── Text scope=1:11..1:18 anchor=null attributes={} literal="Title ^t" children=0
    └── Paragraph scope=2:3..2:9 anchor="p" attributes={} children=1
        └── Text scope=2:3..2:6 anchor=null attributes={} literal="body" children=0
````````````````````````````````

GitHub's alerts spell a subset of this grammar: `> [!NOTE]` is a callout with
`variant="NOTE"`, and the [conflicts](conflicts.md) register records the
difference between the two sources:

```````````````````````````````` example callouts
> [!NOTE]
> text
.
Document scope=1:1..2:6 anchor=null attributes={} children=1
└── Callout scope=1:1..2:6 anchor=null attributes={} variant="NOTE" fold=none children=1
    └── Paragraph scope=2:3..2:6 anchor=null attributes={} children=1
        └── Text scope=2:3..2:6 anchor=null attributes={} literal="text" children=0
````````````````````````````````

## Option behavior and fallback

With `callouts=false`, `[!type]` is paragraph text inside a metadata-free
`Callout`:

```````````````````````````````` example
> [!info] x
.
Document scope=1:1..1:11 anchor=null attributes={} children=1
└── Callout scope=1:1..1:11 anchor=null attributes={} variant=null fold=none children=1
    └── Paragraph scope=1:3..1:11 anchor=null attributes={} children=1
        └── Text scope=1:3..1:11 anchor=null attributes={} literal="[!info] x" children=0
````````````````````````````````

With the option on, an invalid type, a marker after the first line, or a
marker separated from the first position by other content is ordinary
content of a metadata-free `Callout`; no second kind exists.

## Scopes

`Callout.scope` covers every quote marker, the metadata line, the title, and
the body. `title` child scopes cover the title bytes only.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover
title-only callouts with every fold marker, every built-in alias spelling
stored as written, invalid type characters, misplaced markers, deeper nested
combinations, attached block identifiers on the callout itself, exact
scopes, allocation failure, and adversarial quote depth.
