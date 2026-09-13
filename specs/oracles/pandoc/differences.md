# Pandoc differences explained by case (originally 34, now 33)

The original 34 entries represent **34 input cases** in the difference registry,
not 34 unimplemented features or 34 additional Pandoc extensions to support. One
rule can produce several cases. Below, Core means Markdown Core and Pandoc means
the pinned 3.11 oracle, using each case's corpus-defined `markdown_strict` reader
with selected extensions.

Acceptance is bounded by the requested features and existing specifications.
Pandoc provides behavioral evidence without expanding feature scope. The parser
preserves syntactic and semantic facts; numbering, coordinate expansion, and
layout remain consumer responsibilities. Every difference still needs an
explanation: registration must not conceal missing functionality.

See [corpus.json](corpus.json) for original inputs and reader configurations,
and [deltas.json](deltas.json) for exact result digests and explanations.
The original audit numbering is retained below. Item 21 has been fixed and
removed from the registry. The historical `projection` label does not mean that
every entry is merely an AST representation difference.

The empty superscript, author-tail citation grouping, and malformed-group
fallback findings from the 2026-09-12
[O/I/P source audit](../../../docs/plans/2026-09-12-oip-oracle-drift-audit.md)
were fixed as requested. Items 1, 10, and 20 retain only their remaining
independent differences; item 21 now agrees without a waiver. Item 7 still
requires distinguishing reader configuration from failed-candidate fallback.

