# Properties

Status: normative target module of the [Markdown Core dialect](../dialect.md).
O6 was reopened on 2026-09-08 to follow Obsidian's Properties scope. PR #216's
current decoder, fixtures, and oracle still require migration; their passing
results do not establish conformance to this corrected target.

Source: [Obsidian Properties](https://help.obsidian.md/properties), including
its property types, property format, and JSON properties sections (checked
2026-09-08). YAML supplies the storage syntax. The pinned `yaml` 2.9.0
Document/CST oracle supplies syntax evidence; it does not define the product's
feature set or identify Obsidian's internal implementation. Landing is owned by
[O6](../../plans/2026-09-04-canonical-vnext-landing-plan.md).
The [example format](../dialect.md#examples) is defined by the index.

## Purpose and implementation boundary

Properties attach small, atomic values to a note. Obsidian documents Text,
List, Number, Checkbox, Date, Date & time, and Tags property types; it also
accepts JSON objects as their source spelling. The source-only AST below
represents that domain without implementing vault settings or a Properties
editor. Dates and date-times remain text, numbers keep exact spellings, and
links remain quoted text. `aliases` is an ordinary property name, independent
of YAML alias syntax. Nested property objects and Markdown inside values do
not add AST structure.

O6 does not implement general YAML object construction. Anchor declarations,
alias references, explicit tags, merge keys, complex keys, and nested values
are outside its supported source domain. Their owning source members remain
`comment(String)` without interpretation or expansion. Quoted occurrences of
those characters remain ordinary text. This boundary is this repository's
Properties contract; the documentation's UI limitations do not prove that
Obsidian's underlying YAML parser rejects the same input.

YAML syntax decoding and Properties projection have separate responsibilities.
Evaluate a maintained C-compatible vendored parser for quoting, escapes,
indentation, scalar decoding, collections, and source positions. The core
producer owns the exact envelope, ordered projection, duplicate handling,
comment retention, and recovery of neighboring members. A library must fit
those requirements, including allocator/OOM and all binding targets, before
adoption; its accepted language must not automatically become data. The task
does not require a handwritten YAML parser, an alias registry, an expansion
budget, or a second fallback decoder. Parser choice remains an O6 deliverable.

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

- `null` is an empty scalar or a plain `null`. It differs from empty text
  and from an empty list.
- `bool` is an unquoted `true` or `false`.
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

A data value has no explicit tag, anchor, or alias. Such constructs do not
participate in type inference and cannot introduce references between members.
A payload holds at most 65536 data records; subsequent members remain comments.
The record limit bounds projection, never retention of source. Numeric spelling
remains exact because converting it can lose precision. Unsupported YAML
constructs are retained in place:

```````````````````````````````` example
---
x: &a 1
y: *a
z: !!str 1
---
.
Document scope=1:1..5:3 anchor=null attributes={} children=0
└── Metadata scope=1:1..5:3 children=3
    ├── MetadataContent value=comment("x: &a 1") children=0
    ├── MetadataContent value=comment("y: *a") children=0
    └── MetadataContent value=comment("z: !!str 1") children=0
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
when the preceding quote or flow value was not closed. Flow punctuation inside
block plain keys and values remains text; it never starts a collection or
extends member ownership. An invalid member is
never reparsed at an interior colon. Flow root mappings use their comma-delimited
members, with nested collections and quoted commas kept in their owning member;
a JSON root object uses this same mapping operation.

Each member either commits one data record and its authored YAML comments, or
commits its original source as a comment. A scalar or sequence root, explicit
null, malformed YAML, unsupported key, nested value, anchor, tag, alias, merge, and a
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
value ends at the colon. A key-only flow pair, when decoded as an empty
value by the syntax parser, ends at its key's last byte. `Document.scope` covers the complete source, and
each body block covers only its own occurrence.

## Oracle

The gate must apply the envelope grammar above, then use pinned `yaml` 2.9.0
Document/CST parsing to witness syntax only within the supported Properties
domain. JSON scalar resolution with a string fallback and ordered mapping
pairs preserve names and numeric spelling without building a JavaScript
object. Library support for aliases, tags, merge keys, or nested values is not
an additional conformance requirement. The previous alias-resolution and
tagged-empty-null success canaries must be removed during O6 migration.

The comparison projects away metadata comment cases. Product fixtures own raw
source retention, duplicates, unsupported members, recovery after malformed
members, allocation failure, resource bounds, and binding-coordinate scopes.
The whole-document YAML oracle cannot establish member-level recovery of
invalid payloads. That recovery and ordered retention are explicit user-directed
repository behavior; they are not claimed as Obsidian runtime parity.

## Required conformance cases

Every example of this module must become a package fixture as part of the O6
rework. Official Properties examples cover text, quoted links, number,
checkbox/empty, date/date-time, list, tags, ordinary `aliases`, and JSON roots.
Tests also cover absent/empty/populated metadata; BOM and LF/CR/CRLF; closing at
EOF; names, duplicate decoded names, exact numbers, quoting and escapes,
single-line decoded text, block/flow lists, list comments at any permitted
indentation, and metadata/record/body scopes.

Mixed inputs must preserve each valid property on either side of malformed or
unsupported source. Anchor declarations, aliases (including missing/cyclic
spellings), explicit tags, merge keys, complex keys, nested objects/lists,
multiline decoded text, and boolean/null list items remain comments. They must
not trigger reference resolution, expansion, or additional public types.
Quoting those spellings keeps them ordinary strings. Strict envelope-negative
cases and container opacity keep their existing coverage.

All transports must agree on ordered data/comments and source-independent
ownership. Size-doubling probes must bound member scanning and source lookup,
including long strings/lists and repeated unsupported syntax. Peak live-memory
and allocation-failure checks must prove that temporary decoding state is
released and no partial document is published. Parser integration must pass
the repository's C, Swift, Kotlin/JNI/Native, and ES/Wasm validation gates.
