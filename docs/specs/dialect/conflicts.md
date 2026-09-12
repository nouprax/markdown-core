# Conflicts and decisions

Status: the decision register of the [Markdown Core dialect](../dialect.md).
A conflict is a place where two of the dialect's sources define the same
bytes, or the same feature, differently. Each open entry names the modules
involved, what each source does, the provisional rule the modules state now,
and the alternatives; the provisional rule stands until a product decision
replaces it, and the decision is recorded here and in the module in the same
change. The settled entries record decisions already made and the reason, and
the exclusions record features of the sources that the dialect deliberately
does not absorb.

## Open for decision

No open entries. The three O/I/P audit findings were resolved by the product
on 2026-09-12 as recorded below.

## Settled

### Rulings on the former open entries

These entries were open until the product ruled on them on 2026-09-06; the
modules state the ruled behavior and the ruling is final.

- **C-1 A single tilde.** A single tilde is subscript syntax and never a
  strikethrough delimiter; strikethrough is `~~` only. cmark-gfm's
  single-tilde strikethrough is a registered delta, removed from the engine
  by `P6`. Product ruling.
- **C-2 Front matter.** The Obsidian form only: one block at the very start
  of the document, closed by `---`, flat scalars and text or number lists,
  atomic text values; `...` invalidates the candidate and a later `---` pair
  is inherited Markdown. Product ruling that the provisional rule is final.
- **C-3 Cross links.** Not a collision. `CrossLink` and `CrossEmbedded` share
  `Destination.cross` for Obsidian's internal links and transclusions. Pandoc's `wikilinks_title_after_pipe` writes the same order, target
  before the pipe and label after it, so it is a special case of the model.
  Pandoc's `wikilinks_title_before_pipe` reads the identical bytes with the
  two roles swapped; nothing is wrong with that convention, but one input has
  one meaning, and Pandoc itself makes the two extensions mutually exclusive,
  so the dialect keeps Obsidian's order and the swapped reading is a
  deliberate exclusion. Product ruling.
- **C-4 Heading addressing.** Both sides are stored as written: `[[#My
  Header]]` stores `anchor="My Header"`, an automatic heading anchor is
  `my-header`, and matching the two is consumer policy. The parser never
  normalizes one to the other, because the AST does not decide for the
  consumer. Product ruling.
- **C-5 Callouts.** Obsidian's grammar with no type list: `variant` is
  stored as written, text after the marker is the title, and GitHub's five
  alerts are callouts whose variant a consumer recognizes. The AST does not
  decide for the consumer. Product ruling.
- **C-6 Automatic anchors.** The GFM algorithm, as the anchors module
  states. Product ruling.
- **C-7 Colon fences.** One construct. Pandoc's fenced div is the nameless
  container directive: `::: {attrs}` and `::: word` open a `DirectiveBlock`
  with `name=null`, `:::name` a named one, and one closer rule, a colon run
  at least as long as the opener, closes both. The `Div` kind and the
  `fencedDivs` option are gone, because either construct could express the
  other and the dialect keeps one. Product ruling.
- **C-8 Attribute members after a directive.** One grammar, Pandoc's, at
  every attachment site, directives included: a bare name such as
  `{disabled}` is rejected, `-` is the `unnumbered` class, character
  references decode only inside quotes, and an empty `name=` is accepted.
  `micromark-extension-directive`'s member grammar is not a second grammar;
  every runtime difference from remark is a registered delta. Product ruling
  on 2026-09-06 that the earlier decision is final.
- **C-9 Marks.** `==` is a delimiter run under the flanking rules of `*`.
  Product ruling.
- **C-10 Switches.** The dialect has no parse options: every feature is
  always recognized, the CommonMark base is the meaning of source that no
  feature claims, and smart punctuation is consumer policy rather than a
  parser feature. Nothing disables a feature, not even a comparison: the
  oracle gates parse the one language and register where the dialect leaves
  an oracle's. Product ruling on 2026-09-06.

### O/I/P audit corrections, 2026-09-12

The product requested correction of all three semantic drifts identified by the
[provenance audit](../../plans/2026-09-12-oip-oracle-drift-audit.md):

- Restore empty `^^` as Superscript under the shared pair matcher. Each authored
  pair retains its own node and scope; double tildes still belong to strikethrough.
- An author tail's first keyed section is an additional normal citation item,
  with its own prefix and suffix. A key-free first section is the external
  author's suffix. Ordinary bracket-group nesting remains the same.
- A failed citation group releases its contents to ordinary inline parsing,
  retaining valid inner citations and their independently valid tails.

### Earlier decisions


These decisions are made; each names the ground rule or the defining source
that settled it.

- No profiles, no umbrella switch, and, since C-10, no per-feature switch
  either, in the product or in its test tree: the oracle gates compare the
  one language and register where it leaves an oracle's. Product ruling.
- `mailto:` and `xmpp:` autolinks against the text directive: `mailto:x@y.z`
  reads as a bare autolink and as the text directive `:x`, and the
  recognition order gives it to the autolink (A3 before A10), as cmark-gfm
  does; remark-directive, which gives it to the directive, is not the
  authority. A colon that an address follows belongs to the autolink
  scanner, whichever scheme or word precedes it. Product ruling on
  2026-09-06.
- Features are named after the dialect's own kinds and constructs: cross
  links, marks, comments, heading attributes, and implicit heading
  references, not the source's names.
