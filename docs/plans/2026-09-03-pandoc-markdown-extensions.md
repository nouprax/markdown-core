# Pandoc Markdown extensions implementation plan

Status: proposed. This plan implements the independently composable extensions
in [`docs/specs/pandoc.md`](../specs/pandoc.md) on the canonical parser and all
public bindings. It does not add a monolithic Pandoc dialect and does not retain
compatibility aliases for any public model that this work replaces.

## Outcome

Add the selected Pandoc syntax as opt-in rules inside the existing block and
inline engines. All source forms project to one consumer-oriented canonical
AST: every Markup value has the universal anchor and attributes fields, all
table syntaxes produce one Table model, all ordered-list syntaxes produce one
List model, and Pandoc bibliography calls use the shared `Cite`/`Citation`/
`CitationReferent` model. Ordinary links and images own the shared
`Destination.url` branch, including implicit heading references. Reader state
needed only for recognition—reference
definitions, virtual heading references, example counters, attribute
attachment candidates, and table boundary maps—remains parser-owned.

The normative behavior is divided among the modules linked from the
[Pandoc extension index](../specs/pandoc.md). This plan owns implementation
order and proof obligations, not a second copy of their grammars. A phase is
incomplete if its module's model, recognition, fallback, precedence, scopes,
option isolation, allocation behavior, or complexity requirements are unmet.

The inherited language remains the repository's current CommonMark/GFM
implementation. Pandoc 3.11 is authoritative only for the explicitly selected
extension layer. The product must not acquire unrelated behavior merely
because Pandoc enables it in its default `markdown` format.

## Frozen authority and executable oracle

The source and runner contract is already frozen in
[`specs/oracles/pandoc/source.json`](../../specs/oracles/pandoc/source.json):

| Role                          | Frozen choice                                                                                                                      |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| Normative prose               | Pandoc 3.11 `MANUAL.txt` at tag commit `b913622e1ff87c69ab8b1a606577122e220925cd`                                                  |
| Grammar and registry evidence | `Markdown.hs`, `Shared.hs` attribute merge, and `Extensions.hs` blobs at the same commit                                           |
| Executable implementation     | Official Pandoc 3.11 release CLI, verified against its per-platform SHA-256 before first use                                       |
| Observation format            | Pandoc native JSON from `--to=json`, without citeproc, filters, templates, defaults files, or bibliography lookup                  |
| Reader isolation              | `markdown_strict` plus the exact extension set declared by each input-only corpus case; the default `markdown` bundle is forbidden |

The User's Guide owns accepted source syntax. CLI JSON is the executable
oracle for recognition, fallback, grouping, ordering, and Pandoc's semantic
facts after a reviewed projection into the target model. Neither raw Pandoc
constructor names nor its early rendering choices override Markdown Core's
consumer contract. Current cmark and cmark-gfm continue to own inherited
syntax and precedence where no selected Pandoc extension participates.

## Phase 0 — bootstrap the pinned oracle

- [ ] Add `oracle-pandoc` to `scripts/init-environment.sh`. `--install` downloads
      only the host archive declared in `source.json`, verifies its SHA-256, and
      installs it below the repository-managed tools directory. `--check` accepts
      only the exact 3.11 runner. Normal build and test commands perform no
      network access.
- [ ] Implement one oracle adapter that reads
      [`corpus.json`](../../specs/oracles/pandoc/corpus.json), passes each case's
      explicit `from` string to the official CLI, and requests JSON. Before
      accepting an observation it must run canaries for the exact version, native
      JSON API envelope, extension enable/disable behavior, expected top-level
      construct, UTF-8 input, and isolation from user data.
- [ ] Define one semantic projection per target concept, not per example. It may
      discard source locations that Pandoc JSON does not expose and may translate
      representation-only constructors such as `Plain`; it may not erase
      recognition, content, order, anchors, attributes, list
      style/start/delimiter, citation mode/affixes, heading anchors, table groups,
      column alignment/width, or cell spans.
- [ ] Keep the corpus input-only. Product expected AST belongs in the C fixtures
      and `specs/canonical-ast/`; raw Pandoc JSON and projected Markdown Core
      output are generated during comparison and never committed as product
      goldens.

- [ ] **Exit criterion:** a clean checkout can explicitly install or check the pinned
      runner, and the oracle-side canaries reject the wrong binary, API version,
      reader bundle, user-data environment, or extension behavior. This phase makes
      no product-parity claim because the target options and AST do not yet exist.

## Phase 1 — freeze the public consumer model

