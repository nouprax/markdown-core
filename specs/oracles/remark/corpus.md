---
title: mdast parity corpus
---

Inputs for `scripts/check-mdast-parity.mjs`, in the repository's spec-fixture
format. The expected block of each example is this parser's canonical dump, as
everywhere else — but the parity gate never reads it. It compares against
remark's tree instead, so these expected blocks exist only to keep the file
readable and to let `spec_runner` treat it as an ordinary fixture if it is ever
registered as one.

The constructs here need corrective or supplementary evidence beyond the two
primary C-family oracles: footnote placement and reference-link representation.
Directive and formula inputs are not repeated — the gate reads those from the
existing extension fixtures.

A footnote definition is a `Footnote` value the document owns, ordered by
scope start, and an unreferenced one is kept; cmark-gfm moves definitions to
the document tail in reference order and drops the unreferenced. remark keeps
every definition where it was written, which the gate lifts into the same
document-owned order before comparing.

```````````````````````````````` example
a[^f]

[^f]: body

tail
.
Document scope=1:1..5:4 children=2
├── Paragraph scope=1:1..1:5 children=2
│   ├── Text scope=1:1..1:1 literal="a" children=0
│   └── Cite scope=1:2..1:5 children=1
│       └── Citation scope=1:3..1:4 referent=footnote(id="f") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
├── Paragraph scope=5:1..5:4 children=1
│   └── Text scope=5:1..5:4 literal="tail" children=0
└── Footnote scope=3:1..4:0 id="f" children=1
    └── Paragraph scope=3:7..3:10 children=1
        └── Text scope=3:7..3:10 literal="body" children=0
````````````````````````````````

Several definitions, out of first-reference order, each staying at its own
source position.

```````````````````````````````` example
x[^b] y[^a]

[^a]: A

mid

[^b]: B
.
Document scope=1:1..7:7 children=2
├── Paragraph scope=1:1..1:11 children=4
│   ├── Text scope=1:1..1:1 literal="x" children=0
│   ├── Cite scope=1:2..1:5 children=1
│   │   └── Citation scope=1:3..1:4 referent=footnote(id="b") children=0
│   │       ├── CitationPrefix children=0
│   │       └── CitationSuffix children=0
│   ├── Text scope=1:6..1:7 literal=" y" children=0
│   └── Cite scope=1:8..1:11 children=1
│       └── Citation scope=1:9..1:10 referent=footnote(id="a") children=0
│           ├── CitationPrefix children=0
│           └── CitationSuffix children=0
├── Paragraph scope=5:1..5:3 children=1
│   └── Text scope=5:1..5:3 literal="mid" children=0
├── Footnote scope=3:1..4:0 id="a" children=1
│   └── Paragraph scope=3:7..3:7 children=1
│       └── Text scope=3:7..3:7 literal="A" children=0
└── Footnote scope=7:1..7:7 id="b" children=1
    └── Paragraph scope=7:7..7:7 children=1
        └── Text scope=7:7..7:7 literal="B" children=0
````````````````````````````````

An unreferenced definition is kept, not dropped.

```````````````````````````````` example
no references here

[^orphan]: still a definition
.
Document scope=1:1..3:29 children=1
├── Paragraph scope=1:1..1:18 children=1
│   └── Text scope=1:1..1:18 literal="no references here" children=0
└── Footnote scope=3:1..3:29 id="orphan" children=1
    └── Paragraph scope=3:12..3:29 children=1
        └── Text scope=3:12..3:29 literal="still a definition" children=0
````````````````````````````````

A footnote reference with no definition is literal text in both models, label
included and unparsed — the same rule the missing link reference below follows.

```````````````````````````````` example
dangling[^nope] tail
.
Document scope=1:1..1:20 children=1
└── Paragraph scope=1:1..1:20 children=1
    └── Text scope=1:1..1:20 literal="dangling[^nope] tail" children=0
