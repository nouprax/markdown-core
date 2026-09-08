# Properties

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the document metadata model and the one source rule that populates it.
Source: Obsidian's Properties and the
YAML 1.2.2 specification it links. Executable oracle: `yaml` 2.9.0 through
its Document/CST API, behind the Obsidian gate's exact envelope scanner, under
`specs/oracles/obsidian/`. Landing: the value types with `M7`, recognition
with `O6`. The [example format](../dialect.md#examples) is defined by the
index.

## Model

```text
Document(content: [Markup], metadata: Metadata?, footnotes: [Footnote])

Metadata(content: [MetadataContent], scope: Scope)
MetadataContent = comment(String) | data(MetadataRecord)
MetadataRecord(name: String, value: MetadataValue, scope: Scope)

MetadataScalar   = null | bool(Bool) | number(String) | text(String)
MetadataListItem = number(String) | text(String)
MetadataValue    = scalar(MetadataScalar) | list([MetadataListItem])
```

`Document.metadata == null` means no complete beginning-of-file envelope
occurred. A non-null `Metadata(content=[])` means an empty or whitespace-only
envelope occurred. `content` keeps source order and has one representation for
each interpreted property or retained source fragment: `data(record)` contains
a `MetadataRecord`, and `comment(String)` contains uninterpreted source. A YAML
comment, non-YAML text, `...`, or an unsupported property stays inside metadata.
These comments are value cases, **not** the dialect's `Comment` markup kind.

`Metadata` and `MetadataRecord` are scoped values, not `Markup`: they never enter
`Document.content`, have no `anchor` or `attributes`, and receive no visitor
callbacks. `MetadataContent` is an unscoped enum; its data branch owns the scoped
record and its comment string is located by the enclosing metadata. There is no
second stored `records` list or lookup map. Data names are unique; the first
successfully decoded occurrence keeps the name, and later occurrences become
comments, retaining the complete duplicate member.

In the dump, a non-null `Metadata` prints before the document's content. A data
case prints its `MetadataRecord` line; a comment case prints
`MetadataContent value=comment("...") children=0`, without a scope, anchor,
attributes, or any Markup children. `Metadata.children` counts content cases.

```````````````````````````````` example
---
title: Note
tags: [a, b]
---
Body
.
Document scope=1:1..5:4 anchor=null attributes={} children=1
├── Metadata scope=1:1..4:3 children=2
│   ├── MetadataRecord scope=2:1..2:11 name="title" value=scalar(text("Note")) children=0
│   └── MetadataRecord scope=3:1..3:12 name="tags" value=list([text("a"),text("b")]) children=0
└── Paragraph scope=5:1..5:4 anchor=null attributes={} children=1
    └── Text scope=5:1..5:4 anchor=null attributes={} literal="Body" children=0
````````````````````````````````

Names are non-empty single-line decoded Unicode strings, case-preserving and
case-sensitive, never lowercased, slugged, pluralized, or rewritten. The name
set is open: `tags`, `aliases`, `cssclasses`, and `publish` are ordinary
names, and no name is a parser keyword or a dedicated `Document` field:

```````````````````````````````` example
---
tags:
  - project
aliases: ["Alt name"]
cssclasses: wide
link: "[[Episode IV]]"
---
.
Document scope=1:1..7:3 anchor=null attributes={} children=0
└── Metadata scope=1:1..7:3 children=4
    ├── MetadataRecord scope=2:1..3:11 name="tags" value=list([text("project")]) children=0
    ├── MetadataRecord scope=4:1..4:21 name="aliases" value=list([text("Alt name")]) children=0
    ├── MetadataRecord scope=5:1..5:16 name="cssclasses" value=scalar(text("wide")) children=0
    └── MetadataRecord scope=6:1..6:22 name="link" value=scalar(text("[[Episode IV]]")) children=0
````````````````````````````````

Scalar values:

- `null` is an empty scalar, a plain scalar that resolves to null, or a
  scalar with the standard null tag. It differs from empty text and from an
  empty list.
- `bool` is an unquoted `true` or `false` or a scalar with the standard bool
  tag.
- `number` holds the complete decoded ASCII spelling of the number, never a
  host integer or float; integers, decimals, and exponents keep their exact
  spelling on every surface.
- `text` holds the decoded single-line string after YAML quoting, escapes,
  and folding. Date and date-time spellings are text; whether a name is a
  Date property is vault state that the source cannot express. Text is atomic:
  `title: "**Draft**"`, `tag: "#topic"`, and `link: "[[Episode IV]]"` keep
  those strings with no inline children, and no other dialect feature is
  recognized inside a value.

```````````````````````````````` example
---
n: null
e:
b: true
i: 12
f: 1.50
d: 2024-01-01
s: "**Draft**"
---
.
Document scope=1:1..9:3 anchor=null attributes={} children=0
└── Metadata scope=1:1..9:3 children=7
    ├── MetadataRecord scope=2:1..2:7 name="n" value=scalar(null) children=0
    ├── MetadataRecord scope=3:1..3:2 name="e" value=scalar(null) children=0
    ├── MetadataRecord scope=4:1..4:7 name="b" value=scalar(bool(true)) children=0
    ├── MetadataRecord scope=5:1..5:5 name="i" value=scalar(number("12")) children=0
    ├── MetadataRecord scope=6:1..6:7 name="f" value=scalar(number("1.50")) children=0
    ├── MetadataRecord scope=7:1..7:13 name="d" value=scalar(text("2024-01-01")) children=0
    └── MetadataRecord scope=8:1..8:14 name="s" value=scalar(text("**Draft**")) children=0
````````````````````````````````

A list is ordered and holds only text and number items. `bool` and `null`
items, nested sequences, and mappings make their owning member a comment; block and
flow spellings give the same value; an empty list differs from `null`:

```````````````````````````````` example
---
l:
  - a
  - 1
m: []
---
.
Document scope=1:1..6:3 anchor=null attributes={} children=0
└── Metadata scope=1:1..6:3 children=2
    ├── MetadataRecord scope=2:1..4:5 name="l" value=list([text("a"),number("1")]) children=0
    └── MetadataRecord scope=5:1..5:5 name="m" value=list([]) children=0
````````````````````````````````

## Envelope

Recognition is block-start step 0 and runs once, before the first inherited
block start, on the first decoded line of the document after an optional
UTF-8 byte order mark:

```text
document         = [ BOM ] [ properties-block ] markdown-body
properties-block = opening-fence line-ending *payload-line closing-fence
                   [ line-ending ]
opening-fence    = "---"
closing-fence    = "---"
payload-line     = *( any scalar except LF and CR ) line-ending
line-ending      = LF / CR / CRLF
```

The opening fence is exactly three hyphens with no indentation, prefix,
suffix, or trailing whitespace, and it must be the first line. The closing
fence is the first later line that is exactly `---` at column one; it may end
at the end of the document. A payload line is therefore never exactly `---`;
an indented `---` inside a scalar is payload. An empty payload has no content; a comment-only payload retains its comments:

```````````````````````````````` example
---
---
.
Document scope=1:1..2:3 anchor=null attributes={} children=0
└── Metadata scope=1:1..2:3 children=0
````````````````````````````````

```````````````````````````````` example
---
# just a comment
---
body
.
Document scope=1:1..4:4 anchor=null attributes={} children=1
├── Metadata scope=1:1..3:3 children=1
│   └── MetadataContent value=comment("# just a comment") children=0
└── Paragraph scope=4:1..4:4 anchor=null attributes={} children=1
    └── Text scope=4:1..4:4 anchor=null attributes={} literal="body" children=0
````````````````````````````````

Only one block can occur: a later `---` pair is inherited Markdown. `...`,
four or more hyphens, a fence with an info word, and a fence preceded by a
blank line are not fences. A `...` line is retained as a comment; it never
closes the envelope or returns its payload to Markdown. No metadata syntax is
recognized inside any container:

```````````````````````````````` example

---
a: 1
---
.
Document scope=1:1..4:3 anchor=null attributes={} children=2
├── ThematicBreak scope=2:1..2:3 anchor=null attributes={} children=0
└── Heading scope=3:1..4:3 anchor=null attributes={} level=2 children=1
    └── Text scope=3:1..3:4 anchor=null attributes={} literal="a: 1" children=0
````````````````````````````````

```````````````````````````````` example
---
a: 1
...
.
Document scope=1:1..3:3 anchor=null attributes={} children=2
├── ThematicBreak scope=1:1..1:3 anchor=null attributes={} children=0
└── Paragraph scope=2:1..3:3 anchor=null attributes={} children=3
    ├── Text scope=2:1..2:4 anchor=null attributes={} literal="a: 1" children=0
    ├── SoftBreak scope=2:5..2:5 anchor=null attributes={} children=0
    └── Text scope=3:1..3:3 anchor=null attributes={} literal="..." children=0
````````````````````````````````

## YAML projection

The payload is an ordered sequence of recoverable source members. Each data
member uses YAML 1.2.2 scalar, sequence, and mapping syntax; the payload itself
need not be a valid YAML document. Directives and document indicators are
comments and never change the decoding schema. A member containing a byte
outside YAML's `c-printable` set is retained as a comment. Plain scalars in
value position resolve as JSON scalars with a string fallback: exactly `null`
is null; exactly `true` and `false` are booleans; a scalar matching
`^-?(0|[1-9][0-9]*)(\.[0-9]*)?([eE][-+]?[0-9]+)?$` is a number; every other
valid plain scalar and every quoted scalar is text. YAML 1.1 booleans,
timestamps, infinities, base prefixes, underscores, and leading plus signs
resolve to text. A JSON object as the root payload decodes through the same
operation.

A key is directly authored if and only if it is a plain, single-quoted, or
double-quoted scalar carrying no tag, anchor, or explicit-key indicator; any
other key makes its owning member a comment. A directly authored key is decoded as
text without the value-side resolution, so `1` and `"1"` name the same
record, `true`, `null`, and `~` are names with those spellings, and `1`,
`1.0`, `1e0`, `01`, `0`, and `-0` are six distinct names. Uniqueness is
checked on decoded text; a later duplicate becomes a comment:

```````````````````````````````` example
---
"1": a
1.0: b
true: c
~: d
---
.
Document scope=1:1..6:3 anchor=null attributes={} children=0
└── Metadata scope=1:1..6:3 children=4
    ├── MetadataRecord scope=2:1..2:6 name="1" value=scalar(text("a")) children=0
    ├── MetadataRecord scope=3:1..3:6 name="1.0" value=scalar(text("b")) children=0
    ├── MetadataRecord scope=4:1..4:7 name="true" value=scalar(text("c")) children=0
    └── MetadataRecord scope=5:1..5:4 name="~" value=scalar(text("d")) children=0
````````````````````````````````

A tagged scalar is valid if and only if its content is exactly what the plain
form of the branch requires: `!!null` empty or `null`; `!!bool` `true` or
`false`; `!!int` `-?(0|[1-9][0-9]*)`; `!!float` the number grammar; `!!str`
any single-line scalar, giving text even for `null`, `true`, or a numeric
spelling. `!!map` and `!!seq` may state the required collection kind. Every
other tag makes its owning member a comment. Support is decided on decoded values: a
folded or escaped source form is valid when its decoded content contains no
U+000A or U+000D, and U+0085, U+2028, and U+2029 are ordinary characters.

An alias contributes the resolved value at its occurrence when its anchor was
defined earlier in a successful data member, or earlier within the current
member, and the value is supported. An anchor shadows an earlier name as soon
as its value starts; referring to it before completion is a cycle and makes
the member a comment. An unsuccessful member rolls back all its anchor
bindings, including shadows, and never supplies an alias target. Thus aliases
are resolved once to complete values; they create no public reference graph. The sum, over all committed alias
occurrences, of the aliased node's source byte length (from its first tag or
anchor prefix, or first value token, through its final value token) may not exceed
the dialect's alias expansion budget of 1048576 bytes, and a payload holds at
most 65536 data records. A member that would exceed either limit becomes a
comment; earlier data survives, and a failed member consumes no alias budget.
The limits bound data projection, never retention of source. Anchor names,
quote style, and flow versus block style are presentation details of data.
Comments are retained source, and exact numeric spelling is kept because
converting it loses precision:

```````````````````````````````` example
---
x: &a 1
y: *a
z: !!str 1
---
.
Document scope=1:1..5:3 anchor=null attributes={} children=0
└── Metadata scope=1:1..5:3 children=3
    ├── MetadataRecord scope=2:1..2:7 name="x" value=scalar(number("1")) children=0
    ├── MetadataRecord scope=3:1..3:5 name="y" value=scalar(number("1")) children=0
    └── MetadataRecord scope=4:1..4:10 name="z" value=scalar(text("1")) children=0
````````````````````````````````

## Member ownership, recovery, and attachment

The first complete envelope commits independently of its contents. It owns all
bytes through its closing fence and populates `Document.metadata`. The remaining
body is parsed once by the ordinary block parser. Only an absent or unclosed
envelope leaves the source to inherited Markdown. Allocation failure remains a
parse failure, never a comment or a Markdown fallback.

A block member begins on a nonblank source line and owns its indented
continuations; an indentless sequence is part of its owning value. A new member
at the same or smaller indentation ends the preceding member. Quoted and flow
values retain continuation lines and their closing delimiters. A directly
authored mapping key at the member's indentation is a recovery boundary even
when the preceding quote or flow value was not closed. An invalid member is
never reparsed at an interior colon. Flow root mappings use their comma-delimited
members, with nested collections and quoted commas kept in their owning member;
a JSON root object uses this same mapping operation.

