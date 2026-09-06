# Lists

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the ordered-list facts of every list and two options: `fancyLists` and
`exampleLists` (each default `false`). Source: Pandoc's `fancy_lists`,
`startnum`, and `example_lists`. Executable oracle: the Pandoc 3.11 CLI under
`specs/oracles/pandoc/`. Landing: the list facts with `M5`, fancy markers
with `P9a`, example lists with `P9b`. Each example in this module names the
options it adds to the product defaults; the
[example format](../dialect.md#examples) is defined by the index.

## Model

```text
OrderedListStyle     = decimal | lowerAlpha | upperAlpha | lowerRoman |
                       upperRoman | example | default
OrderedListDelimiter = period | oneParen | twoParens | default

List(flavor: bullet | ordered, start: Int?, style: OrderedListStyle?,
     delimiter: OrderedListDelimiter?, tight: Bool, items: [ListItem])
ListItem(marker: String?, exampleLabel: String?, content: [Markup])
ExampleReference(label: String)
```

For a bullet list `start`, `style`, and `delimiter` are `null`. For an
ordered list all three are non-null and `start >= 0`. `start` is always the
numeric value of the first marker; there is no option that changes it. Style
and delimiter are authored facts and are never reconstructed from `start`.
`ExampleReference` is an inline leaf. `ListItem.marker` belongs to the
[task lists](task-lists.md) module.

```````````````````````````````` example
- a
+ b
* c
.
Document scope=1:1..3:3 anchor=null attributes={} children=3
├── List scope=1:1..1:3 anchor=null attributes={} flavor=bullet start=null style=null delimiter=null tight=true children=1
│   └── ListItem scope=1:1..1:3 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=1:3..1:3 anchor=null attributes={} children=1
│           └── Text scope=1:3..1:3 anchor=null attributes={} literal="a" children=0
├── List scope=2:1..2:3 anchor=null attributes={} flavor=bullet start=null style=null delimiter=null tight=true children=1
│   └── ListItem scope=2:1..2:3 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=2:3..2:3 anchor=null attributes={} children=1
│           └── Text scope=2:3..2:3 anchor=null attributes={} literal="b" children=0
└── List scope=3:1..3:3 anchor=null attributes={} flavor=bullet start=null style=null delimiter=null tight=true children=1
    └── ListItem scope=3:1..3:3 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=3:3..3:3 anchor=null attributes={} children=1
            └── Text scope=3:3..3:3 anchor=null attributes={} literal="c" children=0
````````````````````````````````

## Inherited ordered lists

With `fancyLists=false`, the inherited markers `N.` and `N)` are the only
ordered markers: `N` is one to nine ASCII digits, `style` is `decimal`,
`delimiter` is `period` or `oneParen`, and `start` is the value of the first
marker, so `0.` starts at zero:

```````````````````````````````` example
3. a
4. b

1) c

0. z
.
Document scope=1:1..6:4 anchor=null attributes={} children=3
├── List scope=1:1..3:0 anchor=null attributes={} flavor=ordered start=3 style=decimal delimiter=period tight=true children=2
│   ├── ListItem scope=1:1..1:4 anchor=null attributes={} marker=null exampleLabel=null children=1
│   │   └── Paragraph scope=1:4..1:4 anchor=null attributes={} children=1
│   │       └── Text scope=1:4..1:4 anchor=null attributes={} literal="a" children=0
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=2:4..2:4 anchor=null attributes={} children=1
│           └── Text scope=2:4..2:4 anchor=null attributes={} literal="b" children=0
├── List scope=4:1..5:0 anchor=null attributes={} flavor=ordered start=1 style=decimal delimiter=oneParen tight=true children=1
│   └── ListItem scope=4:1..5:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=4:4..4:4 anchor=null attributes={} children=1
│           └── Text scope=4:4..4:4 anchor=null attributes={} literal="c" children=0
└── List scope=6:1..6:4 anchor=null attributes={} flavor=ordered start=0 style=decimal delimiter=period tight=true children=1
    └── ListItem scope=6:1..6:4 anchor=null attributes={} marker=null exampleLabel=null children=1
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
├── List scope=1:1..2:0 anchor=null attributes={} flavor=ordered start=123456789 style=decimal delimiter=period tight=true children=1
│   └── ListItem scope=1:1..2:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=1:12..1:15 anchor=null attributes={} children=1
│           └── Text scope=1:12..1:15 anchor=null attributes={} literal="nine" children=0
└── Paragraph scope=3:1..3:15 anchor=null attributes={} children=1
    └── Text scope=3:1..3:15 anchor=null attributes={} literal="1234567890. ten" children=0