- [ ] Add the shared `Destination` enum defined by
      [`docs/specs/destinations.md`](../specs/destinations.md) to every public
      surface. Replace `Link.destination: String` and `Image.source: String`
      with `dest: Destination` on both nodes, require the `url` branch for
      ordinary links and images, and coordinate the same canonical change with
      Obsidian's `cross` branch and optional anchor; do not retain either old
      string as parallel compatibility state.
- [ ] Add the universal nullable `anchor` field defined by
      [`docs/specs/anchors.md`](../specs/anchors.md) and the non-null
      `attributes` field defined by
      [`docs/specs/attributes.md`](../specs/attributes.md) to every canonical
      Markup kind and all four public surfaces. `anchor` is one
      source-independent string; `Attributes` contains ordered classes and
      ordered `Record` values. Kinds without an enabled source rule retain
      `anchor=null` and `Attributes.empty`; do not create node-specific copies.
- [ ] Add `Span`, `Superscript`, `Subscript`, `Div`, `DefinitionList`,
      `Definition`, and `ExampleReference`, plus the ordered-list style and
      delimiter values, `ListItem.exampleLabel`, heading anchors through the
      universal field, and the complete unified Table values defined by the
      module specs.
- [ ] Add the shared bibliography branch to `Cite`, `Citation`, and
      `CitationReferent`. Coordinate the same canonical change with the Obsidian
      footnote migration: Pandoc `@key` creates `CitationReferent.bib`, while
      footnote syntax creates `CitationReferent.footnote`; neither profile gets a
      parallel citation node.
- [ ] Allocate C kind/field identifiers once, then update the native tree, C
      facade, canonical wire schema, dump format, Swift/Kotlin/ES values,
      exhaustive visitors, walkers, and AST projection audit atomically.
- [ ] Expose one independent option per public extension named by the index.
      `auto_anchors` composes the two pinned Pandoc extension rules internally;
      compact definition syntax remains part of `definition_lists` and receives
      no invented option.
- [ ] Activate product comparison once those options and target values can be
      represented. Register every initial gap in a fail-closed `deltas.json` with
      both semantic digests and the phase that closes it; add the offline gate to
      `check:oracle-parity`. A new gap, changed gap, or registered gap that
      disappears without removal fails the gate.

- [ ] **Exit criterion:** fixtures can express every target fact, every surface is
      exhaustive and schema-identical, invalid field combinations are rejected, and
      no parser syntax is public before its consumer representation exists on all
      platforms. Every initial oracle case is either equal or registered.

## Phase 2 — one attribute operation and heading registry

- [ ] Implement the shared attribute scanner and normalization operation once.
      The pinned Pandoc 3.11 reader supplies its exact source grammar and `Attr`
      semantics; project its identifier into the owner anchor and its remaining
      components into classes and records. Remark and Pandoc profile modules
      contribute attachment sites only. No node owns a private parser or storage
      shape.
- [ ] Replace the existing directive-only Remark attribute parser and pair-array
      storage with the shared Pandoc operation. Update the directive fixtures and
      add every resulting Remark-oracle grammar difference to its fail-closed
      registry in that same commit; do not retain Remark shorthand, bare-name,
      empty-assignment, or entity behavior as a hidden mode.
- [ ] Attach inline code, heading, fenced-code, link/image, bracketed-Span, and
      fenced-Div attributes during construction of their owning node. Reference
      definitions retain attributes only in the existing parser-owned resolution
      table. An occurrence-local suffix belongs to that occurrence's
      source-faithful scope; definition inheritance transfers semantic values
      only and never expands, unions, or substitutes the occurrence scope.
      Failed suffixes release source transactionally.
- [ ] Finalize explicit and generated heading anchors in one document registry.
      Use the specified GFM algorithm, reserve every explicit anchor from every
      enabled profile before synthesis, generate headings in source order,
      resolve generated collisions deterministically, and build virtual
      implicit-reference entries from the same final values.
- [ ] Audit every existing Link/Image, Heading, Code/CodeBlock, directive, and
      reference-definition caller. Remove repair passes or duplicated fields made
      obsolete by the shared operation.

- [ ] **Exit criterion:** each authored attribute container has exactly one owner,
      generated anchors have no fictional source, reference occurrences merge
      anchors and attributes once without inheriting definition ranges,
      directives and Pandoc sites coexist without precedence drift, and long or
      malformed containers remain linear.

## Phase 3 — inline extensions and document resolution

- [ ] Add bracketed spans to the existing bracket stack so complete attribute
      suffixes, links/images, citations, implicit references, and literal fallback
      are decided by one ownership algorithm.
- [ ] Add superscript and subscript through the delimiter engine with their exact
      escape/whitespace boundaries. Do not rescan Text nodes after inline parsing.
