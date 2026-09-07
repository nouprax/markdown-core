# Links and images

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the shared `Destination` value, the consumer model of every link and
image form, the resolved reference model, the ordered bracket procedure that
every bracket-closing module participates in, GFM bare autolinks, and the
Obsidian image-dimension suffix. Sources: CommonMark links, images, and
reference definitions; cmark-gfm's autolink extension; Obsidian's external
image dimensions. Executable oracles: cmark and cmark-gfm; image dimensions
are product fixtures. Landing: `Destination` with `M1`, resolved references
with `M2`, dimensions with `O9`. The
[example format](../dialect.md#examples) is defined by the index.

## Model

```text
Destination =
  url(String)
  | cross(path: String, anchor: String?)

Link(dest: Destination, title: String?, content: [Markup])
Image(dest: Destination, title: String?, width: Int?, height: Int?,
      content: [Markup])
```

`Destination` is a tagged value, not a node: it has no scope, children,
anchor, or attributes, and branch fields exist only in their branch. Every
`Link` and `Image` owns the `url` branch; every `CrossLink` of the
[cross links](cross-links.md) module owns the `cross` branch. `Link.content`
is the parsed label content and `Image.content` the parsed alt content.
`Int` is a 32-bit signed integer on every surface.

`url` holds the complete semantic destination produced by the inherited
grammar: the bytes between angle brackets or the bare destination, with
CommonMark backslash escapes and character references decoded and no
percent-encoding, normalization, or resolution. It may be empty. `title` is
the decoded title, or `null` when none was written; absent and empty titles
remain distinct:

```````````````````````````````` example
[text](/url "title") [a]() [b](<> "")
.
Document scope=1:1..1:37 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:37 anchor=null attributes={} children=5
    ├── Link scope=1:1..1:20 anchor=null attributes={} dest=url("/url") title="title" children=1
    │   └── Text scope=1:2..1:5 anchor=null attributes={} literal="text" children=0
    ├── Text scope=1:21..1:21 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:22..1:26 anchor=null attributes={} dest=url("") title=null children=1
    │   └── Text scope=1:23..1:23 anchor=null attributes={} literal="a" children=0
    ├── Text scope=1:27..1:27 anchor=null attributes={} literal=" " children=0
    └── Link scope=1:28..1:37 anchor=null attributes={} dest=url("") title="" children=1
        └── Text scope=1:29..1:29 anchor=null attributes={} literal="b" children=0
````````````````````````````````

```````````````````````````````` example
[a](</u\)x> "t&amp;") [b](/u\)x)
.
Document scope=1:1..1:32 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:32 anchor=null attributes={} children=3
    ├── Link scope=1:1..1:21 anchor=null attributes={} dest=url("/u)x") title="t&" children=1
    │   └── Text scope=1:2..1:2 anchor=null attributes={} literal="a" children=0
    ├── Text scope=1:22..1:22 anchor=null attributes={} literal=" " children=0
    └── Link scope=1:23..1:32 anchor=null attributes={} dest=url("/u)x") title=null children=1
        └── Text scope=1:24..1:24 anchor=null attributes={} literal="b" children=0
````````````````````````````````

The parser does not fetch a URL, open a file, test existence, or infer a media
type; no such result is a field or a branch.

## Resolved references

Every successful link form produces the same node:

```text
[text](url "title")   \
[text][label]          |
[text][]               |--> Link(dest=url(url), title, content, scope)
[label]                |
<autolink>            /
```

and every successful direct or reference image produces `Image`. A reference
resolves through the parser-owned reference map: one lookup of the normalized
label per candidate, the first definition in source order winning among
duplicates, and the inherited definition grammar deciding what is a
definition. The resolved occurrence takes the definition's destination and
title, keeps the content authored at the occurrence, and keeps the scope of
its own occurrence; the definition's range is never copied, unioned, or
substituted. A definition is parser state and produces no node; an
unreferenced definition produces nothing. A reference whose label resolves to
no definition, and bracket text that satisfies no form, is the inherited
literal text with its brackets:

```````````````````````````````` example
[text][r] [r][] [r] [text][none]

[r]: /url "title"
.
Document scope=1:1..3:17 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:32 anchor=null attributes={} children=6
    ├── Link scope=1:1..1:9 anchor=null attributes={} dest=url("/url") title="title" children=1
    │   └── Text scope=1:2..1:5 anchor=null attributes={} literal="text" children=0
    ├── Text scope=1:10..1:10 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:11..1:15 anchor=null attributes={} dest=url("/url") title="title" children=1
    │   └── Text scope=1:12..1:12 anchor=null attributes={} literal="r" children=0
    ├── Text scope=1:16..1:16 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:17..1:19 anchor=null attributes={} dest=url("/url") title="title" children=1
    │   └── Text scope=1:18..1:18 anchor=null attributes={} literal="r" children=0
    └── Text scope=1:20..1:32 anchor=null attributes={} literal=" [text][none]" children=0
