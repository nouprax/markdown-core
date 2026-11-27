# Pandoc differences explained by case (originally 34, now 34)

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
The original audit numbering is retained below. Items 8 and 21 have been fixed
and removed from the registry. The historical `projection` label does not mean that
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
| 3 | `multiline-table`: multiline table column widths | **Removed from the registry.** Multiline tables are no longer part of the dialect, so the case left the corpus. |
| 4 | `grid-table-block-cells`: widths of grid tables containing blocks | **Removed from the registry.** Grid tables and captions are no longer part of the dialect. |
| 5 | `grid-table-row-and-column-spans`: widths of tables with spans | **Removed from the registry.** Grid tables are no longer part of the dialect. |
| 6 | `example-lists-and-reference`: numbered examples and references | Core has no numbered examples: `(@label)` is ordinary paragraph text and a bare `@label` is a bibliography citation. Pandoc emits numbered example Lists and replaces references with number text. |
| 7 | `p6-escaped-space`: escaped spaces in scripts | For `^a\ b^` and `~*c\ d*~`, Core produces scripts and decodes their escaped spaces to NBSP; this Pandoc reader does not produce scripts. Core preserves the original backslash and whitespace outside scripts or when a candidate fails, without global replacement. |
| 8 | `p6-whitespace-recovery`: Unicode whitespace in scripts | **Fixed and removed from the registry.** Only a raw space, tab or line ending ends a script candidate, as in Pandoc's reader, so `^a b^` containing U+2003 EM SPACE is a superscript on both sides. Both sides also agree on recovery after the earlier ASCII-space failure in this case. |
| 9 | `p6-entity-space`: entity-decoded whitespace | In `~a&Tab;b~`, Core preserves the decoded TAB, while Pandoc normalizes it to a space. Entity-decoded whitespace is distinct from a raw source whitespace boundary. |
| 10 | `p6-pairing-and-tilde-runs`: empty pairs and tilde runs | `^^` now agrees. Core preserves `^^^^` as two empty Superscript nodes with independent scopes; Pandoc merges adjacent nodes of the same kind. Core retains runs of three or more tildes and unmatched double tildes as text. Pandoc can produce empty subscripts and parse `~~~c~~~` as subscript `c`. |
| 11 | `p6-opaque-tokens`: code spans within scripts | For an outer script candidate containing code `x y`, Core recognizes the complete code span first, so its internal space does not interrupt the outer script. Pandoc does not recognize that outer script. Both preserve `^` and `~` inside standalone code as code text. |
| 12 | `anchor-global-reservation`: later explicit heading IDs | The same rule as item 59: the later `{#x}` is Core's only anchor, and the earlier `# x` has none. Pandoc generates `x` for the earlier heading and also uses `x` for the later explicit ID. |
| 13 | `anchor-permitted-scalars`: emoji in anchors | The same rule as item 59: Core's heading has no anchor. Pandoc's GFM anchor algorithm generates `a‿b--grinning`, converting 😀 to `grinning`. |
| 14 | `anchor-simple-lowercase`: Unicode lowercase conversion | The same rule as item 59: Core's heading has no anchor. Pandoc generates one with full lowercase, `İ` becoming `i` followed by U+0307 COMBINING DOT ABOVE. |
| 15 | `anchor-whitespace-scalars`: Unicode whitespace in anchors | The same rule as item 59: Core's heading has no anchor. Pandoc deletes U+0085 and U+2028 as it generates one. |
| 16 | `anchor-inline-code-reservation`: reserving explicit code IDs | The same rule as item 59, with explicit `{#x}` attached to inline code: the code keeps `x` on both sides, and Core's earlier heading has no anchor. |
| 17 | `implicit-reference-adjacency`: adjacency of reference brackets | The same dropped syntax as item 44: `[Foo] [Foo][] [go][Foo]` names no definition, so Core keeps all three as bracket text. Pandoc resolves them to the heading and crosses the space to combine the first two bracket groups. |
| 18 | `p5-heading-span-reservation`: reserving explicit Span IDs | The same rule as item 59, with the explicit ID from `[owner]{#taken}`: Core's `# Taken` has no anchor; Pandoc gives it `taken`. |
| 19 | `p7-tail-and-nesting`: edge whitespace in citation affixes | Core trims source whitespace at both ends of citation prefixes/suffixes. Pandoc retains the suffix's leading space before nested brackets. Nested content is still compared individually. |
| 20 | `p7-tail-later-author`: another citation in an author-style tail | For `@a [@b [p. 7]]`, both sides now retain `@b` as a normal item of the outer Cite. Only affix whitespace differs: Core's suffix is `[p. 7]`, while Pandoc retains a leading space. |
| 21 | `p7-malformed-group`: malformed citation-group fallback | **Fixed and removed from the registry.** Ordinary inline parsing continues after the outer group fails; valid inner keys become authorInText citations and valid tails survive. The original case now agrees with Pandoc. |
| 22 | `p7-heading-projection`: citations in heading anchors | The same rule as item 59: both sides read the same citations, and Core's heading has no anchor where Pandoc generates `pre-a-suffix-b`. |
| 23 | `p7-unicode-boundary`: citation starts after underscores | `_@a` cannot start a Core citation because an underscore, Unicode letter, or digit cannot immediately precede the start. Pandoc accepts a citation after an underscore. |
| 24 | `p9a-nested-start`: nested list candidates starting above 1 | Under CommonMark only a list starting at 1 may interrupt a paragraph, so indented `2. ordinary` remains a lazy continuation of the parent-list paragraph in Core. Pandoc puts the line in another Plain block. The following `1. nested` starts a nested list on both sides. |
| 25 | `p9b-resolution`: forward, duplicate, anonymous examples and start numbers | The same dropped syntax as item 6, covering forward references, explicit start number 7, duplicate labels, and anonymous definitions. Core reads them as text and bibliography citations. Pandoc renders references as text such as `7` and puts definitions in an example List. |
| 26 | `p9b-heading`: example references in headings | Core keeps `## @label` as a heading holding a bibliography citation; Pandoc replaces the heading content with text `1`. |
| 27 | `p7-unresolved-reference-tail`: unresolved link-reference tails | For `[@a][missing]`, Core accepts the preceding complete citation group. Pandoc retains the first pair of brackets as text and parses its `@a` as authorInText, producing different citation modes and structure. |
| 28 | `p7-explicit-citation-shortcut`: citation/reference-definition precedence | Core still registers `[@a]: /u` as a link-reference definition, but a complete `[@a]` citation in content takes precedence over a shortcut link. Pandoc also parses the definition-shaped line as a citation followed by text. |
| 29 | `p9a-start-always-authored`: control reader without startnum | The dialect dropped letter markers, so `h. eight` and `i. nine` are one Core paragraph. This control omits Pandoc's `startnum`, and Pandoc makes a lower-alpha list starting at 1. |
| 30 | `p8-closer-width`: container closing-fence width | The same dropped syntax as item 55: `:::: {.outer}` is paragraph text in Core. Pandoc opens a Div there and also accepts any run of at least three colons as its closer. |
| 31 | `p8-explicit-anchor-reservation`: reserving explicit container IDs | The same dropped syntax as item 55: `::: {#reserved}` is text in Core, so its ID reserves nothing and `# Reserved` keeps `reserved`. Pandoc makes a Div with that ID. |
| 32 | `p10-nested-and-lazy`: compactness of nested definition bodies | Core stores a separate compact boolean determined by source blank lines between term and body. Pandoc represents compactness through Plain/Para. When the first body is a nested DefinitionList, no corresponding flag is available and the comparison value is null; term and body content agree. |
| 33 | `p10-padding-and-tabs`: compactness of code definition bodies | The same AST-information difference as item 32, with a CodeBlock as the first body. This is not a TAB or code-content parsing error; Pandoc does not retain the corresponding compact flag. |
| 34 | `p10-empty-bodies`: compactness of empty definition bodies | The same AST-information difference as item 32, with an empty first body. The empty body and subsequent bodies are preserved and compared. Only Core's explicitly retained compactness information differs. |
| 35 | `attribute-class-html-whitespace`: separators in a `class=` value | Core splits a `class=` value where HTML splits a `class` attribute: at tab, LF, form feed, CR and space. The case writes `a`, a space, `b`, NBSP, `c`, IDEOGRAPHIC SPACE, `d`, `&#11;` and `e`. Core has two classes: `a`, and one holding everything after the space. Pandoc splits at every Unicode space, the referenced vertical tab included, and has the five classes `a` to `e`. |
| 36 | `p7-braced-key-whitespace`: whitespace in a braced citation key | A braced key excludes whitespace, which is a space, a tab or a line ending, so Core keys `@{a b}`, written with NBSP, as `a b`, and `[@{c　d}, p. 1]`, written with IDEOGRAPHIC SPACE, as `c　d`. Pandoc excludes every Unicode space and keeps both as text. Both reject the ASCII space in `@{a b}`. |
| 37 | `definition-list-compact`: the `~` definition marker | The dialect keeps only `:` as a definition marker. Core reads each `~ Definition` line as text of the term's paragraph; Pandoc's `definition_lists` reader also accepts `~` and makes a DefinitionList. |
| 38 | `p10-marker-only`: a lone `~` before an indented body | The same rule as item 37: the lone `~` and the indented `body` are lines of one paragraph in Core, and a definition with a code-indented body in Pandoc. |
| 39 | `pandoc-special-unnumbered`: the `{-}` shorthand | The dialect dropped Pandoc's `-` member: Core reads `{-}` as the bare record `-="true"`; Pandoc reads it as the class `unnumbered`. |
| 40 | `fancy-list-and-startnum`: Pandoc's Roman sublist | The dialect keeps only decimal markers with `.` or `)`. The `9)` list agrees; the Roman `i.`, `ii.`, `iii.` lines are a paragraph in Core and an ordered list in Pandoc. |
| 41 | `p9a-variants`: letter and Roman markers | The same rule as item 40: `a)`, `(A)`, `iv.` and `IV)` lists are paragraphs in Core. |
| 42 | `p9a-default-and-disambiguation`: letters and `#` markers | The same rule as item 40, for letters, `#.` and `(#)`. |
| 43 | `p9a-capital-period`: capital letter and `.` | The same rule as item 40; Pandoc's two-space rule for `A.` has no counterpart in Core. |
| 44 | `implicit-header-references`: Pandoc's implicit heading references | Core has no implicit heading references: a heading declares no reference label, so `[First Chapter]` and `[go there][First Chapter]` stay bracket text unless a link reference definition declares the label. Pandoc's `implicit_header_references` links both to the heading. |
| 45 | `implicit-authored-label`: authored markup in a heading label | The same rule as item 44, for `[*Foo*]` after `# *Foo*`: Core keeps the brackets as text around the emphasis. |
| 46 | `implicit-forward-and-duplicates`: forward and duplicate heading labels | The same rule as item 44, for uses before the heading and a repeated heading. |
| 47 | `implicit-occurrence-attributes`: attributes on a heading reference | The same rule as item 44: `[Foo][]{#own .jump}` is the text `[Foo]` and an empty Span holding the attributes in Core, a heading link with its own attributes in Pandoc. |
| 48 | `implicit-reference-in-paragraph-definition-text`: a heading reference in definition-shaped text | The same rule as item 44, with `[foo]: <>` inside a paragraph, where it is no definition. |
| 49 | `p5-implicit-heading-shortcut`: Span and heading shortcut precedence | The same rule as item 44: `[Ref]{.span}` is a Span on both sides, while `[Ref][]{.link}` and `[go][Ref]{.link}` are bracket text and Spans in Core and heading links in Pandoc. |
| 50 | `p6-heading-reference`: a script-bearing heading reference | The same rule as item 44, for `[2^10^]` before `# 2^10^`. |
| 51 | `p4-image-shortcut-heading-reference`: an image shortcut to a heading | The same rule as item 44: `![H]` is text in Core and an image whose source is `#h` in Pandoc. |
| 52 | `p4-image-collapsed-heading-reference`: a collapsed image reference to a heading | The same rule as item 44, for `![H][]`. |
| 53 | `p4-image-full-heading-reference`: a full image reference to a heading | The same rule as item 44, for `![alt][H]`. |
| 54 | `p4-isolated-authored-heading-label`: an isolated authored heading label | The same rule as item 44, for `[*Foo*]` alone after `# *Foo*`. |
| 55 | `fenced-divs-nested`: nested nameless divs | Core has no nameless fenced divs: every container is named, `:::name`, so `::::: {#special .sidebar}` and `::: Warning` are paragraph text. Pandoc's `fenced_divs` reads them as nested Divs. |
| 56 | `p8-class-and-empty`: class-word and empty-attribute openers | The same rule as item 55, for `::: warning` and `::: {}`. |
| 57 | `p8-opaque-and-nested`: opaque lines inside a div | The same rule as item 55; code fences and HTML still own their lines on both sides. |
| 58 | `p8-p10-closing-boundary`: a definition list inside a div | The same rule as item 55: `::: {.box}` is text and the definition list is not inside a container in Core. |
| 59 | `gfm-auto-anchors`: automatic heading anchors | Core has no automatic anchors: a heading has only the anchor its attributes declare, so `## My Header` twice has two headings with no anchor. Pandoc's `auto_identifiers` generates `my-header` and `my-header-1`. Every other case read with `auto_identifiers` also differs in its headings' anchors. |
| 60 | `anchor-empty-fallback`: a heading with no anchor characters | The same rule as item 59: `# !!!` has no anchor in Core; Pandoc generates `section`. |
| 61 | `p3-connector-and-combining-mark`: connector punctuation and marks | The same rule as item 59: Core's heading has no anchor; Pandoc generates `a‿b-é`. |
| 62 | `p5-p6-heading-content`: spans and scripts in a heading | The same rule as item 59: the heading's Span, scripts and class agree, and only Pandoc generates an anchor. |
| 63 | `implicit-explicit-priority`: an explicit definition over a heading | The same rule as item 59: both sides link `[Foo]` to the explicit definition, and only Pandoc's heading has an anchor. |
| 64 | `p7-heading-shortcut`: a citation shortcut after a citation heading | The same rule as item 59: both sides read `[@a]` as a citation, and only Pandoc's heading has an anchor. |
| 65 | `p4-isolated-plain-heading-label`: a plain label for an authored heading | The same rule as item 59: `[Foo]` stays bracket text on both sides, and only Pandoc's heading has an anchor. |