````````````````````````````````

## Fancy markers

With `fancyLists=true`, an ordered marker is one of:

- one to nine decimal digits, `style=decimal`;
- one ASCII letter, `lowerAlpha` or `upperAlpha`, with value its one-based
  position in the alphabet; `i` and `I` alone are Roman one;
- a Roman numeral `M* [CM] [D] [CD] C* [XC] [L] [XL] X* [IX] [V] [IV] I*`
  of at least one character in one case, `lowerRoman` or `upperRoman`, with
  the usual value, the whole marker consumed; or
- `#`, `style=default`, with value 1.

The marker is followed by `.`, by `)`, or is enclosed in `(...)`, giving
`period`, `oneParen`, or `twoParens`:

```````````````````````````````` example fancy_lists
a. x
b. y

A) x

(i) x
(ii) y

IV. x
.
Document scope=1:1..9:5 anchor=null attributes={} children=4
├── List scope=1:1..3:0 anchor=null attributes={} flavor=ordered start=1 style=lowerAlpha delimiter=period tight=true children=2
│   ├── ListItem scope=1:1..1:4 anchor=null attributes={} marker=null exampleLabel=null children=1
│   │   └── Paragraph scope=1:4..1:4 anchor=null attributes={} children=1
│   │       └── Text scope=1:4..1:4 anchor=null attributes={} literal="x" children=0
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=2:4..2:4 anchor=null attributes={} children=1
│           └── Text scope=2:4..2:4 anchor=null attributes={} literal="y" children=0
├── List scope=4:1..5:0 anchor=null attributes={} flavor=ordered start=1 style=upperAlpha delimiter=oneParen tight=true children=1
│   └── ListItem scope=4:1..5:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=4:4..4:4 anchor=null attributes={} children=1
│           └── Text scope=4:4..4:4 anchor=null attributes={} literal="x" children=0
├── List scope=6:1..8:0 anchor=null attributes={} flavor=ordered start=1 style=lowerRoman delimiter=twoParens tight=true children=2
│   ├── ListItem scope=6:1..6:5 anchor=null attributes={} marker=null exampleLabel=null children=1
│   │   └── Paragraph scope=6:5..6:5 anchor=null attributes={} children=1
│   │       └── Text scope=6:5..6:5 anchor=null attributes={} literal="x" children=0
│   └── ListItem scope=7:1..8:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=7:6..7:6 anchor=null attributes={} children=1
│           └── Text scope=7:6..7:6 anchor=null attributes={} literal="y" children=0
└── List scope=9:1..9:5 anchor=null attributes={} flavor=ordered start=4 style=upperRoman delimiter=period tight=true children=1
    └── ListItem scope=9:1..9:5 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=9:5..9:5 anchor=null attributes={} children=1
            └── Text scope=9:5..9:5 anchor=null attributes={} literal="x" children=0
````````````````````````````````

`#.` stores `delimiter=default`, while `#)` and `(#)` store `oneParen` and
`twoParens`:

```````````````````````````````` example fancy_lists
#. x
#. y