Each member either commits one data record and its authored YAML comments, or
commits its original source as a comment. A scalar or sequence root, explicit
null, malformed YAML, unsupported key, nested value, invalid tag or alias, and a
duplicate decoded name all use the comment branch. Recovery does not split a
valid record into partly interpreted values. Independently decoded members on
either side remain data. A data record precedes comments inside or after its
value in source-start order. Comments inside scalar text remain scalar text.

Comment strings retain their authored bytes, including `#`, delimiters, and
interior line endings; the line ending separating members is excluded. A
standalone comment retains its indentation. Inline comments begin at `#`.
Whitespace-only separators create no content case. Source inside either branch
is opaque to every Markdown feature.

```````````````````````````````` example
---
title: Note
not YAML
...
key: [a, {b: c}]
count: 2
---
body
.
Document scope=1:1..8:4 anchor=null attributes={} children=1
├── Metadata scope=1:1..7:3 children=5
│   ├── MetadataRecord scope=2:1..2:11 name="title" value=scalar(text("Note")) children=0
│   ├── MetadataContent value=comment("not YAML") children=0
│   ├── MetadataContent value=comment("...") children=0
│   ├── MetadataContent value=comment("key: [a, {b: c}]") children=0
│   └── MetadataRecord scope=6:1..6:8 name="count" value=scalar(number("2")) children=0
└── Paragraph scope=8:1..8:4 anchor=null attributes={} children=1
    └── Text scope=8:1..8:4 anchor=null attributes={} literal="body" children=0
