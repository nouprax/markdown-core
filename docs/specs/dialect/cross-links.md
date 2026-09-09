# Cross links

Status: normative module of the [Markdown Core dialect](../dialect.md).
Source: Obsidian's internal links and embeds (`[[...]]` and `![[...]]`).
Executable oracle: `@quartz-community/remark-obsidian` 0.2.4 under
`specs/oracles/obsidian/`, for the forms it parses; its label-nullability and
destination projections are registered deltas. Landing: `O1`, the first
producer of `Destination.cross`. The [example format](../dialect.md#examples)
is defined by the index.

## Model

```text
CrossLink(dest: Destination, label: String?)
CrossEmbedded(dest: Destination, label: String?, dimensions: Dimensions?)
```

`[[...]]` produces a `CrossLink`; `![[...]]` produces a `CrossEmbedded` requesting
workspace transclusion. Both are inline leaves with `Destination.cross(path,
anchor)`. The node kind carries the distinction; neither stores an `embedded`
flag. Only `Media` and `CrossEmbedded` have `dimensions`.

- A target without an anchor has `anchor == null` and a non-empty `path`.
- A target with an anchor stores a non-empty `anchor`; its `path` excludes the
  anchor, label and delimiters, and may be empty to address the current document.
- `label == null` means no `|` was authored; an authored empty label is `""`.
- `CrossLink.label` is the complete raw label. `CrossEmbedded.label` is the raw
  prefix remaining after any valid dimension suffix, including `""` for size only.

`path`, `anchor`, and retained label bytes are stored as written: not trimmed,
slugged, URL-decoded, case-folded, resolved or validated. Labels are not parsed
as inline content. Anchor punctuation affects recognition and is absent from
the value. `Destination.cross.anchor` names the declaration-side `Markup.anchor`
a consumer looks for; it declares nothing on the cross-reference node itself.

## Syntax

Recognition is inline step A6, run at every unescaped `[[` or `![[` before
inherited bracket handling:

```text
cross-link     = [ "!" ] "[[" target [ separator label ] "]]"
separator      = "\|" / "|"
target         = [ path ] [ block-anchor / heading-anchor ]
block-anchor   = "#^" block-id
heading-anchor = "#" heading-part *( "#" heading-part )
path           = 1*path-char
heading-part   = 1*path-char
path-char      = any scalar except "#", "|", "[", "]", LF, and CR, and
                 not a "\" immediately followed by "|"
block-id       = 1*( ASCII-letter / DIGIT / "-" )
label          = *( any scalar except "[", "]", LF, and CR )
```

The anchor alternatives are ordered: `block-anchor` is tried first and
succeeds only when the target ends after its identifier, at the separator or
the closing `]]`; otherwise the same bytes are parsed as a `heading-anchor`,
whose parts may begin with `^`, so `#^id` alone is a block anchor and
`#^id#x` is a heading anchor. A target alone is a link to a note:

```````````````````````````````` example
See [[Note]] for details.
.
Document scope=1:1..1:25 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:25 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:4 anchor=null attributes={} literal="See " children=0
    ├── CrossLink scope=1:5..1:12 anchor=null attributes={} dest=cross(path="Note",anchor=null) label=null children=0
    └── Text scope=1:13..1:25 anchor=null attributes={} literal=" for details." children=0
````````````````````````````````

A `!` immediately before the opener makes the cross link an embed; nothing
else about the value changes:

```````````````````````````````` example
![[Note]]
.
Document scope=1:1..1:9 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:9 anchor=null attributes={} children=1
    └── CrossEmbedded scope=1:1..1:9 anchor=null attributes={} dest=cross(path="Note",anchor=null) label=null dimensions=null children=0
````````````````````````````````

The first `|` after the target begins the label, which is stored raw:

```````````````````````````````` example
[[Folder/Note|Label]]
.
Document scope=1:1..1:21 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:21 anchor=null attributes={} children=1
    └── CrossLink scope=1:1..1:21 anchor=null attributes={} dest=cross(path="Folder/Note",anchor=null) label="Label" children=0
````````````````````````````````

### Anchors

Every byte after the first `#` of the target is the anchor, as written. A
path may be empty when an anchor is present, and then the cross link
addresses the current document:

```````````````````````````````` example
[[Note#Heading]] and [[#Heading]]
.
Document scope=1:1..1:33 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:33 anchor=null attributes={} children=3
    ├── CrossLink scope=1:1..1:16 anchor=null attributes={} dest=cross(path="Note",anchor="Heading") label=null children=0
    ├── Text scope=1:17..1:21 anchor=null attributes={} literal=" and " children=0
    └── CrossLink scope=1:22..1:33 anchor=null attributes={} dest=cross(path="",anchor="Heading") label=null children=0
````````````````````````````````

A heading anchor may name several nested heading parts; the parts are not
split, and the stored anchor keeps its inner `#`:

```````````````````````````````` example
[[Note#Parent#Child|Label]]
.
Document scope=1:1..1:27 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:27 anchor=null attributes={} children=1
    └── CrossLink scope=1:1..1:27 anchor=null attributes={} dest=cross(path="Note",anchor="Parent#Child") label="Label" children=0
````````````````````````````````

The target is the block form when `#^` immediately follows the path and a
non-empty `block-id` runs to the `|` or `]]`; then `anchor` is the identifier
without `#^`. In every other position `^` is an ordinary heading byte, and a
`^^` at the start of the target is part of the path. This reference spelling
addresses the value declared by `#anchor-id#` in the
[block identifiers](block-identifiers.md) module; declaration delimiters do
not become part of the reference:

```````````````````````````````` example
![[Note#^block-id]] [[A#^id#x]] [[^^text]]
.
Document scope=1:1..1:42 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:42 anchor=null attributes={} children=5
    ├── CrossEmbedded scope=1:1..1:19 anchor=null attributes={} dest=cross(path="Note",anchor="block-id") label=null dimensions=null children=0
    ├── Text scope=1:20..1:20 anchor=null attributes={} literal=" " children=0
    ├── CrossLink scope=1:21..1:31 anchor=null attributes={} dest=cross(path="A",anchor="^id#x") label=null children=0
    ├── Text scope=1:32..1:32 anchor=null attributes={} literal=" " children=0
    └── CrossLink scope=1:33..1:42 anchor=null attributes={} dest=cross(path="^^text",anchor=null) label=null children=0
````````````````````````````````

The parser records dimensions on embeds, while anchors such as `page=3` and
workspace resolution remain consumer-owned. Target extensions do not determine
whether a valid dimension suffix is recognized.

```````````````````````````````` example
![[Image.png|100x145]] ![[Document.pdf#page=3]] [[ Note ]]
.
Document scope=1:1..1:58 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:58 anchor=null attributes={} children=5
    ├── CrossEmbedded scope=1:1..1:22 anchor=null attributes={} dest=cross(path="Image.png",anchor=null) label="" dimensions=(width=100,height=145) children=0
    ├── Text scope=1:23..1:23 anchor=null attributes={} literal=" " children=0
    ├── CrossEmbedded scope=1:24..1:47 anchor=null attributes={} dest=cross(path="Document.pdf",anchor="page=3") label=null dimensions=null children=0
    ├── Text scope=1:48..1:48 anchor=null attributes={} literal=" " children=0
    └── CrossLink scope=1:49..1:58 anchor=null attributes={} dest=cross(path=" Note ",anchor=null) label=null children=0
````````````````````````````````

### Labels

An authored empty label is `""`, distinct from no label, and a second `|` is
label content:

```````````````````````````````` example
[[Note|]] [[Note|a|b]]
.
Document scope=1:1..1:22 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:22 anchor=null attributes={} children=3
    ├── CrossLink scope=1:1..1:9 anchor=null attributes={} dest=cross(path="Note",anchor=null) label="" children=0
    ├── Text scope=1:10..1:10 anchor=null attributes={} literal=" " children=0
    └── CrossLink scope=1:11..1:22 anchor=null attributes={} dest=cross(path="Note",anchor=null) label="a|b" children=0
````````````````````````````````

Inside `[[...]]` the pair `\|` is the other spelling of the label separator:
it is matched as one token before any path or heading character is, so a
path never ends in the backslash of such a pair, the backslash is dropped,
and the pipe separates. No other backslash escape exists inside a cross
link.

```````````````````````````````` example
[[Note\|Label]]
.
Document scope=1:1..1:15 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:15 anchor=null attributes={} children=1
    └── CrossLink scope=1:1..1:15 anchor=null attributes={} dest=cross(path="Note",anchor=null) label="Label" children=0
````````````````````````````````

### Failure

At least one of `path` and an anchor is non-empty, and a heading part that is
empty at any position makes the opener text. A failed candidate consumes
nothing; the inherited rules run from its first `[`:

```````````````````````````````` example
[[]] [[#]] [[|x]]

[[Note#]] [[A##B]] [[A#B#]]
.
Document scope=1:1..3:27 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:17 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:17 anchor=null attributes={} literal="[[]] [[#]] [[|x]]" children=0
└── Paragraph scope=3:1..3:27 anchor=null attributes={} children=1
    └── Text scope=3:1..3:27 anchor=null attributes={} literal="[[Note#]] [[A##B]] [[A#B#]]" children=0
````````````````````````````````

The candidate ends at the first `]]` after the opener. An unescaped `[` or
`]` before that `]]` makes the opener text, and scanning resumes at the next
byte, so a triple bracket is text, a cross link, and text:

```````````````````````````````` example
[[[Note]]]
.
Document scope=1:1..1:10 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:10 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:1 anchor=null attributes={} literal="[" children=0
    ├── CrossLink scope=1:2..1:9 anchor=null attributes={} dest=cross(path="Note",anchor=null) label=null children=0
    └── Text scope=1:10..1:10 anchor=null attributes={} literal="]" children=0
````````````````````````````````

`\[[Note]]` is an inherited escape and is text. `\![[Note]]` is a literal `!`
followed by a `CrossLink`:

```````````````````````````````` example
\[[Note]]

\![[Note]]
.
Document scope=1:1..3:10 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:9 anchor=null attributes={} children=1
│   └── Text scope=1:1..1:9 anchor=null attributes={} literal="[[Note]]" children=0
└── Paragraph scope=3:1..3:10 anchor=null attributes={} children=2
    ├── Text scope=3:1..3:2 anchor=null attributes={} literal="!" children=0
    └── CrossLink scope=3:3..3:10 anchor=null attributes={} dest=cross(path="Note",anchor=null) label=null children=0
````````````````````````````````

A recognized cross link is complete at its `]]`. A following `(`, `[`, or `{`
is text, so no link, reference, or span forms around it:

```````````````````````````````` example
[[Note]](url) [[Note]]{.c}
.
Document scope=1:1..1:26 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:26 anchor=null attributes={} children=4
    ├── CrossLink scope=1:1..1:8 anchor=null attributes={} dest=cross(path="Note",anchor=null) label=null children=0
    ├── Text scope=1:9..1:14 anchor=null attributes={} literal="(url) " children=0
    ├── CrossLink scope=1:15..1:22 anchor=null attributes={} dest=cross(path="Note",anchor=null) label=null children=0
    └── Text scope=1:23..1:26 anchor=null attributes={} literal="{.c}" children=0
````````````````````````````````

Link content and image alt content may contain a cross link:

```````````````````````````````` example
[a [[Note]] b](/u)
.
Document scope=1:1..1:18 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:18 anchor=null attributes={} children=1
    └── Link scope=1:1..1:18 anchor=null attributes={} dest=url("/u") title=null children=3
        ├── Text scope=1:2..1:3 anchor=null attributes={} literal="a " children=0
        ├── CrossLink scope=1:4..1:11 anchor=null attributes={} dest=cross(path="Note",anchor=null) label=null children=0
        └── Text scope=1:12..1:13 anchor=null attributes={} literal=" b" children=0
````````````````````````````````

A line ending before the `]]` makes the opener text:

```````````````````````````````` example
[[No
te]]
.
Document scope=1:1..2:4 anchor=null attributes={} children=1
└── Paragraph scope=1:1..2:4 anchor=null attributes={} children=3
    ├── Text scope=1:1..1:4 anchor=null attributes={} literal="[[No" children=0
    ├── SoftBreak scope=1:5..1:5 anchor=null attributes={} children=0
    └── Text scope=2:1..2:4 anchor=null attributes={} literal="te]]" children=0
````````````````````````````````

Source owned by code spans, HTML tokens, comments, and formulas is opaque to
this module, and a completed cross link is opaque to every later step:

```````````````````````````````` example
`[[a]]` $[[b]]$ <!-- [[c]] -->
.
Document scope=1:1..1:30 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:30 anchor=null attributes={} children=5
    ├── Code scope=1:1..1:7 anchor=null attributes={} literal="[[a]]" children=0
    ├── Text scope=1:8..1:8 anchor=null attributes={} literal=" " children=0
    ├── Formula scope=1:9..1:15 anchor=null attributes={} mode=embedded literal="[[b]]" children=0
    ├── Text scope=1:16..1:16 anchor=null attributes={} literal=" " children=0
    └── Comment scope=1:17..1:30 anchor=null attributes={} literal=" [[c]] " children=0
````````````````````````````````

## Tables

In every table syntax that parses cell content, a `\|` inside a cell is not a cell boundary and reaches the inline scanner,
where it is the label separator inside `[[...]]` and the inherited escaped
pipe elsewhere. An unescaped `|` inside a cross link candidate in a table row
splits the cell, and the unmatched `[[` bytes are text. There is no
table-specific cross-link parser and the inherited delimiter-row grammar is
unchanged.

```````````````````````````````` example
| x | y |
| - | - |
| [[a\|b]] | c |
| [[a | b]] |
.
Document scope=1:1..4:13 anchor=null attributes={} children=1
└── Table scope=1:1..4:13 anchor=null attributes={} columns=[none:null,none:null] children=3
    ├── TableHead children=1
    │   └── TableRow scope=1:1..1:9 anchor=null attributes={} children=2
    │       ├── TableCell scope=1:2..1:4 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:3..1:3 anchor=null attributes={} literal="x" children=0
    │       └── TableCell scope=1:6..1:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=1:7..1:7 anchor=null attributes={} literal="y" children=0
    ├── TableBody children=2
    │   ├── TableRow scope=3:1..3:16 anchor=null attributes={} children=2
    │   │   ├── TableCell scope=3:2..3:11 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │   │   └── CrossLink scope=3:3..3:10 anchor=null attributes={} dest=cross(path="a",anchor=null) label="b" children=0
    │   │   └── TableCell scope=3:13..3:15 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │   │       └── Text scope=3:14..3:14 anchor=null attributes={} literal="c" children=0
    │   └── TableRow scope=4:1..4:13 anchor=null attributes={} children=2
    │       ├── TableCell scope=4:2..4:6 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=4:3..4:5 anchor=null attributes={} literal="[[a" children=0
    │       └── TableCell scope=4:8..4:12 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=4:9..4:11 anchor=null attributes={} literal="b]]" children=0
    └── TableFoot children=0
````````````````````````````````

## Autolinks

A GFM bare URL or `www.` autolink is an earlier scanner step whose run is
opaque, as the [links and images](links-and-images.md) module states, so
`[[y]]` inside a URL is URL text and no cross link forms there:

```````````````````````````````` example
www.x.com/[[y]] z
.
Document scope=1:1..1:17 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:17 anchor=null attributes={} children=2
    ├── Link scope=1:1..1:15 anchor=null attributes={} dest=url("http://www.x.com/[[y]]") title=null children=1
    │   └── Text scope=1:1..1:15 anchor=null attributes={} literal="www.x.com/[[y]]" children=0
    └── Text scope=1:16..1:17 anchor=null attributes={} literal=" z" children=0
````````````````````````````````

## Downstream meaning

For a note, an anchor addresses a heading or an identified block; for another
file kind, raw values such as `page=3`, `height=400`, or `outline` are
interpreted by the resolver. Dimensions describe an authored size request,
independently of the resolved resource type. The parser never fetches the target.

## Dimensions

Only `CrossEmbedded` (`![[...]]`) recognizes a complete `W`, `WxH`,
`label|W`, or `label|WxH` in its authored label. It uses the same `Dimensions`
value and positive 32-bit integer grammar as `Media`: no leading zero, signs,
spaces, uppercase `X`, missing component, overflow or trailing bytes. The last
label separator is the only suffix candidate, and ASCII whitespace immediately before
it invalidates the suffix. Both `|` and `\|` are separators, matching the cross-link
grammar; tables use the existing escaped-pipe contraction and source map.

A valid suffix is removed from `label`; every preceding byte stays raw. A label
consisting only of dimensions becomes `""`, retaining the distinction from no
separator (`null`). Empty and malformed labels stay exactly as authored and have
`dimensions=null`. Ordinary `[[...]]` always retains its entire raw label and has
no dimensions field. No filename extension, destination anchor or resolver result
participates in recognition. Raw label bytes are not parsed as emphasis, code,
HTML or entities, so those constructs never gain inline nodes inside either cross-reference kind.

```````````````````````````````` example
![[movie.mp4|raw *caption*|100x200]] ![[audio|300]] [[Note|100x200]]
.
Document scope=1:1..1:68 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:68 anchor=null attributes={} children=5
    ├── CrossEmbedded scope=1:1..1:36 anchor=null attributes={} dest=cross(path="movie.mp4",anchor=null) label="raw *caption*" dimensions=(width=100,height=200) children=0
    ├── Text scope=1:37..1:37 anchor=null attributes={} literal=" " children=0
    ├── CrossEmbedded scope=1:38..1:51 anchor=null attributes={} dest=cross(path="audio",anchor=null) label="" dimensions=(width=300,height=null) children=0
    ├── Text scope=1:52..1:52 anchor=null attributes={} literal=" " children=0
    └── CrossLink scope=1:53..1:68 anchor=null attributes={} dest=cross(path="Note",anchor=null) label="100x200" children=0
````````````````````````````````

```````````````````````````````` example
![[Note]] ![[Note|]] ![[Note|bad|01]] ![[Note|bad |10]]
.
Document scope=1:1..1:55 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:55 anchor=null attributes={} children=7
    ├── CrossEmbedded scope=1:1..1:9 anchor=null attributes={} dest=cross(path="Note",anchor=null) label=null dimensions=null children=0
    ├── Text scope=1:10..1:10 anchor=null attributes={} literal=" " children=0
    ├── CrossEmbedded scope=1:11..1:20 anchor=null attributes={} dest=cross(path="Note",anchor=null) label="" dimensions=null children=0
    ├── Text scope=1:21..1:21 anchor=null attributes={} literal=" " children=0
    ├── CrossEmbedded scope=1:22..1:37 anchor=null attributes={} dest=cross(path="Note",anchor=null) label="bad|01" dimensions=null children=0
    ├── Text scope=1:38..1:38 anchor=null attributes={} literal=" " children=0
    └── CrossEmbedded scope=1:39..1:55 anchor=null attributes={} dest=cross(path="Note",anchor=null) label="bad |10" dimensions=null children=0
````````````````````````````````

```````````````````````````````` example
| embed | ordinary |
| -- | -- |
| ![[Note\|é\|10x20]] | [[Note\|100]] |
.
Document scope=1:1..3:40 anchor=null attributes={} children=1
└── Table scope=1:1..3:40 anchor=null attributes={} columns=[none:null,none:null] children=2
    ├── TableHead children=1
    │   └── TableRow scope=1:1..1:20 anchor=null attributes={} children=2
    │       ├── TableCell scope=1:2..1:8 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── Text scope=1:3..1:7 anchor=null attributes={} literal="embed" children=0
    │       └── TableCell scope=1:10..1:19 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── Text scope=1:11..1:18 anchor=null attributes={} literal="ordinary" children=0
    ├── TableBody children=1
    │   └── TableRow scope=3:1..3:40 anchor=null attributes={} children=2
    │       ├── TableCell scope=3:2..3:23 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │       │   └── CrossEmbedded scope=3:3..3:22 anchor=null attributes={} dest=cross(path="Note",anchor=null) label="é" dimensions=(width=10,height=20) children=0
    │       └── TableCell scope=3:25..3:39 anchor=null attributes={} rowspan=1 colspan=1 children=1
    │           └── CrossLink scope=3:26..3:38 anchor=null attributes={} dest=cross(path="Note",anchor=null) label="100" children=0
    └── TableFoot children=0
````````````````````````````````


## Scopes

`CrossLink.scope` and `CrossEmbedded.scope` cover the complete opener, both
delimiter pairs, and every
byte between. Field values contain no delimiter bytes.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover `\|`
outside tables, single brackets, all opaque contexts, a cross link inside
image content, exact scopes, allocation failure at every node and string,
and size-doubling inputs made from `!`, `[`, `]`, `#`, `^`, and `|`.
