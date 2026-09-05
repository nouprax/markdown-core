# Directives

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `directives` (default `true`). Source: `remark-directive` 4.0.0 and
its `micromark-extension-directive` envelope syntax. Executable oracle: remark
under `specs/oracles/remark/`, for the envelope, label, and attachment
position; the attribute member grammar is the dialect's own, stated by the
[attributes](attributes.md) module. Landing: present; the attribute model
migrates to the universal fields with `M7`. Every example in this module runs
with the product defaults unless its fence says otherwise; the
[example format](../dialect.md#examples) is defined by the index.

## Model

```text
Directive(name: String, label: DirectiveLabel?)
DirectiveBlock(name: String, label: DirectiveLabel?, content: [Markup])
DirectiveLabel(content: [Markup])
```

`Directive` is an inline leaf whose only owned markup is its optional label.
`DirectiveBlock` is a block kind whose content is block content; a leaf block
directive and an empty container directive produce identical values with
`content=[]`. `DirectiveLabel` is `Markup` owned only by the typed `label`
field, never an element of `content`, and its scope spans its brackets, so an
empty label is a place. Attributes populate the universal `anchor` and
`attributes` fields once `M7` lands; until then the current contract's
`attributes: [DirectiveAttribute]?` stands.

```````````````````````````````` example
A :badge[new]{#id .tag level="3"} here.
.
Document scope=1:1..1:39 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:39 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:2 anchor=null attributes={} literal="A " children=0
    ├── Directive scope=1:3..1:33 anchor="id" attributes={.tag level="3"} name="badge" children=0
    │   └── DirectiveLabel scope=1:9..1:13 anchor=null attributes={} children=1
    │       └── Text scope=1:10..1:12 anchor=null attributes={} literal="new" children=0
    └── Text scope=1:34..1:39 anchor=null attributes={} literal=" here." children=0
````````````````````````````````

## Names

A directive name is one or more Unicode scalars. The first scalar is any
scalar above U+0020 that is neither whitespace nor punctuation under the
dialect's Unicode tables; each further scalar is such a scalar or `-` or `_`.
A name does not end with `-` or `_`. Names are stored as written and compared
by no one in the parser.

## Text directives

A text directive is inline step A10:

```text
text-directive = ":" name [ label ] [ attributes ]
label          = "[" label-content "]"
```

The name, label, and attribute container must be adjacent in that order with
no whitespace between them; a `[` after a container is text, and at most one
label and one container attach. Each part is optional after the name:

```````````````````````````````` example
:name :name[label] :name{.c}
.
Document scope=1:1..1:28 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:28 anchor=null attributes={} children=5
    ├── Directive scope=1:1..1:5 anchor=null attributes={} name="name" children=0
    ├── Text scope=1:6..1:6 anchor=null attributes={} literal=" " children=0
    ├── Directive scope=1:7..1:18 anchor=null attributes={} name="name" children=0
    │   └── DirectiveLabel scope=1:12..1:18 anchor=null attributes={} children=1
    │       └── Text scope=1:13..1:17 anchor=null attributes={} literal="label" children=0
    ├── Text scope=1:19..1:19 anchor=null attributes={} literal=" " children=0
    └── Directive scope=1:20..1:28 anchor=null attributes={.c} name="name" children=0
````````````````````````````````

The colon may not be preceded or followed by another colon, and the name may
not be followed by a colon, so `x ::a y`, `x:::a`, and `:red:` contain no
directive. A name may contain `-` and `_` after its first scalar but not end
with them:

```````````````````````````````` example
x ::a y x:::a :red: :a-b_c :d-
.
Document scope=1:1..1:30 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:30 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:20 anchor=null attributes={} literal="x ::a y x:::a :red: " children=0
    ├── Directive scope=1:21..1:26 anchor=null attributes={} name="a-b_c" children=0
    └── Text scope=1:27..1:30 anchor=null attributes={} literal=" :d-" children=0
````````````````````````````````

`label-content` is balanced-bracket source: a backslash escapes the next
byte, an unescaped `[` increases the nesting depth and an unescaped `]`
decreases it, the label ends at the `]` that returns the depth to zero, and a
label that would reach depth 33 is not a label. The label may span soft line
breaks but not a block boundary. Its content is parsed by the inline parser as
ordinary inline content, so a link, a span, or another directive may occur
inside it:

```````````````````````````````` example
:a[b [c] d] :e[f \] g]
.
Document scope=1:1..1:22 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:22 anchor=null attributes={} children=3
    ├── Directive scope=1:1..1:11 anchor=null attributes={} name="a" children=0
    │   └── DirectiveLabel scope=1:3..1:11 anchor=null attributes={} children=1
    │       └── Text scope=1:4..1:10 anchor=null attributes={} literal="b [c] d" children=0
    ├── Text scope=1:12..1:12 anchor=null attributes={} literal=" " children=0
    └── Directive scope=1:13..1:22 anchor=null attributes={} name="e" children=0
        └── DirectiveLabel scope=1:15..1:22 anchor=null attributes={} children=1
            └── Text scope=1:16..1:21 anchor=null attributes={} literal="f ] g" children=0
````````````````````````````````

A directive commits at its name. A `[` that does not complete a label leaves
the directive without a label and is text; a `{` that does not complete a
valid container leaves the directive without attributes and is text. Both
follow the shared failure rule and consume nothing:

```````````````````````````````` example
:a[b :c{d
.
Document scope=1:1..1:9 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:9 anchor=null attributes={} children=4
    ├── Directive scope=1:1..1:2 anchor=null attributes={} name="a" children=0
    ├── Text scope=1:3..1:5 anchor=null attributes={} literal="[b " children=0
    ├── Directive scope=1:6..1:7 anchor=null attributes={} name="c" children=0
    └── Text scope=1:8..1:9 anchor=null attributes={} literal="{d" children=0
````````````````````````````````

## Block directives

Block directives are step 7 of the block-start order and are tested at the
first non-space byte of a line indented at most three spaces:

```text
leaf-opener      = "::" name [ label ] [ attributes ] *WSP EOL
container-opener = 3*":" name [ label ] [ attributes ] *WSP EOL
closer           = *3SP 3*":" *WSP EOL
```

A leaf directive is complete at its line and has `content=[]`:

```````````````````````````````` example
::note[Label]{.c}
.
Document scope=1:1..1:17 anchor=null attributes={} children=1
└── DirectiveBlock scope=1:1..1:17 anchor=null attributes={.c} name="note" children=0
    └── DirectiveLabel scope=1:7..1:13 anchor=null attributes={} children=1
        └── Text scope=1:8..1:12 anchor=null attributes={} literal="Label" children=0
````````````````````````````````

A container directive opens a block container whose content is parsed by the
ordinary block parser and closes at the first later closer line whose colon
run is at least as long as the opener's, after the enclosing containers'
prefixes are stripped:

```````````````````````````````` example
:::note[Label]{.c}
content
:::
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── DirectiveBlock scope=1:1..3:3 anchor=null attributes={.c} name="note" children=1
    ├── DirectiveLabel scope=1:8..1:14 anchor=null attributes={} children=1
    │   └── Text scope=1:9..1:13 anchor=null attributes={} literal="Label" children=0
    └── Paragraph scope=2:1..2:7 anchor=null attributes={} children=1
        └── Text scope=2:1..2:7 anchor=null attributes={} literal="content" children=0
````````````````````````````````

Containers nest, and a longer opener needs a closer at least as long, so an
inner container closes first:

```````````````````````````````` example
::::outer
:::inner
x
:::
::::
.
Document scope=1:1..5:4 anchor=null attributes={} children=1
└── DirectiveBlock scope=1:1..5:4 anchor=null attributes={} name="outer" children=1
    └── DirectiveBlock scope=2:1..4:3 anchor=null attributes={} name="inner" children=1
        └── Paragraph scope=3:1..3:1 anchor=null attributes={} children=1
            └── Text scope=3:1..3:1 anchor=null attributes={} literal="x" children=0
````````````````````````````````

A closer line shorter than the opener's run is content of the container:

```````````````````````````````` example
::::a
:::
x
::::
.
Document scope=1:1..4:4 anchor=null attributes={} children=1
└── DirectiveBlock scope=1:1..4:4 anchor=null attributes={} name="a" children=1
    └── Paragraph scope=2:1..3:1 anchor=null attributes={} children=3
        ├── Text scope=2:1..2:3 anchor=null attributes={} literal=":::" children=0
        ├── SoftBreak scope=2:4..2:4 anchor=null attributes={} children=0
        └── Text scope=3:1..3:1 anchor=null attributes={} literal="x" children=0
````````````````````````````````

An unclosed container ends where its enclosing container's content ends or at
the end of the document and produces the same node:

```````````````````````````````` example
> :::a
> x

y
.
Document scope=1:1..4:1 anchor=null attributes={} children=2
├── Callout scope=1:1..2:3 anchor=null attributes={} variant=null fold=none children=1
│   └── DirectiveBlock scope=1:3..2:3 anchor=null attributes={} name="a" children=1
│       └── Paragraph scope=2:3..2:3 anchor=null attributes={} children=1
│           └── Text scope=2:3..2:3 anchor=null attributes={} literal="x" children=0
└── Paragraph scope=4:1..4:1 anchor=null attributes={} children=1
    └── Text scope=4:1..4:1 anchor=null attributes={} literal="y" children=0
````````````````````````````````

Both opener forms may interrupt a paragraph:

```````````````````````````````` example
para
::a
.
Document scope=1:1..2:3 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:4 anchor=null attributes={} literal="para" children=0
└── DirectiveBlock scope=2:1..2:3 anchor=null attributes={} name="a" children=0
````````````````````````````````

A tab in the leading indentation disqualifies the line. The colon run must be
followed immediately by the name; a run followed by whitespace, `{`, or the
end of line is not a directive and is left to the [fenced divs](fenced-divs.md)
step. The label and container follow the text-directive rules but must close
on the opener line; an unclosed label or container, or any other byte after
the last accepted element, makes the line ordinary content for the following
steps:

```````````````````````````````` example
::: a
::a[b
:::
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── Paragraph scope=1:1..3:3 anchor=null attributes={} children=5
    ├── Text scope=1:1..1:5 anchor=null attributes={} literal="::: a" children=0
    ├── SoftBreak scope=1:6..1:6 anchor=null attributes={} children=0
    ├── Text scope=2:1..2:5 anchor=null attributes={} literal="::a[b" children=0
    ├── SoftBreak scope=2:6..2:6 anchor=null attributes={} children=0
    └── Text scope=3:1..3:3 anchor=null attributes={} literal=":::" children=0
````````````````````````````````

A closer line inside fenced code, an HTML block, or another opaque block is
that block's content. The closer of a fenced div is the same closer grammar;
a bare colon line closes the innermost open colon container of either kind
that it is long enough to close, as the fenced divs module and the conflicts
register state.

## Attributes

The container is the shared attribute grammar of the
[attributes](attributes.md) module, attached at the owner named above. `{}`
attaches successfully and yields `anchor=null` and `Attributes.empty`:

```````````````````````````````` example
:a{} ::b{}
.
Document scope=1:1..1:10 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:10 anchor=null attributes={} children=2
    ├── Directive scope=1:1..1:4 anchor=null attributes={} name="a" children=0
    └── Text scope=1:5..1:10 anchor=null attributes={} literal=" ::b{}" children=0
````````````````````````````````

In a text directive a quoted value may span the line endings the grammar
permits; in a block directive the container must close on the opener line.

## Option behavior and fallback

Source owned by code, HTML tokens and blocks, comments, formulas, and cross
links is never a directive:

```````````````````````````````` example
`:a` <!-- :b -->
.
Document scope=1:1..1:16 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:16 anchor=null attributes={} children=3
    ├── Code scope=1:1..1:4 anchor=null attributes={} literal=":a" children=0
    ├── Text scope=1:5..1:5 anchor=null attributes={} literal=" " children=0
    └── Comment scope=1:6..1:16 anchor=null attributes={} literal=" :b " children=0
````````````````````````````````

With `directives=false`, no colon has directive meaning and every byte
follows the inherited grammar: `:name` is text, `::name` and `:::name` lines
are paragraph text or, under `fencedDivs`, are tested by that module:

```````````````````````````````` example !directives
:a[b]

::a

:::a
x
:::
.
Document scope=1:1..7:3 anchor=null attributes={} children=3
├── Paragraph scope=1:1..1:5 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:5 anchor=null attributes={} literal=":a[b]" children=0
├── Paragraph scope=3:1..3:3 anchor=null attributes={} children=1
│   └── Text scope=3:1..3:3 anchor=null attributes={} literal="::a" children=0
└── Paragraph scope=5:1..7:3 anchor=null attributes={} children=5
    ├── Text scope=5:1..5:4 anchor=null attributes={} literal=":::a" children=0
    ├── SoftBreak scope=5:5..5:5 anchor=null attributes={} children=0
    ├── Text scope=6:1..6:1 anchor=null attributes={} literal="x" children=0
    ├── SoftBreak scope=6:2..6:2 anchor=null attributes={} children=0
    └── Text scope=7:1..7:3 anchor=null attributes={} literal=":::" children=0
````````````````````````````````

With the option on, a failed candidate consumes nothing.

## Scopes

`Directive.scope` covers the colon through the last byte of the name, label,
or container, whichever was accepted last. `DirectiveBlock.scope` covers the
opener line through the closer line, or through the last consumed content line
when unclosed. `DirectiveLabel.scope` covers its brackets.

## Required conformance cases

Every example of this module is a package fixture, and the directive fixtures
`extensions-directive.txt` and `extensions-directive-option-gates.txt` stay
the oracle of record. Tests also cover Unicode names, names beginning with
`-` or `_`, nesting to depths 32 and 33, multi-line labels, every container
success and failure, closer lines inside opaque blocks, a colon run followed
by whitespace under `fencedDivs` on and off, exact scopes, allocation
failure, and size-doubling colon runs, brackets, and braces.
