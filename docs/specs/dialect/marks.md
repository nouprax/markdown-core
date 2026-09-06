# Marks

Status: normative module of the [Markdown Core dialect](../dialect.md).
Source: Obsidian's `==highlight==`. Executable oracle:
`@quartz-community/remark-obsidian`, whose one-text-child content model is a
registered projection. Landing: `O2`. The
[example format](../dialect.md#examples) is defined by the index.

## Model

```text
Mark(content: [Markup])
```

`Mark` is an inline kind whose content is parsed by the shared inline parser
and may contain any inline construct whose delimiters nest legally inside it.

## Syntax

`=` is a delimiter character on the shared stack at inline step C5. A
candidate is a maximal run of two or more unescaped `=`; a run of one is
text. A candidate can open if and only if it is left-flanking and can close
if and only if it is right-flanking, under the CommonMark definitions with
the same character classes as `*`. Matching uses the inherited
process-emphasis algorithm without the rule of three, and a match consumes
exactly two `=` from the end of the opener and two from the start of the
closer, as a `**` match does. What remains of a run keeps its flanking, so a
run of four can close one mark and open the next, while a remaining single
`=` matches nothing more and is text before or after the mark.

```````````````````````````````` example
This is ==important== text.
.
Document scope=1:1..1:27 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:27 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:8 anchor=null attributes={} literal="This is " children=0
    ├── Mark scope=1:9..1:21 anchor=null attributes={} children=1
    │   └── Text scope=1:11..1:19 anchor=null attributes={} literal="important" children=0
    └── Text scope=1:22..1:27 anchor=null attributes={} literal=" text." children=0
````````````````````````````````

The content is ordinary inline content:

```````````````````````````````` example
==a *b* c==
.
Document scope=1:1..1:11 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:11 anchor=null attributes={} children=1
    └── Mark scope=1:1..1:11 anchor=null attributes={} children=3
        ├── Text scope=1:3..1:4 anchor=null attributes={} literal="a " children=0
        ├── Emphasis scope=1:5..1:7 anchor=null attributes={} children=1
        │   └── Text scope=1:6..1:6 anchor=null attributes={} literal="b" children=0
        └── Text scope=1:8..1:9 anchor=null attributes={} literal=" c" children=0
````````````````````````````````

Intraword pairs are allowed, and a run of four closes one mark and opens
the next, so adjacent marks are separate nodes:

```````````````````````````````` example
a==b==c ==d====e==
.
Document scope=1:1..1:18 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:18 anchor=null attributes={} children=5
    ├── Text scope=1:1..1:1 anchor=null attributes={} literal="a" children=0
    ├── Mark scope=1:2..1:6 anchor=null attributes={} children=1
    │   └── Text scope=1:4..1:4 anchor=null attributes={} literal="b" children=0
    ├── Text scope=1:7..1:8 anchor=null attributes={} literal="c " children=0
    ├── Mark scope=1:9..1:13 anchor=null attributes={} children=1
    │   └── Text scope=1:11..1:11 anchor=null attributes={} literal="d" children=0
    └── Mark scope=1:14..1:18 anchor=null attributes={} children=1
        └── Text scope=1:16..1:16 anchor=null attributes={} literal="e" children=0
````````````````````````````````

A run of one is text. A run of three matches two of its signs and leaves
the third as text outside the mark, and a run that is neither left- nor
right-flanking is text:

```````````````````````````````` example
==a===b==

=a= ===a=== ====
.
Document scope=1:1..3:16 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:9 anchor=null attributes={} children=2
│   ├── Mark scope=1:1..1:5 anchor=null attributes={} children=1
│   │   └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
│   └── Text scope=1:6..1:9 anchor=null attributes={} literal="=b==" children=0
└── Paragraph scope=3:1..3:16 anchor=null attributes={} children=3
    ├── Text scope=3:1..3:5 anchor=null attributes={} literal="=a= =" children=0
    ├── Mark scope=3:6..3:10 anchor=null attributes={} children=1
    │   └── Text scope=3:8..3:8 anchor=null attributes={} literal="a" children=0
    └── Text scope=3:11..3:16 anchor=null attributes={} literal="= ====" children=0
````````````````````````````````

A closer matches the nearest unmatched opener, and a run that finds no
opener is text:

```````````````````````````````` example
==a==b==
.
Document scope=1:1..1:8 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:8 anchor=null attributes={} children=2
    ├── Mark scope=1:1..1:5 anchor=null attributes={} children=1
    │   └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
    └── Text scope=1:6..1:8 anchor=null attributes={} literal="b==" children=0
````````````````````````````````

A run with whitespace on both sides is neither left- nor right-flanking and
cannot open, so a comparison operator contains no mark:

```````````````````````````````` example
if a == b and c == d
.
Document scope=1:1..1:20 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:20 anchor=null attributes={} children=1
    └── Text scope=1:1..1:20 anchor=null attributes={} literal="if a == b and c == d" children=0
````````````````````````````````

An escaped `\=` never delimits, and an unmatched candidate is text:

```````````````````````````````` example
\==a== ==a\==
.
Document scope=1:1..1:13 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:13 anchor=null attributes={} children=1
    └── Text scope=1:1..1:13 anchor=null attributes={} literal="==a== ==a==" children=0
````````````````````````````````

Validity is decided on source. A comment is an earlier class-A step, so a
mark whose content is one `Comment` is a mark:

```````````````````````````````` example
==%%c%%==
.
Document scope=1:1..1:9 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:9 anchor=null attributes={} children=1
    └── Mark scope=1:1..1:9 anchor=null attributes={} children=1
        └── Comment scope=1:3..1:7 anchor=null attributes={} literal="c" children=0
````````````````````````````````

Block structure is decided first, so a Setext underline of `=` is never a
closer:

```````````````````````````````` example
==text
==
.
Document scope=1:1..2:2 anchor=null attributes={} children=1
└── Heading scope=1:1..2:2 anchor="text" attributes={} level=1 children=1
    └── Text scope=1:1..1:6 anchor=null attributes={} literal="==text" children=0
````````````````````````````````

A bare URL autolink is an earlier scanner step whose run is opaque, so `==`
inside a URL is URL text and delimits nothing:

```````````````````````````````` example
http://x/?a==b== c
.
Document scope=1:1..1:18 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:18 anchor=null attributes={} children=2
    ├── Link scope=1:1..1:16 anchor=null attributes={} dest=url("http://x/?a==b==") title=null children=1
    │   └── Text scope=1:1..1:16 anchor=null attributes={} literal="http://x/?a==b==" children=0
    └── Text scope=1:17..1:18 anchor=null attributes={} literal=" c" children=0
````````````````````````````````

Code spans, HTML tokens, comments, formulas, and cross links are opaque:

```````````````````````````````` example
`==a==` $==b==$
.
Document scope=1:1..1:15 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:15 anchor=null attributes={} children=3
    ├── Code scope=1:1..1:7 anchor=null attributes={} literal="==a==" children=0
    ├── Text scope=1:8..1:8 anchor=null attributes={} literal=" " children=0
    └── Formula scope=1:9..1:15 anchor=null attributes={} mode=embedded literal="==b==" children=0
````````````````````````````````

A mark may occur in any inline content, headings included:

```````````````````````````````` example
## ==a== b
.
Document scope=1:1..1:10 anchor=null attributes={} children=1
└── Heading scope=1:1..1:10 anchor="a-b" attributes={} level=2 children=2
    ├── Mark scope=1:4..1:8 anchor=null attributes={} children=1
    │   └── Text scope=1:6..1:6 anchor=null attributes={} literal="a" children=0
    └── Text scope=1:9..1:10 anchor=null attributes={} literal=" b" children=0
````````````````````````````````

## Fallback

A failed pair cannot consume equals signs needed by a
later valid pair. Pandoc's `mark` extension delimits the same bytes with a
different boundary rule; the [conflicts](conflicts.md) register records the
ruling that the flanking rule stands.

## Scopes

`Mark.scope` covers both delimiter runs and the body.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover marks
inside table cells, callout titles, and footnote content, HTML tokens and
cross links as opaque contexts, exact scopes, allocation failure, deep
mixed-delimiter input, and size-doubling equals runs.