````````````````````````````````

A reference link resolves to its definition; the definition itself leaves no
node, as in cmark. remark keeps a `definition` node and an unresolved
`linkReference`, which the gate's normalizer resolves before comparing.

```````````````````````````````` example
[ref]: /r "T"

See [link][ref].
.
Document scope=1:1..3:16 children=1
└── Paragraph scope=3:1..3:16 children=3
    ├── Text scope=3:1..3:4 literal="See " children=0
    ├── Link scope=3:5..3:15 dest=url("/r") title="T" children=1
    │   └── Text scope=3:6..3:9 literal="link" children=0
    └── Text scope=3:16..3:16 literal="." children=0
````````````````````````````````

A collapsed reference and a shortcut reference resolve the same way.

```````````````````````````````` example
[ref]: /r

[ref][] and [ref].
.
Document scope=1:1..3:18 children=1
└── Paragraph scope=3:1..3:18 children=4
    ├── Link scope=3:1..3:7 dest=url("/r") title=null children=1
    │   └── Text scope=3:2..3:4 literal="ref" children=0
    ├── Text scope=3:8..3:12 literal=" and " children=0
    ├── Link scope=3:13..3:17 dest=url("/r") title=null children=1
    │   └── Text scope=3:14..3:16 literal="ref" children=0
    └── Text scope=3:18..3:18 literal="." children=0
````````````````````````````````

A reference whose definition is missing degrades to literal text in both
models.

```````````````````````````````` example
See [missing][nope].
.
Document scope=1:1..1:20 children=1
└── Paragraph scope=1:1..1:20 children=1
    └── Text scope=1:1..1:20 literal="See [missing][nope]." children=0
````````````````````````````````

A definition appearing after its use still resolves.

```````````````````````````````` example
Use [a] first.

[a]: /late "L"
.
Document scope=1:1..3:14 children=1
└── Paragraph scope=1:1..1:14 children=3
    ├── Text scope=1:1..1:4 literal="Use " children=0
    ├── Link scope=1:5..1:7 dest=url("/late") title="L" children=1
    │   └── Text scope=1:6..1:6 literal="a" children=0
    └── Text scope=1:8..1:14 literal=" first." children=0
````````````````````````````````

An image reference resolves to an image.

```````````````````````````````` example
![alt][pic]

[pic]: /p "P"
.
Document scope=1:1..3:13 children=1
└── Paragraph scope=1:1..1:11 children=1
    └── Image scope=1:1..1:11 dest=url("/p") title="P" children=1
        └── Text scope=1:3..1:5 literal="alt" children=0
````````````````````````````````

Definitions are matched case-insensitively and with collapsed whitespace.

```````````````````````````````` example
[Foo   Bar]: /fb

[foo bar]
.
Document scope=1:1..3:9 children=1
└── Paragraph scope=3:1..3:9 children=1
    └── Link scope=3:1..3:9 dest=url("/fb") title=null children=1
        └── Text scope=3:2..3:8 literal="foo bar" children=0
````````````````````````````````

A directive label that never closes leaves the rest of the line as ordinary
inline content, bare URL included. remark stops recognizing the URL on this
path even though it recognizes it without the directive, which is why the
difference is registered rather than fixed.

