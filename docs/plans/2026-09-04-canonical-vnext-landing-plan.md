# Canonical vNext landing plan

Status: proposed. This plan turns the feature contracts merged in #192, #193,
#194, and #196 into an ordered list of pull requests that can each merge
alone. It owns the cross-profile landing order, the per-pull-request
definition of done, and the one-time allocation of kinds, values, and options.
It does not restate grammars or proof obligations: the two implementation
plans remain normative for their phases and exit criteria, and every module
specification remains normative for its behavior.

- [Obsidian Flavored Markdown implementation plan](2026-09-02-obsidian-flavored-markdown.md)
- [Pandoc Markdown extensions implementation plan](2026-09-03-pandoc-markdown-extensions.md)

Both plans freeze the public model in one phase and then add syntax. Landing
that literally would mean one pull request that touches every kind on every
surface, once per profile, while nothing else can merge. This plan instead
lands the shared model one consumer fact at a time and then lands each syntax
feature as one self-contained pull request, so the profile tracks proceed in
parallel and every merge leaves `main` releasable.

## What the last seven commits changed

| Commit                                        | Kind   | Effect on this plan                                                                                                                                                                                             |
| --------------------------------------------- | ------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| #187 node-kind walking visitors               | landed | Every new kind adds entering and exiting callbacks to the Swift, Kotlin, and ES walkers; the public-surface audit derives the required count from `canonical-ast.json`.                                       |
| #190 release dry-run readiness                | landed | Every pull request must pass the credential-free `Release Dry Run - Ready` check, so an intermediate state that cannot build every artifact cannot merge.                                                       |
| #191 UTF-8 repair removal and table positions | landed | Valid UTF-8 is a caller precondition, so new scanners add no validation or repair path. The position ledgers are fail-closed ratchets that every parser change keeps exact.                                    |
| #192 extension profile contracts              | specs  | The Obsidian module set, the Pandoc module set, the shared attributes, citation, and inserted-text contracts, Remark directive attachment, the Pandoc and Obsidian oracle pins, and the two implementation plans. |
| #193 anchors and destinations                 | specs  | The universal `Markup.anchor` field, the tagged `Destination` value on `Link`, `Image`, and `CrossLink`, and the simplified `Attributes` shape.                                                                 |
| #194 Obsidian Properties                      | specs  | `Document.metadata`, the shared metadata value model, the Properties envelope, and the `yaml@2.9.0` oracle.                                                                                                     |
| #196 Properties corrections                   | specs  | Mapping keys are textual names, explicit null roots are rejected, and the oracle canaries were tightened.                                                                                                       |

The inserted-text contract is the one specification that no existing plan
sequences; it is landed here as its own track.

## How to use this plan

- One checkbox below is one pull request. It merges alone, leaves every existing
  profile's fixtures byte-identical unless the item says otherwise, and passes
  required CI including the release dry run. The three stage exit criteria are
  gates rather than pull requests: each is verified in the pull request of the
  last item of its stage, named beside it, and has nothing of its own to tick.
- Tick the item when its pull request has merged. In that same pull request,
  tick the bullets it discharges in the owning implementation plan and update
  the support audit table in
  [`docs/specs/obsidian-flavored-markdown.md`](../specs/obsidian-flavored-markdown.md)
  whenever an Obsidian row changes status.
- The written order is the default order. Any order that respects the `Requires`
  column of the dependency table is valid: that column lists every direct merge
  prerequisite, an item's full requirement is the transitive closure of that
  column, and nothing outside that closure may block an item. Every feature
  track root requires `M7`, so every feature item may rely on the complete
  shared model. A conformance case that composes the syntax of two items is
  owned by the item that merges later. When one of the two items requires the
  other, directly or through the closure, the requiring item is that later item
  and owns the case without further notice; when neither requires the other, the
  case is listed in the `Cross-item cases` column of both items, so the earlier
  item neither waits for it nor claims it. Opacity is the one composition the
  column does not enumerate: code spans and HTML tokens hide every other
  construct's syntax and belong to each item alone, and the comment opacity case
  of any item is owned by whichever of `O3` and that item merges later. The
  model items `M1` through `M7` are serialized because each regenerates shared
  goldens; the three feature tracks are independent of one another after `M7`.
- Item identifiers are stable. A registered oracle gap names the item that
  closes it: Pandoc and inserted-text gaps use these identifiers from the
  start, and the existing Obsidian entries, which name plan phases, are
  retargeted to identifiers by the item that closes them.
- Each item states its scope, its proof, and its `Requires`. The module
  specification named by the item owns the complete conformance-case list;
  the item repeats only what decides the landing boundary.

## Landing rules

- Shape before syntax. A change to the consumer model lands before the syntax
  that populates it, and each such change carries every surface at once:
  `docs/specs/canonical-ast.json`, `canonical-ast.md`, and
  `canonical-ast-dump.md`; the C engine, facade header, export allowlists,
  and dump; the Swift, Kotlin, and ES models, exhaustive visitors, walking
  visitors, and dumpers; the JNI, Kotlin/Native, and Wasm transports; and the
  shared canonical fixtures with their manifest vocabulary.
- A new kind lands together with the first syntax that produces it.
  `scripts/check-canonical-ast-fixtures.mjs` requires every declared kind to
  appear in a parseable canonical case, so a kind without a producer cannot
  pass the contract check. Field and enum changes on existing kinds are
  therefore the only model-only pull requests; they are the `M` items. Every
  item that adds, removes, or renames a kind requires `D8`, so no ordinal is
  assigned before the allocation policy exists.
- A feature pull request is one option's behavior: the C extension or
  scanner, its reviewed position in the attach-order table, the option's
  facade field, registry row, `canonical-ast.md` row, manifest key, and
  binding option, the new kind if any, package fixtures for the module's
  required conformance cases, a canonical case for each new kind or state,
  and the removal of every oracle gap it closes.
- Every option name is allocated in the inventory and registered through the
  registry that `X0` creates. A feature item publishes its option, defaulting
  to off, together with the behavior, option-off cases, oracle evidence, and
  product fixtures that make it public; the composed `obsidian` switch and
  `stripObsidianComments` remain internal test plumbing until `O10`. The
  existing profiles never change language.
- Nothing here ships before `R1`. Between merges, an unproduced enum branch,
  value type, or field is allowed only where an item says so, and the item
  that first produces it is named; a published option always recognizes its
  syntax. No item publishes two representations of one semantic fact, even
  between merges: the item that introduces a replacement removes what it
  replaces.
- Parity gates are ratchets. A model item that changes an input's Markdown
  Core digest re-registers that digest in the same pull request; a feature
  item deletes the gaps it closes in the same pull request. A model delta that
  survives a feature is a general projection with a canary, never a hidden
  normalization.

### Definition of done for every item

- Contract: JSON, prose, and dump grammar updated together;
  `pnpm audit:ast-projections`, `pnpm check:contracts`, and
  `pnpm audit:surface` pass.
- C: engine node type, facade accessors, `core/exports/markdown_core.map` and
  `markdown_core.exports`, the `extensions/ast.c` dump, the CLI, and the
  extension table position; `ctest --preset correctness` and `conformance`,
  ASan, UBSan, TSan, and the strict OOM runner pass.
- Bindings: Swift `Markup/`, `Visitor/`, and `NativeValues.swift`; Kotlin
  `model/`, `visitor/`, the JNI kind enum, decoder, and payload encoder, the
  Kotlin/Native adapter, the API dumps under `api/`, and
  `specs/kotlin/jvm-visible-surface.txt`; ES `model/`, `index.ts` exports,
  `visitor.ts`, `walking-visitor.ts`, `tree-dumper.ts`, `wire/kinds.ts`,
  `wire/node-decoder.ts`, `bridge.c`, and the type consumer.
