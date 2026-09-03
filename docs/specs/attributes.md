# Shared markup attributes

Status: normative target contract for the universal `Markup.attributes` field,
its Pandoc-derived consumer model, the one shared Pandoc 3.11 attribute-list
grammar, normalization, and merge operation. It deliberately defines no
attachment position. Remark directives and Pandoc extensions attach this same
syntax at positions defined by their profile contracts.

Authority: the Pandoc 3.11 User's Guide for
[attributes](https://pandoc.org/MANUAL.html#extension-attributes), the pinned
[Markdown reader](https://github.com/jgm/pandoc/blob/b913622e1ff87c69ab8b1a606577122e220925cd/src/Text/Pandoc/Readers/Markdown.hs#L643-L690),
the pinned
[`combineAttr`](https://github.com/jgm/pandoc/blob/b913622e1ff87c69ab8b1a606577122e220925cd/src/Text/Pandoc/Shared.hs#L599-L607),
and official native/JSON output under the
[Pandoc oracle policy](../../specs/oracles/pandoc/README.md).

## Universal consumer field

```text
Attribute(
  name: String,
  value: String
)

Attributes(
  identifier: String?,
  classes: [String],
  keyValues: [Attribute]
)

Markup(
  attributes: Attributes,
  ...
)
```

Every `Markup` kind has exactly one non-null `attributes` field. This is a
field shared by the tagged union, not an `Attributed` wrapper and not an
opt-in node capability. Concrete node schemas therefore omit it and specify
only their kind-specific fields.

`Attributes.empty` is `identifier=null`, `classes=[]`, and `keyValues=[]`. It
covers all of the following consumer-equivalent states:

- the source kind has no enabled attachment rule;
- no attribute syntax was authored at that occurrence;
- an explicitly authored `{}` attached successfully;
- an attachment extension was disabled or its candidate was malformed.

The consumer AST does not retain whether the empty state came from absence or
`{}`. That distinction affects recognition and scope, not semantic attributes.
Parser-internal state may retain it while recognizing an owner, but it is not a
second public field.

`Attribute` and `Attributes` are values rather than `Markup`: they have no
children, scope, or attributes of their own. There is one universal payload,
not parallel directive, heading, link, or Pandoc attribute types. Bindings may
provide lookups over `keyValues`, but no lookup is a second stored authority.

## Value invariants

The shape is the named projection of Pandoc's `Attr` tuple:

- `identifier` is either `null` or a non-empty string. The last authored ID
  shorthand or exact lowercase `id=` assignment wins. An authored empty ID
  normalizes to `null`; source-presence distinctions remain parser-internal.
- `classes` preserves class order and duplicates. A class shorthand appends
  one class. An exact lowercase `class=` assignment splits its value into
  Unicode whitespace-delimited words and appends those words. The special
  member `-` appends `unnumbered`.
- `keyValues` contains every other assignment in source order. Duplicate names
  remain separate entries; names and values are case-sensitive strings.

The exact names `id` and `class` never also appear in `keyValues`. Empty custom
values remain empty strings, and numeric- or boolean-looking values are never
coerced. An oracle exposing Pandoc's empty-string identifier is projected to
`null`; its class and key/value arrays otherwise map without flattening or
deduplication.

## Attribute-list grammar

```text
attributes        = "{" spacing *( attribute spacing ) "}"
attribute         = identifier | class | assignment | special
identifier        = "#" 1*identifier-character
class             = "." name
assignment        = name "=" value
special           = "-"
name              = unicode-letter *name-rest
name-rest         = unicode-letter | unicode-number | "-" | "_" | ":" | "."
identifier-character
                  = unicode-letter | unicode-number | "-" | "_" | ":" | "."
value             = quoted-value | unquoted-value
quoted-value      = DQUOTE *double-quoted-character DQUOTE |
                    "'" *single-quoted-character "'"
unquoted-value    = *unquoted-character
double-quoted-character
                  = escaped-punctuation | character-reference |
                    permitted-line-ending | any scalar except DQUOTE or line-ending
single-quoted-character
                  = escaped-punctuation | character-reference |
                    permitted-line-ending | any scalar except "'" or line-ending
unquoted-character
                  = escaped-punctuation |
                    any scalar except SP, TAB, line-ending, or unescaped "}"
spacing           = *( SP | TAB ) [ line-ending *( SP | TAB ) ]
```

This is the grammar implemented by Pandoc 3.11's pinned Markdown reader, not
Pandoc's default extension bundle. `spacing` admits at most one line ending and
never a blank line. It is optional, so independently delimited members may be
adjacent: `{#a#b}` is two IDs and the latter wins, while `{-k=v}` contains the
special `-` followed by `k=v`.

An ID shorthand is non-empty, may begin with any identifier character, and
retains dots and colons. A class or assignment name must begin with a Unicode
letter; subsequent characters additionally admit Unicode numbers, `-`, `_`,
`:`, and `.`. Consequently, `{#one.two}` has identifier `one.two`,
`{.one.two}` has the one class `one.two`, `{#1}` is valid, and `{.1}` and
`{_key=value}` are malformed. A generic bare name is not an attribute; the
lone `-` is the only value-less member and appends class `unnumbered`.

An unquoted value extends to ASCII space, tab, a line ending, or an unescaped
`}` and may be empty. Every other scalar, including quotes, `<`, `=`, `>`, and
backtick, is ordinary unquoted content. A backslash removes itself before an
escapable Markdown ASCII-punctuation character; before any other character it
remains literal. This permits an escaped `}` inside an unquoted value.
Non-ASCII whitespace is not `spacing`: it may occur inside a value, but it
cannot separate two members.

A quoted value is selected only when a matching closing quote can be parsed.
It may be empty, uses the same ASCII-punctuation escapes, decodes valid
semicolon-terminated character references, and normalizes each permitted line
ending to an ASCII space. A blank line is invalid. If the opening quote has no
match, the unquoted alternative may consume it as ordinary content. Character
references in unquoted values and shorthand values are not decoded.

## Normalization and merge

Normalization occurs exactly once when attachment commits:

1. Set `identifier` from `#value` or `id=value`, with the later occurrence
   replacing the earlier one and an empty final value becoming `null`.
2. Append `.value` as one class, split `class=value` into words and append them,
   and append `unnumbered` for each special `-`.
3. Append every other assignment to `keyValues` without deduplicating it.

`merge(primary, inherited)` follows Pandoc's `combineAttr` operation for a
resolved reference occurrence:

1. Use the primary identifier when non-null; otherwise use the inherited one.
2. Concatenate primary classes before inherited classes, then stable-deduplicate
   them by exact value.
3. Keep primary key/value entries as written. From the inherited sequence,
   retain only the last occurrence of each name absent from the primary
   sequence; keep the surviving inherited entries in source order and place
   them before the primary entries.

The resolved occurrence receives one `Attributes` value. A reference
definition remains parser-owned and is not emitted as a public metadata node.

## Profile boundary

This contract decides the sole consumer model and braced attribute grammar. A
profile decides only where a complete list may attach, which `Markup` owns it,
whether successful attachment changes node recognition, and how owner scope
changes:

- [Remark attributes](remark/attributes.md) owns directive attachment.
- [Pandoc attributes](pandoc/attributes.md) owns Pandoc attachment extensions.

Every `Markup` kind retains `Attributes.empty` when no enabled rule attaches or
synthesizes attributes. No profile may reinterpret a complete braced list.
Remark's directive envelope remains Remark-owned, but its different attribute
member tokenizer is deliberately not a second grammar. Profile-specific
non-container sugar, such as the unbraced fenced-Div class word, is outside the
braced grammar.

Attributes are inert metadata. Parsing an identifier, class, name, or value
never executes code, opens a target, reads a file, or changes layout.
Interpretation and security policy belong to consumers.

## Failure and complexity

A candidate commits atomically only after its closing `}` and every member has
parsed. Invalid or unclosed candidates emit no partial attributes. Scanning is
a single forward pass; normalization and merging remain linear in source bytes
plus output size. Allocation failure aborts the owning parse operation.

## Required conformance cases

Tests must cover the universal field on every Markup kind; `Attributes.empty`;
the three payload components; dots and colons retained in shorthand; numeric ID
starts; letter-only name starts; `{-}`; generic bare-name rejection; empty,
quoted, and unquoted values; escapes; quoted-only character references;
line-ending normalization; non-ASCII whitespace positions; ID replacement;
ordered duplicate classes and key/value entries; `id=` and `class=` projection;
reference merging; inert unsafe-looking metadata; malformed and unclosed
fallback; allocation failure; and size-doubling valid, duplicate, malformed,
and unclosed inputs. Attachment, scope, option, and profile-specific fallback
cases belong to the corresponding profile suite.