```````````````````````````````` example
:note[See [docs](https://examp
.
Document scope=1:1..1:30 children=1
└── Paragraph scope=1:1..1:30 children=2
    ├── Directive scope=1:1..1:5 name="note" attributes=null children=0
    └── Text scope=1:6..1:30 literal="[See [docs](https://examp" children=0
````````````````````````````````

A row with more cells than the header declares. cmark-gfm drops the excess
cells; remark keeps them. cmark-gfm is the authority for tables, so this is
registered rather than fixed — it is here so that stays checked.

```````````````````````````````` example
a
| --- |
b.| status |
.
Document scope=1:1..3:12 children=1
└── Table scope=1:1..3:12 columns=[none:null] children=2
    ├── TableHead children=1
    │   └── TableRow scope=1:1..1:1 children=1
    │       └── TableCell scope=1:1..1:1 rowspan=1 colspan=1 children=1
    │           └── Text scope=1:1..1:1 literal="a" children=0
    ├── TableBody children=1
    │   └── TableRow scope=3:1..3:12 children=1
    │       └── TableCell scope=3:1..3:2 rowspan=1 colspan=1 children=1
    │           └── Text scope=3:1..3:2 literal="b." children=0
    └── TableFoot children=0
````````````````````````````````

A multi-line paragraph whose last line is a table header row: the split-off
lead is an ordinary paragraph, so its `\|` is CommonMark's escape and its `\\|`
is an escaped backslash plus a literal pipe. micromark reads the lead exactly
this way; these two inputs keep the split, the spelling, and the paragraph's
position under the remark authority. (cmark-gfm pipe-unescapes the lead a
second time — the `table-split-lead-spelling` entry in
specs/oracles/cmark-gfm/deltas.json.)

```````````````````````````````` example
lead \| text
| a | b |
| - | - |
.
Document scope=1:1..3:9 children=2
├── Paragraph scope=1:1..1:12 children=1
│   └── Text scope=1:1..1:12 literal="lead | text" children=0
└── Table scope=2:1..3:9 columns=[none:null,none:null] children=1
    ├── TableHead children=1
    │   └── TableRow scope=2:1..2:9 children=2
    │       ├── TableCell scope=2:2..2:4 rowspan=1 colspan=1 children=1
    │       │   └── Text scope=2:3..2:3 literal="a" children=0
    │       └── TableCell scope=2:6..2:8 rowspan=1 colspan=1 children=1
    │           └── Text scope=2:7..2:7 literal="b" children=0
    ├── TableBody children=0
    └── TableFoot children=0
````````````````````````````````

```````````````````````````````` example
pre \\| lead
| a | b |
| - | - |
.
Document scope=1:1..3:9 children=2
├── Paragraph scope=1:1..1:12 children=1
│   └── Text scope=1:1..1:12 literal="pre \\| lead" children=0
└── Table scope=2:1..3:9 columns=[none:null,none:null] children=1
    ├── TableHead children=1
    │   └── TableRow scope=2:1..2:9 children=2
    │       ├── TableCell scope=2:2..2:4 rowspan=1 colspan=1 children=1
    │       │   └── Text scope=2:3..2:3 literal="a" children=0
    │       └── TableCell scope=2:6..2:8 rowspan=1 colspan=1 children=1
    │           └── Text scope=2:7..2:7 literal="b" children=0
    ├── TableBody children=0
    └── TableFoot children=0
````````````````````````````````

A code span whose content spans a line. CommonMark treats the line ending as a
space; cmark applies that when it builds the node and mdast leaves it to the
renderer, which is the `code-span-line-ending` shape delta.

```````````````````````````````` example
a `x
y` b
.
Document scope=1:1..2:4 children=1
└── Paragraph scope=1:1..2:4 children=3
    ├── Text scope=1:1..1:2 literal="a " children=0
    ├── Code scope=1:3..2:2 literal="x y" children=0
    └── Text scope=2:3..2:4 literal=" b" children=0
````````````````````````````````

A definition whose title candidate is followed by non-whitespace: the title
rewinds out of the definition entirely, and the reference resolves without
it. remark reads it the same way. Current cmark keeps the scanned title in its
map, which is the reviewed `refdef-title-rewind` entry in
specs/oracles/cmark/deltas.json.

The expected block records the resolved model: the definition leaves no node,
and the shortcut reference is the `Link` it names, with no title. The parity
gate compares remark directly and does not use that stored block as an oracle.

```````````````````````````````` example
[foo]: /url
"title" ok

[foo]
.
Document scope=1:1..4:5 children=2
├── Paragraph scope=2:1..2:10 children=1
│   └── Text scope=2:1..2:10 literal="\"title\" ok" children=0
└── Paragraph scope=4:1..4:5 children=1
    └── Link scope=4:1..4:5 dest=url("/url") title=null children=1
        └── Text scope=4:2..4:4 literal="foo" children=0
````````````````````````````````

Attaching an extension must not change what CommonMark emphasis means. The
formula and directive extensions used to fold their own special bytes into the
parser's flanking-skip table, so `scan_delims` walked over `:`, `$` and `}` as
though they were not there — and emphasis was lost, and in the sentinel cases
invented, merely because an extension was attached. remark's extensions have no
such effect on the base language, which is why these three rows are here rather
than in an engine fixture alone.

```````````````````````````````` example
foo:_bar_
.
Document scope=1:1..1:9 children=1
└── Paragraph scope=1:1..1:9 children=2
    ├── Text scope=1:1..1:4 literal="foo:" children=0
    └── Emphasis scope=1:5..1:9 children=1
        └── Text scope=1:6..1:8 literal="bar" children=0
````````````````````````````````

```````````````````````````````` example
foo$_bar_
.
Document scope=1:1..1:9 children=1
└── Paragraph scope=1:1..1:9 children=2
    ├── Text scope=1:1..1:4 literal="foo$" children=0
    └── Emphasis scope=1:5..1:9 children=1
        └── Text scope=1:6..1:8 literal="bar" children=0
````````````````````````````````

```````````````````````````````` example
a}*.foo.*
.
Document scope=1:1..1:9 children=1
└── Paragraph scope=1:1..1:9 children=2
    ├── Text scope=1:1..1:2 literal="a}" children=0
    └── Emphasis scope=1:3..1:9 children=1
        └── Text scope=1:4..1:8 literal=".foo." children=0
````````````````````````````````

Ordered punctuation is absent from mdast; task markers compare as decoded
values, including the space that denotes an incomplete task.

```````````````````````````````` example
1) outer
   - [ ] open
   - [x] done