- Fixtures: one package fixture file per module registered in
  `packages/markdown-core/tests/CMakeLists.txt` with its options; regenerated
  goldens reviewed together with the parser change; canonical cases with
  manifest option and coverage vocabulary and checker validators.
- Ledgers and gates: `specs/positions/`, `specs/reference-resolution/`, and
  every `specs/oracles/*/deltas.json` updated in the same change with the
  reason in the commit message; `pnpm check:oracle-parity` and the fuzz seeds
  pass.
- Documentation: the CHANGELOG entry under the unreleased version, the
  binding READMEs when a public option or preset changes, and the Obsidian
  support audit row.
- Cross-item cases: every case in the item's `Cross-item cases` column whose
  partner item has already merged is part of this item's fixtures.

## Decisions to settle before the item that names them

- [ ] **D1 — Version line.** `VERSION` is `3.0.0` and no release tag exists,
      so decide whether the canonical vNext model ships as part of 3.0.0 or
      whether 3.0.0 ships first and this plan targets 4.0.0. Recommendation:
      keep `3.0.0` while landing, because it is already a source-breaking
      major release with no consumers; if it is tagged before `M7` merges,
      bump to `4.0.0` in the next model item. Needed by every model item.
- [ ] **D2 — Dump spelling.** Choose once how the canonical dump prints the
      inherited fields `anchor` and `attributes`, tagged values
      (`Destination`, `CitationReferent`, `MetadataValue`), and a node or
      traversed value whose several Markup-valued fields must stay
      distinguishable (`Citation` prefix and suffix, `Callout` title,
      `Definition` term and bodies, `Table` head, content, and foot). The
      dump grammar currently forbids
      edge labels, which is ambiguous for those nodes; the recommendation is
      to print inherited fields immediately after `scope` and to nest each
      multi-field relation under a labeled group line. Needed by `M1` for
      tagged values and by `M4`, `M6`, `O8`, and `P10` for grouped relations.
- [ ] **D3 — Traversed values.** `Citation` and `Footnote` carry scopes and
      are entered by the walk, yet their contracts define them as semantic
      values rather than `Markup` kinds, so they neither join the kind
      inventory nor receive the universal `anchor` and `attributes` fields.
      Decide how the exhaustive visitors, walkers, dumpers, wire transports,
      projection audit, and fixture checker represent a traversed value.
      Recommendation: a typed value-callback set beside the `Markup`
      callbacks, a value line form in the dump grammar and the canonical
      fixture checker, and a value surface list in the projection audit.
      Making them kinds instead is an explicit revision of the citation and
      footnote contracts that must land before `M4`. `TableCaption` is a
      scoped block container reached through a typed field, like
      `DirectiveLabel`, and stays a kind unless `P11a` decides otherwise.
      Needed by `M4` and `P11a`.
- [ ] **D4 — One table cell model.** The Pandoc table contract makes every
      cell block content with an inline cell normalized to one `Paragraph`,
      while the canonical contract and the Obsidian integration module still
      describe inline cells. Decide that inherited pipe tables adopt the
      unified model in `M6`, and record the cmark-gfm and remark projection
      change that wraps phrasing content. Needed by `M6`.
- [ ] **D5 — Obsidian switch and comment default.** Decide the public shape
      of the profile switch (recommendation: one `obsidian` boolean plus
      `stripObsidianComments`, with binding presets composing GFM, footnotes,
      formulas, and the modules) and the default of `stripObsidianComments`
      when `obsidian` is off (recommendation: `true`, matching the profile).
      Needed by `O1`.
- [ ] **D6 — Fenced-code language authority.** Under `fenced_code_attributes`
      the first class is the sole stored language, while `CodeBlock.language`
      remains the inherited first info word with the option off. Decide the
      relation between `info`, `language`, and `attributes.classes` so no node
      stores two authorities. Needed by `P2c`.
- [ ] **D7 — Emoji alias table.** GFM automatic anchors replace emoji
      shortcodes and Unicode emoji with GitHub alias names, which requires a
      pinned generated table with recorded provenance, like `entities.inc`.
      Decide the source and the pin. Needed by `P3`.
- [ ] **D8 — Kind ordinals.** The C kind enum ordinal is the wire kind. Decide
      whether the removals in `M2` and `M4` compact the enum or leave holes,
      and allocate the final order from the inventory below so later items
      append at reserved positions. Needed by `M2` and by every item that
      adds, removes, or renames a kind.
- [ ] **D9 — Reference expansion bound.** Resolution copies the winning
      destination into every occurrence, which the current
      `pathological_reference_expansion_bound` case and the reference ledger
      forbid. The bound is a property of every surface: each binding decoder
      materializes a language string per node, and the JNI and Wasm transports
      copy string bytes per record, so interning only the C tree would let the C
      case pass while the binding trees still grow with destination length times
      reference count. Decide the interning rule for the C tree, the
      shared-string layout of both transports, and the decoder rule that shares
      one language string per distinct interned string across occurrences, and
      restate the bound as one adversarial document checked on every surface:
      interned bytes in C, payload bytes per transport, and distinct string
      materializations per decoder. Needed by `M2`.

## Target inventory

This is the one-time allocation both plans ask for. Items introduce kinds and
values only at these positions; the C enum, wire kinds, and manifest order
follow the same list.

### Markup kinds

| Kind                                                                                               | Target fields in canonical order                                                                                  | Change                                          | Item         |
| -------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- | ----------------------------------------------- | ------------ |
| `Document`                                                                                         | `content`, `metadata: Metadata?`, `footnotes: [Footnote]`                                                         | changed                                         | `M4`, `M7`   |
| `Callout`                                                                                          | `variant: String?`, `fold: CalloutFold`, `title: [Markup]?`, `content`                                            | replaces `BlockQuote`                           | `M3`         |
| `Paragraph`, `ThematicBreak`, `HTMLBlock`, `FormulaBlock`                                          | as today                                                                                                          | unchanged                                       | —            |
| `Heading`                                                                                          | `level`, `content`                                                                                                | unchanged; anchors use the inherited field      | —            |
| `List`                                                                                             | `flavor`, `start`, `style: OrderedListStyle?`, `delimiter: OrderedListDelimiter?`, `tight`, `items`               | changed                                         | `M5`         |
| `ListItem`                                                                                         | `marker: String?`, `exampleLabel: String?`, `content`                                                             | changed; `checked` removed                      | `M5`         |
| `CodeBlock`                                                                                        | `info`, `language`, `literal`, `fenced`, `closed`                                                                 | unchanged pending `D6`                          | —            |
| `Table`                                                                                            | `caption: TableCaption?`, `columns: [TableColumn]`, `head: [TableRow]`, `content: [TableRow]`, `foot: [TableRow]` | changed; `caption` arrives with its kind        | `M6`, `P11a` |
| `TableRow`                                                                                         | `cells`                                                                                                           | changed; `isHeader` removed                     | `M6`         |
| `TableCell`                                                                                        | `rowspan: Int`, `colspan: Int`, `content` (block)                                                                 | changed                                         | `M6`         |
| `TableCaption`                                                                                     | `content` (block)                                                                                                 | new; typed field of `Table`                     | `P11a`       |
| `DirectiveBlock`                                                                                   | `name`, `label`, `content`                                                                                        | changed; attributes move to the inherited field | `M7`         |
| `DirectiveLabel`                                                                                   | `content`                                                                                                         | unchanged                                       | —            |
| `Div`                                                                                              | `closed: Bool`, `content`                                                                                         | new                                             | `P8`         |
| `DefinitionList`                                                                                   | `definitions: [Definition]`                                                                                       | new                                             | `P10`        |
| `Definition`                                                                                       | `term: [Markup]`, `content: [[Markup]]`, `compact: Bool`                                                          | new                                             | `P10`        |
| `Text`, `SoftBreak`, `LineBreak`, `Code`, `HTML`, `Formula`, `Emphasis`, `Strong`, `Strikethrough` | as today                                                                                                          | unchanged                                       | —            |
| `Link`                                                                                             | `dest: Destination`, `title`, `content`                                                                           | changed                                         | `M1`         |
| `Image`                                                                                            | `dest: Destination`, `title`, `width: Int?`, `height: Int?`, `content`                                            | changed                                         | `M1`, `M7`   |
| `Directive`                                                                                        | `name`, `label`                                                                                                   | changed; attributes move to the inherited field | `M7`         |
| `CrossLink`                                                                                        | `embedded: Bool`, `dest: Destination`, `label: String?`                                                           | new                                             | `O1`         |
| `Mark`                                                                                             | `content`                                                                                                         | new                                             | `O2`         |
| `Comment`                                                                                          | `literal`                                                                                                         | new; block or inline by its parent edge         | `O3`         |
| `Cite`                                                                                             | `citations: [Citation]`                                                                                           | new                                             | `M4`         |
| `Insert`                                                                                           | `content`                                                                                                         | new                                             | `I1`         |
| `Span`                                                                                             | `content`                                                                                                         | new                                             | `P5`         |
| `Superscript`, `Subscript`                                                                         | `content`                                                                                                         | new                                             | `P6`         |
| `ExampleReference`                                                                                 | `label: String`                                                                                                   | new                                             | `P9b`        |
| `BlockQuote`                                                                                       | —                                                                                                                 | removed                                         | `M3`         |
| `ReferenceDefinition`, `LinkReference`, `ImageReference`                                           | —                                                                                                                 | removed                                         | `M2`         |
| `FootnoteDefinition`, `FootnoteReference`                                                          | —                                                                                                                 | removed                                         | `M4`         |

