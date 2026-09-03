# Obsidian Flavored Markdown implementation plan

Status: proposed. This plan implements the target contract in
[`docs/specs/obsidian-flavored-markdown.md`](../specs/obsidian-flavored-markdown.md)
as a new canonical AST baseline, without compatibility aliases for replaced
kinds or fields.

## Outcome

Add one coherent Obsidian profile to the C parser and all three bindings. The
profile reuses the current CommonMark/GFM block and inline algorithms, adds the
documented OFM syntax as composable extensions, exposes every new semantic fact
through the immutable canonical AST, and keeps vault resolution and rendering
out of the parser. Every `>` container becomes `Callout`; a plain quoted block
has null metadata, while `[!type]` populates that same node model.
Every successful reference link or image is resolved before the public AST is
finalized and is indistinguishable from its direct counterpart; source
definitions and reference forms remain parser-internal.

The profile is an extension preset, not a full Obsidian parser dialect. It
preserves inherited cmark/CommonMark behavior, including Markdown recognition
between paired inline HTML tags, and adds no HTML element-region suppression.
Block identifiers populate the same universal `Markup.anchor` string used by
other profiles; they do not introduce a block-specific target type.

The normative work items are the module specs linked from the
[OFM contract index](../specs/obsidian-flavored-markdown.md): wikilinks/embeds,
block identifiers, footnotes, comments, highlights, tasks, callouts, and
inherited/integration behavior. A phase is incomplete if its module's grammar,
AST invariants, fallback, scopes, or required conformance cases are unmet.
The shared [`Cite`, `Citation`, and `CitationReferent`
contract](../specs/citation-model.md) owns their reusable semantics; this plan does
not enable Pandoc `@key` syntax in the Obsidian profile.

- [ ] **Plan exit criterion:** the in-scope official extension examples, negative
      boundaries, cross-extension interactions, oracle comparison, allocation
      failures, adversarial complexity, and all four public surfaces pass together.
      A partial binding or a C-only hidden node is not a shippable intermediate
      state.

## Phase 1 — freeze the public model

- [ ] Replace `BlockQuote` with one `Callout` kind across the canonical schema and
      every public surface. Its optional `variant` and `title` plus `fold`
      distinguish metadata-free quoted blocks from `[!type]` callouts; do not
      retain a `BlockQuote` alias or parallel node.
- [ ] Replace `FootnoteReference` and `FootnoteDefinition` with one inline
      `Cite(citations, scope)` kind owning scoped
      `Citation(referent, prefix, suffix, scope)` values,
      `CitationReferent = bib(key, mode) | footnote(id)`,
      `BibMode = normal | authorInText | suppressAuthor`, one document-owned
      `Footnote(id, content, scope)` value, and `Document.footnotes`. Referenced and
      inline source forms must lower to the same one-item resolved consumer model;
      retain none of the source-shaped kinds as aliases.
- [ ] Resolve direct, full, collapsed, shortcut, and autolinks to the same `Link`
      shape, and direct/reference images to the same `Image` shape. Remove
      `LinkReference`, `ImageReference`, `ReferenceDefinition`, and
      `ReferenceForm` from the public AST. Keep labels, form, definition storage,
      and normalization in the existing parser-owned lookup operation; add no
      `CitationReferent.link` branch or document link registry.
- [ ] Add the remaining target value types and kinds to
      `docs/specs/canonical-ast.json`, `docs/specs/canonical-ast.md`, and
      `docs/specs/canonical-ast-dump.md`:
      `CrossLink`, `Mark`, and `Comment`; wikilink fragment/block-subpath and
      callout-fold enums; shared image dimensions; the universal nullable
      `anchor` field on every Markup kind; and `marker` on `ListItem`. Only the
      addressable kinds named by the block-identifier grammar receive a non-null
      anchor from that source rule.
- [ ] Replace stored `checked: Bool?` with the authored `marker: String?`.
      Keep source compatibility only through a derived language convenience
      property when that does not duplicate wire state. Treat the public shape
      change as a major-version change rather than preserving two authorities.
- [ ] Allocate stable C kind/field identifiers once, then update the native AST,
      C read-only facade, wire format, Swift/Kotlin/ES values, exhaustive visitors,
      walkers, dumpers, and AST projection audit atomically.
- [ ] Add an `obsidian` preset in each binding and a CLI `--profile obsidian`.
      Keep existing profile grammar and option composition stable, but make the
      canonical `BlockQuote` to `Callout` rename universal. Add only the option
      bits needed to compose the preset; do not add a second parser.

- [ ] **Exit criterion:** all public surfaces compile with exhaustive handling, the
      canonical schema audit proves kind/field parity, and fixtures can express every
      new fact before syntax recognition is enabled. Equivalent direct and reference
      links/images have identical semantic shapes, while only footnote citations
      retain a consumer-visible ID edge.

## Phase 2 — one OFM inline extension

- [ ] Introduce one parser-owned OFM inline extension with scanners for wikilinks,
      comments, highlights, and the inline footnote source form. All scanners use
      the existing subject cursor, delimiter/bracket infrastructure, allocator,
      source map, and extension attachment order.
- [ ] Make `![[...]]` and `[[...]]` one scanner and one `CrossLink` payload. Split
      path, fragment/block subpath, and label value once while scanning. Do not
      rescan the completed literal in a binding or renderer.
- [ ] Make comments opaque during scanning. Support retained `Comment` nodes and
      parser-level stripping from the same recognized construct so stripping
      cannot change where other delimiters bind.
