# Superscript and subscript

Status: normative module of the [Markdown Core dialect](../dialect.md).
Options: `superscript` and `subscript` (each default `false`, independent).
Source: Pandoc's `superscript` and `subscript` extensions. Executable
oracle: the Pandoc 3.11 CLI under `specs/oracles/pandoc/`. Landing: `P6`,
which also removes the harness-only double-tilde strikethrough flag. Each
example in this module names the options it runs with; the
[example format](../dialect.md#examples) is defined by the index.

## Model

```text
Superscript(content: [Markup])
Subscript(content: [Markup])
```

Both are inline kinds whose content is parsed by the shared inline parser.

```````````````````````````````` example superscript subscript
2^10^ and H~2~O
.
Document scope=1:1..1:15 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:15 anchor=null attributes={} children=5
    ├── Text scope=1:1..1:1 anchor=null attributes={} literal="2" children=0
    ├── Superscript scope=1:2..1:5 anchor=null attributes={} children=1
    │   └── Text scope=1:3..1:4 anchor=null attributes={} literal="10" children=0
    ├── Text scope=1:6..1:11 anchor=null attributes={} literal=" and H" children=0
    ├── Subscript scope=1:12..1:14 anchor=null attributes={} children=1
    │   └── Text scope=1:13..1:13 anchor=null attributes={} literal="2" children=0
    └── Text scope=1:15..1:15 anchor=null attributes={} literal="O" children=0
````````````````````````````````

```````````````````````````````` example superscript
^*x*^
.
Document scope=1:1..1:5 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:5 anchor=null attributes={} children=1
    └── Superscript scope=1:1..1:5 anchor=null attributes={} children=1
        └── Emphasis scope=1:2..1:4 anchor=null attributes={} children=1
            └── Text scope=1:3..1:3 anchor=null attributes={} literal="x" children=0
````````````````````````````````

## Syntax

With `superscript=true`, every unescaped `^` is a delimiter unit at inline
step C4; with `subscript=true`, tildes are delimiter units at step C3 under
the tilde rule below. Units of one kind match by this procedure, applied left
to right within one inline container:

- A unit that finds an unmatched opener of its own kind on the stack closes
  it; otherwise it opens. Same-kind delimiters therefore never nest.
- When the scanner reaches an unescaped whitespace scalar or a line ending, or
  the end of the inline container, every unmatched opener of both kinds is
  removed and its byte is text. A body therefore never contains unescaped
  whitespace.
- Inside a body candidate, `\ ` (a backslash followed by an ASCII space) is
  not whitespace and yields U+00A0 NO-BREAK SPACE in the content; elsewhere
  the inherited literal applies.
- A body is non-empty: a closer immediately after its opener matches nothing,
  and both bytes are text.

```````````````````````````````` example superscript subscript
^a b^ ~a b~

P~a\ cat~
.
Document scope=1:1..3:9 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:11 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:11 anchor=null attributes={} literal="^a b^ ~a b~" children=0
└── Paragraph scope=3:1..3:9 anchor=null attributes={} children=2
    ├── Text scope=3:1..3:1 anchor=null attributes={} literal="P" children=0
    └── Subscript scope=3:2..3:9 anchor=null attributes={} children=1
        └── Text scope=3:3..3:8 anchor=null attributes={} literal="a cat" children=0
````````````````````````````````

A character reference that decodes to whitespace never invalidates a
candidate:

```````````````````````````````` example superscript
^a&#32;b^
.
Document scope=1:1..1:9 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:9 anchor=null attributes={} children=1
    └── Superscript scope=1:1..1:9 anchor=null attributes={} children=1
        └── Text scope=1:2..1:8 anchor=null attributes={} literal="a b" children=0
````````````````````````````````

An empty body is not a body. `^^` is text, and an unmatched `~~` run, which
the tilde rule turns into two subscript units, is therefore text as well:

```````````````````````````````` example superscript subscript
^^ a~~b
.
Document scope=1:1..1:7 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:7 anchor=null attributes={} children=1
    └── Text scope=1:1..1:7 anchor=null attributes={} literal="^^ a~~b" children=0
````````````````````````````````

An unmatched delimiter is text and cannot hide a later valid candidate:

```````````````````````````````` example superscript
x^y ^z
.
Document scope=1:1..1:6 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:6 anchor=null attributes={} children=1
    └── Text scope=1:1..1:6 anchor=null attributes={} literal="x^y ^z" children=0
````````````````````````````````

Because a unit closes whenever an opener of its kind is open, consecutive
pairs alternate:

```````````````````````````````` example superscript
^a^b^c^
.
Document scope=1:1..1:7 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:7 anchor=null attributes={} children=3
    ├── Superscript scope=1:1..1:3 anchor=null attributes={} children=1
    │   └── Text scope=1:2..1:2 anchor=null attributes={} literal="a" children=0
    ├── Text scope=1:4..1:4 anchor=null attributes={} literal="b" children=0
    └── Superscript scope=1:5..1:7 anchor=null attributes={} children=1
        └── Text scope=1:6..1:6 anchor=null attributes={} literal="c" children=0
````````````````````````````````

### Tildes

A run of one tilde is subscript syntax and never a strikethrough delimiter:
with `subscript` on it is a subscript unit, and with it off it is text. A run
of two tildes is a strikethrough delimiter, matched under the
[strikethrough](strikethrough.md) rules, and becomes two subscript units only
if it ends unmatched; runs of three or more are text. The
[conflicts](conflicts.md) register records this ruling.

```````````````````````````````` example subscript
~~a~~ ~b~

~~x~ ~~~y~~~
.
Document scope=1:1..3:12 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:9 anchor=null attributes={} children=3
│   ├── Strikethrough scope=1:1..1:5 anchor=null attributes={} children=1
│   │   └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
│   ├── Text scope=1:6..1:6 anchor=null attributes={} literal=" " children=0
│   └── Subscript scope=1:7..1:9 anchor=null attributes={} children=1
│       └── Text scope=1:8..1:8 anchor=null attributes={} literal="b" children=0
└── Paragraph scope=3:1..3:12 anchor=null attributes={} children=1
    └── Text scope=3:1..3:12 anchor=null attributes={} literal="~~x~ ~~~y~~~" children=0
````````````````````````````````

### Carets

An unescaped `^` immediately followed by `[` is an inline-footnote opener
under `inlineFootnotes` and `footnotes`, tested before this module:

```````````````````````````````` example superscript inline_footnotes
text^[note]
.
Document scope=1:1..1:11 anchor=null attributes={} children=1
├── Paragraph scope=1:1..1:11 anchor=null attributes={} children=2
│   ├── Text scope=1:1..1:4 anchor=null attributes={} literal="text" children=0
│   └── Cite scope=1:5..1:11 anchor=null attributes={} children=1
│       └── Citation scope=1:7..1:10 referent=footnote(id="inline-1") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
└── Footnote scope=1:5..1:11 id="inline-1" children=1
    └── Text scope=1:7..1:10 anchor=null attributes={} literal="note" children=0
````````````````````````````````

Without `inlineFootnotes`, the same bytes are a superscript whose content is
bracket text:

```````````````````````````````` example superscript
^[note]^
.
Document scope=1:1..1:8 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:8 anchor=null attributes={} children=1
    └── Superscript scope=1:1..1:8 anchor=null attributes={} children=1
        └── Text scope=1:2..1:7 anchor=null attributes={} literal="[note]" children=0
````````````````````````````````

A `^` removed by [block identifier](block-identifiers.md) attachment is never
a delimiter, because that attachment is decided before inline parsing. A `^`
or `~` owned by an autolink, code span, HTML token, comment, formula, or cross
link is opaque.

## Option behavior and fallback

With `superscript=false`, `^` is text; with `subscript=false`, a single
tilde is text and `~~` follows the strikethrough module alone:

```````````````````````````````` example
2^10^ H~2~O ~~x~~
.
Document scope=1:1..1:17 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:17 anchor=null attributes={} children=2
    ├── Text scope=1:1..1:12 anchor=null attributes={} literal="2^10^ H~2~O " children=0
    └── Strikethrough scope=1:13..1:17 anchor=null attributes={} children=1
        └── Text scope=1:15..1:15 anchor=null attributes={} literal="x" children=0
````````````````````````````````

Escaped delimiters are text:

```````````````````````````````` example superscript subscript
\^a\^ \~b\~
.
Document scope=1:1..1:11 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:11 anchor=null attributes={} children=1
    └── Text scope=1:1..1:11 anchor=null attributes={} literal="^a^ ~b~" children=0
````````````````````````````````

A failed candidate is text and cannot hide a later valid candidate.
Delimiter scalars enter a body only through the shared escape mechanism.

## Scopes

Both scopes cover the two delimiters and the body, including an escaped
space.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover adjacent
and intraword forms, Unicode whitespace and newlines inside candidates, a
caret removed by a block identifier, code, comments, HTML, formulas,
autolinks, and other inline nesting, exact scopes, each option independently
on and off, allocation failure, and adversarial caret and tilde runs.