Every kind also carries the inherited fields `scope: Scope`,
`anchor: String?` (`M7`), and `attributes: Attributes` (`M7`). A traversed
value carries `scope` only.

### Values that are not nodes

| Value                                                                               | Item and first producer                                         |
| ----------------------------------------------------------------------------------- | --------------------------------------------------------------- |
| `Destination = url(String) \| cross(path: String, anchor: String?)`                 | `M1`; `cross` first produced by `O1`                            |
| `Attributes(classes: [String], records: [Record])`, `Record(name, value)`           | `M7`; populated from directive syntax in `M7`                   |
| `CitationReferent = bib(key, mode: BibMode) \| footnote(id)`, `BibMode`             | `M4`; `bib` first produced by `P7`                              |
| `Citation(referent, prefix: [Markup], suffix: [Markup], scope)`                     | `M4`; scoped and traversed, not `Markup` (`D3`)                 |
| `Footnote(id, content: [Markup], scope)`                                            | `M4`; document-owned, scoped and traversed, not `Markup` (`D3`) |
| `CalloutFold = none \| expanded \| collapsed`                                       | `M3`; `expanded` and `collapsed` first produced by `O8`         |
| `OrderedListStyle`, `OrderedListDelimiter`                                          | `M5`; values beyond the inherited forms first by `P9a`, `P9b`   |
| `TableColumn(alignment: TableAlignment, relative: Double?)`                         | `M6`; `relative` first produced by `P11c`                       |
| `Metadata`, `MetadataRecord`, `MetadataValue`, `MetadataScalar`, `MetadataListItem` | `M7`; first produced by `O6`                                    |
| `ReferenceForm`                                                                     | removed by `M2`                                                 |
| `DirectiveAttribute`                                                                | removed by `M7`                                                 |

### Parse options

Binding spelling is shown; the C facade uses `snake_case`, and the CLI `-e`
names and fixture fence tags use the extension names from the Pandoc and
Obsidian indexes. Every new option defaults to `false` except where `D5`
decides otherwise. An option is public from the item in its `Public from`
column; before that item it exists only as registry, CLI, and conformance
plumbing.

| Option                     | Behavior lands in | Public from |
| -------------------------- | ----------------- | ----------- |
| `obsidian`                 | `O1` through `O9` | `O10`       |
| `stripObsidianComments`    | `O3`              | `O10`       |
| `insertedText`             | `I1`              | `I1`        |
| `inlineCodeAttributes`     | `P2a`             | `P2a`       |
| `headerAttributes`         | `P2b`             | `P2b`       |
| `fencedCodeAttributes`     | `P2c`             | `P2c`       |
| `linkAttributes`           | `P2d`             | `P2d`       |
| `autoAnchors`              | `P3`              | `P3`        |
| `implicitHeaderReferences` | `P4`              | `P4`        |
| `bracketedSpans`           | `P5`              | `P5`        |
| `superscript`, `subscript` | `P6`              | `P6`        |
| `citations`                | `P7`              | `P7`        |
| `fencedDivs`               | `P8`              | `P8`        |
| `fancyLists`, `startnum`   | `P9a`             | `P9a`       |
| `exampleLists`             | `P9b`             | `P9b`       |
| `definitionLists`          | `P10`             | `P10`       |
| `tableCaptions`            | `P11a`            | `P11a`      |
| `simpleTables`             | `P11b`            | `P11b`      |
| `multilineTables`          | `P11c`            | `P11c`      |
| `gridTables`               | `P11d`            | `P11d`      |

## Stage 0 — groundwork

- [ ] **X0 — Option registry and harness plumbing.** Create one C-side option
      registry that maps a registered name to its facade field or engine
      extension bit, and route the CLI `-e` names, the `ts_ast_enable`
      fixture tags, and the facade-to-engine mapping through it, so a feature
      item adds exactly one row. Make
      `scripts/check-canonical-ast-fixtures.mjs` read the option vocabulary
      from the `ParseOptions` table in `canonical-ast.md` instead of a second
      hardcoded list, and give the C, Swift, Kotlin, and ES conformance
      runners an internal option path so a canonical case can enable a
      registered option that its public surface has not yet published. No
      public option is added here: the inventory allocates the names, and
      each feature item publishes its own option together with its behavior.
      Exit: every existing fixture and canonical case is byte-identical, and
      the registry, CLI, fixture tags, and checker agree on the nine existing
      names.
- [ ] **P0 — Pandoc oracle gate.** Add `oracle-pandoc` to
      `scripts/init-environment.sh`: `--install` fetches only the host
      archive named by `specs/oracles/pandoc/source.json` and verifies its
      SHA-256, and `--check` accepts only the exact 3.11 runner. Add one
      adapter that passes each `corpus.json` case's exact `from` string to
      the CLI with an empty data directory and requests JSON, with canaries
      for the version prefix, the `[1, 23, 1, 2]` API envelope, extension
      enable and disable behavior, UTF-8 input, and user-data isolation.
      Define one semantic projection per target concept, register all 25
      cases as gaps in a fail-closed `specs/oracles/pandoc/deltas.json`
      carrying both digests and the closing item, and wire
      `check:pandoc-parity` into `check:oracle-parity`, the External parity
      CI job, and `scripts/audit-test-topology.sh`. Normal build and test
      commands still perform no network access. The adapter enables each
      case's options that the registry already knows and records the rest
      as gaps. Requires `X0`.
