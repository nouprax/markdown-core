# Strikethrough

Status: normative module of the [Markdown Core dialect](../dialect.md). Option:
`strikethrough` (default `true`). Source: cmark-gfm's strikethrough extension.
Executable oracle: cmark-gfm under `specs/oracles/cmark-gfm/`. Landing:
partial; `P6` removes single-tilde strikethrough from the engine, as the
[conflicts](conflicts.md) register's ruling C-1 requires. Every example in this
module runs with the product defaults unless its fence says otherwise; the
[example format](../dialect.md#examples) is defined by the index.

## Model

```text
Strikethrough(content: [Markup])
```

`Strikethrough` is an inline kind whose content is parsed by the shared inline
parser.

## Syntax

Tildes are delimiters on the shared stack at inline steps C2 and C3. A run of
exactly two unescaped tildes is one strikethrough delimiter unit; a run of
one tilde is subscript syntax, owned by the
[superscript and subscript](superscript-and-subscript.md) module, and is never
a strikethrough delimiter; a run of three or more is text. A unit can open
when it is left-flanking and close when it is right-flanking under the
CommonMark definitions. With `subscript` off a single tilde is text:

```````````````````````````````` example
~~struck~~ but not ~this~
.
Document scope=1:1..1:25 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:25 anchor=null attributes={} children=2
    ├── Strikethrough scope=1:1..1:10 anchor=null attributes={} children=1
    │   └── Text scope=1:3..1:8 anchor=null attributes={} literal="struck" children=0
    └── Text scope=1:11..1:25 anchor=null attributes={} literal=" but not ~this~" children=0
````````````````````````````````

A closer matches the nearest unmatched opener; a single tilde never matches
a double one, and an unmatched unit is text:

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

A single tilde is subscript syntax whatever the options say: with
`subscript` on it is a subscript delimiter, and with it off it is text. A
run of two tildes is never subscript syntax: it is strikethrough when matched
under the rules above and otherwise text, whatever `subscript` says.
cmark-gfm's single-tilde
strikethrough is a registered delta; the [conflicts](conflicts.md) register
records the ruling, and the subscript module's examples show the result.

## Option behavior and fallback

With `strikethrough=false`, `~~` is text; single tildes belong to `subscript`
whatever this option says:

```````````````````````````````` example !strikethrough
~~a~~
.
Document scope=1:1..1:5 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:5 anchor=null attributes={} children=1
    └── Text scope=1:1..1:5 anchor=null attributes={} literal="~~a~~" children=0
````````````````````````````````

With `subscript` on and `strikethrough` off, a double run is still text
rather than two subscript units, so a single-tilde opener before it finds no
closer in it, while a single-tilde pair elsewhere is a subscript:

```````````````````````````````` example subscript !strikethrough
~a~~b ~~c~~ ~d~
.
Document scope=1:1..1:15 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:15 anchor=null attributes={} children=2
    ├── Text scope=1:1..1:12 anchor=null attributes={} literal="~a~~b ~~c~~ " children=0
    └── Subscript scope=1:13..1:15 anchor=null attributes={} children=1
        └── Text scope=1:14..1:14 anchor=null attributes={} literal="d" children=0
````````````````````````````````

## Scopes

`Strikethrough.scope` covers both tilde runs and the body.

## Required conformance cases

Every example of this module is a package fixture; `P6` regenerates the
single-tilde rows of the GFM corpus and the extension fixtures, and every other
row remains byte-identical. Tests also cover tildes beside every other
delimiter, HTML tokens, comments, and cross links as opaque contexts, exact
scopes, every combination with `subscript`, allocation failure, and
size-doubling tilde runs.
