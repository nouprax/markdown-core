# Lists

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns ordered-list facts and fancy markers. Specimen definitions and
references belong to [specimens](specimens.md). Source: Pandoc's `fancy_lists`,
`startnum`. Executable oracle: the Pandoc 3.11 CLI under
`specs/oracles/pandoc/`. Landing: the list facts with `M5`, fancy markers
with `P9a`. The
[example format](../dialect.md#examples) is defined by the index.

## Model

```text
OrderedListVariant     = decimal | alpha(lowercased: Bool) |
                       roman(lowercased: Bool) | default
OrderedListDelimiter = period | parenthesis(closed: Bool) | default

List(flavor: bullet | ordered, start: Int?, variant: OrderedListVariant?,
     delimiter: OrderedListDelimiter?, tight: Bool, items: [ListItem])
ListItem(marker: String?, content: [Markup])
```

For a bullet list `start`, `variant`, and `delimiter` are `null`. For an
ordered list all three are non-null and `start >= 0`. `start` is always the
numeric value of the first marker; nothing changes it. Variant
and delimiter are authored facts and are never reconstructed from `start`.
`ListItem.marker` belongs to the
[task lists](task-lists.md) module.

```````````````````````````````` example
- a
+ b
* c
.
Document scope=1:1..3:3 anchor=null attributes={} children=3
├── List scope=1:1..1:3 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=1
│   └── ListItem scope=1:1..1:3 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=1:3..1:3 anchor=null attributes={} children=1
│           └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
├── List scope=2:1..2:3 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=1
│   └── ListItem scope=2:1..2:3 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=2:3..2:3 anchor=null attributes={} children=1
│           └── Text scope=2:3..2:3 anchor=null attributes={} literal="b" children=0
└── List scope=3:1..3:3 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=true children=1
    └── ListItem scope=3:1..3:3 anchor=null attributes={} marker=null children=1
        └── Paragraph scope=3:3..3:3 anchor=null attributes={} children=1
            └── Text scope=3:3..3:3 anchor=null attributes={} literal="c" children=0
````````````````````````````````

## Decimal markers

The inherited markers `N.` and `N)` are decimal markers: `N` is one to nine
ASCII digits, `variant` is `decimal`,
`delimiter` is `period` or `parenthesis(closed=false)`, and `start` is the value of the first
marker, so `0.` starts at zero:

```````````````````````````````` example
3. a
4. b

1) c

0. z
.
Document scope=1:1..6:4 anchor=null attributes={} children=3
├── List scope=1:1..3:0 anchor=null attributes={} flavor=ordered start=3 variant=decimal delimiter=period tight=true children=2
│   ├── ListItem scope=1:1..1:4 anchor=null attributes={} marker=null children=1
│   │   └── Paragraph scope=1:4..1:4 anchor=null attributes={} children=1
│   │       └── Text scope=1:4..1:4 anchor=null attributes={} literal="a" children=0
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=2:4..2:4 anchor=null attributes={} children=1
│           └── Text scope=2:4..2:4 anchor=null attributes={} literal="b" children=0
├── List scope=4:1..5:0 anchor=null attributes={} flavor=ordered start=1 variant=decimal delimiter=parenthesis(closed=false) tight=true children=1
│   └── ListItem scope=4:1..5:0 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=4:4..4:4 anchor=null attributes={} children=1
│           └── Text scope=4:4..4:4 anchor=null attributes={} literal="c" children=0
└── List scope=6:1..6:4 anchor=null attributes={} flavor=ordered start=0 variant=decimal delimiter=period tight=true children=1
    └── ListItem scope=6:1..6:4 anchor=null attributes={} marker=null children=1
        └── Paragraph scope=6:4..6:4 anchor=null attributes={} children=1
            └── Text scope=6:4..6:4 anchor=null attributes={} literal="z" children=0
````````````````````````````````

A ten-digit run is not a marker. Every inherited rule about padding,
continuation, tightness, and which markers may interrupt a paragraph is
unchanged:

```````````````````````````````` example
123456789. nine

1234567890. ten
.
Document scope=1:1..3:15 anchor=null attributes={} children=2
├── List scope=1:1..2:0 anchor=null attributes={} flavor=ordered start=123456789 variant=decimal delimiter=period tight=true children=1
│   └── ListItem scope=1:1..2:0 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=1:12..1:15 anchor=null attributes={} children=1
│           └── Text scope=1:12..1:15 anchor=null attributes={} literal="nine" children=0
└── Paragraph scope=3:1..3:15 anchor=null attributes={} children=1
    └── Text scope=3:1..3:15 anchor=null attributes={} literal="1234567890. ten" children=0
````````````````````````````````

## Fancy markers

An ordered marker is one of:

- one to nine decimal digits, `variant=decimal`;
- one ASCII letter, `alpha(lowercased=true)` or `alpha(lowercased=false)`, with value its one-based
  position in the alphabet; `i` and `I` alone are Roman one;
- a Roman numeral `M* [CM] [D] [CD] C* [XC] [L] [XL] X* [IX] [V] [IV] I*`
  of at least one character in one case, `roman(lowercased=true)` or `roman(lowercased=false)`, with
  the usual value, which is at most the nine-digit decimal ceiling of
  999999999, the whole marker consumed; a numeral of greater value, whatever
  its components, is not a marker; or
- `#`, `variant=default`, with value 1.

The marker is followed by `.`, by `)`, or is enclosed in `(...)`, giving
`period`, `parenthesis(closed=false)`, or `parenthesis(closed=true)`:

```````````````````````````````` example
a. x
b. y

A) x

(i) x
(ii) y

IV. x
.
Document scope=1:1..9:5 anchor=null attributes={} children=4
├── List scope=1:1..3:0 anchor=null attributes={} flavor=ordered start=1 variant=alpha(lowercased=true) delimiter=period tight=true children=2
│   ├── ListItem scope=1:1..1:4 anchor=null attributes={} marker=null children=1
│   │   └── Paragraph scope=1:4..1:4 anchor=null attributes={} children=1
│   │       └── Text scope=1:4..1:4 anchor=null attributes={} literal="x" children=0
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=2:4..2:4 anchor=null attributes={} children=1
│           └── Text scope=2:4..2:4 anchor=null attributes={} literal="y" children=0
├── List scope=4:1..5:0 anchor=null attributes={} flavor=ordered start=1 variant=alpha(lowercased=false) delimiter=parenthesis(closed=false) tight=true children=1
│   └── ListItem scope=4:1..5:0 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=4:4..4:4 anchor=null attributes={} children=1
│           └── Text scope=4:4..4:4 anchor=null attributes={} literal="x" children=0
├── List scope=6:1..8:0 anchor=null attributes={} flavor=ordered start=1 variant=roman(lowercased=true) delimiter=parenthesis(closed=true) tight=true children=2
│   ├── ListItem scope=6:1..6:5 anchor=null attributes={} marker=null children=1
│   │   └── Paragraph scope=6:5..6:5 anchor=null attributes={} children=1
│   │       └── Text scope=6:5..6:5 anchor=null attributes={} literal="x" children=0
│   └── ListItem scope=7:1..8:0 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=7:6..7:6 anchor=null attributes={} children=1
│           └── Text scope=7:6..7:6 anchor=null attributes={} literal="y" children=0
└── List scope=9:1..9:5 anchor=null attributes={} flavor=ordered start=4 variant=roman(lowercased=false) delimiter=period tight=true children=1
    └── ListItem scope=9:1..9:5 anchor=null attributes={} marker=null children=1
        └── Paragraph scope=9:5..9:5 anchor=null attributes={} children=1
            └── Text scope=9:5..9:5 anchor=null attributes={} literal="x" children=0
````````````````````````````````

`#.` stores `delimiter=default`, while `#)` and `(#)` store `parenthesis(closed=false)` and
`parenthesis(closed=true)`:

