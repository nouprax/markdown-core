# Anchors

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the universal `Markup.anchor` field, the document anchor registry, and
two options: `autoAnchors` and `implicitHeadingReferences` (each default
`false`). Sources: Pandoc's attribute identifiers, `auto_identifiers` with
`gfm_auto_identifiers`, and `implicit_header_references`; Obsidian's block
identifiers populate the same field under the
[block identifiers](block-identifiers.md) module. Executable oracle: the
Pandoc 3.11 CLI under `specs/oracles/pandoc/`. Landing: the field with `M7`,
`autoAnchors` with `P3`, `implicitHeadingReferences` with `P4`.

## Model

```text
Markup(anchor: String?, ...)
```

Every `Markup` kind has exactly one nullable `anchor` field. `null` means the
node declares no link target; a non-null anchor is a non-empty string. An
anchor names the target node for consumers. It is not the node's identity, a
reference edge, or a resolver result, and it does not record which source rule
produced it.

Source punctuation is excluded from the value, and different rules produce
one consumer fact:

| Source rule                                 | Value          |
| ------------------------------------------- | -------------- |
| attribute `{#foo}` or `{id=foo}`            | `anchor="foo"` |
| automatic heading anchor for `# Foo`        | `anchor="foo"` |
| block identifier `^foo`                     | `anchor="foo"` |

There is no heading, block, or fragment discriminator. The reference side of a
link never populates its own `anchor`: `Link`, `CrossLink`, `Cite`, and a
footnote call declare nothing by referring to something.
`Destination.url("#foo")` and `Destination.cross(path="", anchor="foo")` are two
reference spellings that a consumer may match against a node whose `anchor ==
"foo"`; matching is downstream and never mutates the AST.

## Population and precedence

Only these rules populate `anchor`:

- the attribute grammar's identifier at every attachment site of the
  [attributes](attributes.md) module, under that site's option;
- automatic heading anchors under `autoAnchors`; and
- block identifiers under `blockIdentifiers`.

With no anchor-producing option on, `anchor` is `null` on every node. One node
has at most one final anchor:

- within one attribute container, the last identifier wins and an empty final
  `id=` clears the candidate;
- a non-empty explicit heading anchor wins over automatic synthesis;
- a resolved reference occurrence's own non-null anchor wins over the anchor
  inherited from its definition; and
- a block identifier attaches only to a node whose anchor is still `null`
  when block finalization reaches it; otherwise its bytes are ordinary
  content.

No operation concatenates anchors or stores a second value. Two explicit
declarations may author the same anchor; both keep their values and the parser
emits no diagnostic.

## The document registry and synthesis

Synthesis runs once, after block and inline parsing of the whole document has
completed, over every node reachable from `Document.content` and
`Document.footnotes`:

1. Reserve the final anchor of every emitted node that an enabled option
   populated explicitly. An anchor stored on a reference definition is reserved
   by the occurrences that inherit it, not by the definition; an unreferenced
   definition reserves nothing.
2. Visit headings in ascending order of `Heading.scope.start`. For each heading
   whose `anchor` is `null`, compute the base below, then set `anchor` to the
   base if it is not registered, otherwise to `base-N` for the smallest `N` of
   at least 1 such that `base-N` is not registered. Register the result.

A generated anchor has no scope and adds no source position; `Heading.scope`
is the authored heading range.

### Automatic anchor algorithm

With `autoAnchors=true`, the base of a heading is derived from its parsed
inline content:

1. Project the content to plain text: `Text` and `Code` contribute `literal`;
   `Emphasis`, `Strong`, `Strikethrough`, `Span`, `Superscript`, `Subscript`,
   `Mark`, `Insert`, `Link`, `Image`, and `DirectiveLabel` contribute their
   concatenated child text; `Directive` contributes its label text; `SoftBreak`
   and `LineBreak` contribute one space; `Formula` contributes `literal`;
   `CrossLink` contributes `label` when non-null and otherwise its authored
   path and anchor text; a bibliography `Cite` contributes, per item, prefix
   text, `@` and the key, and suffix text in order; `ExampleReference`
   contributes `@` and its label; `HTML`, `Comment`, and a footnote `Cite`
   contribute nothing.