#) z
.
Document scope=1:1..4:4 anchor=null attributes={} children=2
├── List scope=1:1..3:0 anchor=null attributes={} flavor=ordered start=1 style=default delimiter=default tight=true children=2
│   ├── ListItem scope=1:1..1:4 anchor=null attributes={} marker=null exampleLabel=null children=1
│   │   └── Paragraph scope=1:4..1:4 anchor=null attributes={} children=1
│   │       └── Text scope=1:4..1:4 anchor=null attributes={} literal="x" children=0
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=2:4..2:4 anchor=null attributes={} children=1
│           └── Text scope=2:4..2:4 anchor=null attributes={} literal="y" children=0
└── List scope=4:1..4:4 anchor=null attributes={} flavor=ordered start=1 style=default delimiter=oneParen tight=true children=1
    └── ListItem scope=4:1..4:4 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=4:4..4:4 anchor=null attributes={} children=1
            └── Text scope=4:4..4:4 anchor=null attributes={} literal="z" children=0
````````````````````````````````

Padding follows the inherited rule of one to four columns of spaces or tabs,
or the marker ends the line. A single capital letter followed by `.` and
same-line content requires at least two columns of whitespace after the `.`,
so `B. Russell` is text; no exception for `p.` exists:

```````````````````````````````` example fancy_lists
B. Russell

B.  Russell
.
Document scope=1:1..3:11 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:10 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:10 anchor=null attributes={} literal="B. Russell" children=0
└── List scope=3:1..3:11 anchor=null attributes={} flavor=ordered start=2 style=upperAlpha delimiter=period tight=true children=1
    └── ListItem scope=3:1..3:11 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=3:5..3:11 anchor=null attributes={} children=1
            └── Text scope=3:5..3:11 anchor=null attributes={} literal="Russell" children=0
````````````````````````````````

After the first item commits a style and delimiter, each later marker is
read in that style first; `#` continues any style; a marker unreadable in the
committed style, or with another delimiter, ends the list and may start
another:

```````````````````````````````` example fancy_lists
a. x
1. y

i. x
j. y
.
Document scope=1:1..5:4 anchor=null attributes={} children=4
├── List scope=1:1..1:4 anchor=null attributes={} flavor=ordered start=1 style=lowerAlpha delimiter=period tight=true children=1
│   └── ListItem scope=1:1..1:4 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=1:4..1:4 anchor=null attributes={} children=1
│           └── Text scope=1:4..1:4 anchor=null attributes={} literal="x" children=0
├── List scope=2:1..3:0 anchor=null attributes={} flavor=ordered start=1 style=decimal delimiter=period tight=true children=1
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=2:4..2:4 anchor=null attributes={} children=1
│           └── Text scope=2:4..2:4 anchor=null attributes={} literal="y" children=0
├── List scope=4:1..4:4 anchor=null attributes={} flavor=ordered start=1 style=lowerRoman delimiter=period tight=true children=1
│   └── ListItem scope=4:1..4:4 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=4:4..4:4 anchor=null attributes={} children=1
│           └── Text scope=4:4..4:4 anchor=null attributes={} literal="x" children=0
└── List scope=5:1..5:4 anchor=null attributes={} flavor=ordered start=10 style=lowerAlpha delimiter=period tight=true children=1
    └── ListItem scope=5:1..5:4 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=5:4..5:4 anchor=null attributes={} children=1
            └── Text scope=5:4..5:4 anchor=null attributes={} literal="y" children=0
````````````````````````````````

With `fancyLists` on, an ordered list whose first item lies inside a list
item or a definition body must have value 1 (`1`, `a`, `A`, `i`, `I`, or
`#`), or the line is paragraph text; example lists are exempt:

```````````````````````````````` example fancy_lists
- x
  b. y

- x
  a. y
