# Insertion

Status: normative module of the [Markdown Core dialect](../dialect.md).
Source: `markdown-it-ins` 4.0.0 at
commit `d1a13b290c944e8f212d3a6bd2de2f70b751c924`, whose README is the pinned
source for the valid source form and the oracle's `<ins>` output; this module
states the rule, and the plugin is evidence whose differences become registered deltas.
Executable oracle: `markdown-it` 14.2.0 with the plugin registered, under
`specs/oracles/markdown-it-ins/`, landed with `I0` and `I1`. The
[example format](../dialect.md#examples) is defined by the index.

## Model

```text
Insertion(content: [Markup])
```

`Insertion` is an inline kind representing inserted content, delimited by
`++content++`. Its content is parsed by the shared inline parser and may
contain any inline construct whose delimiters nest legally inside it.
A matched pair has non-empty source between its delimiter runs; its
final content may nevertheless be empty when another feature semantically
removes every child in that region.

```````````````````````````````` example
This is ++inserted++ text.
.
Document scope=1:1..1:26 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:26 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:8 anchor=null attributes={} literal="This is " children=0
    ├── Insertion scope=1:9..1:20 anchor=null attributes={} children=1
    │   └── Text scope=1:11..1:18 anchor=null attributes={} literal="inserted" children=0
    └── Text scope=1:21..1:26 anchor=null attributes={} literal=" text." children=0
````````````````````````````````

Properly nested markup is parsed into the content:

```````````````````````````````` example
++**b**++ ++c *d*++
.
Document scope=1:1..1:19 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:19 anchor=null attributes={} children=3
    ├── Insertion scope=1:1..1:9 anchor=null attributes={} children=1
    │   └── Strong scope=1:3..1:7 anchor=null attributes={} children=1
    │       └── Text scope=1:5..1:5 anchor=null attributes={} literal="b" children=0
    ├── Text scope=1:10..1:10 anchor=null attributes={} literal=" " children=0
    └── Insertion scope=1:11..1:19 anchor=null attributes={} children=2
        ├── Text scope=1:13..1:14 anchor=null attributes={} literal="c " children=0
        └── Emphasis scope=1:15..1:17 anchor=null attributes={} children=1
            └── Text scope=1:16..1:16 anchor=null attributes={} literal="d" children=0
````````````````````````````````

## Delimiter runs

A plus run is a maximal sequence of one or more unescaped `+` scalars in one
inline container. A run of one `+` is text. A longer run is tokenized:

1. If its length is odd, one `+` is literal.
2. The remainder is partitioned from left to right into two-character `++`
   units.
3. Every unit inherits the opening and closing eligibility of the complete
   run.

For a complete run let `before` and `after` be the adjacent scalars outside
the run; the beginning and end of the inline container count as whitespace.
With the CommonMark 0.31.2 definitions of Unicode whitespace and punctuation:

```text
leftFlanking  = after is not whitespace and
                (after is not punctuation or before is whitespace or
                 before is punctuation)
rightFlanking = before is not whitespace and
                (before is not punctuation or after is whitespace or
                 after is punctuation)
```

A unit may open exactly when the run is left-flanking and close exactly when
it is right-flanking. Intraword opening and closing are allowed, and the rule
of three is not applied:

```````````````````````````````` example
a++b++c
.
Document scope=1:1..1:7 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:7 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:1 anchor=null attributes={} literal="a" children=0
    ├── Insertion scope=1:2..1:6 anchor=null attributes={} children=1
    │   └── Text scope=1:4..1:4 anchor=null attributes={} literal="b" children=0
    └── Text scope=1:7..1:7 anchor=null attributes={} literal="c" children=0
````````````````````````````````

Eligible units enter the shared delimiter stack at inline step C6 in source
order. A unit that can both open and close is first tried as a closer against
the nearest legal unmatched opener; if none matches it stays on the stack as
a potential opener. Multiple matching units nest rather than merge:

```````````````````````````````` example
++++text++++
.
Document scope=1:1..1:12 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:12 anchor=null attributes={} children=1
    └── Insertion scope=1:1..1:12 anchor=null attributes={} children=1
        └── Insertion scope=1:3..1:10 anchor=null attributes={} children=1
            └── Text scope=1:5..1:8 anchor=null attributes={} literal="text" children=0
````````````````````````````````

The literal `+` of an odd run is placed after all of that run's closing units
and before all of its opening units:

```````````````````````````````` example
+++text+++
.
Document scope=1:1..1:10 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:10 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:1 anchor=null attributes={} literal="+" children=0
    ├── Insertion scope=1:2..1:9 anchor=null attributes={} children=1
    │   └── Text scope=1:4..1:7 anchor=null attributes={} literal="text" children=0
    └── Text scope=1:10..1:10 anchor=null attributes={} literal="+" children=0
````````````````````````````````

Units of the same run cannot match one another, and a unit that is not
eligible on the side it needs is text, so `++++` and `a++++b` are entirely
literal and a run with whitespace on its outer side never delimits:

```````````````````````````````` example
++++ a++++b + not ++
.
Document scope=1:1..1:20 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:20 anchor=null attributes={} children=1
    └── Text scope=1:1..1:20 anchor=null attributes={} literal="++++ a++++b + not ++" children=0
````````````````````````````````

## Composition and opacity

Plus delimiters use the same machinery and precedence boundary as emphasis.
A closer never crosses an already established inline boundary, so crossed
delimiters are not repaired:

```````````````````````````````` example
**++text**++
.
Document scope=1:1..1:12 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:12 anchor=null attributes={} children=2
    ├── Strong scope=1:1..1:10 anchor=null attributes={} children=1
    │   └── Text scope=1:3..1:8 anchor=null attributes={} literal="++text" children=0
    └── Text scope=1:11..1:12 anchor=null attributes={} literal="++" children=0
````````````````````````````````

Backslash-escaped plus signs are text and join no run:

```````````````````````````````` example
\++a++ ++a\++
.
Document scope=1:1..1:13 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:13 anchor=null attributes={} children=1
    └── Text scope=1:1..1:13 anchor=null attributes={} literal="++a++ ++a++" children=0
````````````````````````````````

A soft line break may occur inside an inserted span, and a line ending beside a
candidate is whitespace for the flanking tests. Pairing is local to the
current inline container:

```````````````````````````````` example
++a
b++
.
Document scope=1:1..2:3 anchor=null attributes={} children=1
└── Paragraph scope=1:1..2:3 anchor=null attributes={} children=1
    └── Insertion scope=1:1..2:3 anchor=null attributes={} children=3
        ├── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
        ├── SoftBreak scope=1:4..1:4 anchor=null attributes={} children=0
        └── Text scope=2:1..2:1 anchor=null attributes={} literal="b" children=0
````````````````````````````````

An inserted span may occur inside link content:

```````````````````````````````` example
[++link++](/u)
.
Document scope=1:1..1:14 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:14 anchor=null attributes={} children=1
    └── Link scope=1:1..1:14 anchor=null attributes={} dest=url("/u") title=null children=1
        └── Insertion scope=1:2..1:9 anchor=null attributes={} children=1
            └── Text scope=1:4..1:7 anchor=null attributes={} literal="link" children=0
````````````````````````````````

Code spans, comments, HTML tokens, formulas, cross links, and autolinks are
opaque; text between paired HTML tags is eligible:

```````````````````````````````` example
`++a++` <b>++c++</b>
.
Document scope=1:1..1:20 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:20 anchor=null attributes={} children=5
    ├── Code scope=1:1..1:7 anchor=null attributes={} literal="++a++" children=0
    ├── Text scope=1:8..1:8 anchor=null attributes={} literal=" " children=0
    ├── HTML scope=1:9..1:11 anchor=null attributes={} literal="<b>" children=0
    ├── Insertion scope=1:12..1:16 anchor=null attributes={} children=1
    │   └── Text scope=1:14..1:14 anchor=null attributes={} literal="c" children=0
    └── HTML scope=1:17..1:20 anchor=null attributes={} literal="</b>" children=0
````````````````````````````````

## Fallback

An unmatched or ineligible unit is text, and failed
recognition consumes no escape, bracket, or plus sign a later construct
needs. Each run is scanned once, parsing stays linear for long runs and many
unmatched candidates, and allocation failure follows the shared rule. Delimiter
nesting has no fixed syntax limit.

## Oracle

The deterministic syntax oracle is `markdown-it@14.2.0` with
`markdown-it-ins@4.0.0` registered through `use`:

```text
markdown-it@14.2.0
  sourceCommit (14.2.0 tag): 829797aa00353ce0b62ddeb9b4583b837b1ffd9b
  integrity: sha512-1TGiQiJVRQ3NPmZH6sx5Cfnmg6GQm9jvC1ch4TK511NjSJvjzKLzn5pPfZRNZkRPZP0HqCioSndqH8v2nRaWVQ==

markdown-it-ins@4.0.0
  gitHead: d1a13b290c944e8f212d3a6bd2de2f70b751c924
  integrity: sha512-sWbjK2DprrkINE4oYDhHdCijGT+MIDhEupjSHLXe5UXeVr5qmVxs/nTUVtgi0Oh/qtF+QKV0tNWDhQBEPxiMew==
```

The comparison maps each matched `ins_open`/`ins_close` pair to one `Insertion`
and compares placement and nesting, not rendered HTML. A canary requires
`++inserted++` to contain exactly one matched pair before any result is
accepted. The rules above are normative; a disagreement with the plugin is a
registered delta.

## Scopes

`Insertion.scope` covers both delimiter runs and the body.

## Required conformance cases

Every example of this module is a package fixture, and the pinned upstream
cases are replayed. Tests also cover Unicode and punctuation flanking; nested
and crossed strikethrough, cross links, marks, cites, and inserted spans; every
opaque context; exact content order and scopes; allocation failure; the
deep nesting; long unmatched sequences; and size-doubling plus runs.
