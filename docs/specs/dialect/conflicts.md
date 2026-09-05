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

### C-1 A single tilde: strikethrough or subscript

- Modules: [strikethrough](strikethrough.md),
  [superscript and subscript](superscript-and-subscript.md).
- cmark-gfm makes `~x~` a strikethrough, exactly like `~~x~~`. Pandoc makes
  `~x~` a subscript and knows only `~~x~~` as strikeout.
- Provisional rule: the explicitly enabled feature owns the bytes. With
  `subscript` on, a single tilde is a subscript delimiter and never a
  strikethrough delimiter, `~~x~~` stays strikethrough, and an unmatched
  `~~` run becomes two subscript delimiters; with `subscript` off, the
  inherited single-tilde strikethrough stands. The harness-only double-tilde
  flag is removed.
- Alternatives: make single-tilde strikethrough part of the dialect's GFM
  layer only, so that `subscript` never changes an inherited parse, and accept
  that `~x~` then has two meanings the caller must keep apart by option; or
  drop single-tilde strikethrough from the dialect altogether as a registered
  delta against cmark-gfm, so `~x~` is text unless `subscript` is on.

### C-2 Front matter: Obsidian properties or Pandoc metadata blocks

- Module: [properties](properties.md).
- Obsidian recognizes one YAML block at the very start of the file, closed by
  `---`, with a flat domain of scalars and text or number lists and no
  Markdown inside values. Pandoc's `yaml_metadata_block` accepts a block
  anywhere a block can start, several of them merged, `...` as a closer,
  arbitrarily nested values, and parses string values as Markdown. remark's
  `remark-frontmatter` also accepts TOML between `+++` fences.
- Provisional rule: the Obsidian form only. A later `---` pair is inherited
  Markdown, `...` invalidates the candidate, nested values invalidate it, and
  values are atomic text.
- Alternatives, separately decidable: accept `...` as a closer; accept nested
  mappings and sequences as a generic value tree instead of invalidating;
  accept blocks after the first line; parse Markdown inside string values.
  Each widens the consumer model of the properties module.

### C-3 Cross links: pipe order and result kind

- Module: [cross links](cross-links.md).
- Obsidian and Pandoc's `wikilinks_title_after_pipe` write
  `[[target|label]]`; GitHub wikis, Gollum, and Pandoc's
  `wikilinks_title_before_pipe` write `[[label|target]]`. Obsidian adds `#`
  heading and `#^` block anchors and the `![[...]]` embed form; Pandoc
  produces an ordinary link with the class `wikilink` and no anchor
  structure.
- Provisional rule: the Obsidian order and forms, producing `CrossLink` with
  a raw label and a structured `Destination.cross`.
- Alternatives: the label-before-target order; or project a cross link to an
  ordinary `Link` with class `wikilink` and lose the anchor structure.

### C-4 Heading addressing: written text or generated anchor

- Modules: [cross links](cross-links.md), [anchors](anchors.md).
- Obsidian addresses a heading by its text, `[[#My Header]]`. Pandoc and GFM
  address it by a generated identifier, `#my-header`.
- Provisional rule: both values are stored as written. `[[#My Header]]`
  stores `anchor="My Header"`, `autoAnchors` gives the heading
  `anchor="my-header"`, and matching the two is consumer policy.
- Alternative: while `autoAnchors` is on, run the heading part of a cross
  link's anchor through the same algorithm, so the two values agree in the
  AST. This gives one answer inside the parser at the cost of a value that is
  no longer as written.

### C-5 Callouts: Obsidian grammar or GitHub alerts

- Module: [callouts](callouts.md).
- Obsidian accepts any type, matches it case-insensitively, allows a `+` or
  `-` fold marker, and reads the rest of the first line as a title. GitHub
  alerts accept exactly `NOTE`, `TIP`, `IMPORTANT`, `WARNING`, and `CAUTION`
  in uppercase, require nothing else on the marker line, and have no fold or
  title.
- Provisional rule: the Obsidian grammar, with `variant` stored as written,
  so `> [!NOTE] Title` has a title and `> [!Note]` has `variant="Note"`.
  GitHub's five alerts parse as callouts whose variant a consumer recognizes.
- Alternatives: reject types outside a fixed set; or treat text after the
  marker as body content rather than a title.

### C-6 Automatic anchors: GFM algorithm or Pandoc's default

