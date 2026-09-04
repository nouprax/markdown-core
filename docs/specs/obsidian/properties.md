# Obsidian Properties

Status: normative target contract for the Obsidian file-header Properties
extension. The shared [document metadata contract](../metadata.md) owns the
consumer values; this module owns only source recognition, attachment,
fallback, and profile composition.

Authority: the official Obsidian
[Properties](https://obsidian.md/help/properties) page at the pinned
`obsidianmd/obsidian-help` snapshot, together with the YAML specification it
links. The help page states that Properties are YAML at the top of the file,
property names are user-defined and unique, JSON is accepted, Markdown and
nested properties are unsupported, and property types are assigned across a
vault rather than encoded completely in an individual note.

The help page does not publish a malformed-fence grammar or choose one of
YAML's optional scalar-resolution schemas. Exact fence spelling and the YAML
1.2.2 JSON-schema projection below are Markdown Core boundaries: they make
fallback and cross-platform scalar types deterministic without treating a
permissive library behavior as Obsidian syntax.

## Envelope grammar

```text
document             = [ utf8-bom ] [ properties-block ] markdown-body
properties-block     = opening-fence line-ending properties-payload
                       closing-fence [ line-ending ]
opening-fence        = "---"
closing-fence        = "---"
line-ending          = LF | CRLF
properties-payload   = *yaml-character
```

The opening fence is recognized only as the first decoded line, after an
optional UTF-8 BOM. It contains exactly three ASCII hyphens and no indentation,
prefix, suffix, or trailing whitespace. The closing fence is the first later
line with the same exact spelling at column one. It may end at EOF. An indented
`---` inside a YAML scalar is payload, not a closing fence.

Only one Properties block can occur. A later `---` line is inherited Markdown
syntax even if it surrounds valid YAML. `...`, four or more hyphens, a fence
with an info word, and a fence preceded by a blank line are not Properties
fences. The payload may be empty, whitespace-only, or comment-only; each form
produces non-null empty `Metadata` when the YAML document is valid.

For example, this complete block is valid:

```yaml
---
# Maintainer note; not metadata content.
---
```

Its canonical projection is `Metadata(records=[])`. The comment contributes no
record or comment node, but `Metadata.scope` covers both fences and the comment
line because the complete header remains an authored source region.

The outer fences are an Obsidian Markdown extension rather than YAML stream
document markers. The payload between them is passed as one independent YAML
1.2.2 JSON-schema document to the operation defined by the shared metadata
contract. A top-level JSON object uses the same envelope and produces the same
records as an equivalent YAML mapping.

## Attachment and body parsing

Recognition occurs before the first inherited block-start decision. A complete
valid block populates `Document.metadata`, owns every byte through its closing
fence, and is removed from `Document.content`. The remaining bytes are parsed
once by the ordinary inherited block parser. A blank line after the closing
fence is ordinary separation and does not belong to metadata. The closing
line's terminator separates the header from the body but is outside the
inclusive `Metadata.scope` defined by the shared contract.

No metadata syntax is recognized inside a block quote, list item, directive,
HTML block, fenced code block, footnote, or any other container. Parsing a
Properties block does not enable any other Obsidian extension inside scalar
values.

The source names are unrestricted beyond the shared non-empty string and
uniqueness invariants. There is no parser keyword table. Official default
names and plugin conventions have no effect on recognition or AST shape:

```yaml
---
aliases:
  - Doggo
project-status: active
arbitrary user key: 17
---
```

All three entries are ordinary records. A downstream vault resolver may
interpret `aliases`, but the parser does not resolve `[[Doggo]]`, add a
dedicated aliases field, or rewrite `Destination.cross.path`.

The mapping-key position always denotes a textual property name. Plain scalar
spellings such as `1`, `true`, and `null` are therefore names with those exact
strings, not Number, Checkbox, or empty values; quoting them does not change
the consumer name. JSON-schema scalar resolution applies only on the value
side of an entry. Sequence, mapping, and alias keys are not property names.

## Property values

The source forms project as follows:

| Authored form | Consumer value |
| --- | --- |
| `name:` or `name: null` | `scalar(null)` |
| `name: true` / `name: false` | `scalar(bool(...))` |
| `name: 1977`, `name: 3.14` | `scalar(number("..."))` |
| plain, single-quoted, or double-quoted scalar | `scalar(text(...))` |
| block or flow sequence of text and numbers | `list([...])` |
| `{ "name": "value" }` as the root payload | one ordinary record |

Date and date-time examples are text at parse time. Text values and list text
items must each decode to one line. They remain atomic, so
`title: "**Draft**"`, `tag: "#topic"`, and
`link: "[[Episode IV]]"` retain those decoded strings without `Strong`, tag,
or `CrossLink` children. This follows the documented absence of Markdown in
Properties and keeps vault indexing outside the parser.

Obsidian's Properties UI exposes Text, List, Number, Checkbox, Date, Date &
time, and Tags types, but the type chosen for a name is vault-wide external
state. The source alone can determine YAML scalar shape; it cannot determine
whether a text-shaped date is configured as Date or Text. No `PropertyType`
field is therefore stored or inferred.

List items are limited to the documented text and number forms; a checkbox or
null item is unsupported even though the same scalar can be valid as a direct
record value. Nested YAML mappings and sequences are left to source-mode tools
by Obsidian and are unsupported here. They invalidate the Properties candidate
instead of being flattened, stringified, or exposed through a second
arbitrary-object model.

## Transactional fallback and precedence

The parser tentatively scans the complete envelope and validates its payload
before committing. An unclosed fence, invalid YAML, duplicate name,
non-mapping root, unsupported nested value, invalid alias, or other projection
failure sets `Document.metadata` to null and returns all bytes to the inherited
Markdown block parser. It must not silently remove a thematic break, heading,
paragraph, or later fence.

Consequently, an opening-looking line has higher precedence than an inherited
thematic break only when the complete Properties block succeeds. There is no
post-parse deletion of already constructed Markdown blocks and no regular
expression rescan of a finished AST.

The `obsidian` preset enables this rule. Other profiles retain their existing
source grammar and always leave `Document.metadata` null unless they explicitly
enable this same extension in the future. This module does not introduce an
Obsidian dialect parser or Pandoc metadata behavior.

## Oracle and product evidence

The official help snapshot is normative. The offline executable oracle uses
exact-pinned `gray-matter` for the documented beginning-of-file envelope and
`js-yaml` with `JSON_SCHEMA` for the supported value intersection. This is
supplementary implementation evidence: syntax or values accepted only by
those packages do not extend this contract.

Oracle comparison omits scopes and covers only successful documented forms.
Markdown Core fixtures own exact scopes, invalid-candidate fallback, BOM and
line-ending boundaries, strict fence spelling, property-name source shape and
source order, nested-value rejection, precision-preserving number payloads,
and resource failures.

## Required conformance cases

In addition to the shared metadata cases, profile tests must cover a block at
byte zero and after a BOM; LF and CRLF; closing at EOF; empty, whitespace, and
comment-only payloads; arbitrary names; plain and quoted numeric- and
boolean-looking names; YAML and JSON root mappings; body parsing immediately
after the close; and option-off behavior. Boundary cases must cover leading
blank/text, second blocks, `...`, short/long/indented/trailed/info-word fences,
indented scalar separators, missing close, malformed payloads, empty or
multiline names, sequence/mapping/alias keys, duplicate names after decoding,
non-mapping roots including an explicit root null, multiline text,
boolean/null list items, nested values, interaction with thematic breaks and
Setext headings, and source-like bytes inside every container. Every accepted
case must assert `Metadata.scope`, each record scope, body scopes, source
order, and absence of a metadata node in content.