| Number | Case ID / topic | Specific Core and Pandoc difference |
| --- | --- | --- |
| 1 | `empty-superscript-and-subscript`: empty scripts | Both sides now produce an empty Superscript for `^^`. The only remaining difference is `~~`: Core reserves double tildes for strikethrough and retains an unmatched pair as text; this Pandoc reader produces an empty Subscript. |
| 2 | `pandoc-reference-attribute-merge`: reference attribute merging | Core preserves classes and attribute records in definition-then-occurrence order, including duplicates. Pandoc deduplicates, overrides, and reorders them. For example, Core retains both declarations in `k=def ... k=occ`, while Pandoc retains only the occurrence's `k=occ`. |
| 3 | `multiline-table`: multiline table column widths | Core stores each source column width as a proportion of the total column width. Pandoc includes separators in widths and divides by the default page width. The difference is numerical column width; table content is implemented and agrees. |
| 4 | `grid-table-block-cells`: widths of grid tables containing blocks | The same width semantics as item 3. For source interior widths 15, 15, and 20, Core stores `0.3, 0.3, 0.4`, while Pandoc stores `16/72, 16/72, 21/72`. Paragraphs, lists, and other block content within cells agree. |
| 5 | `grid-table-row-and-column-spans`: widths of tables with spans | The same width difference remains. Row/column-span geometry and cell content agree; rowspan/colspan support is not missing. |
| 6 | `example-lists-and-reference`: specimen definitions and references | Core stores Specimen definitions in source order on the document and preserves exact reference IDs. Pandoc emits numbered example Lists and replaces references with number text. Core delegates numbering to consumers. |
| 7 | `p6-escaped-space`: escaped spaces in scripts | For `^a\ b^` and `~*c\ d*~`, Core produces scripts and decodes their escaped spaces to NBSP; this Pandoc reader does not produce scripts. Core preserves the original backslash and whitespace outside scripts or when a candidate fails, without global replacement. |
| 8 | `p6-whitespace-recovery`: Unicode whitespace in scripts | Core treats all raw Unicode White_Space as script-pairing boundaries. Pandoc accepts `^a b^` containing U+2003 EM SPACE; Core retains it as text. Both sides agree on recovery after the earlier ASCII-space failure in this case. |
| 9 | `p6-entity-space`: entity-decoded whitespace | In `~a&Tab;b~`, Core preserves the decoded TAB, while Pandoc normalizes it to a space. Entity-decoded whitespace is distinct from a raw source whitespace boundary. |
| 10 | `p6-pairing-and-tilde-runs`: empty pairs and tilde runs | `^^` now agrees. Core preserves `^^^^` as two empty Superscript nodes with independent scopes; Pandoc merges adjacent nodes of the same kind. Core retains runs of three or more tildes and unmatched double tildes as text. Pandoc can produce empty subscripts and parse `~~~c~~~` as subscript `c`. |
| 11 | `p6-opaque-tokens`: code spans within scripts | For an outer script candidate containing code `x y`, Core recognizes the complete code span first, so its internal space does not interrupt the outer script. Pandoc does not recognize that outer script. Both preserve `^` and `~` inside standalone code as code text. |
| 12 | `anchor-global-reservation`: later explicit heading IDs | Core reserves all explicit IDs before generating automatic IDs: an earlier `# x` gets `x-1` if a later `{#x}` exists. Pandoc generates `x` for the earlier heading and also uses `x` for the later explicit ID. |
| 13 | `anchor-permitted-scalars`: emoji in anchors | Core filters by character rules without converting emoji to names. Pandoc's GFM anchor algorithm converts 😀 to `grinning`. The anchors in this case are `a‿b--` and `a‿b--grinning`, respectively. |
| 14 | `anchor-simple-lowercase`: Unicode lowercase conversion | Core uses Unicode 17 simple lowercase, converting `İ` to a single `i`. Pandoc's full lowercase produces `i` followed by U+0307 COMBINING DOT ABOVE, yielding a different anchor character sequence. |
| 15 | `anchor-whitespace-scalars`: Unicode whitespace in anchors | Core converts Unicode White_Space such as U+0085 and U+2028 to hyphens. Pandoc deletes those two characters in this case, yielding `a-b-c` and `abc`, respectively. |
| 16 | `anchor-inline-code-reservation`: reserving explicit code IDs | The same global reservation rule as item 12, with explicit `{#x}` attached to inline code. Core gives the earlier heading `x-1`; Pandoc keeps `x`. |
| 17 | `implicit-reference-adjacency`: adjacency of reference brackets | Core retains CommonMark's adjacency requirement and parses `[Foo] [Foo][] [go][Foo]` as three separate heading links. Pandoc crosses the space to combine the first two bracket groups, leaving different text and link structure. |
| 18 | `p5-heading-span-reservation`: reserving explicit Span IDs | The same rule as item 12, with the explicit ID from `[owner]{#taken}`. Core gives the earlier `# Taken` the anchor `taken-1`; Pandoc uses `taken`. |
| 19 | `p7-tail-and-nesting`: edge whitespace in citation affixes | Core trims source whitespace at both ends of citation prefixes/suffixes. Pandoc retains the suffix's leading space before nested brackets. Nested content is still compared individually. |
| 20 | `p7-tail-later-author`: another citation in an author-style tail | For `@a [@b [p. 7]]`, both sides now retain `@b` as a normal item of the outer Cite. Only affix whitespace differs: Core's suffix is `[p. 7]`, while Pandoc retains a leading space. |
| 21 | `p7-malformed-group`: malformed citation-group fallback | **Fixed and removed from the registry.** Ordinary inline parsing continues after the outer group fails; valid inner keys become authorInText citations and valid tails survive. The original case now agrees with Pandoc. |
| 22 | `p7-heading-projection`: citations in heading anchors | For `## [pre @a suffix; @b]`, Core generates `preasuffixb` from stored citation content. Pandoc generates `pre-a-suffix-b` from the original display text, which retains separating spaces. |
| 23 | `p7-unicode-boundary`: citation starts after underscores | `_@a` cannot start a Core citation because an underscore, Unicode letter, or digit cannot immediately precede the start. Pandoc accepts a citation after an underscore. |
| 24 | `p9a-nested-start`: nested list candidates starting above 1 | After a parent-list paragraph, indented `2. ordinary` cannot start a nested Core list and remains a lazy continuation of the paragraph. Pandoc puts the line in another Plain block. The following `1. nested` starts a nested list on both sides. |
| 25 | `p9b-resolution`: forward, duplicate, anonymous specimens and start numbers | The same model difference as item 6, covering forward references, explicit start number 7, duplicate labels, and anonymous definitions. Core preserves all definitions, authored start values, and reference IDs. Pandoc renders references as text such as `7` and puts definitions in an example List. |
| 26 | `p9b-heading`: specimen references in headings | Core keeps a specimen citation as the content of `## @label`; Pandoc replaces the heading content with text `1`. **Both anchors are `label`.** The difference is heading content and the definition model, not the anchor value. |
| 27 | `p7-unresolved-reference-tail`: unresolved link-reference tails | For `[@a][missing]`, Core accepts the preceding complete citation group. Pandoc retains the first pair of brackets as text and parses its `@a` as authorInText, producing different citation modes and structure. |
| 28 | `p7-explicit-citation-shortcut`: citation/reference-definition precedence | Core still registers `[@a]: /u` as a link-reference definition, but a complete `[@a]` citation in content takes precedence over a shortcut link. Pandoc also parses the definition-shaped line as a citation followed by text. |
| 29 | `p9a-start-always-authored`: control reader without startnum | Core always preserves start=8 for `h. eight`. This control deliberately omits Pandoc's `startnum`, so Pandoc stores 1. The normal selected-feature mapping enables `fancy_lists` and `startnum` together; start-number support is not missing. |
| 30 | `p8-closer-width`: container closing-fence width | Core requires the closing colon run to be at least as long as the opening fence. Pandoc accepts any run of at least three colons. A container opened with four colons therefore ends at a different position when it encounters three colons. |
| 31 | `p8-explicit-anchor-reservation`: reserving explicit container IDs | The same rule as item 12, with the later explicit ID on a fenced div. Core gives the earlier `# Reserved` the anchor `reserved-1`; Pandoc keeps `reserved`. |
| 32 | `p10-nested-and-lazy`: compactness of nested definition bodies | Core stores a separate compact boolean determined by source blank lines between term and body. Pandoc represents compactness through Plain/Para. When the first body is a nested DefinitionList, no corresponding flag is available and the comparison value is null; term and body content agree. |
| 33 | `p10-padding-and-tabs`: compactness of code definition bodies | The same AST-information difference as item 32, with a CodeBlock as the first body. This is not a TAB or code-content parsing error; Pandoc does not retain the corresponding compact flag. |
| 34 | `p10-empty-bodies`: compactness of empty definition bodies | The same AST-information difference as item 32, with an empty first body. The empty body and subsequent bodies are preserved and compared. Only Core's explicitly retained compactness information differs. |

The clearest repeated relationships are:

- 3–5: one column-width rule across three table cases.
- 6, 25, 26: one specimen definition/reference model across ordinary content,
  forward references, and headings.
- 12, 16, 18, 31: one global explicit-ID reservation rule across four declaration
  sites.
- 32–34: one compactness-information difference across nested blocks, code
  blocks, and empty bodies.

These records are governed by the existing modules:
[scripts](../../../docs/specs/dialect/superscript-and-subscript.md),
[attributes](../../../docs/specs/dialect/attributes.md),
[tables](../../../docs/specs/dialect/tables.md),
[specimens](../../../docs/specs/dialect/specimens.md),
[anchors](../../../docs/specs/dialect/anchors.md),
[links](../../../docs/specs/dialect/links-and-images.md),
[citations](../../../docs/specs/dialect/citations.md),
[lists](../../../docs/specs/dialect/lists.md),
[containers](../../../docs/specs/dialect/directives.md), and
[definition lists](../../../docs/specs/dialect/definition-lists.md).
Implementation gaps must be assessed against the corresponding module's
features, boundaries, and composition tests, not inferred from the number of
difference entries.