````````````````````````````````

Property values enable no other feature and configure nothing; a properties
block is data for consumers.

## Scopes

`Metadata.scope` runs from the opening fence's first hyphen to the closing
fence's third hyphen, fences and payload included; a line ending after the
closing fence is outside it, and the scope never covers the body. Each
`MetadataRecord.scope` starts at the first byte of its key, quotes included,
and ends at the last non-whitespace byte of the value's last owned line,
excluding trailing comments, flow separators, and the line ending; an empty
value without an authored tag or anchor ends at the colon. An alias-resolved value keeps the
alias-owning record's range. `Document.scope` covers the complete source, and
each body block covers only its own occurrence.

## Oracle

The gate applies the envelope grammar above exactly, then parses the payload
with `yaml` 2.9.0 through its Document and node API with source tokens
retained, JSON scalar resolution with a string fallback, and duplicate
checking disabled, and projects the ordered mapping pairs directly without
building a JavaScript object. It witnesses supported YAML data, key shape, source order, exact numeric
lexemes, aliases, and values. The comparison projects away metadata comment
cases because YAML presentation comments are not data records. Product canaries
and fixtures own member recovery, retained source, duplicates, and budget
boundaries; malformed payloads are not sent to the YAML oracle as documents. Scopes are compared by product fixtures only;
the package's offsets are not binding coordinates. Syntax the package accepts
beyond this module never enters the dialect.

## Required conformance cases

Every example of this module is a package fixture. Tests also cover a block
after a BOM; LF, CR, and CRLF; closing at the end of the document;
whitespace-only payloads; arbitrary Unicode and punctuation-bearing names;
`1` with `"1"` as a duplicate; JSON roots; exact large integers, exponents,
date-times, quoted escapes, folded single-line text, every allowed tag, and
alias chains; body parsing immediately after the close; every record scope,
`Metadata.scope`, and body scopes. Envelope-negative cases cover leading text lines,
second blocks, directives and document indicators after a mapping, short,
long, indented, trailed, and info-word fences, missing closers, malformed
payloads, thematic-break and Setext interaction, and source-like bytes inside
every container. Comment-retention cases cover empty or multiline names,
sequence, mapping, alias, tagged and explicit keys, multiline text, boolean
and null list items, nested values, unsupported tags, invalid explicitly tagged
numbers, undefined, cyclic and over-budget aliases, and non-printable bytes.
Mixed cases must prove that data survives before and after each failed member;
comments, duplicate names, alias rollback, and allocation failure must be tested
on every transport. Size-doubling probes cover malformed members, shared long
prefixes, many comments, repeated aliases, and long single-line flow lists.
Work counters bound disjoint member decoding and source-line lookup work.