The clearest repeated relationships are:

- 6, 25, 26: the dropped numbered examples, across ordinary content, forward
  references, and headings.
- 17, 44–54: the dropped implicit heading references, across shortcut, full,
  collapsed and image forms.
- 30, 31, 55–58: the dropped nameless fenced divs.
- 12–16, 18, 22, 59–65: the dropped automatic heading anchors, across Unicode
  scalars, explicit IDs elsewhere and heading content.
- 32–34: one compactness-information difference across nested blocks, code
  blocks, and empty bodies.
- 37, 38: the dropped `~` definition marker.
- 29, 40–43: the dropped fancy-list markers.

These records are governed by the existing modules:
[scripts](../../../docs/specs/dialect/superscript-and-subscript.md),
[attributes](../../../docs/specs/dialect/attributes.md),
[tables](../../../docs/specs/dialect/tables.md),
[anchors](../../../docs/specs/dialect/anchors.md),
[links](../../../docs/specs/dialect/links-and-images.md),
[citations](../../../docs/specs/dialect/citations.md),
[lists](../../../docs/specs/dialect/lists.md),
[containers](../../../docs/specs/dialect/directives.md), and
[definition lists](../../../docs/specs/dialect/definition-lists.md).
Implementation gaps must be assessed against the corresponding module's
features, boundaries, and composition tests, not inferred from the number of
difference entries.