- [ ] Parse highlight children through the normal inline engine, with source
      ownership by code, comments, and HTML tokens taking precedence. Paired inline
      HTML tags do not create a suppressing region. The inline footnote scanner
      creates one one-item `Cite` containing a `Citation` whose referent is
      `CitationReferent.footnote(id)` and whose prefix and suffix are empty, plus
      one document-owned `Footnote`; it normalizes the inline body to a paragraph
      and obtains the ID from the same parser-owned footnote collection used by
      inherited referenced footnotes.
- [ ] Resolve inherited `[^label]` calls through the same operation, so repeated
      calls share one `Footnote` without body duplication. Merge referenced and
      inline values in source order and assign their deterministic document-local
      IDs once during document finalization.
- [ ] Audit every inline extension caller and delete any product-specific
      delimiter skip table or repair path made obsolete by the shared model.

- [ ] **Exit criterion:** official positive forms and unmatched/escaped/code/comment/
      HTML-token negative forms have native golden AST tests; extension syntax between
      paired inline HTML tags remains enabled; referenced and inline footnotes produce
      the same one-item `Cite` edge to a `Footnote`; and size-doubling probes show
      linear scanner work on long runs of `[`, `]`, `=`, `%`, and mixed openers.

## Phase 3 — callout containers and metadata

- [ ] Add block identifiers during block finalization, when ownership is known.
      One attachment operation handles paragraph suffixes, structured-block
      follower lines, and list-item suffixes. It writes the stripped identifier
      into the owned block's universal anchor and removes the marker from visible
      content; it does not record an Obsidian or block discriminator.
- [ ] Construct `Callout` for every `>` container through the existing block
      algorithm. Default `variant` and `title` to null and `fold` to `none`.
- [ ] When the first content line has a valid marker, normalize its source type
      identifier into `variant`, populate fold state and title before ordinary body
      blocks are finalized, and remove the marker line from content. Do not
      mutate/repair a finished tree in a post-pass.
- [ ] Use the same container recursion for nested callouts. Unknown/custom types
      remain metadata-bearing callouts; alias-to-style mapping stays outside the
      parser.
- [ ] Cover metadata-free, title-only, empty-body, formatted-title, nested,
      invalid-position, mixed-case, custom-type, and whole-structured-block
      identifier cases.

- [ ] **Exit criterion:** every block identifier has exactly one owner, no valid
      marker survives as visible text, every `>` container is a `Callout`,
      metadata-free and metadata-bearing states satisfy their field invariants,
      nested callouts are stack-safe, and failure/OOM unwinds through the
      existing node ownership path.

## Phase 4 — task markers, media parameters, and tables

- [ ] Generalize the existing task-list scanner from `[ xX]` to one Unicode code
      point. Store the marker, derive completion, and retain the existing rule that
      only the item prefix is inspected.
- [ ] Parse external image `W`, `alt|W`, and `alt|W x H` suffixes in the shared
      image construction path. Keep wikilink label parameters raw until vault
      resolution establishes the embedded file kind.
- [ ] Move wiki alias-pipe awareness into the shared table/inline boundary so
      `[[target\|label]]` and `![[image\|100]]` stay inside one cell. Do not add a
      table-only wikilink parser.
- [ ] Preserve current GFM semantics for ordinary tables and task items under the
      existing profiles.

- [ ] **Exit criterion:** task markers round-trip through every public AST, two-hyphen
      tables retain current behavior, escaped wiki pipes never create extra cells,
      and image dimensions are absent rather than guessed on malformed suffixes.

## Phase 5 — product fixtures and external evidence

- [ ] Add package-owned C fixtures with a manifest mapping every example back to
      its normative OFM module. They own in-scope official extension examples,
      strict fallbacks, option gates, cross-extension conflicts, scopes, and
      source-order behavior. Do not copy product goldens into `specs/oracles/`.
- [ ] Extend `specs/canonical-ast/` with compact cross-binding cases covering every
      new kind, enum state, nullable field, ownership edge, escaping rule, and
      direct/reference link and image equivalence.
- [ ] Move each resolved entry out of
      `specs/oracles/obsidian/deltas.json` in the implementation commit that makes
      it agree. If a deliberate AST-shape difference remains, register a general
      projection and prove that it fires; never replace a semantic difference with
      normalization.
- [ ] Update the cmark/cmark-gfm and remark comparison projections for the
      universal reference-link/image normalization. Those authorities continue to
      own recognition, precedence, and fallback, but their source-shaped
      definition/reference nodes do not override the consumer AST contract.
- [ ] Keep official-only requirements—callouts, block identifiers, inline
      footnote recognition, the
      `Cite`/`Citation`/`CitationReferent`/`Footnote` projection,
      and image dimensions—under product goldens, because the selected oracle does
      not parse them. Its silence is not agreement.
- [ ] Add deterministic fuzz seeds and pathological cases for delimiter runs,
      nested callouts, inline-HTML boundaries, escaped table pipes, long
      paths/headings, and repeated block identifiers. Assert semantic output and
      structural resource bounds, not wall-clock thresholds.

- [ ] **Exit criterion:** C correctness/conformance, Swift macOS, Kotlin JVM, ES Node and
      browser, all static audits, and every external parity gate pass. Required CI on
      the remaining supported hosts then supplies the platform release evidence.

## Delivery sequence

The durable review sequence is model, inline engine, block ownership, existing
extension integration, and evidence. Each change must leave all existing
profiles green. No phase may ship the `obsidian` preset publicly until its AST
exists on every platform and the full target fixture is enabled; before that
point the preset remains internal test plumbing.

- [ ] Publish release notes listing the documented OFM subset, parser-only
      boundary, reference-link/image normalization, the `BlockQuote` to `Callout`,
      source-shaped footnote to `Cite`/`Citation`/`CitationReferent`/`Footnote`, and
      `checked` to `marker` migrations, the unchanged legacy-profile source
      grammar, and the exact official help snapshot used for conformance.