.
Document scope=1:1..5:6 anchor=null attributes={} children=1
└── List scope=1:1..5:6 anchor=null attributes={} flavor=bullet start=null style=null delimiter=null tight=false children=2
    ├── ListItem scope=1:1..3:0 anchor=null attributes={} marker=null exampleLabel=null children=1
    │   └── Paragraph scope=1:3..2:6 anchor=null attributes={} children=3
    │       ├── Text scope=1:3..1:3 anchor=null attributes={} literal="x" children=0
    │       ├── SoftBreak scope=1:4..1:4 anchor=null attributes={} children=0
    │       └── Text scope=2:3..2:6 anchor=null attributes={} literal="b. y" children=0
    └── ListItem scope=4:1..5:6 anchor=null attributes={} marker=null exampleLabel=null children=2
        ├── Paragraph scope=4:3..4:3 anchor=null attributes={} children=1
        │   └── Text scope=4:3..4:3 anchor=null attributes={} literal="x" children=0
        └── List scope=5:3..5:6 anchor=null attributes={} flavor=ordered start=1 style=lowerAlpha delimiter=period tight=true children=1
            └── ListItem scope=5:3..5:6 anchor=null attributes={} marker=null exampleLabel=null children=1
                └── Paragraph scope=5:6..5:6 anchor=null attributes={} children=1
                    └── Text scope=5:6..5:6 anchor=null attributes={} literal="y" children=0
````````````````````````````````

Only a marker with value 1 may interrupt a paragraph, and example markers
never do:

```````````````````````````````` example fancy_lists
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
└── List scope=8:1..8:4 anchor=null attributes={} flavor=ordered start=1 style=decimal delimiter=period tight=true children=1
    └── ListItem scope=8:1..8:4 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=8:4..8:4 anchor=null attributes={} children=1
            └── Text scope=8:4..8:4 anchor=null attributes={} literal="x" children=0
````````````````````````````````

## Example lists

With `exampleLists=true`, `@` is a marker character only inside parentheses:
`(@)`, `(@label)`, `(N@)`, and `(N@label)`, with `style=example` and
`delimiter=twoParens`. Items are numbered document-wide in ascending order
of item `scope.start` across content and footnotes: the counter starts at 1,
increases by one per item, continues across separated lists, and is set to
`N` by an explicit `(N@)` on the first item of a list before that item is
numbered, so a list's `start` is the number its first item received:

```````````````````````````````` example example_lists
(@) First example.
(@) Second example.

Intervening text.

(@) Third example.
.
Document scope=1:1..6:18 anchor=null attributes={} children=3
├── List scope=1:1..3:0 anchor=null attributes={} flavor=ordered start=1 style=example delimiter=twoParens tight=true children=2
│   ├── ListItem scope=1:1..1:18 anchor=null attributes={} marker=null exampleLabel=null children=1
│   │   └── Paragraph scope=1:5..1:18 anchor=null attributes={} children=1
│   │       └── Text scope=1:5..1:18 anchor=null attributes={} literal="First example." children=0
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=2:5..2:19 anchor=null attributes={} children=1
│           └── Text scope=2:5..2:19 anchor=null attributes={} literal="Second example." children=0
├── Paragraph scope=4:1..4:17 anchor=null attributes={} children=1
│   └── Text scope=4:1..4:17 anchor=null attributes={} literal="Intervening text." children=0
└── List scope=6:1..6:18 anchor=null attributes={} flavor=ordered start=3 style=example delimiter=twoParens tight=true children=1
    └── ListItem scope=6:1..6:18 anchor=null attributes={} marker=null exampleLabel=null children=1
        └── Paragraph scope=6:5..6:18 anchor=null attributes={} children=1
            └── Text scope=6:5..6:18 anchor=null attributes={} literal="Third example." children=0
````````````````````````````````

