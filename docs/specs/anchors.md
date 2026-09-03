# Shared markup anchors

Status: normative target contract for the universal `Markup.anchor` field.
This module owns the consumer meaning and invariants of an anchor; profile
modules own the source rules that populate it.

## Consumer model

```text
Markup(
  anchor: String?,
  ...
)
```

Every `Markup` kind has exactly one nullable `anchor` field. It is a field of
the tagged union, not an `Anchor` node, an opt-in capability, or a field copied
onto selected concrete schemas. `null` means that the node declares no link
target. A non-null anchor is always a non-empty string.

An anchor names the target node for consumers. It is not the node's object
identity, a source label, a reference edge, or a resolver result. The value
does not record which profile or source spelling produced it.

## Source-independent value

Source punctuation is excluded from the stored value:

| Source rule                                     | Owner value    |
| ----------------------------------------------- | -------------- |
| Pandoc attribute `{#foo}` or `{id=foo}` | `anchor="foo"` |
| generated heading identifier `foo` | `anchor="foo"` |
| Obsidian block identifier `^foo` | `anchor="foo"` |

These are one consumer fact. There is no heading/block/fragment discriminator
and no distinct heading, attribute, directive, or Obsidian anchor type.
Characters such as `#` and `^` describe a reference spelling or an attachment
grammar; they do not qualify the identity declared by the target node.
Accordingly, `[[#Heading]]` and `[[#^block-id]]` both populate
`Destination.cross.anchor`; the parser does not reproduce their punctuation as
an anchor-kind tag.

Incoming `Link`, `CrossLink`, citation, and footnote values do not populate
their own `anchor` merely because they refer to something. A resolver may use
a link's shared [`Destination`](destinations.md) or a citation referent to find
a node whose anchor matches, but the reference-side value and declaration-side
anchor remain different facts. Resolution remains downstream and does not
mutate the AST.

## Population and precedence

Only a successfully enabled attachment or synthesis rule may populate an
anchor. The current rules are:

- the shared attribute grammar's ID shorthand and exact lowercase `id=`
  assignment at Remark or Pandoc attachment sites;
- Pandoc/GFM automatic heading-anchor synthesis; and
- Obsidian block-identifier attachment.

The defining profile module owns recognition, attachment position, fallback,
and owner scope. The [shared attributes contract](attributes.md) owns ID
normalization and reference-occurrence inheritance. The
[Pandoc heading-anchor contract](pandoc/headings-and-anchors.md) owns generated
values and collisions. The
[Obsidian block-identifier contract](obsidian/block-identifiers.md) owns its
addressable set and duplicate-candidate boundary.

One node has at most one final anchor. An explicit non-empty heading anchor
wins over automatic synthesis. Within one attribute list, the last authored ID
member wins and an empty final `id=` removes that list's candidate. When a
resolved reference occurrence inherits metadata, its own non-null anchor wins
over the definition's anchor. No operation concatenates anchors or silently
stores a second value.

Document-wide duplicate handling is source-rule-specific. Automatic heading
anchors are uniquified by their registry; explicit duplicates remain authored
facts and may produce diagnostics. Obsidian vault-wide uniqueness and target
selection require workspace context and remain outside the parser.

## Scope and lifecycle

Authored anchor syntax is included in the owning Markup's scope even though its
punctuation is absent from visible content and from the stored anchor. A
synthesized anchor has no fictional source range and does not extend the owner
scope. `anchor` is an owned immutable string and has no scope or attributes of
its own.

Recognizing an anchor never opens a link, resolves a document, creates an HTML
`id`, or changes rendering. Those are consumer policies. Allocation failure
aborts the owning parse operation without publishing a partially attached
anchor.

## Required conformance cases

Tests must cover `anchor=null` on every Markup kind; explicit attribute IDs;
automatic heading anchors; Obsidian block identifiers; removal of source
punctuation; identical values produced by different source rules; last-ID and
explicit-over-generated precedence; reference inheritance; explicit and
generated duplicates; exact owner scopes; unresolved incoming references not
mutating targets; allocation failure; and size-doubling anchor candidates.
