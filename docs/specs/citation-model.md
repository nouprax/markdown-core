# Citation semantic model

Status: normative target contract for the shared consumer AST. This document
defines citation occurrences and referents independently of any source syntax.
Pandoc bibliography syntax is defined in
[Pandoc citations](pandoc/citations.md); OFM footnote syntax is defined in the
[footnote module](obsidian/footnotes.md).

## Consumer AST

```text
BibMode = normal | authorInText | suppressAuthor

Cite(
  citations: [Citation],
  scope: Scope
)

Citation(
  referent: CitationReferent,
  prefix: [Markup],
  suffix: [Markup],
  scope: Scope
)

CitationReferent =
  bib(
    key: String,
    mode: BibMode
  )
  | footnote(
    id: String
  )
```

`Cite` is an inline `Markup` kind. Its non-empty `citations` array preserves
source item order. `Citation` is a scoped semantic value owned only by a
`Cite`; `prefix` and `suffix` are non-null inline-content arrays, with absence
represented by an empty array.

Traversal enters the `Cite`, then each `Citation` in order, visiting that
item's prefix before its suffix. `CitationReferent` has no `Markup` children or
independent scope; the owning `Citation.scope` covers the source occurrence.

Every item has exactly one referent. A `bib` referent has a non-empty opaque
key and one mode. A `footnote` referent has a non-empty document-local ID.
Fields belonging to the other branch do not exist as nullable parallel state.
One `Cite` cannot mix referent families.

## Semantic boundaries

`Cite` is the occurrence/cluster boundary on which sorting, punctuation, and
style rendering operate. Flattening it into adjacent `Citation` nodes loses a
consumer-visible fact. Conversely, group-level copies of prefix, suffix, key,
mode, or ID would duplicate item state and are forbidden.

`CitationReferent.bib` denotes an external bibliography record. Its `BibMode`
is an authored occurrence role, not a final layout instruction. Selecting a CSL
note style does not turn it into `footnote`; a citation processor may render a
bibliographic cite as a note without changing its referent.

`CitationReferent.footnote` denotes an edge to a `Footnote` in the same
document's side table. Display numbering is derived from cite order and is not
the footnote ID. The same string may independently identify a bibliography
record and a footnote because the tagged branch selects the namespace.

A Markdown reference-link label is parser indirection and is not a citation
referent. Successful resolution produces an ordinary `Link`; the model has no
`CitationReferent.link` branch.

The AST stores no rendered fallback, CSL locator classification, note number,
or implementation hash. Those are processor output, locale-dependent
interpretation, derived document state, or cache keys rather than parser-owned
semantic inputs.

## Required conformance cases

Tests must cover one- and multi-item groups, both referent branches, all
`BibMode` values, empty and populated affixes, item/traversal order, rejection
of empty referents and mixed families, exact scopes, binding projection, and
allocation failure.
