# OFM footnotes

Status: normative target module. Authority:
[Basic formatting syntax — Footnotes](https://obsidian.md/help/syntax#Footnotes)
at the snapshot pinned by the
[OFM contract index](../obsidian-flavored-markdown.md).

The official syntax has two authoring forms: a `[^label]` citation resolved by
a `[^label]:` definition, and an inline `^[content]` footnote. Those spellings
do not create different consumer concepts. Both produce an inline `Cite`
containing one `Citation` whose `CitationReferent.footnote` resolves to a
document-owned `Footnote`.

## Source forms

Referenced calls, named or numeric labels, definitions, multiline definition
continuations, label matching, and duplicate-definition precedence retain the
inherited GFM footnote grammar. Obsidian additionally recognizes:

```text
inline-footnote = "^[" inline-content "]"
```

The caret immediately precedes the opening bracket. The documented form has a
non-empty body. A definition may contain block content; the inline form contains
only inline content. Obsidian's Reading-view-only note for the inline form is an
editor limitation and does not disable parser recognition.

## Consumer AST

Footnotes use one resolved-reference model:

```text
Document(
  content: [Markup],
  footnotes: [Footnote],
  scope: Scope
)

Cite(
  citations: [Citation],
  scope: Scope
)

Citation(
  referent: CitationReferent.footnote(id),
  prefix: [],
  suffix: [],
  scope: Scope
)

Footnote(
  id: String,
  content: [Markup],
  scope: Scope
)
```

`Cite` is an inline `Markup` kind. `Citation` is the scoped semantic item defined
by the shared [citation contract](../citation-model.md); every footnote cite contains
exactly one item. `Footnote` is a document-owned semantic value, not block
content and not an inline child. `Document.footnotes` is a named owned field
visited after `Document.content`. `Footnote.content` is block content; an inline
footnote body is normalized to one `Paragraph` whose content is the parsed
inline body.

For OFM and inherited GFM footnote syntax, `prefix` and `suffix` are present,
empty inline-content arrays and `referent` is `CitationReferent.footnote`. Text
surrounding a source footnote marker remains sibling paragraph content; it is
not moved into either field. The fields belong to the general citation contract
and do not encode a second footnote representation.

The group's `CitationReferent.footnote.id` resolves to exactly one `Footnote.id`
in the same document. IDs are opaque document-local strings: consumers may
compare and copy them but must not display or otherwise interpret them. A
referenced footnote uses the inherited normalized label key with the source
namespace caret removed. All calls that the inherited resolver matches
therefore carry the same ID.

An inline footnote receives `inline-N`, where `N` is its one-based order among
inline footnotes in source order. If that value collides with an authored ID or
an earlier generated ID, the parser appends `-K`, using the smallest positive
decimal `K` that makes it unique. ID allocation considers all authored
footnotes before assigning generated IDs, so the result is deterministic and
independent of block/inline pass order.

`Document.footnotes` is ordered by `Footnote.scope.start`, including referenced
definitions and inline values in one source order. Display numbering is
separately derived from the first `Cite` occurrence containing a
`CitationReferent.footnote` and never from `id`, its spelling, or
`Document.footnotes` order.

Every resolved `[^label]` call has the ID assigned to the winning inherited
definition. Multiple calls share that ID. Every inline `^[content]` creates one
new `Footnote` and one-item `Cite` whose citation has a
`CitationReferent.footnote` with the same ID.
Inline syntax therefore manufactures a semantic ID, but the generated value
carries no authored-label meaning. A valid unreferenced definition remains a
document-owned `Footnote`. A losing duplicate definition is not another target
and retains the inherited authored-content fallback.

The source label and its normalized lookup key are parser-internal resolution
data. They are not consumer fields. The target AST has no `InlineFootnote`,
`FootnoteCall`, `FootnoteReference`, `FootnoteDefinition`, or `CitationTarget`
kind, and no compatibility alias for any of them.

This retained ID edge is specific to a target with independent consumer
existence. A Markdown reference-link label is only parser indirection: after
resolution it produces an ordinary `Link` and no ID edge. `CitationReferent`
therefore has no link branch, `Citation` has no link-label content, and
`Document` has no link-target table. The inherited normalization is specified
by the
[integration module](inherited-and-integration.md#reference-links-and-images).

## Content and scopes

For a referenced form, both `Cite.scope` and its `Citation.scope`
cover the complete `[^label]` call; `Footnote.scope` covers the complete winning
or unreferenced definition. For an inline form, all three scopes cover the
complete `^[content]` spelling because the same source construct states both the
citation and its target; the synthesized paragraph and its inline descendants
cover only the body according to the shared source-position rules.

A `Citation` with a `CitationReferent.footnote` inside footnote content remains
an ID edge, so nested references do not create an object cycle in the immutable
tree. Consumers that recursively render note bodies must nevertheless detect
semantic ID cycles.

## Recognition and fallback

Inline-footnote recognition participates in the shared bracket algorithm; it
is not a regular-expression post-pass. Brackets escaped by a backslash do not
close the body. Balanced bracket constructs already recognized by the shared
inline engine retain their normal ownership. Inline code, comments, and source
bytes owned by an HTML token are opaque; paired inline HTML tags do not suppress
recognition between them.

An empty `^[]`, escaped opener, or missing closing bracket remains ordinary
text. An inherited `[^label]` call with no winning definition retains the
inherited literal fallback and produces no `Cite`. Failure must not
create a `Footnote`, allocate an ID, affect later display numbering, or consume
a bracket owned by another construct.

Collection, label resolution, source-order merging, and ID assignment are one
document footnote operation. They may not be reimplemented in a binding or
renderer, and they may not duplicate a `Footnote` body at each citation. The
operation is linear in source bytes plus AST output; ID lookup is bounded by the
shared normalized-reference map rather than repeated definition scans.

## Required conformance cases

Tests must cover numeric and named referenced footnotes, definitions before and
after calls, multiple calls, multiline and unreferenced definitions, duplicate
definitions, the exact one-item `Cite`/`CitationReferent.footnote` shape and
empty affixes, a plain inline body, mixed referenced/inline source order,
nested formatting and citations in note content, escaped brackets, empty and
unclosed inline forms, unresolved calls, code, comments, HTML, exact scopes,
deterministic IDs and visitation order, semantic cycles, allocation failure,
and adversarial runs of `^`, `[`, and `]`.
