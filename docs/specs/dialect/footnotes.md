# Footnotes

Status: normative module of the [Markdown Core dialect](../dialect.md).
Options: `footnotes` (default `true`) and `inlineFootnotes` (default `false`,
effective only while `footnotes` is on). Sources: cmark-gfm's footnote
extension for the referenced form; Obsidian and Pandoc for the inline form.
Executable oracles: cmark-gfm and remark for the referenced form; the inline
form is product fixtures. Landing: the citation model with `M4`, the inline
form with `O4`; until `M4` the current contract's `FootnoteReference` and
`FootnoteDefinition` stand.

## The citation model

```text
BibMode = normal | authorInText | suppressAuthor

Cite(citations: [Citation])

Citation(referent: CitationReferent, prefix: [Markup], suffix: [Markup],
         scope: Scope)

CitationReferent =
  bib(key: String, mode: BibMode)
  | footnote(id: String)

Footnote(id: String, content: [Markup], scope: Scope)

Document(content: [Markup], metadata: Metadata?, footnotes: [Footnote])
```

`Cite` is an inline `Markup` kind whose non-empty `citations` array keeps
source order. `Citation` and `Footnote` are scoped values, not `Markup`: they
are written, so they carry a scope, and the walking visitor gives each a value
callback and descends into their markup arrays in declared field order.
`Citation.prefix` and `suffix` are non-null inline-content arrays, empty when
absent. Every item has exactly one referent; one `Cite` cannot mix referent
families; a `bib` referent is produced only by the [citations](citations.md)
module. `CitationReferent` has no scope; the owning `Citation.scope` covers
the occurrence. A Markdown reference link is parser indirection and is never a
citation; the model has no `link` branch and `Document` has no link registry.

`Cite` is the cluster on which a consumer sorts, punctuates, and renders. A
`footnote` referent is an edge to the `Footnote` in `Document.footnotes`
whose `id` is equal; display numbering is derived from cite order by the
consumer and is never the id. `Document.footnotes` is a named owned field,
visited after `Document.content`, ordered by `Footnote.scope.start`, and
holds referenced definitions and inline values in one sequence.
`Footnote.content` is inline-or-block content: a referenced definition holds
its parsed block content, and an inline footnote holds its parsed inline body
directly, with no synthesized `Paragraph`.

## Referenced footnotes

A footnote definition is step 13 of the block-start order:

```text
definition = *3SP "[^" label "]:" [ inline-content ] EOL continuation*
label      = 1*( any scalar except "]", "[", SP, and TAB )
```

A definition is recognized only while `footnotes` is on, only when the label
is at most 1000 bytes, and only at footnote container depth below 100. Its
continuation lines are indented at least four columns, and its content is
block content parsed by the ordinary block parser. Its key is the label under
the inherited reference-label normalization; the stored `Footnote.id` is that
key and never contains the caret. When two definitions share a key, the first
in source order wins; each later one is parsed in place as ordinary blocks
beginning with the literal `[^label]:`, produces no `Footnote`, and calls
resolve to the winner. A `[^label]:` line is never a link reference
definition. A valid definition that no call references is still a `Footnote`.

A footnote call is the first alternative of the bracket procedure: an
unescaped `[^label]` whose label is defined produces a one-item `Cite` whose
`Citation` has referent `footnote(id)` and empty affixes, whatever follows the
`]`, so `[^a](u)` and `[^a]{.x}` are a `Cite` followed by text. Only a literal
source caret opens a call: `[\^a]`, `[&#94;a]`, and `[&Hat;a]` are text.
Repeated calls share one `Footnote`; the body is never duplicated. A call
inside a footnote's own content is an id edge, not an object cycle; a consumer
that renders bodies recursively detects semantic cycles itself.

A call whose label no definition defines is not a call: the brackets are
inherited bracket text, may become a shortcut reference only when a link
definition labelled `^label` exists, and never create a `Footnote`, allocate an
id, or affect numbering.

## Inline footnotes

With `inlineFootnotes=true` and `footnotes=true`, an unescaped `^`
immediately followed by `[` pushes an inline-footnote opener onto the shared
bracket stack at inline step A7. The `]` that matches it closes the footnote
without attempting any link, reference, span, cite, or attribute tail, so
`^[a](b)` is a `Cite` followed by text `(b)`. Inside the body, a link opener
is closed by its own `]` and tail, and a footnote opener inside a link label
is closed by the first `]`. At one `^`, the inline footnote wins over a
`[^label]` call and over superscript, so `^[^1]` is a footnote whose body is
text `^1`. `\^[` never opens.

A body that is empty or consists only of spaces and tabs is invalid; the
opener is text. Every recognized inline footnote creates one `Footnote` whose
content is the parsed inline body and one one-item `Cite` with referent
`footnote(id)` and empty affixes.

Ids are assigned once, during document finalization, after every authored id
is known. Let `A` be the ids of every `Footnote` produced from a winning or
unreferenced definition. Inline footnotes are numbered in ascending order of
the start position of their `^[`, an outer footnote before one nested in its
body; the `N`-th receives `inline-N` when that string is not in `A` and not
already assigned, otherwise `inline-N-K` for the smallest `K` of at least 1
in neither set. The value carries no authored meaning; consumers compare and
copy ids and never display them.

## Option behavior and fallback

With `footnotes=false`, `[^label]`, `[^label]:`, and `^[content]` are all
ordinary text under the inherited grammar, and `Document.footnotes` is empty.
With `footnotes=true` and `inlineFootnotes=false`, `^[` follows inherited
bracket handling. Failed recognition consumes nothing. Inline code, HTML
tokens, comments, formulas, and cross links are opaque to both forms.

## Scopes

For a referenced call, `Cite.scope` covers `[^label]` and `Citation.scope`
covers `^label`; `Footnote.scope` covers the complete winning or unreferenced
definition through its last continuation line. For an inline footnote,
`Cite.scope` and `Footnote.scope` cover `^[content]` and `Citation.scope`
covers `content`; the body's descendants cover only their own bytes.

## Required conformance cases

Tests cover numeric and named labels; definitions before and after calls;
multiple calls; multiline and unreferenced definitions; duplicate definitions
including nested ones; the label length and container depth limits; escaped
and referenced carets; unresolved calls with and without a matching link
definition; a defined call followed by a tail or a container; the exact
one-item `Cite` shape and empty affixes; plain, formatted, empty, and
whitespace-only inline bodies; `^[a](b)`, `^[^1]`, and a footnote inside a
link label; nested inline footnotes and their id order; id collisions with
authored ids; mixed referenced and inline source order in `Document.footnotes`;
calls inside footnote content; code, comments, HTML, and formulas; exact
scopes; every option combination; allocation failure; and size-doubling runs
of `^`, `[`, and `]`.