- [ ] **I0 — Inserted-text oracle gate.** Pin `markdown-it@13.0.2` and
      `markdown-it-ins@4.0.0` as exact development dependencies with the
      integrity values recorded in `docs/specs/inserted-text.md`; add
      `specs/oracles/markdown-it-ins/` with a README, an input-only corpus
      replaying the pinned upstream cases plus the contract's composition
      cases, and a fail-closed `deltas.json`; add `check:ins-parity`, which
      compares `ins_open` and `ins_close` placement and nesting to `Insert`
      after a canary requiring exactly one pair for `++inserted++`, to
      `check:oracle-parity`, CI, and the topology audit. Every case is a
      registered gap until `I1`.

## Stage 1 — shared model, one consumer fact per pull request

- [ ] **M1 — `Destination` on `Link` and `Image`.** Add the tagged
      `Destination` value with both branches and replace `Link.destination`
      and `Image.source` with `dest`; only `url` is produced until `O1`. New
      facade accessors reporting the branch and its strings replace
      `markdown_core_node_link_properties` and
      `markdown_core_node_image_properties`; the dump prints the value per
      `D2`; the cmark, cmark-gfm, remark, and Obsidian projections read the
      real tagged value instead of wrapping a string. Manifest states:
      `destination.url.empty`, `destination.url.value`. Requires `D1`, `D2`.
- [ ] **M2 — Resolved reference links and images.** Resolve every successful
      full, collapsed, shortcut, and autolink form to `Link(dest=url(...))` and
      every reference image to `Image` inside the existing parser-owned lookup,
      and remove `LinkReference`, `ImageReference`, `ReferenceDefinition`, and
      `ReferenceForm` from every surface with their facade accessors;
      `markdown_core_node_association` narrows to the footnote kinds until `M4`.
      A resolved occurrence keeps its own scope and never acquires the
      definition's range; unresolved calls and invalid definitions keep the
      inherited literal fallback. Update the cmark, cmark-gfm, and remark
      projections (remark resolves mdast definitions inside the projection),
      rewrite `scripts/audit-reference-order-independence.mjs` and
      `specs/reference-resolution/` to compare resolved links, and re-derive
      `pathological_reference_expansion_bound` under `D9` together with its
      transport and decoder counterparts on every surface. Manifest: the
      `reference.form.*` states are replaced by a case proving that a direct and
      a reference occurrence dump identically apart from scope. Requires `M1`,
      `D8`, `D9`.
- [ ] **M3 — `Callout` replaces `BlockQuote`.** Rename the kind in every
      profile, add `CalloutFold`, and give every `>` container `variant=null`,
      `fold=none`, and `title=null` with unchanged content and scope; no alias
      or wrapper survives. The C node type, facade accessor, dump, bindings,
      walkers, and every fixture containing a quote regenerate. The Obsidian
      `universal-callout-container` delta stays as the general projection of
      mdast `blockquote`. Manifest states: `callout.variant.null`,
      `callout.fold.none`, `callout.title.null`. Requires `D1`, `D8`.
- [ ] **M4 — Citation and footnote model.** Replace `FootnoteReference` and
      `FootnoteDefinition` with inline `Cite(citations)`, the scoped
      `Citation(referent, prefix, suffix)` value, the complete
      `CitationReferent` union with `BibMode`, the document-owned
      `Footnote(id, content)` value, and `Document.footnotes` visited after
      `content` and ordered by scope start; `Citation` and `Footnote` are
      traversed values outside the `Markup` union and receive the value
      callbacks, dump form, and wire records that `D3` decides. Inherited
      `[^label]` calls lower to a one-item `Cite` with empty
      affixes whose ID is the normalized label without the caret; repeated
      calls share one `Footnote`; a valid unreferenced definition remains a
      `Footnote`; a losing duplicate keeps the inherited content fallback.
      Remove `markdown_core_node_association` and add cite, citation, and
      footnote accessors; update the remark projection; only the `footnote`
      branch is produced until `P7`. Manifest states and orders:
      `citation.referent.footnote`, `citation.affix.empty`,
      `document.footnotes.empty`, `document.footnotes.populated`,
      `document.content-before-footnotes`, `cite.items-in-order`. Requires
      `M2`, `D2`, `D3`.
- [ ] **M5 — List and item facts.** Replace `ListItem.checked` with `marker:
      String?` (`" "`, `"x"`, and `"X"` under the inherited task-list rule,
      `null` otherwise) and expose `isTask` and `isComplete` only as derived
      binding conveniences; add `List.style` and `List.delimiter`, populated as
      `decimal` with `period` or `oneParen` for inherited ordered lists and
      `null` for bullets; add `ListItem.exampleLabel` as `null`. Replace the
      task-list and list facade accessors, update the cmark-gfm and remark
      projections, and regenerate the list fixtures. Manifest states:
      `listItem.marker.null`, `listItem.marker.space`, `listItem.marker.value`,
      `list.style.decimal`, `list.style.null`, `list.delimiter.period`,
      `list.delimiter.oneParen`, `listItem.exampleLabel.null`. Requires `D1`.
- [ ] **M6 — One table model.** Emit `Table(columns, head, content, foot=[])`
      with `TableColumn(alignment, relative=null)` from the existing pipe-table
      path, remove `TableRow.isHeader`, add `TableCell.rowspan` and `colspan` as
      `1`, and make cell content block content by normalizing each inherited
      inline cell to one `Paragraph` (`D4`). Replace the table facade accessors,
      update the cmark-gfm and remark projections to wrap phrasing content, keep
      the empty-cell positions from #191 exact in the ledgers, and regenerate
      every table fixture. `Table.caption` is not added here: a typed field
      cannot precede its kind, and the `TableCaption` kind cannot precede a
      producer, so `P11a` adds the field and the kind together. Manifest states
      and orders: `table.column.relative.null`, `tableCell.span.one`,
      `table.head-content-foot`. Requires `D1`, `D2`, `D4`.
- [ ] **M7 — Universal fields and the one attribute operation.** Add the
      inherited `anchor: String?` and `attributes: Attributes` to every kind by
      turning the contract's single inherited field into an ordered set that the
      projection audit, the fixture checker, and the dump grammar understand;
      add `Document.metadata: Metadata?` with the metadata value types; add
      `Image.width` and `Image.height` as `null`. Add facade accessors for the
      anchor, classes, records, metadata records, and dimensions, and carry the
      values through the JNI and Wasm transports. Implement the shared Pandoc
      3.11 braced-attribute scanner and normalization once in the C core (the
      last ID wins and an empty final `id=` clears it; `.class` and `class=`
      words append; `-` appends `unnumbered`; every other assignment appends a
      `Record` in order, duplicates included), and make directives its first
      attachment site: delete the directive-only tokenizer, the
      `[DirectiveAttribute]?` fields, the `DirectiveAttribute` value, the
      directive-specific facade and extension accessors, and the
      absent-versus-empty distinction, so inline, leaf, and container directives
      populate `anchor` and `attributes` through the shared operation alone and
      `{}` attaches as empty. Register every resulting remark grammar difference
      (bare names, empty assignments, entity decoding, `-`, duplicate names) in
      `specs/oracles/remark/deltas.json`, make the remark projection compare the
      universal fields, rewrite the directive fixtures, retarget the
      `directive.attributes.*` manifest states to `markup.anchor.value`,
      `markup.attributes.classes`, `markup.attributes.records`,
      `markup.attributes.source-order`, and `escaping.attribute-value`, point
      the `canonical-ast.md` attribute section at `attributes.md` and the Remark
      attachment module, and regenerate every golden once. The Obsidian gate
      reads `metadata` from the dump's compact JSON field. Manifest states:
      `markup.anchor.null`, `markup.attributes.empty`, `document.metadata.null`,
      `image.dimensions.null`. Exit: the attributes and Remark attribute
      conformance cases pass, no second attribute tokenizer remains, and
      size-doubling valid, duplicate, malformed, and unclosed containers are
      linear. Requires `M1` through `M6`, `D2`.
