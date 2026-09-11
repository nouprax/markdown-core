# Pandoc extension oracle

The executable oracle is the official Pandoc 3.11 CLI. Pandoc is both the
producer of these extensions and the producer of its native JSON model, so a
third-party popularity proxy would add uncertainty rather than independence.
The pinned Pandoc User's Guide is the source of the feature definitions, the
dialect modules under `docs/specs/dialect/` are the rule, and the CLI supplies
reproducible evidence for recognition, fallback, grouping, ordering, and
semantic facts.

[`source.json`](source.json) is the immutable authority and runner contract. It
pins the 3.11 tag commit, the exact manual, Markdown reader, shared attribute
merge, and extension-registry blobs, and the official portable archives with
the SHA-256 digests published by GitHub's release service. A later version is
not accepted implicitly; moving any pin is a reviewed language-baseline change.

This is an extension-layer oracle, not a Pandoc-dialect oracle. Every case in
[`corpus.json`](corpus.json) declares `markdown_strict` plus only the extension
set required by that case. Invoking Pandoc's default `markdown` reader is
forbidden because it would enable unrelated rules. The base reader is merely
a controlled harness substrate: current cmark and cmark-gfm policies continue
to own Markdown Core's inherited CommonMark/GFM behavior.

Pandoc 3.11 is also the source and evidence for the one shared braced-attribute
grammar and its semantic `Attr` shape, which `docs/specs/dialect/attributes.md`
states. Markdown Core projects that tuple into its
consumer model: the identifier becomes `Markup.anchor`, classes become
`Attributes.classes`, and ordered key/value pairs become `Attributes.records`
of `Record` values. Pandoc's empty identifier projects to `anchor=null`; neither
array is otherwise flattened or deduplicated. The same grammar applies at
Remark directive attachment sites; Remark owns the directive envelope, not a
second attribute member language.

Pandoc JSON's `Link` and `Image` target URLs are projected into the shared
consumer value as `dest = Destination.url(url)` on their respective nodes. A
fragment-only target such as `#section` remains a URL-branch reference to the
declaration-side `Markup.anchor`; the projection does not copy that value into
the reference node's own anchor field.

The corpus contains inputs and exact reader strings only. Install its pinned
host executable explicitly with `scripts/init-environment.sh --install oracle-pandoc`;
`--check oracle-pandoc` verifies it without downloads. `pnpm check:pandoc-parity`
runs CLI canaries, both parsers and the fail-closed digest registry. Normal
builds and tests never install the executable or access the network.

The active corpus has 86 cases: 51 agreements, 31 documented divergences
and 4 missing-feature gaps assigned to later landing items. Every difference
pins input/reader, oracle projection and product projection SHA-256 digests.
New, changed, stale, duplicate and unknown entries fail. P5's span and shared
attribute grammar cases now agree, including empty containers, nesting,
shortcut precedence, complete link tails and malformed fallback. P6's ordinary
superscript/subscript case agrees. Four isolated agreement cases separately pin
`^a b^` and `~a b~` as text, `^ab^` as Superscript, and `^a&#32;b^`
as Superscript containing a decoded space; no difference waiver applies to
these assertions. Empty bodies, contextual escaped spaces,
Unicode whitespace, decoded TAB preservation, maximal tilde runs and code-token
opacity retain exact deliberate-difference witnesses. Inline-footnote composition
is tested in the product fixtures, where the document-owned footnote model and
source scopes are observable. P3/P4 compositions agree for heading text,
script-bearing heading references and Span precedence over implicit shortcuts;
the later Span ID reservation retains its exact global-reservation difference.

`inline-code-attributes`, `header-attributes`, `fenced-code-attributes` and
`link-and-image-attributes` agree; `pandoc-reference-attribute-merge` remains an
exact deliberate difference, with an executable `combineAttr` canary.

P3/P4 retire `gfm-auto-anchors` and `implicit-header-references`. Added agreements
cover empty fallback, authored markup labels, forward and duplicate labels,
explicit-definition priority, occurrence attributes, and a heading reference
inside declaration-shaped paragraph text (the reduced CI seed-1 witness). Exact deliberate
differences retain global explicit-anchor reservation (including inline code),
Unicode simple lowercase, `White_Space`, permitted scalars without emoji aliases,
and CommonMark reference adjacency. CLI canaries independently pin these Pandoc
behaviors; the projections preserve both sides rather than erase differences.
The `multiline-table` gap retains its missing recognition and now records the
generated anchor on the product's existing heading fallback; `P11c` owns it.

Representation projections are shared by concept, not selected by case:
`Plain` uses paragraph content, Pandoc spaces join adjacent Text values, empty
link titles project to absent, and CodeBlock compares Pandoc's language/class
sequence to the authored language followed by Markdown Core classes. Pandoc
cannot distinguish a language token from an attribute class or preserve a
complete info string, so product fixtures separately verify that distinction.
Pandoc omits one terminal code-block newline; the comparison removes that same
newline from the product. Neither projection changes attributes on other kinds.
Product source ranges remain owned by the canonical and package fixtures.

The comparison must use `--to=json` without citeproc, filters, defaults files,
templates, metadata files, bibliography lookup, or network access. A semantic
projection may translate representation-only Pandoc constructors into the
canonical consumer model; it may not erase a syntax, ordering, content,
attribute, citation, list, heading, or table difference. Product scopes remain
owned by Markdown Core fixtures because Pandoc JSON does not expose compatible
source ranges. Attribute cases compare all three projected components and
their order; the records sequence may not collapse into a unique-key map.

Table comparison retains column alignment and width, row groups, cell spans and
cell content. Pandoc-only cell alignment is outside the canonical intersection.
Ordered-list variants and delimiters map to the canonical value spelling.

P7/P9a/P9b retire the bibliography, fancy-list and example-list feature gaps.
Cite comparison keeps every key, mode and ordered prefix/suffix tree. Pandoc's
second Cite tuple member is fallback rendering for consumers without citation
processing, not another semantic child list; it has no canonical counterpart.
The key and affix trees are compared directly, with unit tests ensuring a
changed key, mode, prefix or suffix cannot pass. Specimen definitions retain
id/start/content and reference IDs on the product side. Pandoc's example lists
and rendered numbers remain exact model differences. Affix trimming, nested
author tails, malformed candidates, underscore boundaries and unresolved
reference tails retain isolated witnesses. Direct/resolving tails and Spans,
complete citation versus virtual heading shortcut, ordinary author tails,
alphabetic/Roman/default markers and capital-period spacing have agreements.

P8/P10 retire both definition-list gaps and `fenced-divs-nested` by agreement.
The Div projection uses `name=null`. DefinitionList projects each term and its
ordered body collections to Definition, including empty bodies. A first body
starting with Plain means compact, with Para means loose; other first-body
shapes expose no flag, so Pandoc projects null and the product boolean remains
compared. Exact differences cover those unobservable flags, the dialect's
minimum closer width, and global explicit-ID reservation. Unit tests detect
changed names, compact flags, terms and body grouping independently.
