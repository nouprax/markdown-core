# Anchors

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the universal `Markup.anchor` field, the document anchor registry,
automatic heading anchors, and implicit heading references. Sources: Pandoc's
attribute identifiers, `auto_identifiers` with `gfm_auto_identifiers`, and
`implicit_header_references`; Obsidian's block identifiers populate the same
field under the [block identifiers](block-identifiers.md) module. Executable
oracle: the Pandoc 3.11 CLI under `specs/oracles/pandoc/`. Landing: the field
with `M7`, automatic anchors with `P3`, implicit heading references with `P4`.
The [example format](../dialect.md#examples) is defined by the index.

## Model

```text
Markup(anchor: String?, ...)
```

Every `Markup` kind has exactly one nullable `anchor` field. `null` means the
node declares no link target; a non-null anchor is a non-empty string. An
anchor names the target node for consumers. It is not the node's identity, a
reference edge, or a resolver result, and it does not record which source rule
produced it.

Source punctuation is excluded from the value, and different rules produce
one consumer fact:

| Source rule                                 | Value          |
| ------------------------------------------- | -------------- |
| attribute `{#foo}` or `{id=foo}`            | `anchor="foo"` |
| automatic heading anchor for `# Foo`        | `anchor="foo"` |
| block identifier `^foo`                     | `anchor="foo"` |

There is no heading, block, or fragment discriminator. The reference side of a
link never populates its own `anchor`: `Link`, `CrossLink`, `Cite`, and a
footnote call declare nothing by referring to something.
`Destination.url("#foo")` and `Destination.cross(path="", anchor="foo")` are two
reference spellings that a consumer may match against a node whose `anchor ==
"foo"`; matching is downstream and never mutates the AST.

`anchor` is `null` on every node that no rule populates. A heading always
receives one, from an explicit identifier or from synthesis:

```````````````````````````````` example
# My Header

A paragraph.
.
Document scope=1:1..3:12 anchor=null attributes={} children=2
├── Heading scope=1:1..1:11 anchor="my-header" attributes={} level=1 children=1
│   └── Text scope=1:3..1:11 anchor=null attributes={} literal="My Header" children=0
└── Paragraph scope=3:1..3:12 anchor=null attributes={} children=1
    └── Text scope=3:1..3:12 anchor=null attributes={} literal="A paragraph." children=0
````````````````````````````````

## Population and precedence

Only these rules populate `anchor`:

- the attribute grammar's identifier at every attachment site of the
  [attributes](attributes.md) module;
- automatic heading anchors; and
- block identifiers.

One node has at most one final anchor:

- within one attribute container, the last identifier wins and an empty final
  `id=` clears the candidate;
- a non-empty explicit heading anchor wins over automatic synthesis;
- a resolved reference occurrence's own non-null anchor wins over the anchor
  inherited from its definition; and
- a block identifier attaches only to a node whose anchor is still `null`
  when block finalization reaches it; otherwise its bytes are ordinary
  content.

No operation concatenates anchors or stores a second value. Two explicit
declarations may author the same anchor; both keep their values and the parser
emits no diagnostic.

## The document registry and synthesis

Synthesis runs once, after block and inline parsing of the whole document has
completed, over every node reachable from `Document.content` and
`Document.footnotes`:

1. Reserve the final anchor of every emitted node that a rule other than
   synthesis populated explicitly. An anchor stored on a reference definition is reserved
   by the occurrences that inherit it, not by the definition; an unreferenced
   definition reserves nothing.
2. Visit headings in ascending order of `Heading.scope.start`. For each heading
   whose `anchor` is `null`, compute the base below, then set `anchor` to the
   base if it is not registered, otherwise to `base-N` for the smallest `N` of
   at least 1 such that `base-N` is not registered. Register the result.

```````````````````````````````` example
# My Header

## My Header
.
Document scope=1:1..3:12 anchor=null attributes={} children=2
├── Heading scope=1:1..1:11 anchor="my-header" attributes={} level=1 children=1
│   └── Text scope=1:3..1:11 anchor=null attributes={} literal="My Header" children=0
└── Heading scope=3:1..3:12 anchor="my-header-1" attributes={} level=2 children=1
    └── Text scope=3:4..3:12 anchor=null attributes={} literal="My Header" children=0
````````````````````````````````

An explicit anchor is reserved before synthesis, so a later automatic base
that collides with it is suffixed:

```````````````````````````````` example
# T {#x}

# x
.
Document scope=1:1..3:3 anchor=null attributes={} children=2
├── Heading scope=1:1..1:8 anchor="x" attributes={} level=1 children=1
│   └── Text scope=1:3..1:3 anchor=null attributes={} literal="T" children=0
└── Heading scope=3:1..3:3 anchor="x-1" attributes={} level=1 children=1
    └── Text scope=3:3..3:3 anchor=null attributes={} literal="x" children=0
````````````````````````````````

A generated anchor has no scope and adds no source position; `Heading.scope`
is the authored heading range.

### Automatic anchor algorithm

The base of a heading is derived from its parsed inline content:

1. Project the content to plain text: `Text` and `Code` contribute `literal`;
   `Emphasis`, `Strong`, `Strikethrough`, `Span`, `Superscript`, `Subscript`,
   `Mark`, `Insert`, `Link`, `Image`, and `DirectiveLabel` contribute their
   concatenated child text; `Directive` contributes its label text; `SoftBreak`
   and `LineBreak` contribute one space; `Formula` contributes `literal`;
   `CrossLink` contributes `label` when non-null and otherwise its authored
   path and anchor text; a bibliography `Cite` contributes, per item, prefix
   text, `@` and the key, and suffix text in order; `ExampleReference`
   contributes `@` and its label; `HTML`, `Comment`, and a footnote `Cite`
   contribute nothing.
2. Apply the simple lowercase mapping.
3. Replace each Unicode whitespace scalar with one `-`, without collapsing
   adjacent replacements.
4. Remove every scalar that is not a letter, a number, a combining mark,
   connector punctuation, `-`, or `_`.
5. If the result is empty, use `section`.

Punctuation removal inserts nothing, so `A.B` becomes `ab`. There is no
emoji step: `:tada:` keeps `tada` and loses its colons. Leading digits are
kept, and each interior whitespace scalar becomes its own hyphen:

```````````````````````````````` example
# Hello, World!

# 1. Intro

# A.B

# :tada: Party

# *Em* and `code`

# !!!

# Ünïcode Ĝ

#   spaced   words
.
Document scope=1:1..15:18 anchor=null attributes={} children=8
├── Heading scope=1:1..1:15 anchor="hello-world" attributes={} level=1 children=1
│   └── Text scope=1:3..1:15 anchor=null attributes={} literal="Hello, World!" children=0
├── Heading scope=3:1..3:10 anchor="1-intro" attributes={} level=1 children=1
│   └── Text scope=3:3..3:10 anchor=null attributes={} literal="1. Intro" children=0
├── Heading scope=5:1..5:5 anchor="ab" attributes={} level=1 children=1
│   └── Text scope=5:3..5:5 anchor=null attributes={} literal="A.B" children=0
├── Heading scope=7:1..7:14 anchor="tada-party" attributes={} level=1 children=1
│   └── Text scope=7:3..7:14 anchor=null attributes={} literal=":tada: Party" children=0
├── Heading scope=9:1..9:17 anchor="em-and-code" attributes={} level=1 children=3
│   ├── Emphasis scope=9:3..9:6 anchor=null attributes={} children=1
│   │   └── Text scope=9:4..9:5 anchor=null attributes={} literal="Em" children=0
│   ├── Text scope=9:7..9:11 anchor=null attributes={} literal=" and " children=0
│   └── Code scope=9:12..9:17 anchor=null attributes={} literal="code" children=0
├── Heading scope=11:1..11:5 anchor="section" attributes={} level=1 children=1
│   └── Text scope=11:3..11:5 anchor=null attributes={} literal="!!!" children=0
├── Heading scope=13:1..13:14 anchor="ünïcode-ĝ" attributes={} level=1 children=1
│   └── Text scope=13:3..13:14 anchor=null attributes={} literal="Ünïcode Ĝ" children=0
└── Heading scope=15:1..15:18 anchor="spaced---words" attributes={} level=1 children=1
    └── Text scope=15:5..15:18 anchor=null attributes={} literal="spaced   words" children=0
````````````````````````````````

## Implicit heading references

Every heading with a non-null final anchor contributes a virtual reference
definition. Its label source is the
authored heading text after removing the ATX or Setext heading syntax, the
optional ATX closing sequence, and a trailing attribute container, normalized
by the inherited reference-label normalization. The virtual definition
targets `#` followed by the final anchor and has `title=null`, `anchor=null`,
and `Attributes.empty` for `merge`. The full, collapsed, and shortcut forms
all resolve to an ordinary `Link` through the resolver of the
[links and images](links-and-images.md) module, in the same order-independent
document finalization that resolves example labels:

```````````````````````````````` example
# First chapter

[First chapter] [First chapter][] [go there][First chapter]
.
Document scope=1:1..3:59 anchor=null attributes={} children=2
├── Heading scope=1:1..1:15 anchor="first-chapter" attributes={} level=1 children=1
│   └── Text scope=1:3..1:15 anchor=null attributes={} literal="First chapter" children=0
└── Paragraph scope=3:1..3:59 anchor=null attributes={} children=5
    ├── Link scope=3:1..3:15 anchor=null attributes={} dest=url("#first-chapter") title=null children=1
    │   └── Text scope=3:2..3:14 anchor=null attributes={} literal="First chapter" children=0
    ├── Text scope=3:16..3:16 anchor=null attributes={} literal=" " children=0
    ├── Link scope=3:17..3:33 anchor=null attributes={} dest=url("#first-chapter") title=null children=1
    │   └── Text scope=3:18..3:30 anchor=null attributes={} literal="First chapter" children=0
    ├── Text scope=3:34..3:34 anchor=null attributes={} literal=" " children=0
    └── Link scope=3:35..3:59 anchor=null attributes={} dest=url("#first-chapter") title=null children=1
        └── Text scope=3:36..3:43 anchor=null attributes={} literal="go there" children=0
````````````````````````````````

An explicit reference definition with the same normalized label always wins
over the virtual one:

```````````````````````````````` example
# First chapter

[First chapter]

[First chapter]: /explicit
.
Document scope=1:1..5:26 anchor=null attributes={} children=2
├── Heading scope=1:1..1:15 anchor="first-chapter" attributes={} level=1 children=1
│   └── Text scope=1:3..1:15 anchor=null attributes={} literal="First chapter" children=0
└── Paragraph scope=3:1..3:15 anchor=null attributes={} children=1
    └── Link scope=3:1..3:15 anchor=null attributes={} dest=url("/explicit") title=null children=1
        └── Text scope=3:2..3:14 anchor=null attributes={} literal="First chapter" children=0
````````````````````````````````

Inline markup remains part of the label, so `# *Foo*` is referenced by
`[*Foo*]`, not `[Foo]`; an unresolved candidate keeps the inherited fallback:

```````````````````````````````` example
# *Foo*

[*Foo*] [Foo]
.
Document scope=1:1..3:13 anchor=null attributes={} children=2
├── Heading scope=1:1..1:7 anchor="foo" attributes={} level=1 children=1
│   └── Emphasis scope=1:3..1:7 anchor=null attributes={} children=1
│       └── Text scope=1:4..1:6 anchor=null attributes={} literal="Foo" children=0
└── Paragraph scope=3:1..3:13 anchor=null attributes={} children=2
    ├── Link scope=3:1..3:7 anchor=null attributes={} dest=url("#foo") title=null children=1
    │   └── Emphasis scope=3:2..3:6 anchor=null attributes={} children=1
    │       └── Text scope=3:3..3:5 anchor=null attributes={} literal="Foo" children=0
    └── Text scope=3:8..3:13 anchor=null attributes={} literal=" [Foo]" children=0
````````````````````````````````

When several headings have the same normalized label, the virtual definition
targets the first in source order:

```````````````````````````````` example
# Dup

# Dup

[Dup]
.
Document scope=1:1..5:5 anchor=null attributes={} children=3
├── Heading scope=1:1..1:5 anchor="dup" attributes={} level=1 children=1
│   └── Text scope=1:3..1:5 anchor=null attributes={} literal="Dup" children=0
├── Heading scope=3:1..3:5 anchor="dup-1" attributes={} level=1 children=1
│   └── Text scope=3:3..3:5 anchor=null attributes={} literal="Dup" children=0
└── Paragraph scope=5:1..5:5 anchor=null attributes={} children=1
    └── Link scope=5:1..5:5 anchor=null attributes={} dest=url("#dup") title=null children=1
        └── Text scope=5:2..5:4 anchor=null attributes={} literal="Dup" children=0
````````````````````````````````

A heading whose label cannot be written as a reference label, such as one
containing an unescaped `]`, contributes no definition. Attributes authored
at the occurrence follow the link rule of the [attributes](attributes.md)
module.

## Cross links and anchors

Cross links spell a heading target as written: `[[#My Header]]` stores
`anchor="My Header"`, and the automatic anchor of that heading is
`my-header`. The parser normalizes neither side, because the AST does not
decide for the consumer; matching the two values is consumer policy, as the
[conflicts](conflicts.md) register records:

```````````````````````````````` example
# My Header

[[#My Header]]
.
Document scope=1:1..3:14 anchor=null attributes={} children=2
├── Heading scope=1:1..1:11 anchor="my-header" attributes={} level=1 children=1
│   └── Text scope=1:3..1:11 anchor=null attributes={} literal="My Header" children=0
└── Paragraph scope=3:1..3:14 anchor=null attributes={} children=1
    └── CrossLink scope=3:1..3:14 anchor=null attributes={} embedded=false dest=cross(path="",anchor="My Header") label=null children=0
````````````````````````````````

## Scopes and lifecycle

Anchor syntax that is lexically part of an occurrence is inside that
occurrence's scope even though its punctuation is absent from visible content
and from the stored value. An anchor inherited from a reference definition
changes only the resolved occurrence's field; the occurrence keeps its own
range. `anchor` is an owned immutable string and has no scope or attributes
of its own. Recognizing an anchor never opens a link, resolves a document,
creates an HTML `id`, or changes rendering.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover
`anchor=null` on every kind; explicit identifiers at every attachment site;
block identifiers; identical values from different rules; last-identifier,
clearing, and occurrence-over-definition precedence; every kind of the
projection table inside a heading, `Formula`, `HTML`, `Comment`, `Image`,
line breaks, and directive labels included; combining marks and connectors;
reservation of every explicit anchor from every rule before synthesis, including an anchor on an unreferenced definition reserving
nothing; headings inside footnotes; explicit and generated duplicates;
occurrence attributes; exact scopes; allocation failure; and large duplicate heading sets.