````````````````````````````````

```````````````````````````````` example
[r]

[r]: /first
[r]: /second
.
Document scope=1:1..4:12 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:3 anchor=null attributes={} children=1
    └── Link scope=1:1..1:3 anchor=null attributes={} dest=url("/first") title=null children=1
        └── Text scope=1:2..1:2 anchor=null attributes={} literal="r" children=0
````````````````````````````````

```````````````````````````````` example
![alt *em*](/i.png "t") ![alt][r]

[r]: /r.png
.
Document scope=1:1..3:11 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:33 anchor=null attributes={} children=3
    ├── Image scope=1:1..1:23 anchor=null attributes={} dest=url("/i.png") title="t" width=null height=null children=2
    │   ├── Text scope=1:3..1:6 anchor=null attributes={} literal="alt " children=0
    │   └── Emphasis scope=1:7..1:10 anchor=null attributes={} children=1
    │       └── Text scope=1:8..1:9 anchor=null attributes={} literal="em" children=0
    ├── Text scope=1:24..1:24 anchor=null attributes={} literal=" " children=0
    └── Image scope=1:25..1:33 anchor=null attributes={} dest=url("/r.png") title=null width=null height=null children=1
        └── Text scope=1:27..1:29 anchor=null attributes={} literal="alt" children=0
````````````````````````````````

Two occurrences resolved through one definition share the definition's
resource: the C tree stores a long destination or title once,
`markdown_core_node_resource` answers one identity for both, and each binding
materializes that resource once and lets every occurrence read the same
value. The public AST has no `LinkReference`, `ImageReference`,
`ReferenceDefinition`, or `ReferenceForm`, and no reference is modeled as a
`Citation` or through a document link registry.

## The bracket procedure

At every unescaped `]` that matches an active bracket opener, the inline
parser tests these alternatives in order and takes the first success. A failed
alternative leaves the cursor at the `]`; the container after a failed
alternative is text.

1. A valid direct tail `(...)` produces `Link` or `Image`; a following
   container attaches.
2. A full `[label]` or collapsed `[]` tail whose label resolves, explicitly or
   through a virtual heading definition, produces `Link` or `Image`; a
   following container attaches. A tail whose label does not resolve does
   not block the later alternatives.
3. A valid attribute container beginning at the byte after `]` produces a
   `Span`.
4. A valid cite group produces a `Cite`.
5. A shortcut reference whose label resolves, not followed by `[]` or by a
   link label, produces `Link` or `Image`; a following container belongs to
   alternative 3, so none attaches here.
6. A `[^label]` whose label is defined is a footnote call and produces a
   `Cite`; the [footnotes](footnotes.md) module states it. As cmark-gfm
   tests it, the call is the last alternative before the literal fallback, so
   a reference tail or a shortcut reference that resolves wins over it.
7. Otherwise the pair is the inherited literal text.

For an image opener `![`, alternatives 3 through 6 yield a literal `!`
followed by the node. A `[[` is claimed by the cross-link
scanner before this procedure runs, and a text directive's label is claimed by
the directive scanner; neither reaches this procedure. The modules named in
each step show the examples of their alternative.

## Autolinks

Angle-bracket autolinks `<https://example.com>` and `<user@example.com>` are
inherited, at inline step A3, and the three GFM bare forms of cmark-gfm's
extension are recognized at the same step:

- The URL form is a scanner step of class A, listed at A3: at a `:` followed
  by `//`, the scanner rewinds over the preceding ASCII letters and accepts
  the candidate when they spell `http`, `https`, or `ftp` in any case and a
  valid domain follows.
- The `www.` form is the same scanner step at a `w` that begins `www.` at the
  start of the container or after whitespace, `*`, `_`, `~`, or `(`, with a
  domain containing at least one dot.
- The email form is step E, a post-pass over `Text` nodes only.

A URL or `www.` candidate extends to the first whitespace or `<`; then
trailing `?`, `!`, `.`, `,`, `:`, `*`, `_`, `~`, `'`, `"`, a trailing
entity-shaped `&...;`, and every `)` beyond the number of `(` inside the run
are excluded from its end. Neither form is recognized inside an open bracket.
Every bare form produces `Link(dest=url(...), title=null)` with the link text
as content; an email destination carries the inherited `mailto:` prefix and a
`www.` destination the inherited `http://` prefix:

```````````````````````````````` example
<https://x.y> https://x.y/z www.x.y x@y.z <x@y.z>
.
Document scope=1:1..1:49 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:49 anchor=null attributes={} children=9
    ├── Link scope=1:1..1:13 anchor=null attributes={} dest=url("https://x.y") title=null children=1
    │   └── Text scope=1:2..1:12 anchor=null attributes={} literal="https://x.y" children=0
    ├── Text scope=1:14..1:14 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:15..1:27 anchor=null attributes={} dest=url("https://x.y/z") title=null children=1
    │   └── Text scope=1:15..1:27 anchor=null attributes={} literal="https://x.y/z" children=0
    ├── Text scope=1:28..1:28 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:29..1:35 anchor=null attributes={} dest=url("http://www.x.y") title=null children=1
    │   └── Text scope=1:29..1:35 anchor=null attributes={} literal="www.x.y" children=0
    ├── Text scope=1:36..1:36 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:37..1:41 anchor=null attributes={} dest=url("mailto:x@y.z") title=null children=1
    │   └── Text scope=1:37..1:41 anchor=null attributes={} literal="x@y.z" children=0
    ├── Text scope=1:42..1:42 anchor=null attributes={} literal=" " children=0
    └── Link scope=1:43..1:49 anchor=null attributes={} dest=url("mailto:x@y.z") title=null children=1
        └── Text scope=1:44..1:48 anchor=null attributes={} literal="x@y.z" children=0
````````````````````````````````

Because the URL and `www.` forms are scanner steps, their run is opaque from
its first byte to its terminator: a code span, formula, emphasis delimiter,
or cross link that begins inside the run is URL text, an `<` ends the run,
and a bare autolink never accepts an attribute container, since the
termination rule applies to the braces:

```````````````````````````````` example
https://x.y/*a* www.x.y/`b`. https://x.y/<b>c</b> https://x.y{.c}
.
Document scope=1:1..1:65 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:65 anchor=null attributes={} children=10
    ├── Link scope=1:1..1:14 anchor=null attributes={} dest=url("https://x.y/*a") title=null children=1
    │   └── Text scope=1:1..1:14 anchor=null attributes={} literal="https://x.y/*a" children=0
    ├── Text scope=1:15..1:16 anchor=null attributes={} literal="* " children=0
    ├── Link scope=1:17..1:27 anchor=null attributes={} dest=url("http://www.x.y/`b`") title=null children=1
    │   └── Text scope=1:17..1:27 anchor=null attributes={} literal="www.x.y/`b`" children=0
    ├── Text scope=1:28..1:29 anchor=null attributes={} literal=". " children=0
    ├── Link scope=1:30..1:41 anchor=null attributes={} dest=url("https://x.y/") title=null children=1
    │   └── Text scope=1:30..1:41 anchor=null attributes={} literal="https://x.y/" children=0
    ├── HTML scope=1:42..1:44 anchor=null attributes={} literal="<b>" children=0
    ├── Text scope=1:45..1:45 anchor=null attributes={} literal="c" children=0
    ├── HTML scope=1:46..1:49 anchor=null attributes={} literal="</b>" children=0
    ├── Text scope=1:50..1:50 anchor=null attributes={} literal=" " children=0
    └── Link scope=1:51..1:65 anchor=null attributes={} dest=url("https://x.y{.c}") title=null children=1
        └── Text scope=1:51..1:65 anchor=null attributes={} literal="https://x.y{.c}" children=0
````````````````````````````````

The run is decided at its own position, so surrounding delimiters still match
around it, and no bare form is recognized inside link text:

```````````````````````````````` example
*https://x.y* [https://x.y](/u) (www.x.y)
.
Document scope=1:1..1:41 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:41 anchor=null attributes={} children=6
    ├── Emphasis scope=1:1..1:13 anchor=null attributes={} children=1
    │   └── Link scope=1:2..1:12 anchor=null attributes={} dest=url("https://x.y") title=null children=1
    │       └── Text scope=1:2..1:12 anchor=null attributes={} literal="https://x.y" children=0
    ├── Text scope=1:14..1:14 anchor=null attributes={} literal=" " children=0
    ├── Link scope=1:15..1:31 anchor=null attributes={} dest=url("/u") title=null children=1
    │   └── Text scope=1:16..1:26 anchor=null attributes={} literal="https://x.y" children=0
    ├── Text scope=1:32..1:33 anchor=null attributes={} literal=" (" children=0
    ├── Link scope=1:34..1:40 anchor=null attributes={} dest=url("http://www.x.y") title=null children=1
    │   └── Text scope=1:34..1:40 anchor=null attributes={} literal="www.x.y" children=0
    └── Text scope=1:41..1:41 anchor=null attributes={} literal=")" children=0
````````````````````````````````

## Image dimensions

An image whose alt label ends with one of these
complete suffixes receives typed dimensions:

```text
W
WxH
alt|W
alt|WxH
```

`W` and `H` are ASCII digit strings with no leading zero and values from 1 to
2147483647, `x` is lowercase, and no whitespace surrounds `x` or `|`. For a
numeric-only label the alt content is empty; for a pipe form the bytes before
the pipe are the alt content, parsed by the inline parser, and may be empty.
`width` is `W`; `height` is `H` or `null` for a width-only form:

```````````````````````````````` example
![100x145](a.png)

![alt|100](a.png) ![alt|100x145](a.png) ![|200](a.png)
.
Document scope=1:1..3:54 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:17 anchor=null attributes={} children=1
│   └── Image scope=1:1..1:17 anchor=null attributes={} dest=url("a.png") title=null width=100 height=145 children=0
└── Paragraph scope=3:1..3:54 anchor=null attributes={} children=5
    ├── Image scope=3:1..3:17 anchor=null attributes={} dest=url("a.png") title=null width=100 height=null children=1
    │   └── Text scope=3:3..3:5 anchor=null attributes={} literal="alt" children=0
    ├── Text scope=3:18..3:18 anchor=null attributes={} literal=" " children=0
    ├── Image scope=3:19..3:39 anchor=null attributes={} dest=url("a.png") title=null width=100 height=145 children=1
    │   └── Text scope=3:21..3:23 anchor=null attributes={} literal="alt" children=0
    ├── Text scope=3:40..3:40 anchor=null attributes={} literal=" " children=0
    └── Image scope=3:41..3:54 anchor=null attributes={} dest=url("a.png") title=null width=200 height=null children=0
````````````````````````````````

The suffix is matched against the raw source bytes between the last top-level
unescaped `|` that is not inside a code span or nested brackets and the
closing `]`; for a label with no such pipe, against the whole label:

```````````````````````````````` example
![*a* `b|c`|300](a.png)
.
Document scope=1:1..1:23 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:23 anchor=null attributes={} children=1
    └── Image scope=1:1..1:23 anchor=null attributes={} dest=url("a.png") title=null width=300 height=null children=3
        ├── Emphasis scope=1:3..1:5 anchor=null attributes={} children=1
        │   └── Text scope=1:4..1:4 anchor=null attributes={} literal="a" children=0
        ├── Text scope=1:6..1:6 anchor=null attributes={} literal=" " children=0
        └── Code scope=1:7..1:11 anchor=null attributes={} literal="b|c" children=0
````````````````````````````````

Zero, a leading zero, a value above the limit, signs, whitespace, missing
components, or non-decimal components produce no dimensions, and the whole
label is alt content:

```````````````````````````````` example
![0x1](a.png)

![alt| 100](a.png)
.
Document scope=1:1..3:18 anchor=null attributes={} children=2
├── Paragraph scope=1:1..1:13 anchor=null attributes={} children=1
│   └── Image scope=1:1..1:13 anchor=null attributes={} dest=url("a.png") title=null width=null height=null children=1
│       └── Text scope=1:3..1:5 anchor=null attributes={} literal="0x1" children=0
└── Paragraph scope=3:1..3:18 anchor=null attributes={} children=1
    └── Image scope=3:1..3:18 anchor=null attributes={} dest=url("a.png") title=null width=null height=null children=1
        └── Text scope=3:3..3:10 anchor=null attributes={} literal="alt| 100" children=0
````````````````````````````````

The rule applies to direct and resolved reference images alike:

```````````````````````````````` example
![alt|100][r]

[r]: /i.png
.
Document scope=1:1..3:11 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:13 anchor=null attributes={} children=1
    └── Image scope=1:1..1:13 anchor=null attributes={} dest=url("/i.png") title=null width=100 height=null children=1
        └── Text scope=1:3..1:5 anchor=null attributes={} literal="alt" children=0
````````````````````````````````

A `width` or `height` attribute record is independent:
it never populates the typed fields, and the typed fields never produce a
record. Internal image embeds are `CrossLink` values whose `label` stays raw;
this rule does not apply to them.

## Scopes

`Link.scope` and `Image.scope` cover the opener, the content, the tail, and
an occurrence-local attribute container. An autolink's scope covers the angle
brackets or the bare URL. Content and alt child scopes end before a dimension
suffix, and the suffix is inside the image's scope.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover
fragment-only destinations; unused definitions; the identical dump of a direct
and a resolved occurrence apart from scope; the shared-resource bound for a
long destination or title referenced many times, on every surface; every step
of the bracket procedure; bare
autolinks ending at every node boundary; every valid and invalid dimension
form, pipes inside brackets, the limit, and coexistence with dimension
records; exact scopes; allocation failure; and size-doubling brackets,
parentheses, URLs, and digit runs.