- [ ] Add Pandoc bibliography syntax to the shared citation builder. Preserve
      item order, `BibMode`, prefixes, and complete suffixes; bibliography and CSL
      processing remain consumer responsibilities.
- [ ] Collect example labels and virtual heading definitions while parsing the
      document, then resolve their conflicts with citations and ordinary
      references through one deterministic finalization operation. Resolution
      order may not depend on whether the declaration precedes the call.

- [ ] **Exit criterion:** all inline precedence and malformed boundaries pass with every
      option independently on and off, citations project to the shared model, and
      size-doubling runs of brackets, attributes, `@`, braces, carets, and tildes
      show linear work.

## Phase 4 — block containers, lists, and definitions

- [ ] Recognize fenced Divs in the existing container stack. Opening attributes
      are required, closing fences have no separate semantic node, nesting obeys
      the shared limit, and contained blocks are parsed in place.
- [ ] Generalize the ordered-list marker operation for Pandoc style, delimiter,
      and starting-number facts. Preserve one List model and the ordinary list
      continuation algorithm; do not add a Pandoc-only list tree.
- [ ] Add example-list numbering and label registration as document parser state.
      Store authored labels and list starts, not a second derived ordinal on each
      item.
- [ ] Add definition lists before paragraph fallback using bounded lookahead for a
      complete term/first-marker prefix. Feed each definition body directly to the
      shared block parser and preserve multiple bodies without an intermediate
      item node or a reparse pass.

- [ ] **Exit criterion:** container and list ownership is unambiguous, compact and loose
      definitions share one algorithm, list style changes split at the specified
      boundary, example resets/duplicates resolve deterministically, and OOM/nesting
      failures unwind through existing ownership paths.

## Phase 5 — one table parser and logical grid

- [ ] Generalize the current table construction path so inherited pipe, simple,
      multiline, and grid recognition all emit one Table model. Syntax-specific
      scanners may discover boundaries, but they must share cell block parsing,
      caption attachment, allocation, scope construction, and final validation.
- [ ] Implement captions as an attachment candidate claimed by the nearest
      complete eligible table. Do not emit a caption paragraph and later repair
      the AST.
- [ ] For simple and multiline forms, derive alignment and optional relative
      widths directly from established column ranges. Normalize inline-only cells
      to one paragraph without losing their source scopes.
- [ ] For grid tables, maintain a row-width occupancy array. Store a spanning cell
      once in the row containing its upper-left coordinate; validate every
      occupied coordinate and prevent a span from crossing head/content/foot
      boundaries.
- [ ] Decide table, thematic-break, Setext-heading, fenced-code, definition-list,
      and caption precedence before commitment. Malformed candidates must return
      all unowned source to the inherited block parser.

- [ ] **Exit criterion:** all source forms produce the same canonical shape, interleaved
      row/column spans validate without placeholders, nested block cells use the
      ordinary parser, boundary work is linear in bytes plus emitted cells, and
      adversarial grids use memory proportional to row width plus output.

## Phase 6 — conformance, bindings, and release evidence

- [ ] Add package-owned fixtures mapped to every normative Pandoc module. Cover
      official positive examples, negative boundaries, option gates, extension
      conflicts, exact scopes, allocation failures, nesting limits, and
      adversarial size-doubling inputs.
- [ ] Extend the shared canonical AST corpus with every new kind, enum case,
      nullable field, universal anchor and attributes state, citation branch,
      list form, definition body shape, and table span arrangement.
- [ ] Remove each parity gap in the implementation commit that closes it. Any
      intentional consumer-model projection must be general, documented, and
      exercised by a canary; it cannot conceal a source-recognition difference.
- [ ] Run native correctness/conformance, Swift macOS, Kotlin JVM, ES Node and
      browser, static audits, fuzz seeds, and the complete external oracle suite.
      Supported-host CI supplies the remaining release evidence.

- [ ] **Exit criterion:** every selected extension is independently composable, all four
      surfaces expose one canonical model, the pinned Pandoc corpus has no
      unregistered divergence, inherited profiles remain green, and no test or build
      step fetches mutable external state.

## Delivery sequence

Review and land in the order: oracle bootstrap, canonical model and parity
skeleton, attribute/heading infrastructure, inline recognition, block/list
recognition, table recognition, and integration evidence. Each change must
leave existing profiles green. No Pandoc extension is public until its AST is
available on every binding and its option-off, oracle, and product conformance
cases pass.

- [ ] Publish release notes listing the exact supported extension names, the
      absence of a monolithic Pandoc preset, the universal anchor and attributes
      fields, the shared citation model, the exact 3.11 source/runner pin, and
      every public AST migration.
