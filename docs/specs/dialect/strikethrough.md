# Strikethrough

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `strikethrough` (default `true`). Source: cmark-gfm's strikethrough
extension. Executable oracle: cmark-gfm under `specs/oracles/cmark-gfm/`.
Landing: present; the single-tilde interaction with `subscript` lands with
`P6`. Every example in this module runs with the product defaults unless its
fence says otherwise; the [example format](../dialect.md#examples) is
defined by the index.

## Model

```text
Strikethrough(content: [Markup])
```

`Strikethrough` is an inline kind whose content is parsed by the shared inline
parser.

## Syntax

Tildes are delimiters on the shared stack at inline steps C2 and C3. A run of
one or two unescaped tildes is one delimiter unit; a run of three or more is
text. A unit can open when it is left-flanking and close when it is
right-flanking under the CommonMark definitions. `~~a~~` and `~a~` are both
`Strikethrough("a")` while `subscript` is off:

```````````````````````````````` example
~~struck~~ and ~also~
.
Document scope=1:1..1:21 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:21 anchor=null attributes={} children=3
    ├── Strikethrough scope=1:1..1:10 anchor=null attributes={} children=1
    │   └── Text scope=1:3..1:8 anchor=null attributes={} literal="struck" children=0
    ├── Text scope=1:11..1:15 anchor=null attributes={} literal=" and " children=0
    └── Strikethrough scope=1:16..1:21 anchor=null attributes={} children=1
        └── Text scope=1:17..1:20 anchor=null attributes={} literal="also" children=0
````````````````````````````````

A closer matches the nearest unmatched opener of the same length; a
one-tilde unit and a two-tilde unit never match each other, and an unmatched
unit is text:

```````````````````````````````` example
~~a~ ~a~~
.
Document scope=1:1..1:9 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:9 anchor=null attributes={} children=1
    └── Text scope=1:1..1:9 anchor=null attributes={} literal="~~a~ ~a~~" children=0
````````````````````````````````

A run of three or more tildes is text and delimits nothing; a line that
begins with `~~~` is a code fence under the inherited grammar, so the example
starts with other text:

```````````````````````````````` example
x ~~~a~~~ ~~a~~~
.
Document scope=1:1..1:16 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:16 anchor=null attributes={} children=1
    └── Text scope=1:1..1:16 anchor=null attributes={} literal="x ~~~a~~~ ~~a~~~" children=0
````````````````````````````````

Matching uses the inherited process-emphasis algorithm without the rule of
three. `~` itself is transparent to the flanking tests of every delimiter:
when the scalar next to a delimiter run is a tilde, the test looks past the
tilde run to the next scalar, so `*~~d~~*` is `Emphasis(Strikethrough("d"))`:

```````````````````````````````` example
~~a *b* c~~ *~~d~~*
.
Document scope=1:1..1:19 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:19 anchor=null attributes={} children=3
    ├── Strikethrough scope=1:1..1:11 anchor=null attributes={} children=3
    │   ├── Text scope=1:3..1:4 anchor=null attributes={} literal="a " children=0
    │   ├── Emphasis scope=1:5..1:7 anchor=null attributes={} children=1
    │   │   └── Text scope=1:6..1:6 anchor=null attributes={} literal="b" children=0
    │   └── Text scope=1:8..1:9 anchor=null attributes={} literal=" c" children=0
    ├── Text scope=1:12..1:12 anchor=null attributes={} literal=" " children=0
    └── Emphasis scope=1:13..1:19 anchor=null attributes={} children=1
        └── Strikethrough scope=1:14..1:18 anchor=null attributes={} children=1
            └── Text scope=1:16..1:16 anchor=null attributes={} literal="d" children=0
````````````````````````````````

Intraword pairs are allowed:

```````````````````````````````` example
a~~b~~c
.
Document scope=1:1..1:7 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:7 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:1 anchor=null attributes={} literal="a" children=0
    ├── Strikethrough scope=1:2..1:6 anchor=null attributes={} children=1
    │   └── Text scope=1:4..1:4 anchor=null attributes={} literal="b" children=0
    └── Text scope=1:7..1:7 anchor=null attributes={} literal="c" children=0
````````````````````````````````

An escaped `\~` never delimits:

```````````````````````````````` example
\~~a~~ ~~a\~~
.
Document scope=1:1..1:13 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:13 anchor=null attributes={} children=1
    └── Text scope=1:1..1:13 anchor=null attributes={} literal="~~a~~ ~~a~~" children=0
````````````````````````````````

Code spans, HTML tokens, comments, formulas, and cross links are opaque:

```````````````````````````````` example
`~~a~~` $~~b~~$
.
Document scope=1:1..1:15 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:15 anchor=null attributes={} children=3
    ├── Code scope=1:1..1:7 anchor=null attributes={} literal="~~a~~" children=0
    ├── Text scope=1:8..1:8 anchor=null attributes={} literal=" " children=0
    └── Formula scope=1:9..1:15 anchor=null attributes={} mode=embedded literal="~~b~~" children=0
````````````````````````````````

## Interaction with `subscript`

With `subscript` on, the
[superscript and subscript](superscript-and-subscript.md) module owns single
tildes: a run of one tilde is never a strikethrough
delimiter, a run of two is strikethrough when matched under the rules above
and otherwise two subscript units, and runs of three or more stay text. This
is the one place where two sources define the same bytes differently; the
[conflicts](conflicts.md) register records it, and that module's examples
show the result.

## Option behavior and fallback

With `strikethrough=false`, every tilde is text, and with `subscript` also
off nothing recognizes a tilde:

```````````````````````````````` example !strikethrough
~~a~~
.
Document scope=1:1..1:5 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:5 anchor=null attributes={} children=1
    └── Text scope=1:1..1:5 anchor=null attributes={} literal="~~a~~" children=0
````````````````````````````````

## Scopes

`Strikethrough.scope` covers both tilde runs and the body.

## Required conformance cases

Every example of this module is a package fixture, and the GFM strikethrough
sections of the specification corpus and the repository's extension fixtures
remain byte-identical. Tests also cover tildes beside every other delimiter,
HTML tokens, comments, and cross links as opaque contexts, exact scopes,
every combination with `subscript`, allocation failure, and size-doubling
tilde runs.