`label` is `alnum-run *( ("_" / "-") alnum-run )` over the dialect's letters
and numbers; the item stores it in `exampleLabel`, non-null only in an example
list. No item stores its derived number. In inline content outside opaque
constructs, the exact spelling `(@label)` with no internal whitespace is an
`ExampleReference` when the label is registered anywhere in the document;
where it is a valid list marker, the marker rule wins. A repeated label never
splits a list: the item is an ordinary item that advances the counter like
any other, `exampleLabel` records the label, and the label stays registered
to the first item that carried it:

```````````````````````````````` example example_lists
(@good) This is a good example.

As (@good) illustrates, the label resolves.

(@a) x
(@a) y
.
Document scope=1:1..6:6 anchor=null attributes={} children=3
├── List scope=1:1..2:0 anchor=null attributes={} flavor=ordered start=1 style=example delimiter=twoParens tight=true children=1
│   └── ListItem scope=1:1..2:0 anchor=null attributes={} marker=null exampleLabel="good" children=1
│       └── Paragraph scope=1:9..1:31 anchor=null attributes={} children=1
│           └── Text scope=1:9..1:31 anchor=null attributes={} literal="This is a good example." children=0
├── Paragraph scope=3:1..3:43 anchor=null attributes={} children=3
│   ├── Text scope=3:1..3:3 anchor=null attributes={} literal="As " children=0
│   ├── ExampleReference scope=3:4..3:10 anchor=null attributes={} label="good" children=0
│   └── Text scope=3:11..3:43 anchor=null attributes={} literal=" illustrates, the label resolves." children=0
└── List scope=5:1..6:6 anchor=null attributes={} flavor=ordered start=2 style=example delimiter=twoParens tight=true children=2
    ├── ListItem scope=5:1..5:6 anchor=null attributes={} marker=null exampleLabel="a" children=1
    │   └── Paragraph scope=5:6..5:6 anchor=null attributes={} children=1
    │       └── Text scope=5:6..5:6 anchor=null attributes={} literal="x" children=0
    └── ListItem scope=6:1..6:6 anchor=null attributes={} marker=null exampleLabel="a" children=1
        └── Paragraph scope=6:6..6:6 anchor=null attributes={} children=1
            └── Text scope=6:6..6:6 anchor=null attributes={} literal="y" children=0
````````````````````````````````

Registration and lookup are one document-wide operation, so a reference
before its definition resolves and parser order never changes a result:

```````````````````````````````` example example_lists
See (@later).

(@later) Defined afterwards.
.
Document scope=1:1..3:28 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:13 anchor=null attributes={} children=3
│   ├── Text scope=1:1..1:4 anchor=null attributes={} literal="See " children=0
│   ├── ExampleReference scope=1:5..1:12 anchor=null attributes={} label="later" children=0
│   └── Text scope=1:13..1:13 anchor=null attributes={} literal="." children=0
└── List scope=3:1..3:28 anchor=null attributes={} flavor=ordered start=1 style=example delimiter=twoParens tight=true children=1
    └── ListItem scope=3:1..3:28 anchor=null attributes={} marker=null exampleLabel="later" children=1
        └── Paragraph scope=3:10..3:28 anchor=null attributes={} children=1
            └── Text scope=3:10..3:28 anchor=null attributes={} literal="Defined afterwards." children=0
````````````````````````````````

`N` is one to nine decimal digits with value at least 1: on the first item of
a list it sets the counter before that item is numbered, and on a later item
it is ignored. `(0@)` and longer runs are not markers:

```````````````````````````````` example example_lists
(5@) x
(@) y

(0@) z
.
Document scope=1:1..4:6 anchor=null attributes={} children=2
├── List scope=1:1..3:0 anchor=null attributes={} flavor=ordered start=5 style=example delimiter=twoParens tight=true children=2
│   ├── ListItem scope=1:1..1:6 anchor=null attributes={} marker=null exampleLabel=null children=1
│   │   └── Paragraph scope=1:6..1:6 anchor=null attributes={} children=1
│   │       └── Text scope=1:6..1:6 anchor=null attributes={} literal="x" children=0
│   └── ListItem scope=2:1..3:0 anchor=null attributes={} marker=null exampleLabel=null children=1
│       └── Paragraph scope=2:5..2:5 anchor=null attributes={} children=1
│           └── Text scope=2:5..2:5 anchor=null attributes={} literal="y" children=0
└── Paragraph scope=4:1..4:6 anchor=null attributes={} children=1
    └── Text scope=4:1..4:6 anchor=null attributes={} literal="(0@) z" children=0
