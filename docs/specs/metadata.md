# Document metadata

Status: normative target contract for the document-owned metadata value used
by the Obsidian Properties extension. It defines one consumer model; the
[Obsidian Properties module](obsidian/properties.md) owns the only source
attachment rule. Pandoc `yaml_metadata_block`, metadata files, title blocks,
and metadata-string Markdown parsing are outside this contract.

Authority: the official Obsidian
[Properties](https://obsidian.md/help/properties) contract and its linked
[YAML](https://yaml.org/spec/1.2.2/) data-language specification. Where the
Properties page deliberately exposes a smaller value domain than YAML, this
contract follows that documented Obsidian domain. The pinned source files and
executable evidence are recorded by the
[Obsidian oracle policy](../../specs/oracles/obsidian/README.md).

## Consumer model

```text
Document(
  content: [Markup],
  metadata: Metadata?,
  footnotes: [Footnote],
  scope: Scope
)

Metadata(
  records: [MetadataRecord],
  scope: Scope
)

MetadataRecord(
  name: String,
  value: MetadataValue,
  scope: Scope
)

MetadataScalar =
    null
  | bool(Bool)
  | number(String)
  | text(String)

MetadataListItem =
    number(String)
  | text(String)

MetadataValue =
    scalar(MetadataScalar)
  | list([MetadataListItem])
```

`Document.metadata == null` means that no valid Properties block occurred.
A non-null `Metadata(records=[])` means that an explicitly authored, valid
block with no semantic records occurred. Its payload may be empty,
whitespace-only, or contain only YAML comments. YAML comments never produce a
`MetadataRecord`, metadata value, or `Markup` node; they remain source bytes
owned by `Metadata.scope`. This distinguishes every such authored block from
absent metadata while giving all three forms the same consumer value.

`Metadata`, `MetadataRecord`, `MetadataScalar`, `MetadataListItem`, and
`MetadataValue` are values, not `Markup` kinds. Metadata is out-of-band
document data and is never inserted into `Document.content`. It does not
acquire the universal `Markup.anchor` or `Markup.attributes` fields and does
not participate in Markup traversal.

`records` preserves source order and contains each decoded name exactly once.
It is not a keyword enum or a fixed schema. Bindings may provide exact-name
lookup conveniences, but a lookup is derived behavior and is not a second
stored map.

## Names and value invariants

A record name is a non-empty, single-line decoded Unicode string. Names are
case-preserving and case-sensitive in the canonical value. No spelling is
lowercased, slugged, pluralized, or rewritten. A root mapping key is a
property-name position rather than a property-value position: a directly
authored plain, single-quoted, or double-quoted scalar is decoded as text
without applying the value-side null, boolean, or number resolution operation.
Consequently `1` and `"1"` name the same record, as do `true` and `"true"`. A
sequence, mapping, or alias cannot supply a record name. Uniqueness is checked
after text decoding, so differently spelled keys which decode to the same name
are duplicates.

Text decoding preserves the complete scalar value rather than a numeric or
boolean canonicalization. For example, the plain keys `1`, `1.0`, `1e0`, `01`,
`0`, `-0`, `null`, and `~` produce those eight exact names. They remain
distinct from one another; in particular, `1` and `1.0` may coexist even when
a general-purpose YAML binding would resolve both to the same numeric value.
Each plain form is instead a duplicate of a quoted key that decodes to the
same text, such as `1.0` and `"1.0"`.

The accepted name set is open. `tags`, `aliases`, `cssclasses`, `publish`, and
every other product-known spelling occupy the same `String` domain as a user
name such as `project-status`. The parser neither rejects unknown names nor
adds dedicated `Document` fields for known ones. Deprecated singular names
are not rewritten to plural names.

Scalar values have these exact meanings:

- `null` is an empty scalar, a plain scalar whose implicit resolution is null,
  or a scalar carrying the allowed explicit standard null tag. It differs from
  an empty text value and an empty list.
- `bool` is an unquoted lowercase `true` or `false` value or a scalar carrying
  the allowed explicit standard boolean tag.
- `number` contains the complete decoded ASCII number spelling without
  conversion through a platform integer or floating-point type. The branch,
  rather than the payload's host type, carries numeric semantics. This avoids
  precision loss and preserves integers, decimals, and exponents uniformly
  across C, Swift, Kotlin, and ECMAScript.
- `text` contains the single-line decoded string after YAML quoting, escapes,
  and character processing. A source form which decodes a line ending is not a
  supported Text property. Date (`YYYY-MM-DD`) and date-time
  (`YYYY-MM-DDTHH:mm:ss`) spellings remain text because Obsidian assigns
  property types by name across a vault; that external registry is not encoded
  in one Markdown document.

A list is ordered and contains only single-line text and number items, matching
the documented List property domain. Checkbox and null items, nested sequences,
and mappings are not Properties list values in the target model. An empty list
differs from null. Block and flow sequence spellings have the same consumer
value.

Text is atomic data. Markdown emphasis, headings, tags, HTML, and other inline
or block syntax are not parsed inside it. A quoted `[[Internal link]]` remains
the decoded text value; any vault index or resolver that interprets such a
value operates outside this AST and must not replace the stored text. URLs are
handled identically.

## YAML projection

The Properties payload is decoded as one YAML 1.2.2 document. Plain values use
JSON scalar resolution with a string fallback: exactly `null` resolves to null;
exactly `true` and `false` resolve to booleans; a scalar matching
`^-?(0|[1-9][0-9]*)(\.[0-9]*)?([eE][-+]?[0-9]+)?$` resolves to a number; every
other valid plain scalar resolves to text. This admits Obsidian's ordinary
unquoted text without also applying YAML 1.1 boolean, timestamp, infinity,
base-prefix, underscore, or leading-plus resolution. Quoted scalars resolve to
text unless an allowed explicit standard scalar tag says otherwise. Root
mapping keys use the property-name projection above instead of scalar
resolution. JSON object syntax is accepted through the same operation; it is
not a second parser or value model.

The payload is a single-node document, not a YAML stream embedded inside the
Properties envelope. YAML directives and explicit document-start or
document-end indicators are unsupported. In particular, `...` is not an empty
root and does not terminate Properties; it invalidates the complete candidate.
Here, an empty node means that the payload itself contains no representation
node—only zero bytes, whitespace, or comments. Only the following
representation graph projects successfully:

- the root is an empty node or one mapping;
- every mapping key is a directly authored scalar which decodes to a unique,
  non-empty, single-line name;
- every root value resolves to a supported scalar or a sequence of supported
  list items; and
- every alias in a value position is defined earlier in the same Properties
  block, is acyclic, and resolves to one of those supported values; and
- the payload contains no YAML stream directive, document-start indicator, or
  document-end indicator.

The standard `str`, `null`, `bool`, `int`, and `float` scalar tags may state a
supported scalar branch explicitly; the standard `map` and `seq` tags may
state the already-required collection kind. Every other explicit tag,
including application-specific tags, is unsupported. Complex or aliased keys,
duplicate names, nested mappings, nested sequences, cyclic aliases, non-finite
numbers, and multiple YAML documents do not have a canonical Metadata value.
YAML presentation details—comments, anchor names, quote style, flow versus
block style, and numeric formatting—are not additional public fields. The
exact numeric scalar spelling is the sole deliberate lexical payload because
converting it would lose consumer-visible precision.

An alias contributes the resolved value at the alias occurrence. It creates no
public reference node and no shared mutable object. Expansion must observe the
resource bounds below.

## Ownership and scopes

`Metadata.scope` is the source-faithful contiguous editor cursor range from the
opening fence through the closing fence, including both fences and all payload
bytes. It begins at the opening fence's first hyphen and ends at the closing
fence's third hyphen. A line ending after the closing fence is an envelope
separator rather than part of this cursor range. `Metadata.scope` is never the
range of the remaining document body.

Each `MetadataRecord.scope` covers that record's complete authored mapping
entry, including its key, value, continuation lines, and any sequence items it
owns, but excluding either Properties fence. An alias-resolved value retains
the alias-owning record's range; it does not copy or union the anchor
definition's range.

YAML decoding, escape processing, alias expansion, and other semantic
normalization never mutate a scope. A text or number payload may differ from
its source spelling without creating a fictional range. The enclosing
`Document.scope` continues to cover the complete document source, including
the Properties block, while each block in `Document.content` retains only its
own lexical occurrence.

## Semantic boundary

Metadata is inert document data. Parsing it does not:

- recognize a whitelist of names or apply special behavior to `aliases`,
  `tags`, `cssclasses`, or publishing fields;
- consult or mutate a vault-wide property-type registry;
- resolve paths, aliases, URLs, dates, tags, or internal links;
- load another file, execute a YAML tag, instantiate a host-language object,
  or invoke a template; or
- render, merge template values, or synthesize missing defaults.

A vault consumer may derive an alias index by exact-name lookup of an
`aliases` record, but that convention neither changes the generic stored value
nor turns `aliases` into a parser keyword. Link resolution results are not
written back into this AST.

## Failure and complexity

The source attachment operation is transactional. A candidate produces a
non-null `Metadata` only after the envelope, YAML document, root mapping,
unique names, and complete value graph validate. Failure releases the complete
candidate to inherited Markdown parsing and emits no partial records.

Parsing performs work linear in source bytes plus emitted metadata size.
Mapping lookup for duplicate detection is linear expected time or
`O(n log n)` worst-case with a comparison tree. Nesting, alias depth, decoded
scalar bytes, record count, list length, and total emitted metadata bytes obey
the parser's shared structural limits. Alias cycles are rejected. Repeated
aliases may not bypass the output budget or cause superlinear unbounded
expansion. Allocation failure aborts the owning document parse without a
partial public AST.

## Required conformance cases

Tests must cover absent and explicitly empty metadata; arbitrary Unicode and
punctuation-bearing names; source order; exact-name case preservation; null,
empty text, empty list, booleans, exact large integers, decimals, exponents,
dates and date-times retained as text, quoted escapes, text/number block and
flow lists, quoted internal-link text, JSON object syntax, comments, and
aliases; plain and quoted numeric-, boolean-, and null-looking names; exact
name preservation for `1.0`, `1e2`, `-0`, and `~`; and simultaneous distinct
names such as `1` and `1.0`. Negative cases must cover empty or multiline
names; sequence, mapping, or alias keys; duplicates after name decoding such
as `1` with `"1"`; multiline text; boolean/null list items; nested values;
unsupported tags; non-finite numbers;
undefined/cyclic/explosive aliases; multiple YAML documents; malformed YAML;
YAML directives and document boundary indicators, including document-end-only
and mapping-followed-by-document-end payloads; allocation failure; and each
structural limit. Profile fixtures own the envelope, option gate, fallback,
precedence, and exact block/record scopes.