```````````````````````````````` example
#. x
#. y

#) z
.
Document scope=1:1..4:4 anchor=null attributes={} children=2
├── List scope=1:1..3:0 anchor=null attributes={} flavor=ordered start=1 variant=default delimiter=default tight=true children=2
│   ├── ListItem scope=1:1..1:4 anchor=null attributes={} marker=null children=1
│   │   └── Paragraph scope=1:4..1:4 anchor=null attributes={} children=1
│   │       └── Text scope=1:4..1:4 anchor=null attributes={} literal="x" children=0
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=2:4..2:4 anchor=null attributes={} children=1
│           └── Text scope=2:4..2:4 anchor=null attributes={} literal="y" children=0
└── List scope=4:1..4:4 anchor=null attributes={} flavor=ordered start=1 variant=default delimiter=parenthesis(closed=false) tight=true children=1
    └── ListItem scope=4:1..4:4 anchor=null attributes={} marker=null children=1
        └── Paragraph scope=4:4..4:4 anchor=null attributes={} children=1
            └── Text scope=4:4..4:4 anchor=null attributes={} literal="z" children=0
````````````````````````````````

Padding follows the inherited rule of one to four columns of spaces or tabs,
or the marker ends the line. A single capital letter followed by `.` and
same-line content requires at least two columns of whitespace after the `.`,
so `B. Russell` is text; no exception for `p.` exists:

```````````````````````````````` example
B. Russell

B.  Russell
.
Document scope=1:1..3:11 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:10 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:10 anchor=null attributes={} literal="B. Russell" children=0
└── List scope=3:1..3:11 anchor=null attributes={} flavor=ordered start=2 variant=alpha(lowercased=false) delimiter=period tight=true children=1
    └── ListItem scope=3:1..3:11 anchor=null attributes={} marker=null children=1
        └── Paragraph scope=3:5..3:11 anchor=null attributes={} children=1
            └── Text scope=3:5..3:11 anchor=null attributes={} literal="Russell" children=0
````````````````````````````````

After the first item commits a variant and delimiter, each later marker is
read in that variant first; `#` continues any variant; a marker unreadable in the
committed variant, or with another delimiter, ends the list and may start
another:

```````````````````````````````` example
a. x
1. y

i. x
j. y
.
Document scope=1:1..5:4 anchor=null attributes={} children=4
├── List scope=1:1..1:4 anchor=null attributes={} flavor=ordered start=1 variant=alpha(lowercased=true) delimiter=period tight=true children=1
│   └── ListItem scope=1:1..1:4 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=1:4..1:4 anchor=null attributes={} children=1
│           └── Text scope=1:4..1:4 anchor=null attributes={} literal="x" children=0
├── List scope=2:1..3:0 anchor=null attributes={} flavor=ordered start=1 variant=decimal delimiter=period tight=true children=1
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=2:4..2:4 anchor=null attributes={} children=1
│           └── Text scope=2:4..2:4 anchor=null attributes={} literal="y" children=0
├── List scope=4:1..4:4 anchor=null attributes={} flavor=ordered start=1 variant=roman(lowercased=true) delimiter=period tight=true children=1
│   └── ListItem scope=4:1..4:4 anchor=null attributes={} marker=null children=1
│       └── Paragraph scope=4:4..4:4 anchor=null attributes={} children=1
│           └── Text scope=4:4..4:4 anchor=null attributes={} literal="x" children=0
└── List scope=5:1..5:4 anchor=null attributes={} flavor=ordered start=10 variant=alpha(lowercased=true) delimiter=period tight=true children=1
    └── ListItem scope=5:1..5:4 anchor=null attributes={} marker=null children=1
        └── Paragraph scope=5:4..5:4 anchor=null attributes={} children=1
            └── Text scope=5:4..5:4 anchor=null attributes={} literal="y" children=0
````````````````````````````````