.
Document scope=1:1..3:13 children=1
└── List scope=1:1..3:13 flavor=ordered start=1 variant=decimal delimiter=parenthesis(closed=false) tight=true children=1
    └── ListItem scope=1:1..3:13 marker=null children=2
        ├── Paragraph scope=1:4..1:8 children=1
        │   └── Text scope=1:4..1:8 literal="outer" children=0
        └── List scope=2:4..3:13 flavor=bullet start=null variant=null delimiter=null tight=true children=2
            ├── ListItem scope=2:4..2:13 marker=" " children=1
            │   └── Paragraph scope=2:10..2:13 children=1
            │       └── Text scope=2:10..2:13 literal="open" children=0
            └── ListItem scope=3:4..3:13 marker="x" children=1
                └── Paragraph scope=3:10..3:13 children=1
                    └── Text scope=3:10..3:13 literal="done" children=0
````````````````````````````````

Ragged rows use their own table's width, including tables nested in containers.

```````````````````````````````` example
| a | b |
| - | - |
:badge[short]
| first | second | ignored |

> | a | b | c |
> | - | - | - |
> short
> | first | second | third | ignored |
.
Document scope=1:1..9:38 children=2
├── Table scope=1:1..4:28 columns=[none:null,none:null] children=3
│   ├── TableHead children=1
│   │   └── TableRow scope=1:1..1:9 children=2
│   │       ├── TableCell scope=1:2..1:4 rowspan=1 colspan=1 children=1
│   │       │   └── Text scope=1:3..1:3 literal="a" children=0
│   │       └── TableCell scope=1:6..1:8 rowspan=1 colspan=1 children=1
│   │           └── Text scope=1:7..1:7 literal="b" children=0
│   ├── TableBody children=2
│   │   ├── TableRow scope=3:1..3:13 children=2
│   │   │   ├── TableCell scope=3:1..3:13 rowspan=1 colspan=1 children=1
│   │   │   │   └── Directive scope=3:1..3:13 name="badge" attributes=null children=0
│   │   │   │       └── DirectiveLabel scope=3:7..3:13 children=1
│   │   │   │           └── Text scope=3:8..3:12 literal="short" children=0
│   │   │   └── TableCell scope=3:13..3:13 rowspan=1 colspan=1 children=0
│   │   └── TableRow scope=4:1..4:28 children=2
│   │       ├── TableCell scope=4:2..4:8 rowspan=1 colspan=1 children=1
│   │       │   └── Text scope=4:3..4:7 literal="first" children=0
│   │       └── TableCell scope=4:10..4:17 rowspan=1 colspan=1 children=1
│   │           └── Text scope=4:11..4:16 literal="second" children=0
│   └── TableFoot children=0
└── Callout scope=6:1..9:38 variant=null collapsed=null children=1
    └── Table scope=6:3..9:38 columns=[none:null,none:null,none:null] children=3
        ├── TableHead children=1
        │   └── TableRow scope=6:3..6:15 children=3
        │       ├── TableCell scope=6:4..6:6 rowspan=1 colspan=1 children=1
        │       │   └── Text scope=6:5..6:5 literal="a" children=0
        │       ├── TableCell scope=6:8..6:10 rowspan=1 colspan=1 children=1
        │       │   └── Text scope=6:9..6:9 literal="b" children=0
        │       └── TableCell scope=6:12..6:14 rowspan=1 colspan=1 children=1
        │           └── Text scope=6:13..6:13 literal="c" children=0
        ├── TableBody children=2
        │   ├── TableRow scope=8:3..8:7 children=3
        │   │   ├── TableCell scope=8:3..8:7 rowspan=1 colspan=1 children=1
        │   │   │   └── Text scope=8:3..8:7 literal="short" children=0
        │   │   ├── TableCell scope=8:7..8:7 rowspan=1 colspan=1 children=0
        │   │   └── TableCell scope=8:7..8:7 rowspan=1 colspan=1 children=0
        │   └── TableRow scope=9:3..9:38 children=3
        │       ├── TableCell scope=9:4..9:10 rowspan=1 colspan=1 children=1
        │       │   └── Text scope=9:5..9:9 literal="first" children=0
        │       ├── TableCell scope=9:12..9:19 rowspan=1 colspan=1 children=1
        │       │   └── Text scope=9:13..9:18 literal="second" children=0
        │       └── TableCell scope=9:21..9:27 rowspan=1 colspan=1 children=1
        │           └── Text scope=9:22..9:26 literal="third" children=0
        └── TableFoot children=0
````````````````````````````````

List tightness includes separation inside an item, independently of nested list tightness.

```````````````````````````````` example
afte
1) outer

   - [ ] open
| expr |
.
Document scope=1:1..5:8 children=2
├── Paragraph scope=1:1..1:4 children=1
│   └── Text scope=1:1..1:4 literal="afte" children=0
└── List scope=2:1..5:8 flavor=ordered start=1 variant=decimal delimiter=parenthesis(closed=false) tight=false children=1
    └── ListItem scope=2:1..5:8 marker=null children=2
        ├── Paragraph scope=2:4..2:8 children=1
        │   └── Text scope=2:4..2:8 literal="outer" children=0
        └── List scope=4:4..5:8 flavor=bullet start=null variant=null delimiter=null tight=true children=1
            └── ListItem scope=4:4..5:8 marker=" " children=1
                └── Paragraph scope=4:10..5:8 children=3
                    ├── Text scope=4:10..4:13 literal="open" children=0
                    ├── SoftBreak scope=4:14..4:14 children=0
                    └── Text scope=5:1..5:8 literal="| expr |" children=0
````````````````````````````````
