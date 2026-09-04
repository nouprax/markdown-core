# Shared link destinations

Status: normative target contract for the consumer-facing `Destination` value.
This module owns the source-independent shape and invariants of outgoing link
targets. Profile modules own the source rules that construct each branch.

## Consumer model

```text
Destination =
  url(String)
  | cross(
    path: String,
    anchor: String?
  )

Link(
  dest: Destination,
  title: String?,
  content: [Markup],
  scope: Scope
)

CrossLink(
  embedded: Bool,
  dest: Destination,
  label: String?,
  scope: Scope
)
```

`Destination` is a tagged value, not a `Markup` node. It has no children,
scope, declaration-side `Markup.anchor`, or attributes. Branch-specific fields
exist only in their branch; consumers never receive a kind flag alongside a
bag of nullable URL, path, and anchor fields.

An ordinary Markdown `Link` always owns the `url` branch. Its `value` is the
complete semantic destination produced by the inherited link grammar,
including an empty destination, a relative reference, or a fragment-only
reference such as `#section`. Moving this value from `Link.destination` to
`Link.dest = Destination.url(...)` changes the consumer shape, not the
inherited source grammar, escaping, normalization, or resolution behavior.

An Obsidian `CrossLink` owns the `cross(path, anchor)` branch. A null `anchor`
addresses the whole workspace resource and requires a non-empty `path`. A
non-null `anchor` is non-empty and may pair with an empty path to address the
current document. Both `#Heading` and `#^block-id` source spellings populate
the same field after removing their source punctuation. The tagged value
deliberately does not retain a heading, block, or fragment discriminator: those
spellings all address the universal target identity declared by
`Markup.anchor`.

The anchor value is non-empty. It may preserve a hierarchy such as
`Parent#Child` or a resource-specific value such as `page=3`; a parser without
workspace and target-kind context must not reinterpret it. The
[Obsidian wikilink contract](obsidian/wikilinks-and-embeds.md) owns the exact
source projection and fallback rules.

The node kinds remain distinct because they carry different consumer
semantics: `Link.content` is parsed inline label content and `Link.title` is
Markdown title metadata, while `CrossLink.label` is one raw authored parameter
and `CrossLink.embedded` requests transclusion. Sharing `Destination` does not
collapse those nodes or turn a normal Markdown link into a `CrossLink`.

## Declaration and resolution boundary

`Destination` describes an outgoing reference. The universal
[`Markup.anchor`](anchors.md) field declares a target on the referenced node.
Neither value populates or owns the other.

For example, `Destination.url("#foo")` and
`Destination.cross(path="", anchor="foo")` may both resolve to a node whose
`anchor == "foo"`. Those are still two authored reference forms plus one
declaration fact. Resolution is downstream, may require document or workspace
context, and never rewrites the destination or target AST node.

The parser does not fetch a URL, open a file, resolve a vault path, test target
existence, or infer a target media type. Such results are not additional enum
branches or cached fields in the parse tree.

## Ownership and lifecycle

Each reference node owns exactly one immutable `Destination`. Its strings are
owned values and cannot borrow parser scratch storage or reference-definition
tables. A successfully resolved Markdown reference link copies or shares the
winning definition's URL internally but publishes an owned
`Destination.url`; the source definition and lookup key remain parser state.

Allocation failure aborts construction of the owning reference node without
publishing a partial destination. Traversal visits the owning `Link` or
`CrossLink` but does not visit `Destination` as a child node.

## Required conformance cases

Tests must cover empty, absolute, relative, and fragment-only `url` values;
whole-resource `cross` values with null anchors; empty same-document paths;
heading and block source spellings populating the same `cross.anchor` field;
hierarchical and resource-specific anchor values; rejection of invalid branch/field
combinations; direct and resolved reference links producing the same `url`
branch; declaration/reference separation; exact owner scopes; all public
binding projections; allocation failure at every owned string; and
size-doubling URL, path, and anchor inputs.
