# Superscript and subscript

Status: normative module of the [Markdown Core dialect](../dialect.md).
Source: Pandoc's `superscript` and `subscript` extensions. Executable
oracle: the Pandoc 3.11 CLI under `specs/oracles/pandoc/`. Landing: `P6`,
which also removes the engine's unused double-tilde strikethrough flag. The
[example format](../dialect.md#examples) is defined by the index.

## Model

```text
Superscript(content: [Markup])
Subscript(content: [Markup])
```

Both are inline kinds whose content is parsed by the shared inline parser.

```````````````````````````````` example
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

```````````````````````````````` example
^*x*^
.
Document scope=1:1..1:5 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:5 anchor=null attributes={} children=1
    └── Superscript scope=1:1..1:5 anchor=null attributes={} children=1
        └── Emphasis scope=1:2..1:4 anchor=null attributes={} children=1
            └── Text scope=1:3..1:3 anchor=null attributes={} literal="x" children=0
````````````````````````````````

## Syntax

Every unescaped `^` is a delimiter unit at inline step C4, and tildes are
delimiter units at step C3 under the tilde rule below. Units of one kind match by this procedure, applied left
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

```````````````````````````````` example
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

```````````````````````````````` example
^a&#32;b^
.
Document scope=1:1..1:9 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:9 anchor=null attributes={} children=1
    └── Superscript scope=1:1..1:9 anchor=null attributes={} children=1
        └── Text scope=1:2..1:8 anchor=null attributes={} literal="a b" children=0
````````````````````````````````

An empty body is not a body. `^^` is text, and an unmatched `~~` run is text
under the tilde rule rather than two subscript units:

```````````````````````````````` example
^^ a~~b
.
Document scope=1:1..1:7 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:7 anchor=null attributes={} children=1
    └── Text scope=1:1..1:7 anchor=null attributes={} literal="^^ a~~b" children=0
````````````````````````````````

An unmatched delimiter is text and cannot hide a later valid candidate:

```````````````````````````````` example
x^y ^z
.
Document scope=1:1..1:6 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:6 anchor=null attributes={} children=1
    └── Text scope=1:1..1:6 anchor=null attributes={} literal="x^y ^z" children=0
````````````````````````````````

Because a unit closes whenever an opener of its kind is open, consecutive
pairs alternate:

```````````````````````````````` example
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

A run of one tilde is a subscript unit and never a strikethrough delimiter.
A run of two tildes is never subscript syntax: it is a strikethrough
delimiter unit matched under the [strikethrough](strikethrough.md) rules, so
an unmatched double run is text; runs of three or more are text. The
[conflicts](conflicts.md) register records this ruling.

```````````````````````````````` example
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

An unescaped `^` immediately followed by `[` is an inline-footnote opener,
tested before this module:

```````````````````````````````` example
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

A bracket enters a superscript body only through the escape mechanism:

```````````````````````````````` example
^\[note]^
.
Document scope=1:1..1:9 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:9 anchor=null attributes={} children=1
    └── Superscript scope=1:1..1:9 anchor=null attributes={} children=1
        └── Text scope=1:2..1:8 anchor=null attributes={} literal="[note]" children=0
````````````````````````````````

A `^` removed by [block identifier](block-identifiers.md) attachment is never
a delimiter, because that attachment is decided before inline parsing. A `^`
or `~` owned by an autolink, code span, HTML token, comment, formula, or cross
link is opaque.

## Fallback

Escaped delimiters are text:

```````````````````````````````` example
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
autolinks, and other inline nesting, exact scopes, allocation failure, and adversarial caret and tilde runs.
