# Obsidian Flavored Markdown implementation plan

Status: complete (O10, 2026-09-09). This plan implements the Obsidian-derived
modules of the
[Markdown Core dialect](../specs/dialect.md) as a new canonical AST baseline,
without compatibility aliases for replaced kinds or fields.

The cross-plan landing order that turns these phases into individually
mergeable pull requests is
[`docs/plans/2026-09-04-canonical-vnext-landing-plan.md`](2026-09-04-canonical-vnext-landing-plan.md).

## Outcome

Add the Obsidian syntax extensions to the C parser and all three bindings,
always on, with no parse option. They reuse the current CommonMark/GFM block and inline
algorithms, add the documented OFM syntax as always-on extensions, expose every
new semantic fact through the immutable canonical AST, and keep vault resolution
and rendering out of the parser. Every `>` container becomes `Callout`; a plain
quoted block has `variant=null`, `title=null`, and `collapsed=null`, while `[!type]`
populates that same node model. Every successful reference link or image is
resolved before the public AST is finalized and is indistinguishable from its
direct counterpart; source definitions and reference forms remain
parser-internal. One complete beginning-of-file Properties envelope populates optional
`Document.metadata` with ten direct optional fields. The fixed field set is `name`,
`title`, `subtitle`, `time`, `date`, `authors`, `keywords`, `abstract`, `state`,
and `comment`. Unrecognized or invalid input is ignored. Only `abstract` and
`comment` additionally accept indented literal prose with `: |`.


The modules are independent extensions of one language, not a preset and not a
full Obsidian parser dialect. They preserve inherited cmark/CommonMark
behavior, including Markdown recognition between paired inline HTML tags, and
add no HTML element-region suppression. Block identifiers populate the same
universal `Markup.anchor` string used by other extensions; they do not introduce
a block-specific target type. Outgoing references use the shared tagged
`Destination`: ordinary Markdown `Link` and `Media` values own
`Destination.url`, while `CrossLink` and `CrossEmbedded` values own the
`Destination.cross(path, anchor)` branch. Heading and block source spellings
populate the same optional anchor field and introduce no discriminator. No destination populates the
declaration-side anchor on its owning reference node.

The normative work items are the dialect modules linked from the [dialect
index](../specs/dialect.md): properties, cross links, block identifiers,
footnotes, comments, marks, task lists, callouts, and the links-and-images and
tables rules they extend. A phase is incomplete if its module's grammar, AST
invariants, fallback, scopes, or required conformance cases are unmet. The
shared [`Cite`, `Citation`, and `CitationReferent`
model](../specs/dialect/footnotes.md) owns their reusable semantics; this plan
does not define Pandoc `@key` syntax; the citations module does.

- [x] **Plan exit criterion:** the in-scope official extension examples, negative
      boundaries, cross-extension interactions, oracle comparison, allocation
      failures, adversarial complexity, and all four public surfaces pass together.
      A partial binding or a C-only hidden node is not a shippable intermediate
      state.

## Phase 1 — establish the public model

- [x] Replace `BlockQuote` with one `Callout` kind across the canonical schema and
      every public surface. Its optional `variant`, `title`, and `collapsed`
      distinguish metadata-free quoted blocks from `[!type]` callouts; do not
      retain a `BlockQuote` alias or parallel node.
- [x] Replace `FootnoteReference` and `FootnoteDefinition` with one inline
      `Cite(citations, scope)` kind owning scoped
      `Citation(referent, prefix, suffix, scope)` values,
      `CitationReferent = bib(key, mode) | footnote(id)`,
      `BibMode = normal | authorInText | suppressAuthor`, one document-owned
      `Footnote(id, content, scope)` value, and `Document.footnotes`. Referenced and
      inline source forms must lower to the same one-item resolved consumer model;
      retain none of the source-shaped kinds as aliases.
- [x] Add optional `Document.metadata` with ten direct optional fields and the
      shared scalar/list value model. Under the corrected O6 contract, field
      names use the fixed spelling, numbers retain exact text, and only the
      envelope has a scope. Missing metadata, empty metadata, absent fields,
      and authored null values remain distinct. Metadata is outside Markup and
      visitor callbacks; source field order and per-field scopes are not stored.