- **Stage 1 exit criterion**, verified in the `M7` pull request: every surface
  compiles with exhaustive handling of the inventory kinds that exist so far,
  the projection audit proves kind and field parity, no fixture or document
  names a removed kind or field, no kind stores one semantic fact in two fields,
  and equivalent direct and reference links and images have identical semantic
  shapes.

## Stage 2 — Obsidian track

- [ ] **O1 — Wikilinks and embeds.** Create the parser-owned OFM inline
      extension, its bit, and its reviewed attach-table position (before
      `table`; the extension must see `[` and `!` before inherited bracket
      handling), enabled by the `obsidian` switch this item introduces per `D5`
      as internal test plumbing: an engine-level switch reached by the CLI
      `--profile obsidian` composition, the fixture runners, and the conformance
      runners' internal option path, with the facade field and public binding
      option arriving in `O10`. One scanner recognizes `[[...]]` and `![[...]]`,
      splits path, optional anchor, and label while scanning, removes the `#`
      and `#^` punctuation, and builds one `CrossLink(embedded, dest=cross(path,
      anchor), label)` whose `label` is null only when no `|` was authored. Add
      the `CrossLink` kind on every surface as the first producer of
      `Destination.cross`, fixtures for the module's seven table rows, every
      malformed boundary, opaque contexts, scopes, allocation failure, and
      size-doubling `!`, `[`, `]`, `#`, `^`, and `|` runs, and a canonical case;
      remove the four `wikilink-*` gaps and keep the label-nullability and
      destination projections as general deltas with canaries. Requires `X0`,
      `M7`, `D5`, `D8`.
- [ ] **O2 — Highlights.** Add `==` to the shared delimiter stack under
      `obsidian` with the exact-two-run and non-empty rules, local pairing, and
      opaque code, comment, and HTML-token bytes; add the `Mark(content)` kind,
      fixtures for formatted, adjacent, escaped, unmatched, triple, table-cell,
      and footnote-content bodies plus size-doubling equals runs, and a
      canonical case; remove the `highlight` gap and keep the content-model
      projection. Callout-title cases join in `O8`. Requires `O1`, `D8`.
- [ ] **O3 — Comments.** Scan `%%...%%` from the shared cursor with a linear
      closer search, classify block placement when both delimiters occupy their
      own lines and inline placement otherwise, keep the body opaque, and emit
      `Comment(literal)` when `stripObsidianComments` (internal until `O10`) is
      off or omit the node without changing surrounding delimiter binding when
      it is on. Add the kind, fixtures in both modes covering inline,
      standalone, multiline, empty, adjacent, escaped, unmatched, and
      Markdown-looking bodies, the syntax of every already merged extension
      inside a comment body under the opacity rule, and size-doubling percent
      runs, and a canonical case; remove the two `comment-*` gaps. Requires
      `O1`, `D5`, `D8`.
- [ ] **O4 — Inline footnotes.** Recognize `^[content]` inside the shared
      bracket algorithm, ahead of superscript, producing one one-item `Cite`
      with a `footnote` referent and one document-owned `Footnote` whose body is
      normalized to a paragraph; assign `inline-N` IDs after every authored ID,
      de-collide with `-K`, and merge referenced and inline values in source
      order inside the one document footnote operation. The oracle is silent
      here, so product fixtures own escaped brackets, empty and unclosed forms,
      unresolved calls, nested citations, semantic cycles, deterministic IDs and
      visitation order, allocation failure, and adversarial `^`, `[`, and `]`
      runs. Requires `O1`.
- [ ] **O5 — Task markers.** Generalize the task-list scanner under `obsidian`
      from `[ xX]` to exactly one Unicode scalar followed by a structural
      separator, decoding at most the candidate marker; the GFM profiles keep
      the inherited rule. Fixtures cover the module's marker table, ordered and
      nested lists, tabs and newlines as separators, empty and multi-scalar
      markers, missing separators, scopes, and long malformed bracket runs;
      remove the `custom-task-character` gap. Requires `O1`.
- [ ] **O6 — Properties.** Recognize at most one exact `---` envelope at the
      beginning of the decoded document after an optional BOM, scan it
      transactionally, decode the payload once as a YAML 1.2.2 document with
      JSON scalar resolution and a plain-string fallback, and project one root
      mapping into ordered `Metadata` records with textual key names, exact
      number lexemes, text and number lists, aliases resolved acyclically within
      budget, the allowed standard tags, and JSON root objects. Reject nested
      values, duplicates after decoding, stream indicators including `...`, and
      every other unsupported form by returning all bytes to inherited parsing;
      record `Metadata.scope` and each record scope; parse the body once after
      the closing fence. This is the largest C component of the track and still
      lands as one item, because the envelope scan, the YAML decoding, and the
      projection are one decoding operation; no partial decoder merges. Fixtures
      own the module's profile cases and the shared metadata cases; remove the
      eight `properties-*` gaps, whose oracle projection now reads the real
      field. Requires `O1`.
- [ ] **O7 — Block identifiers.** Attach `^block-id` during block finalization
      through one operation for paragraph suffixes, structured-block follower
      lines with the required blank-line boundaries, and list-item suffixes,
      writing the identifier into the owner's inherited `anchor` and removing it
      from visible content with no repair pass or offset side table; a second
      candidate on one owner is ordinary content, and the owner's scope still
      covers the identifier while child scopes end before it. Fixtures cover
      every placement, metadata-free callouts, nested lists, invalid characters,
      missing separation, duplicates, end of document, scopes, allocation
      failure, and long candidate sequences. Identifier-like bytes inside a
      crosslink source form belong to this item because it requires `O1`. Two
      cross-item cases are owned by whichever item merges later: the
      cross-profile reservation case of the anchor contract with `P3` and an
      identifier attached to a metadata-bearing callout with `O8`. Requires
      `O1`.
- [ ] **O8 — Callout metadata.** Evaluate `[!type]`, the optional `+` or `-`
      fold marker, and the inline title on the first content line of every `>`
      container inside the existing block algorithm, lowercase the type into
      `variant`, remove the metadata line from content before body blocks
      finalize, keep unknown and custom types, leave invalid or misplaced
      markers as content, and nest through the inherited container recursion.
      Add a title accessor and its dump group per `D2`, fixtures for the
      module's table plus formatted titles, every built-in alias, nested
      combinations, lazy continuation, scopes, allocation failure, and
      adversarial depth, and canonical cases for `callout.variant.value`,
      `callout.fold.expanded`, `callout.fold.collapsed`, and
      `callout.title.populated`. An identifier attached to a metadata-bearing
      callout is a cross-item case owned by whichever of `O8` and `O7` merges
      later. Requires `O1`, `O2`, `D2`.
- [ ] **O9 — Image dimensions and table integration.** Under `obsidian`, parse
      the complete `W`, `WxH`, `alt|W`, and `alt|WxH` alt-label suffixes in the
      shared image construction path into `width` and `height`, keep the whole
      label as alt content on any malformed suffix, and leave `CrossLink.label`
      raw. Teach the table boundary scanner the `\|` escape so a wikilink alias
      or embed size stays inside one cell while the inline scanner receives the
      logical pipe, and apply the module's delimiter-cell rule under `obsidian`
      only; the GFM profiles keep their current tables and task items byte for
      byte. Fixtures cover every valid and invalid dimension form, escaped
      pipes, aligned and pipe-optional tables, and formatted cells. Requires
      `O1`.
