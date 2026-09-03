# Shared markup attributes

Status: normative target contract for the universal `Markup.attributes` field,
its value model, shared attribute-list grammar, normalization, and merge
operation. It deliberately defines no profile-specific attachment position.
Remark directive attachment and Pandoc attachment are defined in their own
profile contracts.

## Universal consumer field

```text
Attribute(
  name: String,
  value: String
)

Attributes = [Attribute]

Markup(
  attributes: Attributes,
  ...
)
```

Every `Markup` kind has exactly one non-null `attributes` field. This is a
field shared by the tagged union, not an `Attributed` wrapper and not an
opt-in node capability. Concrete node schemas therefore omit it and specify
only their kind-specific fields.

An empty array means that the node has no semantic attributes. This includes
all of the following cases:

- its source kind has no enabled attribute attachment syntax;
- no attribute syntax was authored at that occurrence;
- an explicitly authored empty container, such as `{}`, attached successfully;
- an attachment extension was disabled or its candidate was malformed.

The consumer AST intentionally does not preserve whether an empty set came
from absence or `{}`. That distinction affects source recognition and scope,
not the node's semantic attributes. Parser-internal state may retain it while
recognizing a construct, but it is not a second public field.

`Attribute` is a value rather than `Markup`: it has no children, `scope`, or
attributes of its own. There is one type named `Attribute`, not parallel
`DirectiveAttribute`, `HeadingAttribute`, or Pandoc pair types. `id` and
`class` are entries in the same collection and are never duplicated as stored
node fields. Bindings may expose derived lookups such as `id`, `classes`, or
`attribute(named:)`.

## Value invariants

The array contains at most one entry for each exact, case-sensitive name and
preserves the source position of that name's first occurrence. A later
occurrence replaces the value in that slot. The exact name `class` is the sole
exception: repeated values accumulate in occurrence order, separated by one
ASCII space. Empty values remain strings, and numeric- or boolean-looking
values are never coerced.

These normalized invariants are the consumer model. An oracle that represents
IDs, classes, and key/value pairs in separate containers must be projected into
this one collection before comparison.

## Attribute-list grammar

```text
attributes          = "{" attribute-space*
                      [ attribute *( attribute-separator attribute ) ]
                      attribute-space* "}"
attribute-separator = 1*attribute-space
attribute           = shorthand-id | shorthand-class | assignment | bare
shorthand-id        = "#" shorthand-value
shorthand-class     = "." shorthand-value
assignment          = name attribute-space* "=" attribute-space* value
bare                = name
value               = unquoted-value | single-quoted-value |
                      double-quoted-value
```

`attribute-space` is ASCII space, tab, form feed, or a source line ending when
the attachment position permits a multiline list. Attributes are whitespace
separated. A `.` or `:` inside a shorthand value belongs to that value, so
`{#one.two}` is one ID rather than an ID followed by a class.

A `name` is one or more Unicode scalar values. Its first scalar must be `-`,
`_`, a Unicode letter/number, or a non-whitespace non-punctuation scalar.
Subsequent scalars additionally permit `.` and `:`. Names are not decoded or
case-folded.

An ID shorthand value is one or more Unicode letters/numbers or `-`, `_`, `:`,
and `.`, and may begin with any of them. A class shorthand value follows the
`name` rule and is non-empty. `#value` desugars to `id=value`; `.value`
desugars to `class=value`.

An unquoted value extends to ASCII whitespace or `}`. Quotes, `<`, `>`, `=`,
and backticks must be quoted or backslash-escaped. A quoted value ends at its
matching unescaped quote and may contain attribute whitespace. `name=`,
`name=''`, `name=""`, and bare `name` all produce an empty string.

Backslash escapes use the shared Markdown ASCII-punctuation rule. Character
references are decoded in values and shorthand values only after the complete
container has been scanned; a decoded `}` cannot terminate its container.

## Normalization and merge

Normalization occurs exactly once when attachment commits:

1. Expand ID and class shorthand and any placement-specific special form.
2. Decode escapes and character references in values.
3. Insert the first occurrence of a name at the current array position.
4. Append repeated `class` values with one ASCII-space separator; replace every
   other repeated value without moving its first slot.

`merge(primary, inherited)` is used when an occurrence receives attributes
through indirection. It keeps the primary value for duplicate non-`class`
names, concatenates primary classes before inherited classes, and appends
inherited-only names in inherited order. The result is one `Attributes` array.

## Profile boundary

This contract does not decide which grammar alternatives a profile accepts,
where an attribute list may attach, which `Markup` owns it, whether successful
syntax changes node recognition, or how owner scope changes. Those are source
language facts and must be stated exactly once by the relevant profile:

- [Remark attributes](remark/attributes.md) owns directive attachment.
- [Pandoc attributes](pandoc/attributes.md) owns Pandoc attachment extensions.

Every `Markup` kind still has the universal field. A profile leaves it empty
when no enabled rule attaches or synthesizes attributes for that occurrence.

Attributes are inert metadata. Parsing a name or value never executes code,
opens a target, reads a file, or changes layout. Interpretation and security
policy belong to consumers.

## Failure and complexity

A candidate commits atomically only after its closing `}` and every member have
parsed. Invalid or unclosed candidates emit no partial attributes. Scanning is
a single forward pass; normalization and merging use the shared bounded
string-key map and remain linear in source bytes plus output size. Allocation
failure aborts the owning parse operation.

## Required conformance cases

Tests must cover the universal field on every Markup kind; empty values; every
grammar form; Unicode names and values; escapes and character references;
normalization; duplicate class, ID, and custom names; primary/inherited merge;
inert unsafe-looking metadata; allocation failure; and size-doubling unique,
duplicate, malformed, and unclosed inputs. Attachment, scope, option, and
profile-specific fallback cases belong to the corresponding profile suite.