2. Apply the simple lowercase mapping.
3. Replace each Unicode whitespace scalar with one `-`, without collapsing
   adjacent replacements.
4. Remove every scalar that is not a letter, a number, a combining mark,
   connector punctuation, `-`, or `_`.
5. If the result is empty, use `section`.

Punctuation removal inserts nothing, so `A.B` becomes `ab`. There is no
emoji step: `:tada:` keeps `tada` and loses its colons. `## My Header`
receives `my-header`; a second `## My Header` receives `my-header-1`.

## Implicit heading references

With `implicitHeadingReferences=true`, every heading with a non-null final
anchor contributes a virtual reference definition. Its label source is the
authored heading text after removing the ATX or Setext heading syntax, the
optional ATX closing sequence, and a trailing attribute container, normalized
by the inherited reference-label normalization; inline markup remains part of
the label, so `# *Foo*` is referenced by `[*Foo*]`, not `[Foo]`. The virtual
definition targets `#` followed by the final anchor and has `title=null`,
`anchor=null`, and `Attributes.empty` for `merge`. A heading whose label
cannot be written as a reference label, such as one containing an unescaped
`]`, contributes no definition.

`[First chapter]`, `[First chapter][]`, and `[go there][First chapter]` all
resolve to an ordinary `Link(dest=url("#first-chapter"), content, ...)`
through the resolver of the [links and images](links-and-images.md) module,
in the same order-independent document finalization that resolves example
labels. An explicit reference definition with the same normalized label always
wins over the virtual one. When several headings have the same normalized
label, the virtual definition targets the first in source order. An unresolved
candidate keeps the inherited fallback. Attributes authored at the occurrence
follow the `linkAttributes` rule.

## Option behavior

With `autoAnchors=false`, no heading anchor is synthesized. With
`implicitHeadingReferences=false`, no virtual definition exists; the option
has no effect on headings without a final anchor. The two options are
independent, but a virtual definition can only exist for a heading that
received an anchor from some rule.

Cross links spell a heading target as written: `[[#My Header]]` stores
`anchor="My Header"`, and the automatic anchor of that heading is
`my-header`. The parser normalizes neither side; the
[conflicts](conflicts.md) register records this as an open decision.

## Scopes and lifecycle

Anchor syntax that is lexically part of an occurrence is inside that
occurrence's scope even though its punctuation is absent from visible content
and from the stored value. An anchor inherited from a reference definition
changes only the resolved occurrence's field; the occurrence keeps its own
range. `anchor` is an owned immutable string and has no scope or attributes
of its own. Recognizing an anchor never opens a link, resolves a document,
creates an HTML `id`, or changes rendering.

## Required conformance cases

Tests cover `anchor=null` on every kind; explicit identifiers at every
attachment site; automatic anchors; block identifiers; punctuation removal;
identical values from different rules; last-identifier, clearing,
explicit-over-generated, and occurrence-over-definition precedence; reference
inheritance without range inheritance; explicit and generated duplicates;
every kind of the projection table inside a heading, `Formula`, `HTML`,
`Comment`, `Image`, line breaks, and directive labels included; Unicode case,
marks, connectors, and whitespace; shortcode-shaped text; empty bases;
reservation of every explicit anchor from every enabled option before
synthesis, including an anchor on an unreferenced definition reserving
nothing; headings inside footnotes; all three reference spellings; formatted
labels and their plain-text mismatch; explicit-definition priority; duplicate
labels; occurrence attributes; unresolved candidates; exact scopes; both
options independently on and off; allocation failure; and large duplicate
heading sets.