````````````````````````````````

With `citations` on, a bare `@label` that is not followed by bracketed
material and names a registered label is also an `ExampleReference`, while
`[@label]` and a bare key with a bracketed tail are citations. An
unregistered `(@label)` is `(` followed by an author-in-text `Cite` and `)`
with `citations` on:

```````````````````````````````` example example_lists citations
(@a) x

@a and [@a] and @nope
.
Document scope=1:1..3:21 anchor=null attributes={} children=2
├── List scope=1:1..2:0 anchor=null attributes={} flavor=ordered start=1 style=example delimiter=twoParens tight=true children=1
│   └── ListItem scope=1:1..2:0 anchor=null attributes={} marker=null exampleLabel="a" children=1
│       └── Paragraph scope=1:6..1:6 anchor=null attributes={} children=1
│           └── Text scope=1:6..1:6 anchor=null attributes={} literal="x" children=0
└── Paragraph scope=3:1..3:21 anchor=null attributes={} children=5
    ├── ExampleReference scope=3:1..3:2 anchor=null attributes={} label="a" children=0
    ├── Text scope=3:3..3:7 anchor=null attributes={} literal=" and " children=0
    ├── Cite scope=3:8..3:11 anchor=null attributes={} children=1
    │   └── Citation scope=3:9..3:10 referent=bib(key="a",mode=normal) children=0
    │       ├── CitationPrefix children=0
    │       └── CitationSuffix children=0
    ├── Text scope=3:12..3:16 anchor=null attributes={} literal=" and " children=0
    └── Cite scope=3:17..3:21 anchor=null attributes={} children=1
        └── Citation scope=3:17..3:21 referent=bib(key="nope",mode=authorInText) children=0
            ├── CitationPrefix children=0
            └── CitationSuffix children=0
````````````````````````````````

Without `citations`, an unregistered `(@label)` is text:

```````````````````````````````` example example_lists
See (@nope).
.
Document scope=1:1..1:12 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:12 anchor=null attributes={} children=1
    └── Text scope=1:1..1:12 anchor=null attributes={} literal="See (@nope)." children=0
````````````````````````````````

The continuation column of an example item is the container start plus four
columns after tab expansion, whatever the marker width. Every item's number
is its list's `start` plus its zero-based position in the list, so a
consumer derives the number of a reference by resolving the first item
registered with its label and computing that item's number.

## Option behavior and fallback

With both options off, output is the inherited grammar's:

```````````````````````````````` example
a. x

(@) y
.
Document scope=1:1..3:5 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:4 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:4 anchor=null attributes={} literal="a. x" children=0
└── Paragraph scope=3:1..3:5 anchor=null attributes={} children=1
    └── Text scope=3:1..3:5 anchor=null attributes={} literal="(@) y" children=0
````````````````````````````````

Invalid numerals, missing marker whitespace, prohibited nested starts,
incomplete parentheses, and ten-digit runs are ordinary text. Counters cannot
overflow because markers and `N` are limited to nine digits. The example map
is parser state, not a public side table.

## Scopes

`List` and `ListItem` scopes are the inherited ones; `ExampleReference.scope`
covers the parentheses and marker, or the bare `@` and label.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover Roman
markers in both cases and every delimiter, `i` and `I`, committed-style
reading, tight and loose items, global examples across footnotes, resets on
later items, four-column continuations, exact scopes, each option
independently, allocation failure, and long numeral, label, and list inputs.