- [ ] **O10 — Obsidian evidence closure.** Add the integration fixtures for
      every pairwise opaque-context interaction, OFM and CommonMark constructs
      between paired inline HTML tags, the five-step precedence order, task
      items carrying block identifiers, `mermaid` and `query` blocks, and
      inline and display math; add canonical cases until every OFM kind,
      state, and order is covered; add deterministic fuzz seeds and
      pathological cases for delimiter runs, nested callouts, inline-HTML
      boundaries, escaped table pipes, long paths and headings, and repeated
      identifiers with structural bounds; audit every inline extension caller
      and delete obsolete skip tables and repair paths; empty `baselineGaps`;
      mark every support-audit row present; publish `obsidian` and
      `stripObsidianComments` in every binding's `ParseOptions`, the facade,
      and the CLI help, and document them in the README and the binding
      READMEs. Requires `O1` through `O9`.
- **Obsidian track exit criterion**, verified in the `O10` pull request: the
  plan exit criterion of the Obsidian implementation plan holds on every public
  surface, with the profile enabled by one composed switch and comment retention
  as its only independent option.

## Stage 3 — inserted-text track

- [ ] **I1 — Inserted text.** Implement `insertedText`: tokenize each plus run
      once into two-character units with the odd-run literal rule, apply the
      flanking rules without the rule of three, push eligible units onto the
      shared delimiter stack, nest rather than merge repeated units, and
      normalize an odd closer's spare `+` after its closing units; escapes,
      code, comments, and HTML-token bytes are opaque and paired tags create no
      region. Add the `Insert(content)` kind, fixtures replaying the pinned
      upstream cases plus the contract's crossed-delimiter, `CrossLink`, `Mark`,
      `Cite`, nesting-limit, allocation-failure, and size-doubling cases, and a
      canonical case; remove every `I0` gap. The `CrossLink` and `Mark`
      composition cases are cross-item cases owned by whichever of `I1` and `O1`
      or `O2` merges later, the `Cite` composition case belongs to `I1` because
      it reaches the citation model through `M7`, and `Comment` opacity follows
      the opacity rule with `O3`. Requires `X0`, `I0`, `M7`, `D8`.

## Stage 4 — Pandoc track

- [ ] **P2a — `inline_code_attributes`.** Attach a container that begins
      immediately after a complete closing backtick run to the `Code` node,
      excluding it from `literal` and including it in scope; whitespace prevents
      attachment and a malformed suffix leaves the code span unchanged. Fixtures
      and a canonical case; remove the `inline-code-attributes` gap. An explicit
      ID from this syntax reserved before heading synthesis is a cross-item case
      owned by whichever of `P2a` and `P3` merges later. Requires `P0`, `M7`.
- [ ] **P2b — `header_attributes`.** Attach a trailing container on ATX and
      Setext headings, after optional closing hashes, removing it from content
      and including it in scope; an invalid suffix stays visible. Fixtures cover
      compact, spaced, Setext, and malformed forms; remove the
      `header-attributes` gap. Requires `P0`, `M7`.
- [ ] **P2c — `fenced_code_attributes`.** Accept a braced list in the opening
      info region of tilde and backtick fences, with a bare language word as
      first-class shorthand lowercased and mapped for `c++` and `objective-c`;
      `numberLines` and its relatives stay inert records; a malformed list
      attaches nothing and does not reinterpret the body or closing fence;
      option-off keeps the inherited info contract. Apply `D6` to `info`,
      `language`, and the first class. Remove the `fenced-code-attributes` gap.
      An explicit ID from this syntax reserved before heading synthesis is a
      cross-item case owned by whichever of `P2c` and `P3` merges later.
      Requires `P0`, `M7`, `D6`.
- [ ] **P2d — `link_attributes`.** Attach an immediate container after a direct
      link, image, resolved reference occurrence, or autolink. Implement the
      attributes contract's `merge(primary, inherited)` operation here with
      reference definitions as its first consumer: store a definition's
      container in the parser reference map and apply it on resolution without
      touching the occurrence scope; keep `width` and `height` unit strings as
      records. Audit every Link, Image, Heading, Code, CodeBlock, directive, and
      reference-definition caller and delete repair passes made obsolete by the
      shared operation. Two cross-item cases are owned by whichever item merges
      later: a link tail claiming the container ahead of a bracketed span with
      `P5`, and attributes authored on an implicit heading reference occurrence
      with `P4`. Remove the `link-and-image-attributes` and
      `pandoc-reference-attribute-merge` gaps. An explicit ID from this syntax
      reserved before heading synthesis is a cross-item case owned by whichever
      of `P2d` and `P3` merges later. Requires `P0`, `M7`.
- [ ] **P3 — `auto_anchors`.** Build one document anchor registry that reserves
      every explicit anchor from every enabled profile before synthesis, then
      generates GFM anchors in heading order from visible text (retaining link
      labels and code, dropping footnotes), emoji aliases from the `D7` table,
      Unicode lowercasing, whitespace to `-` without collapsing, and the
      permitted-scalar filter, falling back to `section` and uniquifying with
      the smallest free `-N`. A generated anchor has no scope. Fixtures cover
      the module's cases including large duplicate sets; remove the
      `gfm-auto-anchors` gap. Reserving an explicit anchor before synthesis is a
      cross-item case with every explicit-anchor producer that neither requires
      nor is required by this item, `O7`, `P2a`, `P2c`, `P2d`, `P5`, and `P8`,
      each owned by whichever merges later; the `P2b` heading and `M7` directive
      cases belong to this item. Requires `P2b`, `D7`.
- [ ] **P4 — `implicit_header_references`.** Register a virtual reference
      definition for every heading with a final anchor, keyed by the authored
      label source after removing heading syntax, closing hashes, and trailing
      attributes and applying inherited label normalization, targeting `#` plus
      the final anchor; explicit definitions win, the first of duplicate labels
      wins, and every reference spelling resolves to an ordinary `Link` through
      the `M2` resolver in one order-independent finalization shared with
      example labels. Attributes authored on such an occurrence are a cross-item
      case owned by whichever of `P4` and `P2d` merges later. Remove the
      `implicit-header-references` gap. Requires `P3`.
- [ ] **P5 — `bracketed_spans`.** Decide `[text]{...}` in the shared bracket
      stack: a valid link tail wins, a complete attribute container after the
      first balanced `]` produces `Span`, `{}` produces an empty-attribute
      `Span`, and an invalid container falls back without consuming the `{`. Add
      the kind, fixtures, and a canonical case; remove the six bracketed-span
      and attribute-grammar gaps. The `Span` containing a `Cite` case belongs to
      `P7`, and a link tail claiming the container ahead of a span is a
      cross-item case owned by whichever of `P5` and `P2d` merges later. An
      explicit ID from this syntax reserved before heading synthesis is a
      cross-item case owned by whichever of `P5` and `P3` merges later. Requires
      `P0`, `M7`, `D8`.
- [ ] **P6 — `superscript` and `subscript`.** Add the single `^` and `~`
      delimiters through the delimiter engine with unescaped-whitespace
      rejection, `\ ` to a no-break space, empty bodies, `^[` and `~~`
      precedence, and independent option gates. Add both kinds, fixtures, and
      canonical cases; remove the `superscript-and-subscript` and
      `empty-superscript-and-subscript` gaps. The `^[` precedence case is a
      cross-item case owned by whichever of `P6` and `O4` merges later. Requires
      `P0`, `M7`, `D8`.