- [x] Resolve direct, full, collapsed, shortcut, and autolinks to the same `Link`
      shape with `dest=Destination.url(...)`, and direct/reference images to the
      same `Media(dest=Destination.url(...), ...)` shape. Remove
      `LinkReference`, `ImageReference`, `ReferenceDefinition`, and
      `ReferenceForm` from the public AST. Keep labels, form, definition storage,
      and normalization in the existing parser-owned lookup operation; add no
      `CitationReferent.link` branch or document link registry.
- [x] Add the remaining target value types and kinds to
      `docs/specs/canonical-ast.json`, `docs/specs/canonical-ast.md`, and
      `docs/specs/canonical-ast-dump.md`:
      the shared `Destination` enum and `Link.dest`/`Media.dest`; `CrossLink`,
      `CrossEmbedded`, `Mark`, and `Comment`; `Destination.cross(path, anchor)`;
      nullable `Callout.collapsed`; `marker` on `ListItem`; and the independent
      `Dimensions` value held only by Media and CrossEmbedded. Only the
      addressable kinds named by the block-identifier grammar receive a non-null
      anchor from that source rule.
- [x] Add the universal nullable `anchor` field and optional image dimensions
      across the facade, models and transports (`M7`, grouped into the
      node-independent `Media.dimensions: Dimensions?` by `O9`, also held by
      `CrossEmbedded.dimensions`). O9 produces typed dimensions from
      labels; directive IDs populate anchors independently.
- [x] Replace stored `checked: Bool?` with the authored `marker: String?`.
      Keep source compatibility only through a derived language convenience
      property when that does not duplicate wire state. Treat the public shape
      change as a major-version change rather than preserving two authorities.
- [x] Add each C kind and field identifier with the item that first produces it,
      then update the native AST, C read-only facade, wire format,
      Swift/Kotlin/ES values, exhaustive visitors, walkers, dumpers, and AST
      projection audit atomically. While 3.0.0 is unreleased, identifiers, wire
      layouts, and manifest order may be renumbered by any later item; nothing
      is reserved in advance.
- [x] Land each module's scanner always on and register in `specs/oracles/`
      each place it deliberately leaves an oracle's language; there is no
      parse option, no preset, no CLI `--profile obsidian`, and no layer
      selection in the test tree. Keep the inherited grammar stable, but make the
      canonical `BlockQuote` to `Callout` rename universal. Add only the engine
      bit each module needs; do not add a second Markdown parser. Properties
      has a bounded source grammar under O6; it does not add a general YAML
      parser or dependency.

- [x] **Exit criterion:** all public surfaces compile with exhaustive handling, the
      canonical schema audit proves kind/field parity, and fixtures express every
      new fact alongside its first syntax producer. Equivalent direct and reference
      links/images have identical semantic shapes, while only footnote citations
      retain a consumer-visible ID edge.

## Phase 2 — inline features

O1 introduces the cross-link descriptor and the shared wikilink/embed scanner.
O2–O4 implement marks, comments, and inline footnotes through their respective
semantic operations. Their source family does not define a runtime module or
descriptor. O4 completes the shared feature-boundary implementation bullet.

- [x] Implement cross links, comments, marks, and inline footnotes at their
      feature boundaries, reusing each feature's existing semantic model and
      operation. All scanners use the existing subject cursor,
      delimiter/bracket infrastructure, allocator, source map, and extension
      attachment order. Do not introduce an OFM umbrella extension.
- [x] Make `![[...]]` and `[[...]]` one scanner with shared reference fields.
      Construct `CrossEmbedded` and `CrossLink`, respectively; only the former
      also carries dimensions. Split path, optional anchor, and label once
      while scanning, then construct one complete `Destination.cross`. Heading and block punctuation must not
      survive as a consumer discriminator. Do not rescan the completed literal
      in a binding or renderer.
- [x] Make comments opaque during scanning and emit the same `Comment` node an
      HTML comment produces; nothing is stripped, so nothing can change where
      other delimiters bind.
- [x] Parse highlight children through the normal inline engine, with source
      ownership by code, formulas, HTML comments/tokens, and cross links taking
      precedence. Paired inline HTML tags do not create a suppressing region.
      The `%%` opacity composition is covered by O3.
