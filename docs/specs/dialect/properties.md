# Properties

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the document metadata model and the one source rule that populates it.
Option: `properties` (default `false`). Source: Obsidian's Properties and the
YAML 1.2.2 specification it links. Executable oracle: `yaml` 2.9.0 through
its Document/CST API, behind the harness's exact envelope scanner, under
`specs/oracles/obsidian/`. Landing: the value types with `M7`, recognition
with `O6`.

## Model

```text
Document(content: [Markup], metadata: Metadata?, footnotes: [Footnote])

Metadata(records: [MetadataRecord], scope: Scope)
MetadataRecord(name: String, value: MetadataValue, scope: Scope)

MetadataScalar   = null | bool(Bool) | number(String) | text(String)
MetadataListItem = number(String) | text(String)
MetadataValue    = scalar(MetadataScalar) | list([MetadataListItem])
```

`Document.metadata == null` means no valid properties block occurred. A
non-null `Metadata(records=[])` means a valid block with no records occurred;
an empty, whitespace-only, or comment-only payload all give that value, and
the comment produces no record or node. `Metadata` and `MetadataRecord` are
scoped values, not `Markup`: they are never in `Document.content`, have no
`anchor` or `attributes`, and take no visitor callbacks. The other value
types have no scope. `records` keeps source order and holds each name exactly
once; a lookup by name is a binding convenience, not a second stored map.

Names are non-empty single-line decoded Unicode strings, case-preserving and
case-sensitive, never lowercased, slugged, pluralized, or rewritten. The name
set is open: `tags`, `aliases`, `cssclasses`, and `publish` are ordinary
names, and no name is a parser keyword or a dedicated `Document` field.

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

A list is ordered and holds only text and number items. `bool` and `null`
items, nested sequences, and mappings invalidate the candidate; block and
flow spellings give the same value; an empty list differs from `null`.

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
an indented `---` inside a scalar is payload. Only one block can occur: a
later `---` pair is inherited Markdown. `...`, four or more hyphens, a fence
with an info word, and a fence preceded by a blank line are not fences, and
a `...` line anywhere in the payload invalidates the whole candidate rather
than closing it. No metadata syntax is recognized inside any container.

## YAML projection

The payload is decoded as one YAML 1.2.2 document, not a stream: a directive,
a document-start indicator, or a document-end indicator invalidates the
candidate, as does any byte outside YAML's `c-printable` set. Plain scalars in
value position resolve as JSON scalars with a string fallback: exactly `null`
is null; exactly `true` and `false` are booleans; a scalar matching
`^-?(0|[1-9][0-9]*)(\.[0-9]*)?([eE][-+]?[0-9]+)?$` is a number; every other
valid plain scalar and every quoted scalar is text. YAML 1.1 booleans,
timestamps, infinities, base prefixes, underscores, and leading plus signs
resolve to text. A JSON object as the root payload decodes through the same
operation.

A key is directly authored if and only if it is a plain, single-quoted, or
double-quoted scalar carrying no tag, anchor, or explicit-key indicator; any
other key invalidates the candidate. A directly authored key is decoded as
text without the value-side resolution, so `1` and `"1"` name the same
record, `true`, `null`, and `~` are names with those spellings, and `1`,
`1.0`, `1e0`, `01`, `0`, and `-0` are six distinct names. Uniqueness is
checked on the decoded text, and a duplicate invalidates the candidate.

A tagged scalar is valid if and only if its content is exactly what the plain
form of the branch requires: `!!null` empty or `null`; `!!bool` `true` or
`false`; `!!int` `-?(0|[1-9][0-9]*)`; `!!float` the number grammar; `!!str`
any single-line scalar, giving text even for `null`, `true`, or a numeric
spelling. `!!map` and `!!seq` may state the required collection kind. Every
other tag invalidates the candidate. Support is decided on decoded values: a
folded or escaped source form is valid when its decoded content contains no
U+000A or U+000D, and U+0085, U+2028, and U+2029 are ordinary characters.

An alias contributes the resolved value at its occurrence when its anchor was
defined earlier in the same block, the graph is acyclic, and the value is
supported; it creates no public reference. The sum, over all alias
occurrences, of the source byte length of the aliased node may not exceed
the dialect's alias expansion budget; exceeding it invalidates the candidate.
Presentation details such as comments, anchor names, quote style, flow versus
block style, and numeric formatting are not public fields; the exact numeric
spelling is the one lexical payload kept, because converting it loses
precision.

## Attachment and fallback

Recognition is transactional. The parser scans the complete envelope and
validates the payload, the root mapping or empty root, the unique names, and
the complete value graph before committing. A complete valid block populates
`Document.metadata`, owns every byte through the closing fence, and is
removed from `Document.content`; the remaining bytes are parsed once by the
ordinary block parser, and a blank line after the fence is ordinary
separation. Any failure, including an unclosed fence, invalid YAML, a
non-mapping root such as an explicit null, a duplicate name, an unsupported
value, an invalid alias, or a limit, sets `metadata` to `null` and returns
every byte to inherited parsing, in which the opening line is a thematic
break or a Setext underline as the inherited grammar decides. No constructed
block is deleted afterwards.

With `properties=false`, the envelope is inherited content and `metadata` is
always `null`. Property values enable no other feature.

## Scopes

`Metadata.scope` runs from the opening fence's first hyphen to the closing
fence's third hyphen, fences and payload included; a line ending after the
closing fence is outside it, and the scope never covers the body. Each
`MetadataRecord.scope` starts at the first byte of its key, quotes included,
and ends at the last non-whitespace byte of the value's last owned line,
excluding trailing comments, flow separators, and the line ending; for an
empty value it ends at the colon. An alias-resolved value keeps the
alias-owning record's range. `Document.scope` covers the complete source, and
each body block covers only its own occurrence.

## Oracle

The gate applies the envelope grammar above exactly, then parses the payload
with `yaml` 2.9.0 through its Document and node API with source tokens
retained, JSON scalar resolution with a string fallback, and duplicate
checking disabled, and projects the ordered mapping pairs directly without
building a JavaScript object. It witnesses empty and comment-only documents,
key shape, decoded-name uniqueness, source order, exact numeric lexemes,
aliases, and supported values. Scopes are compared by product fixtures only;
the package's offsets are not binding coordinates. Syntax the package accepts
beyond this module never enters the dialect.

## Required conformance cases

Tests cover a block at byte zero and after a BOM; LF, CR, and CRLF; closing
at the end of the document; empty, whitespace-only, and comment-only
payloads; arbitrary Unicode and punctuation-bearing names; plain and quoted
numeric-, boolean-, and null-looking names, `1` beside `1.0`, and `1` with
`"1"` as a duplicate; YAML and JSON roots; null, empty text, empty list,
booleans, exact large integers, decimals, exponents, dates and date-times as
text, quoted escapes, folded single-line text, block and flow lists, quoted
link text, every allowed tag, and aliases; body parsing immediately after the
close; source order; every record scope, `Metadata.scope`, and body scopes.
Negative cases cover leading blank or text lines, second blocks, directives
and document indicators including `...` alone and after a mapping, short,
long, indented, trailed, and info-word fences, missing closers, malformed
payloads, empty or multiline names, sequence, mapping, alias, tagged, and
explicit keys, multiline text, boolean and null list items, nested values,
unsupported tags, non-finite numbers, undefined, cyclic, and over-budget
aliases, non-printable bytes, thematic-break and Setext interaction,
source-like bytes inside every container, option-off output, and allocation
failure.