- [ ] **P7 — `citations`.** Recognize bare and braced keys, bracketed groups
      with semicolon items and prefix, mode marker, key, and suffix scopes,
      author-in-text keys with an optional bracketed tail, `-@` for
      `suppressAuthor`, and locator braces retained as suffix text. A bracketed
      candidate followed by a direct-link destination or reference tail belongs
      to that link; one followed by an attribute container belongs to the outer
      `Span` only while `bracketedSpans` is enabled, and with `citations` alone
      the candidate remains a bracketed `Cite` and the container stays inherited
      text; a complete cite beats shortcut-reference lookup; and a label
      collected by an example list beats the same `@key` regardless of order.
      This is the first producer of `CitationReferent.bib`. The
      `reset-citation-positions` class is documented here, and its heading
      fixture is an ordinary `header_attributes` case in `P2b`. Remove the
      `bibliography-citations` gap. Requires `P5`, `P9b`.
- [ ] **P8 — `fenced_divs`.** Open a `Div` on a line of three or more colons
      followed by a braced list or one unbraced class word, close the innermost
      open `Div` on any attribute-free colon line, nest through the normal
      container stack, and record `closed=false` when the document ends first.
      Add the kind, fixtures, and a canonical case; remove the
      `fenced-divs-nested` gap. A definition body ending at an enclosing
      fenced-div close is a cross-item case owned by whichever of `P8` and `P10`
      merges later. An explicit ID from this syntax reserved before heading
      synthesis is a cross-item case owned by whichever of `P8` and `P3` merges
      later. Requires `P0`, `M7`, `D8`.
- [ ] **P9a — `fancy_lists` and `startnum`.** Generalize the ordered-marker
      operation for decimal, alphabetic, Roman, and `#` markers with period,
      one-paren, and two-paren delimiters, the capital-period two-space rule,
      `i` and `I` disambiguation, same-style continuation, a new list on a style
      or delimiter change, and the nested-start restriction; `startnum` stores
      the first marker's value and otherwise `start=1`. Remove the
      `fancy-list-and-startnum` gap. Requires `P0`, `M7`.
- [ ] **P9b — `example_lists`.** Add `(@)`, `(@label)`, `(N@)`, and `(N@label)`
      markers with `style=example`, a document-wide counter and label map as
      parser state, `ListItem.exampleLabel`, the `ExampleReference(label)` kind
      for later `(@label)` occurrences, the repeated-label and reset rules,
      four-space continuations, and counter overflow as a parse error. Fixtures
      and a canonical case; remove the `example-lists-and-reference` gap.
      Requires `P9a`, `D8`.
- [ ] **P10 — `definition_lists`.** Recognize a one-line term, an optional
      single blank line, and a first marker line by bounded non-consuming
      lookahead before paragraph fallback; feed each body's lines to the
      ordinary block parser in place; set `Definition.compact` from the term
      gap; append further bodies and definitions per the module's separator and
      boundary rules; and yield to a complete table candidate. Add
      `DefinitionList` and `Definition`, fixtures for every listed case, and
      canonical cases; remove the two `definition-list-*` gaps. Cross-item cases
      owned by whichever item merges later: caption precedence over term
      lookahead with `P11a` and again for each later table form with `P11b`,
      `P11c`, and `P11d`, and a body ending at an enclosing fenced-div close
      with `P8`. Requires `P0`, `M7`, `D2`, `D8`.
- [ ] **P11a — `table_captions`.** Claim a `Table:`, `table:`, or `:` paragraph
      immediately before or after a supported table for the nearest complete
      eligible table, preceding first, strip the marker into
      `TableCaption.content`, extend `Table.scope` over both, and leave the
      paragraph alone with the option off. Add `Table.caption: TableCaption?`
      and the `TableCaption` kind together on every surface, fixtures for
      before, after, both, multiline, and empty captions, and canonical cases
      for `table.caption.null` and `table.caption.populated`. Requires `P0`,
      `M7`, `D3`, `D8`.
- [ ] **P11b — `simple_tables`.** Establish column ranges from the dash
      separator line, derive alignment from header placement, accept the
      headerless closing-separator form, end at a blank line or closing
      separator, and refuse to steal a thematic break, Setext underline, or
      fence without a complete shape. `TableColumn.relative` stays `null` for
      simple tables because Pandoc's reader gives them default column widths;
      `P11c` is the field's first producer. Remove the
      `simple-table-with-caption` gap. Caption precedence over definition-term
      lookahead for this table form is a cross-item case owned by whichever of
      `P11b` and `P10` merges later. Requires `P11a`.
- [ ] **P11c — `multiline_tables`.** Recognize full-width and segmented dash
      boundaries, combine physical lines into logical rows separated by blank
      lines, populate `TableColumn.relative` from source widths, require the
      blank separator for a one-row table, and accept the headerless form.
      Remove the `multiline-table` gap. Caption precedence over definition-term
      lookahead for this table form is a cross-item case owned by whichever of
      `P11c` and `P10` merges later. Requires `P11b`.
- [ ] **P11d — `grid_tables`.** Parse `+`, `-`, `=`, and `|` boundaries with the
      top line defining columns, `=` separators selecting head and foot, cell
      bodies through the ordinary block parser, missing segments as `rowspan`
      and `colspan` stored once in the upper-left anchor row, a row-width
      occupancy array validating overlap, overrun, uncovered coordinates, and
      cross-group spans, and alignment colons and widths. Remove the
      `grid-table-block-cells` and `grid-table-row-and-column-spans` gaps. Grid
      widths populate `TableColumn.relative` through the producer `P11c`
      establishes. Caption precedence over definition-term lookahead for this
      table form is a cross-item case owned by whichever of `P11d` and `P10`
      merges later. Requires `P11c`.
- [ ] **P12 — Pandoc evidence closure.** Add option-independence fixtures
      for every extension on and off in combination, deterministic fuzz seeds
      and size-doubling cases for brackets, attributes, `@`, braces, carets,
      tildes, colons, numerals, and grids, and canonical cases until every
      Pandoc kind, state, and order is covered; empty the Pandoc
      `deltas.json` of everything except general documented projections with
      canaries; document every option in the README and the binding READMEs.
      Requires `P2a` through `P11d`.
- **Pandoc track exit criterion**, verified in the `P12` pull request: the Phase
  6 exit criterion of the Pandoc implementation plan holds, with every selected
  extension independently composable and no monolithic preset.

## Stage 5 — release

- [ ] **R1 — Release.** Execute `D1`: set `VERSION`, write
      `docs/releases/<VERSION>.md` listing the documented OFM subset and
      parser-only boundary, `Document.metadata`, resolved reference links and
      images, `BlockQuote` to `Callout`, the citation and footnote model,
      `checked` to `marker`, the unified table model, universal anchors and
      attributes, `Destination`, the exact Pandoc 3.11 pin and option names
      without a monolithic preset, and `insertedText`; move the CHANGELOG
      section; run `pnpm release:check-version`, `pnpm verify`, and the
      release dry run; tag. Requires `O10`, `I1`, `P12`.

## Dependency table

Sizes are rough review-effort estimates, not schedules.