An ordered list whose first item lies inside a list
item or a definition body must have value 1 (`1`, `a`, `A`, `i`, `I`, or
`#`), or the line is paragraph text; specimen definitions follow their own module:

```````````````````````````````` example
- x
  b. y

- x
  a. y
.
Document scope=1:1..5:6 anchor=null attributes={} children=1
└── List scope=1:1..5:6 anchor=null attributes={} flavor=bullet start=null variant=null delimiter=null tight=false children=2
    ├── ListItem scope=1:1..3:0 anchor=null attributes={} marker=null children=1
    │   └── Paragraph scope=1:3..2:6 anchor=null attributes={} children=3
    │       ├── Text scope=1:3..1:3 anchor=null attributes={} literal="x" children=0
    │       ├── SoftBreak scope=1:4..1:4 anchor=null attributes={} children=0
    │       └── Text scope=2:3..2:6 anchor=null attributes={} literal="b. y" children=0
    └── ListItem scope=4:1..5:6 anchor=null attributes={} marker=null children=2
        ├── Paragraph scope=4:3..4:3 anchor=null attributes={} children=1
        │   └── Text scope=4:3..4:3 anchor=null attributes={} literal="x" children=0
        └── List scope=5:3..5:6 anchor=null attributes={} flavor=ordered start=1 variant=alpha(lowercased=true) delimiter=period tight=true children=1
            └── ListItem scope=5:3..5:6 anchor=null attributes={} marker=null children=1
                └── Paragraph scope=5:6..5:6 anchor=null attributes={} children=1
                    └── Text scope=5:6..5:6 anchor=null attributes={} literal="y" children=0
````````````````````````````````

Only a marker with value 1 may interrupt a paragraph, and specimen markers
never do:

```````````````````````````````` example
para
2. x

para
b. x

para
1. x
.
Document scope=1:1..8:4 anchor=null attributes={} children=4
├── Paragraph scope=1:1..2:4 anchor=null attributes={} children=3
│   ├── Text scope=1:1..1:4 anchor=null attributes={} literal="para" children=0
│   ├── SoftBreak scope=1:5..1:5 anchor=null attributes={} children=0
│   └── Text scope=2:1..2:4 anchor=null attributes={} literal="2. x" children=0
├── Paragraph scope=4:1..5:4 anchor=null attributes={} children=3
│   ├── Text scope=4:1..4:4 anchor=null attributes={} literal="para" children=0
│   ├── SoftBreak scope=4:5..4:5 anchor=null attributes={} children=0
│   └── Text scope=5:1..5:4 anchor=null attributes={} literal="b. x" children=0
├── Paragraph scope=7:1..7:4 anchor=null attributes={} children=1
│   └── Text scope=7:1..7:4 anchor=null attributes={} literal="para" children=0
└── List scope=8:1..8:4 anchor=null attributes={} flavor=ordered start=1 variant=decimal delimiter=period tight=true children=1
    └── ListItem scope=8:1..8:4 anchor=null attributes={} marker=null children=1
        └── Paragraph scope=8:4..8:4 anchor=null attributes={} children=1
            └── Text scope=8:4..8:4 anchor=null attributes={} literal="x" children=0
````````````````````````````````

## Fallback

Invalid numerals, missing marker whitespace, prohibited nested starts,
incomplete parentheses, ten-digit runs, and Roman numerals whose value
exceeds 999999999 are ordinary text. Counters cannot overflow because
decimal markers and `N` are limited to nine digits and a Roman marker's
value to the same ceiling, which every surface's counter type holds; an
implementation stops accumulating a numeral as soon as it exceeds the
ceiling, so no run of `M`, `C`, `X`, or `I` can overflow.

## Scopes

`List` and `ListItem` scopes are the inherited ones.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover Roman
markers in both cases and every delimiter, `i` and `I`, committed-variant
reading, tight and loose items, exact scopes, allocation failure, Roman numerals at the ceiling and just
above it spelled with runs of `M`, of `C`, of `X`, and of `I`, and long
numeral, label, and list inputs.