- [x] The inline footnote scanner creates one one-item `Cite` containing a
      `Citation` whose referent
      is `CitationReferent.footnote(id)` and whose prefix and suffix are empty,
      plus one document-owned `Footnote`; it stores the parsed inline body
      directly in `Footnote.content` with no synthesized `Paragraph` and obtains
      the ID from the same parser-owned footnote collection used by inherited
      referenced footnotes.
- [x] Resolve inherited `[^label]` calls through the same operation, so repeated
      calls share one `Footnote` without body duplication. Merge referenced and
      inline values in source order and assign their deterministic document-local
      IDs once during document finalization.
- [x] Audit every inline extension caller and delete any product-specific
      delimiter skip table or repair path made obsolete by the shared model.

- [x] **Exit criterion:** official positive forms and unmatched/escaped/code/comment/
      HTML-token negative forms have native golden AST tests; extension syntax between
      paired inline HTML tags remains enabled; referenced and inline footnotes produce
      the same one-item `Cite` edge to a `Footnote`; and size-doubling probes show
      linear scanner work on long runs of `[`, `]`, `=`, `%`, and mixed openers.

## Phase 3 — Properties, anchors, and callout containers

- [x] Recognize at most one exact `---` Properties envelope at the beginning of
      the decoded document after an optional BOM. A complete envelope always
      attaches metadata; only a missing or unclosed envelope leaves its bytes
      to inherited Markdown. Allocation failure remains terminal.
- [x] Rework Properties under the user-corrected O6 goal: recognize the ten
      fixed fields and selected scalar, bracketed-array and block-list forms.
      Store the ten named fields directly, retaining only the envelope scope.
      Ignore unknown names, unnamed text, comments, invalid members and
      duplicates; retain valid neighbors. Add bare `: |` only for
      `abstract` and `comment`, preserving internal newlines and blank lines.
      Remove alias expansion, anchor transactions, explicit tags, multiline
      folding, the comment/data wrapper, and any full YAML parser workstream.
      Verify member recovery, literal indentation, allocator/OOM and bindings.
- [x] Verify metadata/body scopes and native value ownership on every
      surface. Metadata values never enter Markup or visitor callbacks.

- [x] Add `#anchor-id#` block identifiers during block finalization, when
      ownership is known. One attachment operation handles paragraph suffixes,
      structured-block follower lines, and list-item suffixes. It writes the
      identifier without either `#` delimiter into the owned block's universal
      anchor and removes the complete marker from visible content; it does not
      record an Obsidian or block discriminator.
- [x] Construct `Callout` for every `>` container through the existing block
      algorithm. Default `variant`, `title`, and `collapsed` to null.
- [x] When the first content line has a valid marker, store its source type
      identifier as written in `variant`, populate `collapsed` and title before
      ordinary body blocks are finalized, and remove the marker line from
      content. Do not mutate/repair a finished tree in a post-pass.
- [x] Use the same container recursion for nested callouts. Unknown/custom types
      remain metadata-bearing callouts; alias-to-style mapping stays outside the
      parser.
- [x] Revalidate absent/empty/populated Properties, documented property
      values, the ten field names, arrays and block lists, strict fences, ignored unsupported
      input, literal prose, and Properties/body scope boundaries against corrected O6.
- [x] Cover metadata-free, title-only, empty-body, formatted-title, nested,
      invalid-position, mixed-case, custom-type, and whole-structured-block
      identifier cases.

- [x] **Exit criterion:** one complete Properties envelope yields metadata and no body
      node, unsupported members are ignored while valid neighbors remain records, incomplete envelopes remain Markdown, and every
      block identifier has exactly one owner, no valid marker survives as
      visible text, every `>` container is a `Callout`, and plain and
      marker-bearing states satisfy their field invariants; nested
      callouts are stack-safe, and failure/OOM unwinds through the existing node
      ownership path.

## Phase 4 — task markers, media parameters, and tables

- [x] Generalize the existing task-list scanner from `[ xX]` to one Unicode
      scalar. Store the marker, derive completion, and retain the existing rule that
      only the item prefix is inspected.
