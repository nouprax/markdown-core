# Shared markup attributes

Status: normative target contract for the universal `Markup.attributes` field,
its value model, one Remark-derived attribute-list grammar, normalization, and
merge operation. It deliberately defines no attachment position. Remark
directive and Pandoc extensions attach this same syntax at positions defined by
their profile contracts; Pandoc does not contribute a second attribute grammar.

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
exception: repeated values accumulate in occurrence order. A separator is
inserted only when the accumulated class string is already non-empty. Therefore
an initial empty class contributes no leading space, while an empty class after
non-empty content contributes the authored empty position as a trailing or
internal space. Empty values remain strings, and numeric- or boolean-looking
values are never coerced.

These normalized invariants are the consumer model. An oracle that represents
IDs, classes, and key/value pairs in separate containers must be projected into
this one collection before comparison.

## Attribute-list grammar

```text
attributes       = "{" attribute-space* *( attribute attribute-space* ) "}"
attribute        = shorthand-id | shorthand-class | assignment | bare
shorthand-id     = "#" shorthand-value
shorthand-class  = "." shorthand-value
assignment       = name attribute-space* "=" attribute-space* value
bare             = name
value            = unquoted-value | single-quoted-value |
                   double-quoted-value
shorthand-value  = shorthand-first *shorthand-rest
shorthand-first  = any scalar except shorthand-stop or shorthand-invalid
shorthand-rest   = any scalar except shorthand-stop or shorthand-invalid
shorthand-stop   = "#" | "." | "}" | attribute-space
shorthand-invalid = DQUOTE | "'" | "<" | "=" | ">" | "`"
```

`attribute-space` is ASCII space, tab, or a source line ending when the
attachment position permits a multiline list. Form feed and other Unicode
whitespace are not separators. They are value content where the active value
production accepts an arbitrary scalar; where a name or another attribute is
expected, they make the complete container malformed. The grammar follows the
pinned `micromark-extension-directive@4.0.0` attribute tokenizer. Between
attributes, `#` begins ID shorthand and `.` begins class shorthand. Within a
shorthand value, either marker terminates the current value and is reprocessed
as the next shorthand opener without requiring whitespace. Thus
`{#one.two}` produces `id="one"` followed by `class="two"`, and
`{.one.two}` produces two class occurrences that normalize to
`class="one two"`. A colon is not a terminator: `{#one:two}` is the single ID
`one:two`.

A shorthand value is non-empty. A quote, `<`, `=`, `>`, or backtick before its
boundary makes the complete attribute container malformed. Other punctuation
is ordinary value content. A backslash has no escape semantics in a shorthand,
so the dot in `{#a\.b}` still opens a class and the preceding ID value is
`a\`.

A `name` is one or more Unicode scalar values. Its first scalar must be `-`,
`_`, a Unicode letter/number, or a non-whitespace non-punctuation scalar.
Subsequent scalars additionally permit `.` and `:`. Names are not decoded or
case-folded.

An unquoted value is non-empty and extends to attribute whitespace or `}`.
Quotes, `<`, `>`, `=`, and backticks make an unquoted value malformed rather
than ending it. A quoted value may be empty and ends at the next matching quote;
backslash does not escape that quote. The closing quote must be followed by
attribute whitespace or `}`. Consequently, `name=`, `name= `, and
`name="x"next=y` are malformed, while bare `name`, `name=''`, and `name=""`
produce an empty string.

Character references are decoded in assignment and shorthand values only after
the complete container has been scanned, using the pinned Remark
`parse-entities` HTML-attribute context. That context accepts its defined legacy
semicolonless forms; for example, `&amp` may decode before a shorthand marker
while an ambiguous alphanumeric continuation remains literal. A decoded `}`
cannot terminate its source container. Attribute names are not entity-decoded.

## Normalization and merge

Normalization occurs exactly once when attachment commits:

1. Expand ID and class shorthand and any placement-specific special form.
2. Decode character references in values.
3. Insert the first occurrence of a name at the current array position.
4. For repeated `class`, append one ASCII-space separator and the new value when
   the accumulated value is non-empty; otherwise replace the empty value.
   Replace every other repeated value without moving its first slot.

`merge(primary, inherited)` is used when an occurrence receives attributes
through indirection. It keeps the primary value for duplicate non-`class`
names, concatenates primary classes before inherited classes, and appends
inherited-only names in inherited order. The result is one `Attributes` array.

## Profile boundary

This contract decides the only braced attribute grammar. A profile decides only
where a complete list may attach, which `Markup` owns it, whether successful
attachment changes node recognition, and how owner scope changes:

- [Remark attributes](remark/attributes.md) owns directive attachment.
- [Pandoc attributes](pandoc/attributes.md) owns Pandoc attachment extensions.

Every `Markup` kind still has the universal field. A profile leaves it empty
when no enabled rule attaches or synthesizes attributes for that occurrence.
No profile may reinterpret a complete braced list. In particular, Pandoc
attachment sites use the Remark-derived shorthand boundaries, bare-name rule,
empty-value rule, and character-reference behavior rather than Pandoc's
different attribute tokenizer. Profile-specific non-container sugar, such as
the unbraced fenced-Div class word, is outside this grammar.

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

Tests must cover the universal field on every Markup kind; empty quoted and bare
values; rejected empty unquoted values; every grammar form; adjacent `#` and `.`
shorthand boundaries; retained `:`; Unicode names and values; literal
backslashes; Unicode-whitespace value and boundary positions; Remark
attribute-context character references; normalization; duplicate class, ID,
and custom names; primary/inherited merge; inert unsafe-looking metadata;
allocation failure; and size-doubling unique,
duplicate, malformed, and unclosed inputs. Attachment, scope, option, and
profile-specific fallback cases belong to the corresponding profile suite.
