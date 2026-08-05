---
title: mdast parity corpus
---

Inputs for `scripts/check-mdast-parity.mjs`, in the repository's spec-fixture
format. The expected block of each example is this parser's canonical dump, as
everywhere else — but the parity gate never reads it. It compares against
remark's tree instead, so these expected blocks exist only to keep the file
readable and to let `spec_runner` treat it as an ordinary fixture if it is ever
registered as one.

The constructs here are the ones cmark-gfm cannot judge: footnote placement,
and reference-link resolution where the two ecosystems keep different AST
shapes. Directive and formula inputs are not repeated — the gate reads those
from the existing extension fixtures.

Footnote definitions stay where they were written, unlike cmark-gfm which moves
them to the document tail. remark agrees with this repository.

```````````````````````````````` example
a[^f]

[^f]: body

tail
.
Document scope=1:1..5:4 children=3
├── Paragraph scope=1:1..1:5 children=2
│   ├── Text scope=1:1..1:1 literal="a" children=0
│   └── FootnoteReference scope=1:2..1:5 id="f" children=0
├── FootnoteDefinition scope=3:7..3:10 id="f" children=1
│   └── Paragraph scope=3:7..3:10 children=1
│       └── Text scope=3:7..3:10 literal="body" children=0
└── Paragraph scope=5:1..5:4 children=1
    └── Text scope=5:1..5:4 literal="tail" children=0
````````````````````````````````

Several definitions, out of first-reference order, each staying at its own
source position.

```````````````````````````````` example
x[^b] y[^a]

[^a]: A

mid

[^b]: B
.
Document scope=1:1..7:9 children=5
├── Paragraph scope=1:1..1:11 children=4
│   ├── Text scope=1:1..1:1 literal="x" children=0
│   ├── FootnoteReference scope=1:2..1:5 id="b" children=0
│   ├── Text scope=1:6..1:7 literal=" y" children=0
│   └── FootnoteReference scope=1:8..1:11 id="a" children=0
├── FootnoteDefinition scope=3:7..3:7 id="a" children=1
│   └── Paragraph scope=3:7..3:7 children=1
│       └── Text scope=3:7..3:7 literal="A" children=0
├── Paragraph scope=5:1..5:3 children=1
│   └── Text scope=5:1..5:3 literal="mid" children=0
├── FootnoteDefinition scope=7:7..7:7 id="b" children=1
│   └── Paragraph scope=7:7..7:7 children=1
│       └── Text scope=7:7..7:7 literal="B" children=0
└── Paragraph scope=7:1..7:1 children=0
````````````````````````````````

An unreferenced definition is kept, not dropped.

```````````````````````````````` example
no references here

[^orphan]: still a definition
.
Document scope=1:1..3:29 children=2
└── Paragraph scope=1:1..1:18 children=1
````````````````````````````````

A footnote reference with no definition is literal text in both models, label
included and unparsed — the same rule the missing link reference below follows.

```````````````````````````````` example
dangling[^nope] tail
.
Document scope=1:1..1:20 children=1
````````````````````````````````

A reference link resolves to its definition; the definition itself leaves no
node, as in cmark. remark keeps a `definition` node and an unresolved
`linkReference`, which the gate's normalizer resolves before comparing.

```````````````````````````````` example
[ref]: /r "T"

See [link][ref].
.
Document scope=1:1..3:16 children=1
````````````````````````````````

A collapsed reference and a shortcut reference resolve the same way.

```````````````````````````````` example
[ref]: /r

[ref][] and [ref].
.
Document scope=1:1..3:18 children=1
````````````````````````````````

A reference whose definition is missing degrades to literal text in both
models.

```````````````````````````````` example
See [missing][nope].
.
Document scope=1:1..1:20 children=1
````````````````````````````````

A definition appearing after its use still resolves.

```````````````````````````````` example
Use [a] first.

[a]: /late "L"
.
Document scope=1:1..3:15 children=1
````````````````````````````````

An image reference resolves to an image.

```````````````````````````````` example
![alt][pic]

[pic]: /p "P"
.
Document scope=1:1..3:14 children=1
````````````````````````````````

Definitions are matched case-insensitively and with collapsed whitespace.

```````````````````````````````` example
[Foo   Bar]: /fb

[foo bar]
.
Document scope=1:1..3:9 children=1
````````````````````````````````

A directive label that never closes leaves the rest of the line as ordinary
inline content, bare URL included. remark stops recognizing the URL on this
path even though it recognizes it without the directive, which is why the
difference is registered rather than fixed.

```````````````````````````````` example
:note[See [docs](https://examp
.
Document scope=1:1..1:30 children=1
└── Paragraph scope=1:1..1:30 children=3
    ├── Directive scope=1:1..1:5 mode=embedded name="note" attributes=null children=0
    ├── Text scope=1:6..1:17 literal="[See [docs](" children=0
    └── Link scope=1:18..1:30 destination="https://examp" title=null children=1
        └── Text scope=1:18..1:30 literal="https://examp" children=0
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
└── Table scope=1:1..3:12 alignments=[none] children=2
    ├── TableRow scope=1:1..1:1 isHeader=true children=1
    │   └── TableCell scope=1:1..1:1 children=1
    │       └── Text scope=1:1..1:1 literal="a" children=0
    └── TableRow scope=3:1..3:12 isHeader=false children=1
        └── TableCell scope=3:1..3:2 children=1
            └── Text scope=3:1..3:2 literal="b." children=0
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
    ├── Code scope=1:4..2:1 mode=embedded literal="x y" children=0
    └── Text scope=2:3..2:4 literal=" b" children=0
````````````````````````````````

A definition whose title candidate is followed by non-whitespace: the title
rewinds out of the definition entirely, and the reference resolves without
it. remark reads it the same way (cmark-gfm keeps the scanned title in its
map — the `refdef-title-rewind` entry in specs/upstream-parity/deltas.json).

```````````````````````````````` example
[foo]: /url
"title" ok

[foo]
.
Document scope=1:1..4:5 children=3
├── ReferenceDefinition scope=1:1..1:11 label="foo" destination="/url" title="" children=0
├── Paragraph scope=2:1..2:10 children=1
│   └── Text scope=2:1..2:10 literal="\"title\" ok" children=0
└── Paragraph scope=4:1..4:5 children=1
    └── LinkReference scope=4:1..4:5 label="foo" form=shortcut children=1
        └── Text scope=4:2..4:4 literal="foo" children=0
````````````````````````````````
