# Base language

Status: normative module of the [Markdown Core dialect](../dialect.md). This
module states what the dialect inherits and the few rules the inherited layer
leaves to the implementation. Source: the CommonMark specification 0.31.2.
Executable oracle: cmark 0.31.2 under `specs/oracles/cmark/`, compared with
the one language over cmark's own specification inputs. Landing: present; the
HTML-comment rule lands with `M0`, the resolved-reference model with `M1` and
`M2`, and `Callout` with `M3`; smart punctuation was removed by `X0`. The
[example format](../dialect.md#examples) is defined by the index.

## The inherited layer

The base of the dialect is CommonMark 0.31.2 as implemented by the pinned
cmark, with the deltas registered in `specs/oracles/cmark/deltas.json`. Every
block and inline construct of that specification is recognized exactly as it
defines: block structure, container prefixes, laziness, tabs, indentation,
Setext and ATX headings, thematic breaks, fenced and indented code, HTML
blocks, link reference definitions, paragraphs, backslash escapes, character
references, code spans, emphasis and strong emphasis, links, images,
angle-bracket autolinks, raw HTML, and hard and soft line breaks. The
CommonMark specification's own examples are the grammar of this layer and are
not restated here; the examples below fix only what that specification leaves
to the implementation.

On top of it sit the GFM extensions, each in its own module with the pinned
cmark-gfm as executable oracle: [pipe tables](tables.md),
[strikethrough](strikethrough.md), [bare autolinks](links-and-images.md),
[task lists](task-lists.md), and [footnotes](footnotes.md). GFM is one source
and one oracle, cmark-gfm at its pinned commit minus the registered deltas;
the GFM specification text is not a second authority over it.

"Inherited" in every module means the output of this layer: the CommonMark
parse of the source, which is the meaning of every byte that no feature
claims. The dialect has no options, so no source ever parses with a feature
off, and no gate does either: the cmark oracle is compared with the one
language over the inputs it judges, and each place the dialect leaves
CommonMark is registered against those inputs in `specs/oracles/cmark/`.

The engine's `MARKDOWN_CORE_OPT_LIBERAL_HTML_TAG` and
`MARKDOWN_CORE_OPT_STRIKETHROUGH_DOUBLE_TILDE` bits are set by nothing and
have no dialect meaning; the second is removed by `P6`.

## Kinds of the base language

The base language produces these kinds of
[`canonical-ast.md`](../canonical-ast.md) without any dialect extension:
`Document`, `Paragraph`, `Heading`, `ThematicBreak`, `List` and `ListItem`,
`CodeBlock`, `HTMLBlock`, `Comment`, `Text`, `SoftBreak`, `LineBreak`,
`Code`, `HTML`, `Emphasis`, `Strong`, `Link`, `Image`, and `Callout`. The
rules below fix what the CommonMark specification leaves to the
implementation.

An empty document is a `Document` with no content; its scope is the
[coordinate contract's](../canonical-ast.md#coordinates) empty range:

```````````````````````````````` example

.
Document scope=1:1..1:0 anchor=null attributes={} children=0
````````````````````````````````

### Text

Adjacent `Text` nodes in one content array are merged into one node whose
scope runs from the first's start to the last's end, so a `children` count is
never undetermined by escape or character-reference boundaries. A `Text` node
is never empty:

```````````````````````````````` example
a\*b &amp; c
.
Document scope=1:1..1:12 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:12 anchor=null attributes={} children=1
    └── Text scope=1:1..1:12 anchor=null attributes={} literal="a*b & c" children=0
````````````````````````````````

### Line breaks

`SoftBreak` covers the line-ending bytes of a soft break. `LineBreak` covers
the line-ending bytes together with the backslash that produced it; for a
break produced by two or more trailing spaces, the spaces stay inside the
preceding `Text` node's scope and outside its literal, and `LineBreak` covers
the line ending alone. Neither node's literal is stored; a consumer that needs
the bytes reads the scope:

```````````````````````````````` example
soft
break
hard\
break
spaces  
end
.
Document scope=1:1..6:3 anchor=null attributes={} children=1
└── Paragraph scope=1:1..6:3 anchor=null attributes={} children=11
    ├── Text scope=1:1..1:4 anchor=null attributes={} literal="soft" children=0
    ├── SoftBreak scope=1:5..1:5 anchor=null attributes={} children=0
    ├── Text scope=2:1..2:5 anchor=null attributes={} literal="break" children=0
    ├── SoftBreak scope=2:6..2:6 anchor=null attributes={} children=0
    ├── Text scope=3:1..3:4 anchor=null attributes={} literal="hard" children=0
    ├── LineBreak scope=3:5..3:6 anchor=null attributes={} children=0
    ├── Text scope=4:1..4:5 anchor=null attributes={} literal="break" children=0
    ├── SoftBreak scope=4:6..4:6 anchor=null attributes={} children=0
    ├── Text scope=5:1..5:8 anchor=null attributes={} literal="spaces" children=0
    ├── LineBreak scope=5:9..5:9 anchor=null attributes={} children=0
    └── Text scope=6:1..6:3 anchor=null attributes={} literal="end" children=0
````````````````````````````````

### Code

`CodeBlock.literal` holds the block's content bytes as written, with the
inherited indentation removal and no other transformation. `fenced` is `true`
for a fenced block and `false` for an indented one. `closed` is `true` if and
only if a closing fence line was found; an indented block is always `closed`.

`CodeBlock.info` is the fence's info string after CommonMark backslash-escape
and character-reference processing and after stripping leading and trailing
spaces and tabs, with no other transformation. `info` is `null` for an
indented block and for a fence whose info string is empty after stripping.
`CodeBlock.language` is the maximal prefix of `info` before the first space or
tab, or `null` when `info` is `null`. Nothing is lowercased, aliased, or
derived from a class: `c++` is the language `c++`:

```````````````````````````````` example
```  python
print()
```

~~~ c++ extra
x
.
Document scope=1:1..6:1 anchor=null attributes={} children=2
├── CodeBlock scope=1:1..3:3 anchor=null attributes={} info="python" language="python" literal="print()\n" fenced=true closed=true children=0
└── CodeBlock scope=5:1..6:1 anchor=null attributes={} info="c++ extra" language="c++" literal="x\n" fenced=true closed=false children=0
````````````````````````````````

```````````````````````````````` example
    code

    more
.
Document scope=1:1..3:8 anchor=null attributes={} children=1
└── CodeBlock scope=1:5..3:8 anchor=null attributes={} info=null language=null literal="code\n\nmore\n" fenced=false closed=true children=0
````````````````````````````````

A fence whose `info` is exactly `formula` produces a `FormulaBlock` instead,
as the [formulas](formulas.md) module states, and the
[attributes](attributes.md) module removes an attribute container from the
info region before this rule computes `info`.

`Code.literal` is the code span's content after the inherited stripping and
line-ending-to-space conversion:

```````````````````````````````` example
`` a ` b `` and ` c `
.
Document scope=1:1..1:21 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:21 anchor=null attributes={} children=3
    ├── Code scope=1:1..1:11 anchor=null attributes={} literal="a ` b" children=0
    ├── Text scope=1:12..1:16 anchor=null attributes={} literal=" and " children=0
    └── Code scope=1:17..1:21 anchor=null attributes={} literal="c" children=0
````````````````````````````````

A backtick string longer than the 80-backtick [limit](../dialect.md#limits)
is never a code span delimiter and is text, as in cmark, whose reference
parser has the same ceiling:

```````````````````````````````` example
````````````````````````````````````````````````````````````````````````````````a````````````````````````````````````````````````````````````````````````````````

`````````````````````````````````````````````````````````````````````````````````b`````````````````````````````````````````````````````````````````````````````````
.
Document scope=1:1..3:163 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:161 anchor=null attributes={} children=1
│   └── Code scope=1:1..1:161 anchor=null attributes={} literal="a" children=0
└── Paragraph scope=3:1..3:163 anchor=null attributes={} children=1
    └── Text scope=3:1..3:163 anchor=null attributes={} literal="`````````````````````````````````````````````````````````````````````````````````b`````````````````````````````````````````````````````````````````````````````````" children=0
````````````````````````````````

### HTML

An inline raw HTML token is an `HTML` leaf holding the token bytes as
written, except that an HTML comment token is a `Comment`, as the
[comments](comments.md) module states. An HTML block is an `HTMLBlock` holding
its lines as written, except that an HTML block that opens with `<!--` and
whose end line holds only whitespace after the first `-->` is a block
`Comment`. Paired opening and closing tags do not establish an element region:
`<span>**b**</span>` is `HTML`, `Strong`, and `HTML` siblings, and every
module recognizes its syntax between separate HTML tokens:

```````````````````````````````` example
<div>
**a**
</div>

<span>**b**</span> <!-- c -->
.
Document scope=1:1..5:29 anchor=null attributes={} children=2
├── HTMLBlock scope=1:1..3:6 anchor=null attributes={} literal="<div>\n**a**\n</div>\n" children=0
└── Paragraph scope=5:1..5:29 anchor=null attributes={} children=5
    ├── HTML scope=5:1..5:6 anchor=null attributes={} literal="<span>" children=0
    ├── Strong scope=5:7..5:11 anchor=null attributes={} children=1
    │   └── Text scope=5:9..5:9 anchor=null attributes={} literal="b" children=0
    ├── HTML scope=5:12..5:18 anchor=null attributes={} literal="</span>" children=0
    ├── Text scope=5:19..5:19 anchor=null attributes={} literal=" " children=0
    └── Comment scope=5:20..5:29 anchor=null attributes={} literal=" c " children=0
````````````````````````````````

HTML declarations, processing instructions, CDATA sections, and malformed
tags follow the inherited grammar and are `HTML` or `HTMLBlock`.

### Headings

`Heading.content` is the inline content of the heading after the inherited
removal of the ATX opening and closing sequences and of the Setext underline.
A heading's scope covers those sequences and the underline, as the
[index](../dialect.md#scopes) requires of every delimiter, and so the whole of
an ATX heading's line, trailing spaces included, the way a paragraph's scope
covers the whole of its last line:

```````````````````````````````` example
# ATX #

### `code` *em*

Setext
------
.
Document scope=1:1..6:6 anchor=null attributes={} children=3
├── Heading scope=1:1..1:7 anchor="atx" attributes={} level=1 children=1
│   └── Text scope=1:3..1:5 anchor=null attributes={} literal="ATX" children=0
├── Heading scope=3:1..3:15 anchor="code-em" attributes={} level=3 children=3
│   ├── Code scope=3:5..3:10 anchor=null attributes={} literal="code" children=0
│   ├── Text scope=3:11..3:11 anchor=null attributes={} literal=" " children=0
│   └── Emphasis scope=3:12..3:15 anchor=null attributes={} children=1
│       └── Text scope=3:13..3:14 anchor=null attributes={} literal="em" children=0
└── Heading scope=5:1..6:6 anchor="setext" attributes={} level=2 children=1
    └── Text scope=5:1..5:6 anchor=null attributes={} literal="Setext" children=0
````````````````````````````````

An attribute container at the end of the heading is removed first, as the
[attributes](attributes.md) module states. A
heading's `anchor` is `null` unless an explicit or automatic rule of the
[anchors](anchors.md) module populates it.

### Callouts

Every `>` container is a `Callout` with `variant=null`, `fold=none`, and no
title; the [callouts](callouts.md) module owns that kind. The inherited
prefix, laziness, continuation, and blank-line rules are unchanged by the
metadata rule:

```````````````````````````````` example
> quoted
> lazy
continued

> > nested
.
Document scope=1:1..5:10 anchor=null attributes={} children=2
├── Callout scope=1:1..3:9 anchor=null attributes={} variant=null fold=none children=1
│   └── Paragraph scope=1:3..3:9 anchor=null attributes={} children=5
│       ├── Text scope=1:3..1:8 anchor=null attributes={} literal="quoted" children=0
│       ├── SoftBreak scope=1:9..1:9 anchor=null attributes={} children=0
│       ├── Text scope=2:3..2:6 anchor=null attributes={} literal="lazy" children=0
│       ├── SoftBreak scope=2:7..2:7 anchor=null attributes={} children=0
│       └── Text scope=3:1..3:9 anchor=null attributes={} literal="continued" children=0
└── Callout scope=5:1..5:10 anchor=null attributes={} variant=null fold=none children=1
    └── Callout scope=5:3..5:10 anchor=null attributes={} variant=null fold=none children=1
        └── Paragraph scope=5:5..5:10 anchor=null attributes={} children=1
            └── Text scope=5:5..5:10 anchor=null attributes={} literal="nested" children=0
````````````````````````````````

### Links, images, and references

Direct links and images, angle-bracket autolinks, reference definitions, and
the full, collapsed, and shortcut reference forms follow the inherited grammar.
Their consumer model, including `Destination`, unescaping, the resolved
reference model, and the fallback of an unresolved reference, is stated by the
[links and images](links-and-images.md) module:

```````````````````````````````` example
[a](/u "t") [b]() [c](<> "") ![d](/i.png) <https://x.y>
.
Document scope=1:1..1:55 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:55 anchor=null attributes={} children=9
    ├── Link scope=1:1..1:11 anchor=null attributes={} dest=url("/u") title="t" children=1
    │   └── Text scope=1:2..1:2 anchor=null attributes={} literal="a" children=0
    ├── Text scope=1:12..1:12 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:13..1:17 anchor=null attributes={} dest=url("") title=null children=1
    │   └── Text scope=1:14..1:14 anchor=null attributes={} literal="b" children=0
    ├── Text scope=1:18..1:18 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:19..1:28 anchor=null attributes={} dest=url("") title="" children=1
    │   └── Text scope=1:20..1:20 anchor=null attributes={} literal="c" children=0
    ├── Text scope=1:29..1:29 anchor=null attributes={} literal=" " children=0
    ├── Image scope=1:30..1:41 anchor=null attributes={} dest=url("/i.png") title=null width=null height=null children=1
    │   └── Text scope=1:32..1:32 anchor=null attributes={} literal="d" children=0
    ├── Text scope=1:42..1:42 anchor=null attributes={} literal=" " children=0
    └── Link scope=1:43..1:55 anchor=null attributes={} dest=url("https://x.y") title=null children=1
        └── Text scope=1:44..1:54 anchor=null attributes={} literal="https://x.y" children=0
````````````````````````````````

A link reference definition is parser state and produces no node; every
resolving reference form produces the same `Link` as a direct link, keeping
the scope of its own occurrence, and a reference that resolves to no
definition is the inherited bracket text:

```````````````````````````````` example
[full][r] [collapsed][] [shortcut] [none][x]

[r]: /r
[collapsed]: /c "T"
[shortcut]: <>
.
Document scope=1:1..5:14 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:44 anchor=null attributes={} children=6
    ├── Link scope=1:1..1:9 anchor=null attributes={} dest=url("/r") title=null children=1
    │   └── Text scope=1:2..1:5 anchor=null attributes={} literal="full" children=0
    ├── Text scope=1:10..1:10 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:11..1:23 anchor=null attributes={} dest=url("/c") title="T" children=1
    │   └── Text scope=1:12..1:20 anchor=null attributes={} literal="collapsed" children=0
    ├── Text scope=1:24..1:24 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:25..1:34 anchor=null attributes={} dest=url("") title=null children=1
    │   └── Text scope=1:26..1:33 anchor=null attributes={} literal="shortcut" children=0
    └── Text scope=1:35..1:44 anchor=null attributes={} literal=" [none][x]" children=0
````````````````````````````````

A footnote definition line `[^label]:` is never a link reference definition,
and a line the inherited grammar accepts as a link reference definition is one
regardless of a leading `@` or any other byte that another module would read
inline; block starts are decided before inline recognition.

## Smart punctuation

There is no smart punctuation. Quotation marks, hyphen runs, and periods are
stored as written, and typographic replacement is consumer policy, as every
other rendering choice is. cmark's `--smart` mode has no counterpart on any
surface, and the cmark gate replays the inputs of
cmark's own `smart_punct.txt` with the mode off on both sides:

```````````````````````````````` example
"quotes" 'single' -- --- ---- ... a\"b
.
Document scope=1:1..1:38 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:38 anchor=null attributes={} children=1
    └── Text scope=1:1..1:38 anchor=null attributes={} literal="\"quotes\" 'single' -- --- ---- ... a\"b" children=0
````````````````````````````````

## Required conformance cases

Every example of this module is a package fixture, and the cmark
specification corpus is replayed in full by the cmark gate. The package
fixtures additionally cover `SoftBreak` and `LineBreak` scopes inside every
container, `info`, `language`, `fenced`, and `closed` on every code-block
form, backtick strings of 80 and of 81 backticks as opener and as closer,
every HTML block type and inline token, quotation marks, hyphen runs, and
periods stored as written, and the four inherited link forms with and without
a definition.