- Module: [anchors](anchors.md).
- Pandoc's default `auto_identifiers` strips everything up to the first
  letter, keeps `_`, `-`, and `.`, and lowercases; its `gfm_auto_identifiers`
  keeps leading digits and combining marks and removes `.`. The two give
  different values for `1. Intro` and for `A.B`.
- Provisional rule: the GFM algorithm, as `autoAnchors` states.
- Alternative: Pandoc's default algorithm, or a second option selecting it.

### C-7 Colon fences: fenced divs and container directives

- Modules: [fenced divs](fenced-divs.md), [directives](directives.md).
- Pandoc opens a div with three or more colons followed by an attribute
  container or a bare class word, with or without a space after the colons,
  and closes it with any line of three or more colons. remark opens a
  container directive with a colon run immediately followed by a name and
  closes it only with a fence at least as long as the opener.
- Provisional rule: a colon run immediately followed by a name is a
  directive, and a run followed by whitespace or `{` is a div, whichever
  options are on; a bare colon line closes the innermost open colon container
  of either kind only when it is at least as long as that container's opener,
  and a shorter one is content. `:::warning` is therefore never a div, and
  `::::` is not closed by `:::`.
- Alternatives: length-insensitive closers for divs, which gives the two
  constructs different closing rules on the same bytes; or accept
  `:::warning` as a div when `directives` is off, which makes the parse of one
  line depend on an unrelated option.

### C-8 Attribute members after a directive

- Modules: [attributes](attributes.md), [directives](directives.md).
- Pandoc's attribute grammar rejects a bare name such as `{disabled}`,
  treats `-` as the `unnumbered` class, decodes character references only
  inside quotes, and accepts an empty `name=`. `micromark-extension-directive`
  accepts bare names, has no `-` member, and decodes references everywhere.
- Provisional rule: one grammar, Pandoc's, at every attachment site,
  directives included; every runtime difference from remark is a registered
  delta.
- Alternative: keep remark's member grammar at directive sites only, which
  gives the dialect two attribute grammars for one brace syntax.

### C-9 Marks: flanking or Pandoc's boundary rule

- Module: [marks](marks.md).
- Obsidian documents `==text==` without an error grammar. Pandoc's `mark`
  extension opens at `==` followed by a non-space scalar that is not `=` and
  closes at the next `==`, so `==a ==` is a mark containing `a `.
- Provisional rule: `==` is a delimiter run with the flanking rules of `*`,
  so `==a ==` is text and `a==b==c` is a mark, consistent with every other
  delimiter of the dialect.
- Alternative: Pandoc's rule.

## Settled

These decisions are made; each names the ground rule or the defining source
that settled it.

- No profiles and no umbrella switch; one option per feature; the CLI
  `--profile` names are harness shorthands. Product ruling.
- Options are named after the dialect's own kinds and features: `crossLinks`,
  `marks`, `comments`, `headingAttributes`, and
  `implicitHeadingReferences`, not the source's names.
- `startnum` is not an option. Pandoc's switch exists because Pandoc's
  default ignores start numbers; CommonMark already honors them, so
  `List.start` is always the first marker's value for every style.
- The own-line block identifier `text` then `^id` on the next line is a
  paragraph suffix. Obsidian writes an identifier at the end of its block,
  and that source form is one paragraph.
- Pipe-table rows shorter than the delimiter row are completed with empty
  cells and longer rows are truncated, with cmark-gfm's scopes. GFM source
  and the implemented engine.
- Callout `variant` is stored as written. Store-as-written ground rule, as
  for metadata names.
- Valid UTF-8 is a caller precondition of the C entry point. Product ruling
  in #191.
- `Citation` and `Footnote` are scoped values with callbacks;
  `TableCaption`, `Definition`, and `DefinitionList` are `Markup` kinds;
  `Metadata` and `MetadataRecord` are scoped values without callbacks;
  `TableColumn`, `Destination`, `CitationReferent`, and `Attributes` are
  unscoped values. Scope ground rule.
- A bare `@label` with no bracketed tail naming a registered example label is
  an `ExampleReference`, while `[@label]` and a tailed key are citations, and
  `(@label)` is always subject to example resolution. Pandoc's reader, the
  defining source of both features.
- Grid-table spans, multiline tables, example-list resets, definition-list
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
  `native_divs` and `native_spans`, `wikilinks_title_before_pipe`, pipe-table
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