- `startnum` has no counterpart. Pandoc's switch exists because Pandoc's
  default ignores start numbers; CommonMark already honors them, so
  `List.start` is always the first marker's value for every style.
- Block identifiers use `#anchor-id#` for declarations, with both `#`
  delimiters excluded from `Markup.anchor`; the former `^block-id` declaration
  spelling follows the ordinary inline grammar. Cross-link references retain
  their `#^id` spelling. The own-line block identifier `text` then `#id#` on
  the next line is a paragraph suffix, preserving Obsidian's end-of-block
  attachment model. Product syntax ruling on 2026-09-08.
- Pipe-table rows shorter than the delimiter row are completed with empty
  cells and longer rows are truncated, with cmark-gfm's scopes. GFM source
  and the implemented engine.
- Callout `variant` is stored as written. Store-as-written ground rule, as
  for metadata names.
- Valid UTF-8 is a caller precondition of the C entry point. Product ruling
  in #191.
- `Citation`, `Footnote`, and `Specimen` are scoped values with callbacks;
  `TableCaption`, `Definition`, and `DefinitionList` are `Markup` kinds;
  `Metadata` carries an envelope scope and direct fields without callbacks;
  `TableColumn`, `Destination`, `CitationReferent`, and `Attributes` are
  unscoped values. Scope ground rule.
- A bare `@label` with no bracketed tail naming a registered specimen label is
  a `Cite` with a `specimen` referent, while `[@label]` and a tailed key use bibliography referents, and
  `(@label)` is always subject to specimen resolution. Pandoc's reader, the
  defining source of both features.
- Grid-table spans, multiline tables, specimen resets, definition-list
  lazy continuation, and author-in-text citation tails stay in the dialect as
  their modules specify. Pandoc defines each and nothing else depends on it.
- Inline `$` heuristics: an opening `$` needs a non-whitespace scalar after
  it, a closing `$` needs one before it and no digit after it. GitHub, Pandoc,
  and the implemented engine agree; `micromark-extension-math`'s leniency is a
  registered delta of the remark oracle.
- Referenced footnotes follow cmark-gfm: labels without whitespace, first
  definition wins among duplicates, unreferenced definitions kept. Pandoc's
  footnote grammar is not a source of the referenced form; only its inline
  form is adopted, and it agrees with Obsidian's.
- A task prefix needs a separator after `]`; `- [x]` at the end of a line is
  not a task. cmark-gfm's scanner, the defining source.
- Paired inline HTML tags create no region in which Markdown is suppressed.
  CommonMark, the base; Obsidian's suppression is not adopted.
- Fenced-code info and attributes are stored as written; no language is
  lowercased, aliased, or derived from a class. Store-as-written ground rule.
- Comments are never stripped and an HTML comment is a `Comment` node.
  Product ruling.
- There is no emoji support. Product ruling.
- The `\\(` and `\\[` formula forms are Pandoc's `tex_math_double_backslash`
  spelling; the single-backslash spelling stays a CommonMark escape. The
  implemented engine and its fixtures.
- The shared bracket procedure tests direct and resolving reference tails,
  spans and cite groups before a defined footnote call. Thus `[^a](u)` is a
  link and `[^a]{.x}` is a span, both containing the text `^a`. M4 fixed this
  order in the index and links-and-images module; P5 supplies the span branch.
  cmark-gfm owns the inherited reference-before-footnote order.
- Bare URL and `www.` autolinks are inline scanner steps whose run is opaque
  to every later construct, and only the email form is a post-pass over
  `Text`; a cross link, code span, formula, or mark that begins inside a URL
  run is URL text. cmark-gfm's autolink extension, the defining source.
- Empty carets are accepted; double tildes remain reserved for strikethrough.
  The former attribution of empty-body rejection to Pandoc was incorrect and
  the resulting rule was reversed by the 2026-09-12 audit correction.

## Deliberate exclusions

The following features of the sources are not part of the dialect. Their
bytes follow the inherited grammar, and a gate that observes them in an
oracle registers the difference.

- Pandoc: `yaml_metadata_block` (see C-2), `implicit_figures`, `line_blocks`,
  `raw_attribute`, `raw_tex` and `latex_macros`, `tex_math_single_backslash`,
  `autolink_bare_uris` (the GFM autolink rule is used instead), `smart` (the
  cmark rule is used instead), `hard_line_breaks`, `east_asian_line_breaks`,
  `abbreviations`, `four_space_rule`, `compact_definition_lists`, `startnum`,
  `blank_before_header`, `blank_before_blockquote`, `markdown_in_html_blocks`,
  `native_divs` and `native_spans`, `wikilinks_title_before_pipe` (the same
  `[[a|b]]` bytes read with label before target; the dialect reads them with
  Obsidian's order, and one input has one meaning), pipe-table
  relative widths from delimiter-row dashes, and the `p.` exception for
  capital-period list markers.
- Obsidian: `#tag` tags, the suppression of Markdown between paired inline
  HTML tags, editor-only `[[##` and `[[^^` queries, vault-wide property
  types and alias resolution, path and link resolution, Mermaid and `query`
  execution, and comment stripping.
- GitHub: emoji shortcodes, alerts as a syntax distinct from callouts (see
  C-5), the renderer's tag filter, and a task prefix without a separator.
- remark: TOML and other non-YAML front matter, the directive attribute
  member grammar (see C-8), remark-math's lenient `$` rule, and mdast's
  representation choices, which the remark oracle projects.