| Item   | Requires               | Size | Cross-item cases                                                                                                              | Discharges                                                                                                                       |
| ------ | ---------------------- | ---- | ----------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `X0`   | —                      | M    | —                                                                                                                             | option registry serving both plans' option bullets                                                                               |
| `P0`   | `X0`                   | M    | —                                                                                                                             | Pandoc Phase 0; Pandoc Phase 1 gate activation                                                                                   |
| `I0`   | —                      | S    | —                                                                                                                             | inserted-text oracle setup                                                                                                       |
| `M1`   | `D1`, `D2`             | M    | —                                                                                                                             | Obsidian and Pandoc Phase 1 `Destination`                                                                                        |
| `M2`   | `M1`, `D8`, `D9`       | L    | —                                                                                                                             | Obsidian Phase 1 reference normalization; Phase 5 projections                                                                    |
| `M3`   | `D1`, `D8`             | M    | —                                                                                                                             | Obsidian Phase 1 `Callout`                                                                                                       |
| `M4`   | `M2`, `D2`, `D3`       | L    | —                                                                                                                             | Obsidian Phase 1 citation model; Pandoc Phase 1 bibliography branch                                                              |
| `M5`   | `D1`                   | M    | —                                                                                                                             | Obsidian Phase 1 `marker`; Pandoc Phase 1 list values                                                                            |
| `M6`   | `D1`, `D2`, `D4`       | L    | —                                                                                                                             | Pandoc Phase 1 table values                                                                                                      |
| `M7`   | `M1`–`M6`, `D2`        | XL   | —                                                                                                                             | Obsidian Phase 1 metadata, anchor, dimensions; Pandoc Phase 1 fields; Pandoc Phase 2 attribute operation and directive migration |
| `O1`   | `X0`, `M7`, `D5`, `D8` | M    | `Insert` containing `CrossLink` (`I1`)                                                                                        | Obsidian Phase 2 wikilinks                                                                                                       |
| `O2`   | `O1`, `D8`             | S    | `Insert` containing `Mark` (`I1`)                                                                                             | Obsidian Phase 2 highlights                                                                                                      |
| `O3`   | `O1`, `D5`, `D8`       | M    | —                                                                                                                             | Obsidian Phase 2 comments                                                                                                        |
| `O4`   | `O1`                   | M    | `^[` before superscript (`P6`)                                                                                                | Obsidian Phase 2 inline footnotes and resolution                                                                                 |
| `O5`   | `O1`                   | S    | —                                                                                                                             | Obsidian Phase 4 task markers                                                                                                    |
| `O6`   | `O1`                   | XL   | —                                                                                                                             | Obsidian Phase 3 Properties                                                                                                      |
| `O7`   | `O1`                   | M    | anchor reserved before synthesis (`P3`); identifier on a metadata-bearing callout (`O8`)                                      | Obsidian Phase 3 block identifiers                                                                                               |
| `O8`   | `O1`, `O2`, `D2`       | M    | identifier on a metadata-bearing callout (`O7`)                                                                               | Obsidian Phase 3 callouts                                                                                                        |
| `O9`   | `O1`                   | M    | —                                                                                                                             | Obsidian Phase 4 media parameters and tables                                                                                     |
| `O10`  | `O1`–`O9`              | M    | —                                                                                                                             | Obsidian Phase 1 preset publication; Phase 2 caller audit; Phase 5; plan exit criterion                                          |
| `I1`   | `X0`, `I0`, `M7`, `D8` | S    | `Insert` containing `CrossLink`, `Mark` (`O1`, `O2`)                                                                          | inserted-text contract                                                                                                           |
| `P2a`  | `P0`, `M7`             | S    | anchor reserved before synthesis (`P3`)                                                                                       | Pandoc Phase 2 attachment sites                                                                                                  |
| `P2b`  | `P0`, `M7`             | S    | —                                                                                                                             | Pandoc Phase 2 attachment sites                                                                                                  |
| `P2c`  | `P0`, `M7`, `D6`       | M    | anchor reserved before synthesis (`P3`)                                                                                       | Pandoc Phase 2 attachment sites                                                                                                  |
| `P2d`  | `P0`, `M7`             | M    | link tail ahead of a span (`P5`); attributes on an implicit heading reference (`P4`); anchor reserved before synthesis (`P3`) | Pandoc Phase 2 attachment, merge, and caller audit                                                                               |
| `P3`   | `P2b`, `D7`            | M    | anchor reserved before synthesis (`O7`, `P2a`, `P2c`, `P2d`, `P5`, `P8`)                                                      | Pandoc Phase 2 heading registry                                                                                                  |
| `P4`   | `P3`                   | M    | attributes on an implicit heading reference (`P2d`)                                                                           | Pandoc Phase 3 document resolution                                                                                               |
| `P5`   | `P0`, `M7`, `D8`       | M    | link tail ahead of a span (`P2d`); anchor reserved before synthesis (`P3`)                                                    | Pandoc Phase 3 spans                                                                                                             |
| `P6`   | `P0`, `M7`, `D8`       | S    | `^[` before superscript (`O4`)                                                                                                | Pandoc Phase 3 superscript and subscript                                                                                         |
| `P7`   | `P5`, `P9b`            | L    | —                                                                                                                             | Pandoc Phase 3 citations and resolution                                                                                          |
| `P8`   | `P0`, `M7`, `D8`       | M    | definition body ends at a div close (`P10`); anchor reserved before synthesis (`P3`)                                          | Pandoc Phase 4 fenced divs                                                                                                       |
| `P9a`  | `P0`, `M7`             | M    | —                                                                                                                             | Pandoc Phase 4 ordered markers                                                                                                   |
| `P9b`  | `P9a`, `D8`            | M    | —                                                                                                                             | Pandoc Phase 4 example lists                                                                                                     |
| `P10`  | `P0`, `M7`, `D2`, `D8` | L    | caption precedence over term lookahead (`P11a`, `P11b`, `P11c`, `P11d`); definition body ends at a div close (`P8`)           | Pandoc Phase 4 definition lists                                                                                                  |
| `P11a` | `P0`, `M7`, `D3`, `D8` | M    | caption precedence over term lookahead (`P10`)                                                                                | Pandoc Phase 5 captions                                                                                                          |
| `P11b` | `P11a`                 | M    | caption precedence over term lookahead (`P10`)                                                                                | Pandoc Phase 5 simple tables and precedence                                                                                      |
| `P11c` | `P11b`                 | M    | caption precedence over term lookahead (`P10`)                                                                                | Pandoc Phase 5 multiline tables                                                                                                  |
| `P11d` | `P11c`                 | XL   | caption precedence over term lookahead (`P10`)                                                                                | Pandoc Phase 5 grid tables                                                                                                       |
| `P12`  | `P2a`–`P11d`           | M    | —                                                                                                                             | Pandoc Phase 6; plan exit criterion                                                                                              |
| `R1`   | `O10`, `I1`, `P12`     | S    | —                                                                                                                             | both delivery sequences                                                                                                          |

## Working in parallel

- `M1` through `M7` are one open pull request at a time; each regenerates
  goldens that the next one rewrites again.
- `P0` and `I0` touch only scripts and oracle policy and may land at any point
  before the first Pandoc feature item and before `I1`; their registered
  digests are re-registered by whichever model item changes them.
- After `M7`, the Obsidian, inserted-text, and Pandoc tracks are independent.
  Inside a track, items that edit the same engine file are serialized or
  rebased in order: `O2` through `O5` extend the extension `O1` creates; `O6`,
  `O7`, and `O8` edit block finalization; `O9`, `P11b`, `P11c`, and `P11d`
  edit the table path; `P2a` through `P2d` share the attribute callers; `P5`,
  `P6`, and `P7` share the inline bracket and delimiter code; `P8` and `P10`
  share block starts.
- The `Cross-item cases` column, together with the opacity rule, is the complete
  list of fixtures that wait for a second item neither of whose items requires
  the other: `Insert` composed with `CrossLink` and `Mark`; `^[` before
  superscript; an explicit anchor reserved before synthesis, once per producer;
  an identifier on a metadata-bearing callout; a link tail claiming a container
  ahead of a span; attributes on an implicit heading reference; a definition
  body ending at a fenced-div close; caption precedence over definition-term
  lookahead, once per table form; and every comment opacity case. Each is
  written by the later of its two items, and no item's `Requires` grows because
  of it.
