# Block identifiers

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `blockIdentifiers` (default `false`). Source: Obsidian's block
identifiers. Executable oracle: none; the Obsidian package does not parse
them, so product fixtures are the oracle of record. Landing: `O7`. Every
example in this module runs with `blockIdentifiers` on unless its fence says
otherwise; the [example format](../dialect.md#examples) is defined by the
index.

## Model

A block identifier populates the universal `anchor` field of the
[anchors](anchors.md) module on the block it addresses, without the caret and
without a discriminator. It is never a node and never remains visible content
after recognition. The `#^id` reference spelling belongs to the
[cross links](cross-links.md) module.

```text
block-id = 1*( ASCII-letter / DIGIT / "-" )
```

Whitespace, underscores, other punctuation, and non-ASCII letters make a
candidate invalid.

## Placements

Attachment happens while the owning block is finalized, never in a
document-wide repair pass or an offset side table. Three placements exist,
and their owner set is exactly `Paragraph`, `ListItem`, `List`, `Callout`,
and `Table`; headings are addressed through heading anchors, and code blocks,
rows, cells, and callout parts are not addressable.

### Paragraph suffix

The rule matches when the paragraph's last line, after trailing spaces and
tabs are removed, ends with `^block-id` preceded by one or more spaces or
tabs. The removed bytes are the identifier, its caret, and the preceding
whitespace, a tab included; the paragraph's scope still covers them, and the
child scopes end before them:

```````````````````````````````` example block_identifiers
Some text ^abc

Tabbed	^t1
.
Document scope=1:1..3:10 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:14 anchor="abc" attributes={} children=1
│   └── Text scope=1:1..1:9 anchor=null attributes={} literal="Some text" children=0
└── Paragraph scope=3:1..3:10 anchor="t1" attributes={} children=1
    └── Text scope=3:1..3:6 anchor=null attributes={} literal="Tabbed" children=0
````````````````````````````````

The rule also matches when the last line consists solely of `^block-id` at
indentation zero to three while the paragraph has an earlier line; then the
preceding line ending is removed too, so no `SoftBreak` precedes the
identifier:

```````````````````````````````` example block_identifiers
text
^abc
.
Document scope=1:1..2:4 anchor=null attributes={} children=1
└── Paragraph scope=1:1..2:4 anchor="abc" attributes={} children=1
    └── Text scope=1:1..1:4 anchor=null attributes={} literal="text" children=0
````````````````````````````````

```````````````````````````````` example block_identifiers
first
second
^id
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── Paragraph scope=1:1..3:3 anchor="id" attributes={} children=3
    ├── Text scope=1:1..1:5 anchor=null attributes={} literal="first" children=0
    ├── SoftBreak scope=1:6..1:6 anchor=null attributes={} children=0
    └── Text scope=2:1..2:6 anchor=null attributes={} literal="second" children=0
````````````````````````````````

The rule is lexical on the final line's bytes: only block-level owners such
as a code block protect a candidate, while inline code, inline HTML, and
inline comments on that line do not. `\^` never starts an identifier.

### Structured block line

Block-start step 14: a line indented zero to three spaces whose content is
`^block-id` optionally followed by spaces or tabs, preceded by one or more
blank lines and followed by one or more blank lines or the end of the
document, attaches to the last block before those blank lines when that block
is a `List`, `Callout`, or `Table`. The line, the blank lines, and the owner
are direct content of the same container; for nested lists the owner is the
outermost list at that level. The line produces no node, and the owner's
scope extends over it:

```````````````````````````````` example block_identifiers
- a
- b

^lst

next
.
Document scope=1:1..6:4 anchor=null attributes={} children=2
├── List scope=1:1..4:4 anchor="lst" attributes={} flavor=bullet start=null style=null delimiter=null tight=true children=2
│   ├── ListItem scope=1:1..1:3 anchor=null attributes={} marker=null exampleLabel=null children=1
│   │   └── Paragraph scope=1:3..1:3 anchor=null attributes={} children=1
│   │       └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=2:3..2:3 anchor=null attributes={} children=1
│           └── Text scope=2:3..2:3 anchor=null attributes={} literal="b" children=0
└── Paragraph scope=6:1..6:4 anchor=null attributes={} children=1
    └── Text scope=6:1..6:4 anchor=null attributes={} literal="next" children=0
````````````````````````````````

```````````````````````````````` example block_identifiers
> quote

^q

| h |
| - |
| c |

^tbl
.
Document scope=1:1..9:4 anchor=null attributes={} children=2
├── Callout scope=1:1..3:2 anchor="q" attributes={} variant=null fold=none children=1
│   └── Paragraph scope=1:3..1:7 anchor=null attributes={} children=1
│       └── Text scope=1:3..1:7 anchor=null attributes={} literal="quote" children=0
└── Table scope=5:1..9:4 anchor="tbl" attributes={} columns=[none:null] children=2
    ├── TableHead children=1
    │   └── TableRow scope=5:1..5:5 anchor=null attributes={} children=1
    │       └── TableCell scope=5:2..5:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=5:3..5:3 anchor=null attributes={} literal="h" children=0
    ├── TableBody children=1
    │   └── TableRow scope=7:1..7:5 anchor=null attributes={} children=1
    │       └── TableCell scope=7:2..7:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=7:3..7:3 anchor=null attributes={} literal="c" children=0
    └── TableFoot children=0
````````````````````````````````

When a table owns a following caption, the identifier line after the caption
attaches to the `Table`. Otherwise the line is a paragraph:

```````````````````````````````` example block_identifiers
^abc

^def
.
Document scope=1:1..3:4 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:4 anchor=null attributes={} literal="^abc" children=0
└── Paragraph scope=3:1..3:4 anchor=null attributes={} children=1
    └── Text scope=3:1..3:4 anchor=null attributes={} literal="^def" children=0
````````````````````````````````

### List item suffix

When an item's first block is a `Paragraph` and the candidate is on the
marker line, the identifier attaches to the `ListItem` and the `Paragraph`
keeps `anchor=null`; otherwise the paragraph suffix rule applies to the
paragraph that owns the final line:

```````````````````````````````` example block_identifiers
- item ^id
- second
  more ^p
.
Document scope=1:1..3:9 anchor=null attributes={} children=1
└── List scope=1:1..3:9 anchor=null attributes={} flavor=bullet start=null style=null delimiter=null tight=true children=2
    ├── ListItem scope=1:1..1:10 anchor="id" attributes={} marker=null exampleLabel=null children=1
    │   └── Paragraph scope=1:3..1:10 anchor=null attributes={} children=1
    │       └── Text scope=1:3..1:6 anchor=null attributes={} literal="item" children=0
    └── ListItem scope=2:1..3:9 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=2:3..3:9 anchor="p" attributes={} children=3
            ├── Text scope=2:3..2:8 anchor=null attributes={} literal="second" children=0
            ├── SoftBreak scope=2:9..2:9 anchor=null attributes={} children=0
            └── Text scope=3:3..3:6 anchor=null attributes={} literal="more" children=0
````````````````````````````````

Callout metadata is extracted before attachment, so a candidate on a metadata
line is title text. A candidate attaches only when the owner's anchor is
still `null` when finalization reaches it; otherwise its bytes are ordinary
content. The owner's `anchor` receives the identifier value; the consumer
value is indistinguishable from the same anchor produced by another rule.

## Option behavior and fallback

With the option on, a caret without a valid non-empty identifier is text,
missing separation makes the candidate text, an escaped caret is text, and
trailing non-space bytes after an identifier make the candidate ordinary
content:

```````````````````````````````` example block_identifiers
text ^a_b text ^ text^abc

text \^abc

text ^abc extra
.
Document scope=1:1..5:15 anchor=null attributes={} children=3
├── Paragraph scope=1:1..1:25 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:25 anchor=null attributes={} literal="text ^a_b text ^ text^abc" children=0
├── Paragraph scope=3:1..3:10 anchor=null attributes={} children=1
│   └── Text scope=3:1..3:10 anchor=null attributes={} literal="text ^abc" children=0
└── Paragraph scope=5:1..5:15 anchor=null attributes={} children=1
    └── Text scope=5:1..5:15 anchor=null attributes={} literal="text ^abc extra" children=0
````````````````````````````````

Only the final candidate of a line is tested, so an earlier caret on the
same line is text:

```````````````````````````````` example block_identifiers
text ^a ^b
.
Document scope=1:1..1:10 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:10 anchor="b" attributes={} children=1
    └── Text scope=1:1..1:7 anchor=null attributes={} literal="text ^a" children=0
````````````````````````````````

Identifier-like bytes inside code, an HTML block, a comment, or a cross link
are those constructs' bytes:

```````````````````````````````` example block_identifiers
```
x ^id
```
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── CodeBlock scope=1:1..3:3 anchor=null attributes={} info=null language=null literal="x ^id\n" fenced=true closed=true children=0
````````````````````````````````

With `blockIdentifiers=false`, every candidate is ordinary content:

```````````````````````````````` example
text ^abc
.
Document scope=1:1..1:9 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:9 anchor=null attributes={} children=1
    └── Text scope=1:1..1:9 anchor=null attributes={} literal="text ^abc" children=0
````````````````````````````````

## Scopes

Successful recognition removes the separating whitespace, caret, and
identifier from visible content; the owner's scope still covers the complete
authored construct including the identifier, child scopes end before an
inline suffix, and a detached line contributes to the owner's scope although
it produces no child. Allocation failure unwinds through the owner's path and
leaves no detached identifier.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover a table
with a caption, metadata-bearing callouts, nested lists, an owner whose
anchor another rule already set, a caret before superscript parsing, end of
document, exact scopes, allocation failure, and long identifier and
candidate sequences.
