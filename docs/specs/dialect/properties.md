# Properties

Status: normative module of the [Markdown Core dialect](../dialect.md), owned
by [O6](../../plans/2026-09-04-canonical-vnext-landing-plan.md).

The user-directed contract of 2026-09-08 defines a fixed metadata field set.
It borrows field/list notation from [Obsidian Properties](https://help.obsidian.md/properties)
and the indented literal `|` form from
[Pandoc metadata](https://pandoc.org/MANUAL.html#extension-yaml_metadata_block).
It does not implement either application's complete metadata language or YAML.
In particular, Pandoc requires a valid YAML object; ignoring unsupported input
member by member is this repository's rule, not a claim about Pandoc behavior.

## Fields

Only these exact, case-sensitive decoded names populate fields:

| Name | Meaning |
| --- | --- |
| `name` | Document name |
| `title` | Title |
| `subtitle` | Subtitle |
| `time` | Time |
| `date` | Date |
| `authors` | Authors |
| `keywords` | Keywords |
| `abstract` | Abstract; also accepts indented literal text with `: |` |
| `state` | State |
| `comment` | Authored comment field; also accepts `: |` |

`authors` and `keywords` each accept a single string, a bracketed array
(`authors: [Ada, Lin]`), or a block list (`- Ada` on following lines). A single
string stays a text scalar; both list spellings produce the same list model.

The parser does not infer date/time formats, validate a state vocabulary, or
coerce values from a field name. The shared scalar/list domain below applies
to these fields. `abstract` and `comment` additionally accept literal prose.
Unknown names, unnamed text, `#` comments, `...`, invalid values, unsupported
syntax, and later duplicate names are ignored. Only the first successfully
decoded occurrence reserves a name; an invalid occurrence does not prevent a
later valid one. Each of the ten fields is assigned at most once.

## Model

```text
Document(content: [Markup], metadata: Metadata?, footnotes: [Footnote])
Metadata(
  name: MetadataValue?, title: MetadataValue?, subtitle: MetadataValue?,
  time: MetadataValue?, date: MetadataValue?, authors: MetadataValue?,
  keywords: MetadataValue?, abstract: MetadataValue?, state: MetadataValue?,
  comment: MetadataValue?, scope: Scope
)
MetadataScalar   = null | bool(Bool) | number(String) | text(String)
MetadataListItem = number(String) | text(String)
MetadataValue    = scalar(MetadataScalar) | list([MetadataListItem])
```

`Metadata` exposes these ten named optional values directly. It has no content
collection, record wrapper, name lookup map, or retained source order. Missing
fields are absent (`nil`/`null`); a successfully authored `null` is a present
`scalar(null)`, and empty text and empty lists remain distinct values. The
named `comment` field is ordinary data. Only the metadata envelope has a
scope; individual values are located by their owner's scope.

Metadata is not Markup, has no anchors/attributes, and receives no visitor
callbacks. Values remain atomic, including Markdown-looking text in an
abstract. Public constructors create ordinary owned values; parsing owns
field recognition. A missing or unclosed envelope gives `metadata == null`.
A complete envelope with no valid fields gives present metadata with ten
absent fields. Ignored source has no AST representation and never becomes
part of the Markdown body.

```````````````````````````````` example
---
name: Note
authors: [Ada, Lin]
unknown: ignored
free text
state: draft
---
Body
.
Document scope=1:1..8:4 anchor=null attributes={} children=1
├── Metadata scope=1:1..7:3 name=scalar(text("Note")) title=null subtitle=null time=null date=null authors=list([text("Ada"),text("Lin")]) keywords=null abstract=null state=scalar(text("draft")) comment=null children=0
└── Paragraph scope=8:1..8:4 anchor=null attributes={} children=1
    └── Text scope=8:1..8:4 anchor=null attributes={} literal="Body" children=0
````````````````````````````````

## Envelope

Recognition runs once before inherited Markdown block starts, at the beginning
of the document after an optional UTF-8 BOM. The opening and closing lines
must each be exactly `---` at column one, without trailing whitespace. The
opening line must have a line ending; the closing line may end the file.
LF, CR, and CRLF are accepted. The first later exact closing line ends the
block. Only one envelope attaches; subsequent fences are Markdown.

`...` does not close the block. Indented `---` is payload, including inside
literal text. A complete envelope always attaches, regardless of its payload.
Only an absent or unclosed envelope leaves those bytes for Markdown parsing.
Allocation failure fails the whole parse; it never becomes ignored syntax or
a Markdown fallback. The body is parsed once after the closing fence.

```````````````````````````````` example
---
...
# ignored
---
body
.
Document scope=1:1..5:4 anchor=null attributes={} children=1
├── Metadata scope=1:1..4:3 name=null title=null subtitle=null time=null date=null authors=null keywords=null abstract=null state=null comment=null children=0
└── Paragraph scope=5:1..5:4 anchor=null attributes={} children=1
    └── Text scope=5:1..5:4 anchor=null attributes={} literal="body" children=0
````````````````````````````````

## Values and notation

A property uses `name: value`, with whitespace after the colon unless its value
is empty. Names may be plain or single-/double-quoted on one physical line;
recognition and uniqueness use decoded names without case conversion.

- An empty value or plain `null` is null, distinct from `""` and `[]`.
- Plain `true` and `false` are booleans.
- A plain value matching
  `^-?(0|[1-9][0-9]*)(\.[0-9]*)?([eE][-+]?[0-9]+)?$` is a number. Its exact
  spelling is stored, without host integer or floating-point conversion.
- Other supported plain values and quoted values are text. Except for the
  literal form below, text is authored on one physical line and contains no
  decoded CR/LF. Dates, times, URLs and quoted links remain text.
- Lists contain only text and number scalars. They may be bracketed,
  comma-separated lists (an optional trailing comma is allowed), or `- ` items
  below a field. Indentless lists are accepted; comment-only and blank lines
  between items do not split them. Empty lists are distinct from null.

A single-quoted string escapes a quote by doubling it. Double-quoted strings
support these escapes: `\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t`, and
`\uXXXX`; valid surrogate pairs decode to one Unicode scalar. Decoded CR/LF
make a single-line field invalid. Other YAML escapes and multiline quoted or
plain folding are unsupported. Plain text does not begin with reserved
indicators such as `&`, `*`, `!`, `|`, `>`, `[` or `{`; quoted occurrences are
ordinary text. A separated `#` begins an ignored comment outside quotes or
literal prose. A separated colon inside a plain value is unsupported.

There is no JSON root-object form. Bracketed arrays are field values in the
field-line grammar; they do not enable JSON documents or object-valued fields.

Anchors, aliases, tags, merge/complex keys, nested lists/objects, and folded
scalars are unsupported. There is no alias binding, expansion, rollback, or
expansion budget. The shared core implements this bounded grammar directly;
no general YAML parser or alternate fallback parser is added.

## Literal prose

Only `abstract: |` and `comment: |` introduce a literal block. An optional
separated header comment is ignored. The first nonblank body line establishes
its space indentation, which must exceed the field line's indentation. Every
later nonblank body line must have at least that indentation. The decoder
removes exactly that prefix, preserving extra indentation, internal blank
lines, hashes, colons, quotes, and Markdown-looking text literally.

Line endings normalize to LF. The default literal behavior clips trailing
blank lines to one final LF after the last nonblank content line. An empty or
all-blank block is empty text. A field at the same or smaller indentation ends
the block. An insufficiently indented continuation invalidates the owning
member; recovery then proceeds to the next independent member.

Only the bare `|` header is supported: no `>`, `|-`, `|+`, explicit indentation
indicator, literal list items, or literal forms on other fields. This is a
single prose feature, not general YAML block-scalar support.

```````````````````````````````` example
---
abstract: |
  First paragraph.

  Second paragraph.
comment: |
  # literal text
  name: still prose
state: ready
---
Body
.
Document scope=1:1..11:4 anchor=null attributes={} children=1
├── Metadata scope=1:1..10:3 name=null title=null subtitle=null time=null date=null authors=null keywords=null abstract=scalar(text("First paragraph.\n\nSecond paragraph.\n")) state=scalar(text("ready")) comment=scalar(text("# literal text\nname: still prose\n")) children=0
└── Paragraph scope=11:1..11:4 anchor=null attributes={} children=1
    └── Text scope=11:1..11:4 anchor=null attributes={} literal="Body" children=0
````````````````````````````````

## Ownership, recovery, and scopes

A property owns its indented continuations and flat list items. A new member
at the same or smaller indentation ends it. Quoted commas and balanced nested
source stay with their owning bracketed member, even when its value is
unsupported. An unfinished construct recovers at the next independent field
boundary. Interior colons are never retried as a different field. Each valid
member assigns its named field; each invalid member is skipped as a whole.

`Metadata.scope` starts at the opening fence's first hyphen and ends at the
closing fence's third hyphen. The body keeps its original source coordinates.
No individual field scopes or source order are retained. The dump prints all
ten fields in the model's fixed order, including absent fields as `null`.

Field/list/string allocations belong to the document. C accessors borrow
those values; Swift, Kotlin/JNI/Native and ES/Wasm copy them before releasing
the native document or payload. Cleanup reads only active value branches.
Each source member is decoded at most once and assigned directly to its
field; an already-present value also supplies the duplicate check. No record
array, separate name-state table, bracket index or line-start index is built.
The envelope scan counts lines as it locates the closing fence. Text is read
directly, and array boundaries are scanned as part of their owning member.
There is no recursive value expansion or per-member suffix retry.

## Verification

The pinned `yaml@2.9.0` Document/CST oracle compares only this grammar's valid
intersection, including bare literal prose. It is test tooling, not a runtime
dependency or authority for extra syntax. It must reject out-of-domain oracle
inputs; direct core fixtures test that the product ignores those members and
continues. Exact numeric spelling and the envelope range remain oracle
evidence. Pandoc Markdown parsing of string values is outside this AST model.

Fixtures cover all ten names, quoted names, duplicates and retry after an
invalid occurrence, empty metadata, ignored content, literal indentation and
blank lines, supported scalar/list values, unsupported YAML forms, source
coordinates, body separation, long malformed input, flat list complexity,
allocation failures, and owned values on every binding.
