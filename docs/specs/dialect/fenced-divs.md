# Fenced divs

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `fencedDivs` (default `false`). Source: Pandoc's `fenced_divs`.
Executable oracle: the Pandoc 3.11 CLI under `specs/oracles/pandoc/`.
Landing: `P8`. Every example in this module runs with `fencedDivs` on unless
its fence says otherwise; the [example format](../dialect.md#examples) is
defined by the index.

## Model

```text
Div(closed: Bool, content: [Markup])
```

`Div` is a block kind whose content is arbitrary block content. Its attribute
container or class word populates the universal `anchor` and `attributes`
fields under the [attributes](attributes.md) module:

```````````````````````````````` example fenced_divs
::: {.warning}
content
:::
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── Div scope=1:1..3:3 anchor=null attributes={.warning} closed=true children=1
    └── Paragraph scope=2:1..2:7 anchor=null attributes={} children=1
        └── Text scope=2:1..2:7 anchor=null attributes={} literal="content" children=0
````````````````````````````````

## Grammar

Recognition is block-start step 8, tested after the directive step:

```text
opener     = *3SP 3*":" *WSP ( attributes / class-word ) *WSP *":" *WSP EOL
class-word = 1*( non-whitespace scalar other than ":", "{", and "}" )
closer     = *3SP 3*":" *WSP EOL
```

An unbraced class word is shorthand for one class, not an attribute
container; a run may be longer than three colons and may be followed by
trailing colons:

```````````````````````````````` example fenced_divs
::: warning
x
:::

:::: {#id .c k=v} ::::
y
::::
.
Document scope=1:1..7:4 anchor=null attributes={} children=2
├── Div scope=1:1..3:3 anchor=null attributes={.warning} closed=true children=1
│   └── Paragraph scope=2:1..2:1 anchor=null attributes={} children=1
│       └── Text scope=2:1..2:1 anchor=null attributes={} literal="x" children=0
└── Div scope=5:1..7:4 anchor="id" attributes={.c k="v"} closed=true children=1
    └── Paragraph scope=6:1..6:1 anchor=null attributes={} children=1
        └── Text scope=6:1..6:1 anchor=null attributes={} literal="y" children=0
````````````````````````````````

`::: -` produces class `-` while `::: {-}` produces class `unnumbered`;
`::: {}` is a valid opener with `anchor=null` and `Attributes.empty`:

```````````````````````````````` example fenced_divs
::: -
x
:::

::: {-}
y
:::

::: {}
z
:::
.
Document scope=1:1..11:3 anchor=null attributes={} children=3
├── Div scope=1:1..3:3 anchor=null attributes={.-} closed=true children=1
│   └── Paragraph scope=2:1..2:1 anchor=null attributes={} children=1
│       └── Text scope=2:1..2:1 anchor=null attributes={} literal="x" children=0
├── Div scope=5:1..7:3 anchor=null attributes={.unnumbered} closed=true children=1
│   └── Paragraph scope=6:1..6:1 anchor=null attributes={} children=1
│       └── Text scope=6:1..6:1 anchor=null attributes={} literal="y" children=0
└── Div scope=9:1..11:3 anchor=null attributes={} closed=true children=1
    └── Paragraph scope=10:1..10:1 anchor=null attributes={} children=1
        └── Text scope=10:1..10:1 anchor=null attributes={} literal="z" children=0
````````````````````````````````

The div's content is parsed in place by the ordinary block parser through
the shared container stack, so divs nest without a separate algorithm. A
closer is eligible when, after the enclosing containers' prefixes are
stripped, the line matches the closer grammar and its colon run is at least
as long as the innermost open colon container's opening run; it then closes
that container, whether it is a `Div` or a container directive, and may
interrupt a paragraph inside it:

```````````````````````````````` example fenced_divs
:::: {.outer}
::: {.inner}
x
:::
::::
.
Document scope=1:1..5:4 anchor=null attributes={} children=1
└── Div scope=1:1..5:4 anchor=null attributes={.outer} closed=true children=1
    └── Div scope=2:1..4:3 anchor=null attributes={.inner} closed=true children=1
        └── Paragraph scope=3:1..3:1 anchor=null attributes={} children=1
            └── Text scope=3:1..3:1 anchor=null attributes={} literal="x" children=0
````````````````````````````````

A shorter bare colon line is ordinary content of the innermost container:

```````````````````````````````` example fenced_divs
:::: {.a}
:::
x
::::
.
Document scope=1:1..4:4 anchor=null attributes={} children=1
└── Div scope=1:1..4:4 anchor=null attributes={.a} closed=true children=1
    └── Paragraph scope=2:1..3:1 anchor=null attributes={} children=3
        ├── Text scope=2:1..2:3 anchor=null attributes={} literal=":::" children=0
        ├── SoftBreak scope=2:4..2:4 anchor=null attributes={} children=0
        └── Text scope=3:1..3:1 anchor=null attributes={} literal="x" children=0
````````````````````````````````

Pandoc closes with any colon run of three or more; the
[conflicts](conflicts.md) register records the difference.

A div with no eligible closer ends where its enclosing container's content
ends or at the end of the document, with `closed=false`; it is the same node
either way:

```````````````````````````````` example fenced_divs
::: {.a}
text
.
Document scope=1:1..2:4 anchor=null attributes={} children=1
└── Div scope=1:1..2:4 anchor=null attributes={.a} closed=false children=1
    └── Paragraph scope=2:1..2:4 anchor=null attributes={} children=1
        └── Text scope=2:1..2:4 anchor=null attributes={} literal="text" children=0
````````````````````````````````

```````````````````````````````` example fenced_divs
para

::: {.a}
.
Document scope=1:1..3:8 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:4 anchor=null attributes={} literal="para" children=0
└── Div scope=3:1..3:8 anchor=null attributes={.a} closed=false children=0
````````````````````````````````

An opener may interrupt a paragraph, as a code fence does:

```````````````````````````````` example fenced_divs
para
::: {.a}
x
:::
.
Document scope=1:1..4:3 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:4 anchor=null attributes={} literal="para" children=0
└── Div scope=2:1..4:3 anchor=null attributes={.a} closed=true children=1
    └── Paragraph scope=3:1..3:1 anchor=null attributes={} children=1
        └── Text scope=3:1..3:1 anchor=null attributes={} literal="x" children=0
````````````````````````````````

A tab in the leading indentation disqualifies the line. The colon run must be
followed by whitespace or `{`: a run followed immediately by a directive name
is a container directive under `directives`, and with `directives` off such a
line is paragraph text:

```````````````````````````````` example fenced_divs
:::warning
x
:::
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── DirectiveBlock scope=1:1..3:3 anchor=null attributes={} name="warning" children=1
    └── Paragraph scope=2:1..2:1 anchor=null attributes={} children=1
        └── Text scope=2:1..2:1 anchor=null attributes={} literal="x" children=0
````````````````````````````````

```````````````````````````````` example fenced_divs !directives
:::warning
x
:::
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── Paragraph scope=1:1..3:3 anchor=null attributes={} children=5
    ├── Text scope=1:1..1:10 anchor=null attributes={} literal=":::warning" children=0
    ├── SoftBreak scope=1:11..1:11 anchor=null attributes={} children=0
    ├── Text scope=2:1..2:1 anchor=null attributes={} literal="x" children=0
    ├── SoftBreak scope=2:2..2:2 anchor=null attributes={} children=0
    └── Text scope=3:1..3:3 anchor=null attributes={} literal=":::" children=0
````````````````````````````````

A colon run with neither a container nor a class word never opens a div, and
an invalid braced list leaves the line to the following block starts:

```````````````````````````````` example fenced_divs
::: {.1}
x
:::

:::
y
:::
.
Document scope=1:1..7:3 anchor=null attributes={} children=2
├── Paragraph scope=1:1..3:3 anchor=null attributes={} children=5
│   ├── Text scope=1:1..1:8 anchor=null attributes={} literal="::: {.1}" children=0
│   ├── SoftBreak scope=1:9..1:9 anchor=null attributes={} children=0
│   ├── Text scope=2:1..2:1 anchor=null attributes={} literal="x" children=0
│   ├── SoftBreak scope=2:2..2:2 anchor=null attributes={} children=0
│   └── Text scope=3:1..3:3 anchor=null attributes={} literal=":::" children=0
└── Paragraph scope=5:1..7:3 anchor=null attributes={} children=5
    ├── Text scope=5:1..5:3 anchor=null attributes={} literal=":::" children=0
    ├── SoftBreak scope=5:4..5:4 anchor=null attributes={} children=0
    ├── Text scope=6:1..6:1 anchor=null attributes={} literal="y" children=0
    ├── SoftBreak scope=6:2..6:2 anchor=null attributes={} children=0
    └── Text scope=7:1..7:3 anchor=null attributes={} literal=":::" children=0
````````````````````````````````

The content is ordinary block content of every enabled kind:

```````````````````````````````` example fenced_divs
::: {.a}
# h

- i
:::
.
Document scope=1:1..5:3 anchor=null attributes={} children=1
└── Div scope=1:1..5:3 anchor=null attributes={.a} closed=true children=2
    ├── Heading scope=2:1..2:3 anchor=null attributes={} level=1 children=1
    │   └── Text scope=2:3..2:3 anchor=null attributes={} literal="h" children=0
    └── List scope=4:1..4:3 anchor=null attributes={} flavor=bullet start=null style=null delimiter=null tight=true children=1
        └── ListItem scope=4:1..4:3 anchor=null attributes={} marker=null exampleLabel=null children=1
            └── Paragraph scope=4:3..4:3 anchor=null attributes={} children=1
                └── Text scope=4:3..4:3 anchor=null attributes={} literal="i" children=0
````````````````````````````````

A colon line inside fenced code, an HTML block, or another opaque block is
that block's content.

## Option behavior and fallback

With `fencedDivs=false`, colon lines are paragraph text or, under
`directives`, directives:

```````````````````````````````` example
::: {.a}
x
:::
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── Paragraph scope=1:1..3:3 anchor=null attributes={} children=5
    ├── Text scope=1:1..1:8 anchor=null attributes={} literal="::: {.a}" children=0
    ├── SoftBreak scope=1:9..1:9 anchor=null attributes={} children=0
    ├── Text scope=2:1..2:1 anchor=null attributes={} literal="x" children=0
    ├── SoftBreak scope=2:2..2:2 anchor=null attributes={} children=0
    └── Text scope=3:1..3:3 anchor=null attributes={} literal=":::" children=0
````````````````````````````````

With the option on, an invalid braced list or trailing bytes other than
spaces and colons leave the line to the following block starts, and after a
valid opener commits a malformed inner opener is content and cannot close its
parent accidentally. Recognition never scans ahead for a matching fence
before parsing content, and allocation failure follows the shared rule.

## Scopes

A closed `Div.scope` covers both fence lines and everything between. An
unclosed `Div.scope` ends at the end of the last line of its last child
block, or at the end of the opener line when it has no children.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover closers
longer than the opener, a div nested with a container directive, empty
closed content, missing closers at container end, malformed containers, code
and HTML opacity, block content of every kind, exact scopes, nesting limits,
allocation failure, and long colon runs.