- [x] Parse external image `W`, `WxH`, `alt|W`, and `alt|WxH` suffixes,
      lowercase `x` with no surrounding spaces, in the shared image construction
      path. Consume valid embed dimension suffixes through the shared Dimensions parser;
      keep the remaining embed label and ordinary cross-link labels raw. The parser
      does not resolve a vault target or infer its media type.
- [x] Move wiki alias-pipe awareness into the shared table/inline boundary so
      `[[target\|label]]` and `![[image\|100]]` stay inside one cell. Do not add a
      table-only wikilink parser.
- [x] Preserve current GFM semantics for ordinary tables and task items that use
      no Obsidian syntax.

- [x] **Exit criterion:** task markers round-trip through every public AST, two-hyphen
      tables retain current behavior, escaped wiki pipes never create extra cells,
      and image dimensions are absent rather than guessed on malformed suffixes.

## Phase 5 — product fixtures and external evidence

- [x] Add package-owned C fixtures with the
      [O10 module manifest](2026-09-09-obsidian-evidence-closure.md#module-and-fixture-ownership)
      mapping every example back to its normative OFM module. They own in-scope
      official extension examples,
      strict fallbacks, cross-extension conflicts, scopes, and
      source-order behavior. Do not copy product goldens into `specs/oracles/`.
- [x] Extend `specs/canonical-ast/` with compact cross-binding cases covering every
      new kind, enum state, nullable field, ownership edge, escaping rule, and
      direct/reference link and image equivalence.
- [x] Move each resolved entry out of
      `specs/oracles/obsidian/deltas.json` in the implementation commit that makes
      it agree. If a deliberate AST-shape difference remains, register a general
      projection and prove that it fires; never replace a semantic difference with
      normalization.
- [x] Update the cmark/cmark-gfm and remark comparison projections for the
      universal reference-link/image normalization. Those oracles remain
      evidence for recognition, precedence, and fallback, which the dialect
      modules own, and their source-shaped definition/reference nodes do not
      override the consumer AST contract.
- [x] Update the existing exact-envelope / `yaml@2.9.0` Document/CST
      oracle to the corrected Properties domain. Compare ordered mapping
      pairs, decoded names, exact numbers and supported values without a
      JavaScript object intermediary. Remove alias/tag success requirements;
      wider YAML acceptance does not extend the product. Product fixtures own
      ignored input, literal prose, member recovery, scopes, resource bounds and OOM.
- [x] Keep official-only requirements—callouts, block identifiers, inline
      footnote recognition, the
      `Cite`/`Citation`/`CitationReferent`/`Footnote` projection,
      and image dimensions—under product goldens, because the selected oracle does
      not parse them. Its silence is not agreement.
- [x] Add deterministic fuzz seeds and pathological cases for delimiter runs,
      nested callouts, inline-HTML boundaries, escaped table pipes, long
      paths/headings, and repeated block identifiers. Assert semantic output and
      structural resource bounds, not wall-clock thresholds.

- [x] **Exit criterion:** C correctness/conformance, Swift macOS, Kotlin JVM, ES Node and
      browser, all static audits, and every external parity gate pass. Required CI on
      the remaining supported hosts then supplies the platform release evidence.

## Delivery sequence

The durable review sequence is model, inline engine, block ownership, existing
extension integration, and evidence. Each change must leave the existing
fixtures green. No phase may publish a module's syntax until its AST exists on
every platform and the module's target fixture is enabled; before that point the
module's scanner remains internal test plumbing.

- [x] Prepare the unreleased 3.0.0 release notes listing the documented OFM
      subset, parser-only boundary, `Document.metadata` addition, reference-link/image normalization,
      the `BlockQuote` to `Callout`, source-shaped footnote to
      `Cite`/`Citation`/`CitationReferent`/`Footnote`, and `checked` to `marker`
      migrations, the unchanged inherited source grammar, and the exact
      official help snapshot used for conformance.

The [O10 evidence record](2026-09-09-obsidian-evidence-closure.md) maps the
completed model, module fixtures, ownership boundaries, caller audit,
complexity/OOM probes and validation commands. Release publication remains a
separate operation; subsequent Pandoc and insertion items do not hold this
OFM subset open.
