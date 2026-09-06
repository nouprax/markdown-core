# Formulas

Status: normative module of the [Markdown Core dialect](../dialect.md).
Source: GitHub's mathematical-expression
syntax and `micromark-extension-math` 3.1.0, whose padding rule this module
adopts. Executable oracle: remark with `micromark-extension-math` under
`specs/oracles/remark/`, for the `$` forms only; the backslash forms and the
GitHub heuristics are product fixtures and registered deltas. Landing:
present. The [example format](../dialect.md#examples) is defined by the
index.

The module states the grammar the parser implements in
`packages/markdown-core/extensions/formula.c`; the fixtures
`extensions-formula-github.txt`, `extensions-formula-latex.txt`,
and `extensions-formula-conflicts.txt` are its oracle of record.

## Model

```text
Formula(mode: embedded | standalone, literal: String)
FormulaBlock(literal: String)
```

`Formula` is an inline leaf; `mode` is the only placement field in the AST,
because it records a fact about the source. `FormulaBlock` is a block leaf and
is always standalone. Both literals are opaque strings: the parser validates no
TeX, decodes no escape or character reference inside a body, and runs no
renderer.

## Inline forms

Inline recognition is step A4 of the recognition order. Five source forms
exist; every one is opened at its opening delimiter during the scan, so its
body is opaque to every later step, including emphasis, links, and every other
module.

| Form           | Delimiters                     | Result                         |
| -------------- | ------------------------------ | ------------------------------ |
| dollar         | `$` ... `$`                    | `Formula(mode=embedded)`       |
| backtick       | `` $` `` ... `` `$ ``          | `Formula(mode=embedded)`       |
| display dollar | `$$` ... `$$`                  | `Formula(mode=standalone)`     |
| paren          | `\\(` ... `\\)`                | `Formula(mode=embedded)`       |
| bracket        | `\\[` ... `\\]`                | `Formula(mode=standalone)`     |

```````````````````````````````` example
The area is $\pi r^2$.
.
Document scope=1:1..1:22 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:22 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:12 anchor=null attributes={} literal="The area is " children=0
    ├── Formula scope=1:13..1:21 anchor=null attributes={} mode=embedded literal="\\pi r^2" children=0
    └── Text scope=1:22..1:22 anchor=null attributes={} literal="." children=0
````````````````````````````````

`$$` is tested before `$`. A `$$` run opens and closes a display candidate
unconditionally, and a display formula with other content in its paragraph
stays an inline `Formula` with `mode=standalone`:

```````````````````````````````` example
x $$a+b$$ y
.
Document scope=1:1..1:11 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:11 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:2 anchor=null attributes={} literal="x " children=0
    ├── Formula scope=1:3..1:9 anchor=null attributes={} mode=standalone literal="a+b" children=0
    └── Text scope=1:10..1:11 anchor=null attributes={} literal=" y" children=0
````````````````````````````````

A paragraph whose sole inline child is a standalone `Formula` is replaced by
a `FormulaBlock` with the same literal and the paragraph's scope. This is the
one post-pass of the module; it is bounded to paragraphs with exactly one
child and never changes a paragraph with other content:

```````````````````````````````` example
$$a+b$$
.
Document scope=1:1..1:7 anchor=null attributes={} children=1
└── FormulaBlock scope=1:1..1:7 anchor=null attributes={} literal="a+b" children=0
````````````````````````````````

A single `$` can open only when the next byte exists and is not ASCII
whitespace, and can close only when the previous byte is not ASCII whitespace
and the next byte, if any, is not an ASCII digit, so prices contain no
formula:

```````````````````````````````` example
$300B and $100B

$ x$ and $x $
.
Document scope=1:1..3:13 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:15 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:15 anchor=null attributes={} literal="$300B and $100B" children=0
└── Paragraph scope=3:1..3:13 anchor=null attributes={} children=1
    └── Text scope=3:1..3:13 anchor=null attributes={} literal="$ x$ and $x $" children=0
````````````````````````````````

The backtick form is the dollar form whose body begins with a backtick: a
matched `$`...`$` pair whose body starts with `` ` `` must end with `` ` ``,
and the two backticks are removed from the literal. A body that starts with a
backtick and does not end with one is not a formula, and the pair is released:

```````````````````````````````` example
$`a|b`$ and $`c$
.
Document scope=1:1..1:16 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:16 anchor=null attributes={} children=2
    ├── Formula scope=1:1..1:7 anchor=null attributes={} mode=embedded literal="a|b" children=0
    └── Text scope=1:8..1:16 anchor=null attributes={} literal=" and $`c$" children=0
````````````````````````````````

The paren and bracket forms are spelled with two authored backslashes. `\\(`
and `\\[` can only open; `\\)` and `\\]` can only close; an opener and closer
match only when they are of the same form. A single-backslash `\(` or `\[` is
an ordinary CommonMark escape and never a formula delimiter:

```````````````````````````````` example
\\(x\\) \\[y\\] \(z\)
.
Document scope=1:1..1:21 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:21 anchor=null attributes={} children=4
    ├── Formula scope=1:1..1:7 anchor=null attributes={} mode=embedded literal="x" children=0
    ├── Text scope=1:8..1:8 anchor=null attributes={} literal=" " children=0
    ├── Formula scope=1:9..1:15 anchor=null attributes={} mode=standalone literal="y" children=0
    └── Text scope=1:16..1:21 anchor=null attributes={} literal=" (z)" children=0
````````````````````````````````

Inside a paren or bracket body, `\)` respectively `\]` spelled with one
backslash is unescaped to the bare bracket in the literal; every other byte is
stored as written.

Padding: if the body begins and ends with a space, LF, or CR and is not made
entirely of those bytes, one such byte is removed from each end; otherwise the
body is stored as written. Tabs are not padding:

```````````````````````````````` example
a $$ mid$$ b $$  x  $$ c $$ $$ d
.
Document scope=1:1..1:32 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:32 anchor=null attributes={} children=7
    ├── Text scope=1:1..1:2 anchor=null attributes={} literal="a " children=0
    ├── Formula scope=1:3..1:10 anchor=null attributes={} mode=standalone literal=" mid" children=0
    ├── Text scope=1:11..1:13 anchor=null attributes={} literal=" b " children=0
    ├── Formula scope=1:14..1:22 anchor=null attributes={} mode=standalone literal=" x " children=0
    ├── Text scope=1:23..1:25 anchor=null attributes={} literal=" c " children=0
    ├── Formula scope=1:26..1:30 anchor=null attributes={} mode=standalone literal=" " children=0
    └── Text scope=1:31..1:32 anchor=null attributes={} literal=" d" children=0
````````````````````````````````

An escaped `\$` is text, and an unmatched delimiter is text:

```````````````````````````````` example
\$5 and $a
.
Document scope=1:1..1:10 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:10 anchor=null attributes={} children=1
    └── Text scope=1:1..1:10 anchor=null attributes={} literal="$5 and $a" children=0
````````````````````````````````

Delimiters are units on the shared delimiter stack; a closer matches the
nearest unmatched opener of its own form. One formula of a form is open at a
time: while an opener of a form is unmatched, a further opener of that form is
the body's own bytes, and a closer of a form is a delimiter only while an
opener of that form is unmatched, so a body runs from its opener to the first
closer of its form. A body may span soft line breaks within one inline
container but not a block boundary:

```````````````````````````````` example
$a
b$
.
Document scope=1:1..2:2 anchor=null attributes={} children=1
└── Paragraph scope=1:1..2:2 anchor=null attributes={} children=1
    └── Formula scope=1:1..2:2 anchor=null attributes={} mode=embedded literal="a\nb" children=0
````````````````````````````````

## Block forms

Block recognition is step 3 of the block-start order. A line whose content,
after container prefixes and up to three spaces of indentation, is exactly
`$$` or `\\[` followed only by spaces or tabs opens a `FormulaBlock`. The
block may interrupt a paragraph. It is closed by the first later line whose
content, under the same prefixes and indentation bound, is exactly the
matching `$$` or `\\]` followed only by spaces or tabs; the lines between are
the literal with leading and trailing ASCII whitespace removed and interior
line endings kept as written:

```````````````````````````````` example
$$
a + b
$$

\\[
c
\\]
.
Document scope=1:1..7:3 anchor=null attributes={} children=2
├── FormulaBlock scope=1:1..3:2 anchor=null attributes={} literal="a + b" children=0
└── FormulaBlock scope=5:1..7:3 anchor=null attributes={} literal="c" children=0
````````````````````````````````

A block that reaches the end of its container or of the document without a
closer is still a `FormulaBlock` holding every line after the opener:

```````````````````````````````` example
> $$
> a

b
.
Document scope=1:1..4:1 anchor=null attributes={} children=2
├── Callout scope=1:1..2:3 anchor=null attributes={} variant=null fold=none children=1
│   └── FormulaBlock scope=1:3..2:3 anchor=null attributes={} literal="a" children=0
└── Paragraph scope=4:1..4:1 anchor=null attributes={} children=1
    └── Text scope=4:1..4:1 anchor=null attributes={} literal="b" children=0
````````````````````````````````

A fenced code block whose `info` is exactly `formula` produces a
`FormulaBlock` whose literal is the code block's literal, trimmed of leading
and trailing ASCII whitespace; the code block's other fields are discarded:

```````````````````````````````` example
```formula
x
```
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── FormulaBlock scope=1:1..3:3 anchor=null attributes={} literal="x" children=0
````````````````````````````````

## Fallback

Formula bodies are opaque under the shared opacity rule, and formula
delimiters inside code spans, HTML tokens, comments, and cross links are those
constructs' own bytes:

```````````````````````````````` example
`$a$` <!-- $b$ -->
.
Document scope=1:1..1:18 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:18 anchor=null attributes={} children=3
    ├── Code scope=1:1..1:5 anchor=null attributes={} literal="$a$" children=0
    ├── Text scope=1:6..1:6 anchor=null attributes={} literal=" " children=0
    └── Comment scope=1:7..1:18 anchor=null attributes={} literal=" $b$ " children=0
````````````````````````````````

A candidate that fails any rule above releases its bytes
as text and consumes nothing that a later construct needs.

## Scopes

An inline `Formula` scope covers both delimiter runs and the body, including
padding bytes that were removed from the literal. A `FormulaBlock` scope covers
the opening line through the closing line, or through the last consumed line
when unclosed; a block produced from a `formula` fence or from a sole
standalone inline keeps the replaced node's scope.

## Required conformance cases

Every example of this module is a package fixture, and every fixture row of
the four formula fixture files stays byte-identical. Tests also cover
single-backslash brackets inside paren and bracket bodies, padding on every
form, block forms with closers inside containers, formula delimiters inside
every opaque construct, exact scopes, allocation failure, and size-doubling
runs of `$` and `\`.
