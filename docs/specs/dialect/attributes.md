# Attributes

Status: normative module of the [Markdown Core dialect](../dialect.md). It owns
the universal `Markup.attributes` field, the one braced attribute grammar of
the dialect, its normalization and merge operations, and every attachment site.
The [directives](directives.md) and [bracketed spans](bracketed-spans.md)
modules attach the same grammar. Source: Pandoc 3.11's attribute syntax and
its `combineAttr` operation, and `remark-directive`'s attachment position.
Executable oracle: the Pandoc 3.11 CLI under `specs/oracles/pandoc/`; remark's
own attribute member tokenizer is not an oracle for this grammar, and every
difference from it is a registered delta. Landing: the field and the directive
site with `M7`; the inline code, heading, fenced code, and link sites with
`P2a` through `P2d`. The [example format](../dialect.md#examples) is defined
by the index.

## Model

```text
Record(name: String, value: String)
Attributes(classes: [String], records: [Record])
Markup(anchor: String?, attributes: Attributes, scope: Scope, ...)
```

Every `Markup` kind has exactly one non-null `attributes` field and exactly
one nullable `anchor` field; the [anchors](anchors.md) module owns the latter.
They are fields of the tagged union, not a wrapper and not an opt-in
capability. `Record` and `Attributes` are values with no scope, children, or
attributes of their own. There is one payload for every kind; no kind has a
private attribute type, and no lookup over `records` is a second stored
authority. The dump prints the two fields on every node line as
`anchor=<string or null>` and `attributes={...}`, where the braces hold the
classes as `.name` and the records as `name="value"` in order, and
`Attributes.empty` prints as `attributes={}`.

Class dump tokens use `.name` for non-empty printable ASCII strings excluding
`"`, `\`, `{`, `}`, `[`, `]`, `(`, `)`, and `=`. Every other class uses `.`
followed by a JSON string, for example `."a}b"` or `."中文"`. This escaping is
only dump syntax; it never changes the stored class or the attribute grammar.


`Attributes.empty` is `classes=[]` and `records=[]`. It is the value when the
kind has no enabled attachment rule, when no container was authored, when an
authored container was `{}`, when a container held only an ID, and when an
attachment failed. The public AST does not record which of these occurred.

- `classes` preserves order and duplicates. A class shorthand appends one
  class; an exact lowercase `class=` assignment splits its value on ASCII
  whitespace and Zs scalars, drops empty words, and appends the words; the
  special member `-` appends `unnumbered`.
- `records` holds every other assignment in source order, duplicates
  included, names and values case-sensitive and as written after decoding.
  The exact names `id` and `class` never appear in `records`.

Attributes are inert metadata. Parsing a value never executes anything, opens
a target, reads a file, validates a unit, or changes layout. `width=50%`,
`target=_blank`, `numberLines`, and an event-handler-looking name are records
like any other.

## Grammar

```text
attributes        = "{" spacing *( attribute spacing ) "}"
attribute         = identifier / class / assignment / special
identifier        = "#" 1*identifier-character
class             = "." name
assignment        = name "=" value
special           = "-"
name              = unicode-letter *name-rest
name-rest         = unicode-letter / unicode-number / "-" / "_" / ":" / "."
identifier-character
                  = unicode-letter / unicode-number / "-" / "_" / ":" / "."
value             = quoted-value / unquoted-value
quoted-value      = DQUOTE *double-quoted-character DQUOTE /
                    "'" *single-quoted-character "'"
unquoted-value    = *unquoted-character
double-quoted-character
                  = escaped-punctuation / character-reference /
                    permitted-line-ending /
                    any scalar except DQUOTE or line-ending
single-quoted-character
                  = escaped-punctuation / character-reference /
                    permitted-line-ending /
                    any scalar except "'" or line-ending
unquoted-character
                  = escaped-punctuation /
                    any scalar except SP, TAB, line-ending, or unescaped "}"
spacing           = *( SP / TAB ) [ line-ending *( SP / TAB ) ]
line-ending       = LF / CR / CRLF
permitted-line-ending
                  = a line ending inside the extent the owning module grants
                    the container and not followed by a blank line
escaped-punctuation
                  = "\" followed by one ASCII punctuation character
character-reference
                  = a CommonMark named or numeric character reference
unicode-letter    = a scalar of category Lu, Ll, Lt, Lm, or Lo
unicode-number    = a scalar of category Nd, Nl, or No
```

The grammar runs over the owning block's inline content string after block
structure has been decided, so container prefixes are already removed. It is
applied to scalars, not bytes. `spacing` admits at most one line ending and
never a blank line; it is optional, so independently delimited members may be
adjacent. An identifier is non-empty, may begin with any identifier
character, and keeps its dots and colons. A class or assignment name begins
with a letter. A value begins after `=`. If the first scalar is `"` or `'`
and a matching closing quote occurs before the end of the container, the
value is the quoted value; otherwise it is the unquoted value, and no other
backtracking occurs. A quoted value may be empty, decodes its escapes and its
semicolon-terminated character references, and normalizes each permitted line
ending to one ASCII space. An unquoted value may be empty, extends to ASCII
space, tab, a line ending, or an unescaped `}`, decodes escapes but not
character references, and admits quotes, `<`, `=`, `>`, and backticks as
ordinary content. The examples below show the grammar on inline code:

```````````````````````````````` example
`x`{#a#b}

`x`{-k=v}

`x`{.one.two #1}

`x`{k="a\"b&amp;c" m=a&amp;b n= id=}

`x`{class="a b" .a k=1 k=2}
.
Document scope=1:1..9:27 anchor=null attributes={} children=5
├── Paragraph scope=1:1..1:9 anchor=null attributes={} children=1
│   └── Code scope=1:1..1:9 anchor="b" attributes={} literal="x" children=0
├── Paragraph scope=3:1..3:9 anchor=null attributes={} children=1
│   └── Code scope=3:1..3:9 anchor=null attributes={.unnumbered k="v"} literal="x" children=0
├── Paragraph scope=5:1..5:16 anchor=null attributes={} children=1
│   └── Code scope=5:1..5:16 anchor="1" attributes={.one.two} literal="x" children=0
├── Paragraph scope=7:1..7:36 anchor=null attributes={} children=1
│   └── Code scope=7:1..7:36 anchor=null attributes={k="a\"b&c" m="a&amp;b" n=""} literal="x" children=0
└── Paragraph scope=9:1..9:27 anchor=null attributes={} children=1
    └── Code scope=9:1..9:27 anchor=null attributes={.a .b .a k="1" k="2"} literal="x" children=0
````````````````````````````````

A generic bare name such as `{disabled}` is malformed; the lone `-` is the
only value-less member. `{.1}` and `{_key=value}` are malformed because a
class or assignment name begins with a letter. A malformed container attaches
nothing and its `{` is text:

```````````````````````````````` example
`x`{.1} `x`{disabled} `x`{_k=v}
.
Document scope=1:1..1:31 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:31 anchor=null attributes={} children=6
    ├── Code scope=1:1..1:3 anchor=null attributes={} literal="x" children=0
    ├── Text scope=1:4..1:8 anchor=null attributes={} literal="{.1} " children=0
    ├── Code scope=1:9..1:11 anchor=null attributes={} literal="x" children=0
    ├── Text scope=1:12..1:22 anchor=null attributes={} literal="{disabled} " children=0
    ├── Code scope=1:23..1:25 anchor=null attributes={} literal="x" children=0
    └── Text scope=1:26..1:31 anchor=null attributes={} literal="{_k=v}" children=0
````````````````````````````````

A backslash before a non-punctuation scalar is literal. Non-ASCII whitespace
is not `spacing`: it may occur inside a value but cannot separate two
members. Escaped and referenced scalars inside identifier and class
shorthands are not decoded; those members are as written. A permitted line
ending inside a quoted value becomes one space:

```````````````````````````````` example
[a]{k="x
y"}
.
Document scope=1:1..2:3 anchor=null attributes={} children=1
└── Paragraph scope=1:1..2:3 anchor=null attributes={} children=1
    └── Span scope=1:1..2:3 anchor=null attributes={k="x y"} children=1
        └── Text scope=1:2..1:2 anchor=null attributes={} literal="a" children=0
````````````````````````````````

## Normalization

Normalization runs exactly once, when an attachment commits, and yields one
anchor candidate and one `Attributes` value:

1. The anchor candidate is the value of the last identifier or exact lowercase
   `id=` assignment in source order; an empty final `id=` makes it `null`.
2. Each `.value` appends one class; each `class=value` appends its words; each
   `-` appends `unnumbered`.
3. Every other assignment appends one `Record` in source order.

`merge(primary, inherited)` combines an occurrence's own values with the
values inherited from a reference definition:

1. The anchor is the primary anchor when non-null, otherwise the inherited
   anchor.
2. The classes are the inherited classes followed by the primary classes,
   in source order and duplicates included.
3. The records are the inherited records followed by the primary records,
   in source order and duplicates included.

Nothing authored is dropped, so a consumer that resolves a name last-wins
sees the occurrence's own value, and one that wants every declaration has
them all. These three numbered steps are normative; Pandoc's `combineAttr`
is their provenance and evidence, and its deduplication of classes and
records is a registered divergence of the Pandoc gate. Merge transfers
semantic values only and never changes the occurrence's scope.

## Attachment sites

Only these rules may populate `attributes` and the attribute-side anchor
candidate. Every other kind keeps `Attributes.empty`, and under these rules no
table, row, cell, or caption receives attributes or an anchor; a
[block identifier](block-identifiers.md) may still populate `Table.anchor`.

| Site                                           | Owner                          | Position                                                              |
| ---------------------------------------------- | ------------------------------ | --------------------------------------------------------------------- |
| text, leaf, and container directive            | `Directive`, `DirectiveBlock`  | immediately after the name or its label                               |
| inline code                                    | `Code`                         | immediately after the complete closing backtick run                   |
| ATX and Setext heading                         | `Heading`                      | the last non-whitespace bytes of the heading's content line           |
| fenced code                                    | `CodeBlock`                    | the last non-whitespace content of the opening fence line             |
| direct, reference, and autolink link and image | `Link`, `Media`                | immediately after the occurrence, or inherited from its definition    |
| bracketed span                                 | `Span`                         | immediately after the balanced closing `]`                            |
| nameless container directive                   | `DirectiveBlock`               | on the opening colon fence, as a container or one class word          |

Automatic anchors are synthesized by the [anchors](anchors.md) module and are
not an attachment site. A successfully attached container is lexically part of
its owner and inside the owner's scope; the owner's visible content excludes
it.

### Inline code

A container beginning at the byte after the closing backtick run attaches to
that `Code`. The container is excluded from
`Code.literal` and included in `Code.scope`:

```````````````````````````````` example
`printf()`{.c} `<$>`{#operator .haskell role="function"}
.
Document scope=1:1..1:56 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:56 anchor=null attributes={} children=3
    ├── Code scope=1:1..1:14 anchor=null attributes={.c} literal="printf()" children=0
    ├── Text scope=1:15..1:15 anchor=null attributes={} literal=" " children=0
    └── Code scope=1:16..1:56 anchor="operator" attributes={.haskell role="function"} literal="<$>" children=0
````````````````````````````````

Whitespace before `{` prevents attachment. A malformed container leaves the
completed `Code` unchanged and releases the `{` to ordinary inline parsing.
Classes on `Code` have no parser-side meaning; no surface derives a language
from them:

```````````````````````````````` example
`a` {.c} `b`{.1}
.
Document scope=1:1..1:16 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:16 anchor=null attributes={} children=4
    ├── Code scope=1:1..1:3 anchor=null attributes={} literal="a" children=0
    ├── Text scope=1:4..1:9 anchor=null attributes={} literal=" {.c} " children=0
    ├── Code scope=1:10..1:12 anchor=null attributes={} literal="b" children=0
    └── Text scope=1:13..1:16 anchor=null attributes={} literal="{.1}" children=0
````````````````````````````````

### Headings

For an ATX heading: if the line's last
non-whitespace bytes form a valid container, remove it and the whitespace
before it, then apply the inherited closing-sequence rule to the remainder;
attach only when the first step succeeded. For a Setext heading the first step
applies to the last content line. `# {#x}` is a `Heading` with empty content
and `anchor="x"`:

```````````````````````````````` example
# Title {#custom-id .c}

# {#x}

## Chapter ## {#other}

Setext {#s}
===========
.
Document scope=1:1..8:11 anchor=null attributes={} children=4
├── Heading scope=1:1..1:23 anchor="custom-id" attributes={.c} level=1 children=1
│   └── Text scope=1:3..1:7 anchor=null attributes={} literal="Title" children=0
├── Heading scope=3:1..3:6 anchor="x" attributes={} level=1 children=0
├── Heading scope=5:1..5:22 anchor="other" attributes={} level=2 children=1
│   └── Text scope=5:4..5:10 anchor=null attributes={} literal="Chapter" children=0
└── Heading scope=7:1..8:11 anchor="s" attributes={} level=1 children=1
    └── Text scope=7:1..7:6 anchor=null attributes={} literal="Setext" children=0
````````````````````````````````

An invalid suffix remains heading content:

```````````````````````````````` example
# T {.1}
.
Document scope=1:1..1:8 anchor=null attributes={} children=1
└── Heading scope=1:1..1:8 anchor="t-1" attributes={} level=1 children=1
    └── Text scope=1:3..1:8 anchor=null attributes={} literal="T {.1}" children=0
````````````````````````````````

A non-empty explicit anchor wins over automatic synthesis: the
[anchors](anchors.md) module reserves it before it synthesizes any other
heading's anchor.

### Fenced code

The inherited fence rule is applied to the
complete opening line first; a container is then recognized only as the last
non-whitespace content of that line. `CodeBlock.info` is the inherited info
string over the line with the container and the whitespace before it
removed, and `language` is its first token. Nothing is lowercased, aliased,
or derived from a class; `numberLines`, `number-lines`, `lineAnchors`,
`line-anchors`, and `startFrom` are ordinary values:

```````````````````````````````` example
```python {.numberLines startFrom="10"}
print("hello")
```
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── CodeBlock scope=1:1..3:3 anchor=null attributes={.numberLines startFrom="10"} info="python" language="python" literal="print(\"hello\")\n" fenced=true closed=true children=0
````````````````````````````````

A bare word before the container is never an attribute member, and a
container followed by other bytes, or a malformed container, is ordinary
info-string text and attaches nothing; the body and closing fence are never
reinterpreted:

```````````````````````````````` example
``` python {.x} y
z
```
.
Document scope=1:1..3:3 anchor=null attributes={} children=1
└── CodeBlock scope=1:1..3:3 anchor=null attributes={} info="python {.x} y" language="python" literal="z\n" fenced=true closed=true children=0
````````````````````````````````

### Links and images

A container beginning at the byte after a complete direct link or image
tail, a full or collapsed reference tail that resolves, or an angle-bracket
autolink attaches to the resulting `Link` or `Media`; a shortcut reference
followed by a container is a bracketed span, which the bracket procedure
tests first:

```````````````````````````````` example
[text](https://example.com){target="_blank"}

![image](foo.jpg){#hero .wide width=50%}

<https://example.com>{.external}
.
Document scope=1:1..5:32 anchor=null attributes={} children=3
├── Paragraph scope=1:1..1:44 anchor=null attributes={} children=1
│   └── Link scope=1:1..1:44 anchor=null attributes={target="_blank"} dest=url("https://example.com") title=null children=1
│       └── Text scope=1:2..1:5 anchor=null attributes={} literal="text" children=0
├── Paragraph scope=3:1..3:40 anchor=null attributes={} children=1
│   └── Media scope=3:1..3:40 anchor="hero" attributes={.wide width="50%"} dest=url("foo.jpg") title=null dimensions=null children=1
│       └── Text scope=3:3..3:7 anchor=null attributes={} literal="image" children=0
└── Paragraph scope=5:1..5:32 anchor=null attributes={} children=1
    └── Link scope=5:1..5:32 anchor=null attributes={.external} dest=url("https://example.com") title=null children=1
        └── Text scope=5:2..5:20 anchor=null attributes={} literal="https://example.com" children=0
````````````````````````````````

Whitespace prevents attachment. A bare GFM autolink never accepts a
container, and its inherited termination rule applies to the braces. When the
link fails, the container is released and decided by the bracket procedure of
the [links and images](links-and-images.md) module. An occurrence-local
container is outside label or alt content and inside that occurrence's
scope. A malformed container leaves the completed node unchanged:

```````````````````````````````` example
[a](/u) {.c}
.
Document scope=1:1..1:12 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:12 anchor=null attributes={} children=2
    ├── Link scope=1:1..1:7 anchor=null attributes={} dest=url("/u") title=null children=1
    │   └── Text scope=1:2..1:2 anchor=null attributes={} literal="a" children=0
    └── Text scope=1:8..1:12 anchor=null attributes={} literal=" {.c}" children=0
````````````````````````````````

A container may also follow a reference definition. The definition grammar
is the destination, an optional title, optional spaces or tabs, at most one
line ending plus optional spaces or tabs, one container, and then only
whitespace to the line ending; a malformed container leaves the inherited
definition grammar unchanged, so such a line is then not a definition. The container is normalized once when the definition is
stored in the parser's reference map and is part of the shared resource that
every occurrence of the definition references. A resolved occurrence receives
`merge(occurrence, definition)`; explicit duplicate-definition precedence is
unchanged and an unresolved reference inherits nothing. The emitted node keeps
the source-faithful range of its own occurrence, and an occurrence-local
anchor wins over the inherited one:

```````````````````````````````` example
[x][r] [x][r]{#bar}

[r]: /target {#foo}
.
Document scope=1:1..3:19 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:19 anchor=null attributes={} children=3
    ├── Link scope=1:1..1:6 anchor="foo" attributes={} dest=url("/target") title=null children=1
    │   └── Text scope=1:2..1:2 anchor=null attributes={} literal="x" children=0
    ├── Text scope=1:7..1:7 anchor=null attributes={} literal=" " children=0
    └── Link scope=1:8..1:19 anchor="bar" attributes={} dest=url("/target") title=null children=1
        └── Text scope=1:9..1:9 anchor=null attributes={} literal="x" children=0
````````````````````````````````

```````````````````````````````` example
[x][r]{.b .c k=3}

[r]: /t {#d .a .b k=1 m=2}
.
Document scope=1:1..3:26 anchor=null attributes={} children=1
└── Paragraph scope=1:1..1:17 anchor=null attributes={} children=1
    └── Link scope=1:1..1:17 anchor="d" attributes={.a .b .b .c k="1" m="2" k="3"} dest=url("/t") title=null children=1
        └── Text scope=1:2..1:2 anchor=null attributes={} literal="x" children=0
````````````````````````````````

Media `width` and `height` assignments are records stored verbatim,
`width=50%` and `height=2in` alike; the parser validates no unit. The typed
`Media.dimensions: Dimensions?` value is populated only by the
[image dimensions](links-and-images.md) rule, never by a record, and vice
versa.

### Directives and spans

The directive container follows the name or the label with no whitespace, at
most one container attaches, and a container in a block directive must close
on the opener line; a nameless container's attribute container or class word
sits on its opening colon fence. The [directives](directives.md) module
states the rest. A bracketed span's container follows its closing `]`
immediately; the [bracketed spans](bracketed-spans.md) module states the rest.
Both modules show the examples.

## Failure and complexity

A candidate commits atomically only after its closing `}` and every member
have parsed. An invalid or unclosed candidate attaches nothing, emits no
partial value, and releases its source under the owning construct's fallback;
the failed `{` is text. Recognition indexes every suffix once, so overlapping malformed or unclosed
candidates cannot repeatedly scan the same source. Normalization runs forward
only over committed containers; recognition, normalization, and merging are
linear in source bytes plus output. There is one attribute
scanner in the C core, shared by every site; no site keeps a private tokenizer
or storage shape.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover the
universal field on every `Markup` kind; single-quoted values; non-ASCII
whitespace positions; merge without scope mutation; inert unsafe-looking
metadata; unclosed fallback at every site; every attachment site and its
owner's exact scope; allocation failure; and
size-doubling valid, duplicate, malformed, and unclosed containers.
