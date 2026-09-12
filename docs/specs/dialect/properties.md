# Properties

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Place a properties envelope at the very beginning of a document, between two
lines containing exactly `---`.

```markdown
---
title: Field notes
authors: [Ada, Lin]
keywords:
  - Markdown
  - parsing
state: draft
---
The document body starts here.
```

The document has a `Metadata` value with title text, author and keyword lists,
and state text. The envelope is excluded from ordinary document content; the
body starts after its closing fence with its original source coordinates.
Metadata text is atomic and is not parsed as Markdown.

## Supported fields

Only these exact, case-sensitive names are recognized:

| Name | Intended use |
| --- | --- |
| `name` | Document name |
| `title` | Title |
| `subtitle` | Subtitle |
| `time` | Time |
| `date` | Date |
| `authors` | Authors |
| `keywords` | Keywords |
| `abstract` | Abstract; also supports literal prose |
| `state` | State |
| `comment` | Comment data; also supports literal prose |

All fields use the same scalar/list value domain. Names suggest how an
application may use a value; they do not cause date parsing, state validation,
or type coercion. Authors and keywords can be a single text value or a list;
a single value remains scalar.

## Values

```markdown
---
name: ""
title: null
subtitle: []
time: 1.50
date: 2026-09-13
authors: ['Ada Lovelace', Lin]
state: true
---
```

Empty text, authored null, an empty list, the exact number spelling `1.50`,
date text, a text list, and a boolean are distinct values. A missing field is
also distinct from a present null. Numbers retain their decimal spelling;
they are not converted to a host floating-point value.

A field uses `name: value`, with whitespace after the colon unless the value
is empty. Names may also be quoted. Supported values are:

- Empty or plain `null`: a null scalar.
- Plain `true` or `false`: a boolean scalar.
- A plain number matching `^-?(0|[1-9][0-9]*)(\.[0-9]*)?([eE][-+]?[0-9]+)?$`:
  an exact number string.
- Supported plain text or a quoted string: a text scalar.
- A bracketed comma-separated list, or following `- ` lines: a list of text
  and number items only. Optional trailing commas and indentless block lists
  are accepted; blank/comment lines do not split a block list.

`authors: - Ada` is text, not a list. A single quote is escaped by doubling it
inside single-quoted text. Double-quoted strings accept JSON-style escapes and
valid `\uXXXX` surrogate pairs. A decoded CR or LF invalidates a single-line
field. Quoted strings are always text.

Plain text cannot start with reserved indicators such as `&`, `*`, `!`, `|`,
`>`, `[`, or `{`; quote them to store text. A separated `#` starts an ignored
comment outside quotes, and a separated colon within plain text is unsupported.

## Literal prose

Use a bare `|` on `abstract` or `comment` for multiple lines:

```markdown
---
abstract: |
  First paragraph.

  **Still literal text.**
comment: |
  # An editor's note
  title: this remains prose
state: ready
---
Body.
```

The abstract is the text `First paragraph.\n\n**Still literal text.**\n`.
The comment keeps its hash and colon; they are not a heading or another field.
Each value has LF line endings and one final LF after its last nonblank line.
An empty/all-blank literal block becomes empty text.

The first nonblank body line establishes space indentation greater than the
field line's. That prefix is removed from each body line, preserving extra
indentation. A field at the same or lower indentation ends the block; an
insufficiently indented continuation invalidates the member. Only bare `|`
with an optional separated comment is supported: no folding `>`, `|-`, `|+`,
explicit indent indicators, or literal blocks on other fields.

## Envelope and recovery

The opening and closing lines must be exactly `---` at column one, without
trailing spaces. An optional UTF-8 BOM may precede the opener. The opener needs
a line ending; the closer can end the file. The first later exact closer wins.
Only one envelope is recognized, at the document's start. Without a closer,
the input follows normal Markdown parsing.

A complete envelope is accepted even when some or all members are unsupported.
Unknown names, unnamed lines, comments, `...`, invalid values, and later valid
duplicates are ignored. `...` neither closes nor invalidates the envelope.
The first successfully decoded occurrence of a field wins; an invalid earlier
occurrence does not reserve its name. A present envelope with no valid fields
still produces metadata with all fields absent.

Each member owns its continuation lines and list items. An unclosed bracketed
collection owns the remaining envelope payload, so apparent fields inside it
are not recovered as independent properties. Invalid members are skipped as
a whole, with no partial values.

This is a bounded properties language, not general YAML: nested objects/lists,
aliases, tags, merge keys, JSON root objects, and multiline quoted/plain folding
are unsupported. Metadata has one envelope scope, no individual field scopes,
and no markup visitor callbacks. The named `comment` field is data, not a
`Comment` node.
