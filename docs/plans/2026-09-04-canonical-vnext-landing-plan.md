# Canonical vNext landing plan

Status: in progress. This plan turns the feature contracts merged in #192, #193,
#194, and #196 into an ordered list of pull requests that can each merge alone.
It owns the cross-track landing order, the per-pull-request definition of done,
and the target inventory of kinds and values. It does not restate
grammars or proof obligations: the two implementation plans remain normative for
their phases and exit criteria, and every module specification remains normative
for its behavior.

- [Obsidian Flavored Markdown implementation plan](2026-09-02-obsidian-flavored-markdown.md)
- [Pandoc Markdown extensions implementation plan](2026-09-03-pandoc-markdown-extensions.md)
- [Markdown Core dialect](../specs/dialect.md) and its modules, the normative
  statement of every feature this plan lands, with the
  [conflicts register](../specs/dialect/conflicts.md) of open source
  collisions
- [Extension specification audit](2026-09-04-extension-spec-audit.md), the
  historical record whose findings the dialect modules resolved

Both plans freeze the public model in one phase and then add syntax. Landing
that literally would mean one pull request that touches every kind on every
surface, once per module set, while nothing else can merge. This plan instead
lands the shared model one consumer fact at a time and then lands each syntax
feature as one self-contained pull request, so the three tracks proceed in
parallel and every merge leaves `main` releasable.

## What the last seven commits changed

| Commit                                        | Kind   | Effect on this plan                                                                                                                                                                                             |
| --------------------------------------------- | ------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| #187 node-kind walking visitors               | landed | Every new kind adds entering and exiting callbacks to the Swift, Kotlin, and ES walkers; the public-surface audit derives the required count from `canonical-ast.json`.                                       |
| #190 release dry-run readiness                | landed | Every pull request must pass the credential-free `Release Dry Run - Ready` check, so an intermediate state that cannot build every artifact cannot merge.                                                       |
| #191 UTF-8 repair removal and table positions | landed | Valid UTF-8 is a caller precondition, so new scanners add no validation or repair path. The position ledgers are fail-closed ratchets that every parser change keeps exact.                                    |
| #192 extension module contracts               | specs  | The Obsidian module set, the Pandoc module set, the shared attributes, citation, and insertion contracts, Remark directive attachment, the Pandoc and Obsidian oracle pins, and the two implementation plans. |
| #193 anchors and destinations                 | specs  | The universal `Markup.anchor` field, the tagged `Destination` value on `Link`, `Media`, and `CrossLink`, and the simplified `Attributes` shape.                                                                 |
| #194 Obsidian Properties                      | specs  | `Document.metadata`, the shared metadata value model, the Properties envelope, and the `yaml@2.9.0` oracle.                                                                                                     |
| #196 Properties corrections                   | specs  | Textual mapping keys and tightened oracle canaries; the original null-root rejection is superseded by O6 member skipping.                                                                                                       |

The insertion contract is the one specification that no existing plan
sequences; it is landed here as its own track. The dialect rewrite of `S0`
replaced the specification files those commits added with
`docs/specs/dialect.md` and its modules; item identifiers, oracle gap names, and
the two implementation plans are unchanged.

## How to use this plan

- One checkbox below is one pull request. The specification item `S0` is the
  pull request that landed the dialect modules and this plan.
  Every pull request merges alone, leaves every existing
  fixture byte-identical unless the item says otherwise, and passes required CI
  including the release dry run. The three stage exit criteria are gates rather
  than pull requests: each is verified in the pull request of the last item of
  its stage, named beside it, and has nothing of its own to tick.
- Tick the item in its implementing pull request once its work and validation
  are complete, before merge. In that same pull request,
  tick the bullets it discharges in the owning implementation plan, flip the
  item's row of the feature table in
  [`docs/specs/dialect.md`](../specs/dialect.md) to `present`, and add the
  examples of the modules whose behavior the item lands to the package
  fixtures byte for byte; an item that lands part of a module adds the
  examples its behavior makes exact.
- The written order is the default order. Any order that respects the `Requires`
  column of the dependency table is valid: prerequisites must already be merged,
  and that column lists every direct merge
  prerequisite, an item's full requirement is the transitive closure of that
  column, and nothing outside that closure may block an item. Every feature
  track root requires `M7`, so every feature item may rely on the complete
  shared model. A conformance case that composes the syntax of two items is
  owned by the item that merges later. When one of the two items requires the
  other, directly or through the closure, the requiring item is that later item
  and owns the case without further notice; when neither requires the other, the
  case is listed in the `Cross-item cases` column of both items, so the earlier
  item neither waits for it nor claims it. Opacity is the one composition the
  column does not enumerate, and the opaque regions are the opacity list of the
  dialect index. Code spans, HTML tokens, and formula bodies exist before
  every item, so each item proves in its own pull request that its
  syntax stays literal inside all three. Comment bodies and wikilinks arrive
  with `O3` and `O1`, so an item's comment opacity case is owned by whichever of
  `O3` and that item merges later, and its wikilink opacity case by whichever of
  `O1` and that item merges later. The model items `M1` through `M7` are
  serialized because each regenerates shared goldens; the three feature tracks
  are independent of one another after `M7`.
- Item identifiers are stable. A registered oracle gap names the item that
  closes it: Pandoc and insertion gaps use these identifiers from the start,
  and the existing Obsidian entries, which name plan phases, are retargeted to
  identifiers by the item that closes them.
- Each item states its scope, its proof, and its `Requires`. The module
  specification named by the item owns the complete conformance-case list; the
  item repeats only what decides the landing boundary.

## Landing rules

- Shape before syntax. A change to the consumer model lands before the syntax
  that populates it, and each such change carries every surface at once:
  `docs/specs/canonical-ast.json`, `canonical-ast.md`, and
  `canonical-ast-dump.md`; the C engine, facade header, export allowlists, and
  dump; the Swift, Kotlin, and ES models, exhaustive visitors, walking visitors,
  and dumpers; the JNI, Kotlin/Native, and Wasm transports; and the shared
  canonical fixtures with their manifest vocabulary.
- A new kind lands together with the first syntax that produces it.
  `scripts/check-canonical-ast-fixtures.mjs` requires every declared kind to
  appear in a parseable canonical case, so a kind without a producer cannot pass
  the contract check. Field and enum changes on existing kinds are therefore the
  only model-only pull requests; they are the `M` items.
- A feature pull request is one feature's behavior: the C extension or
  scanner, its reviewed position in the attach-order table, the new kind if
  any, package fixtures for the module's required conformance cases, a
  canonical case for each new kind or state, and the removal of every oracle
  gap it closes. The feature is always on from the item that lands it; no
  surface gains a switch.
- Every feature name is allocated in the inventory. A feature item lands the
  behavior, its oracle evidence, and its product fixtures together, and
  registers in `specs/oracles/` each place its syntax deliberately leaves an
  oracle's language, keyed to the exact inputs or as a projection the
  comparison applies; the dialect has no switches, so nothing is published as
  an option, no item adds option-off cases, and no gate parses a part of the
  language.
- Nothing here ships before `R1`. Between merges, an unproduced enum branch,
  value type, or field is allowed only where an item says so, and the item that
  first produces it is named; a landed feature always recognizes its syntax.
  No item publishes two representations of one semantic fact, even between
  merges: the item that introduces a replacement removes what it replaces.
- Oracles are evidence, not targets. A gap entry records a feature this parser
  does not yet implement; when the item lands, the entry becomes either
  agreement or a documented projection with a canary where the specification
  chooses differently, and no item changes a rule to match an oracle. A model
  item that changes an input's Markdown Core digest re-registers that digest in
  the same pull request, and a feature item retires the gap entries it
  implements in the same pull request.

- Contract: JSON, prose, and dump grammar updated together; `pnpm
  audit:ast-projections`, `pnpm check:contracts`, and `pnpm audit:surface` pass.
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
  `packages/markdown-core/tests/CMakeLists.txt`;
  regenerated goldens reviewed together with the parser change; canonical
  cases with manifest coverage vocabulary and checker validators.
- Ledgers and gates: `specs/positions/`, `specs/reference-resolution/`, and
  every `specs/oracles/*/deltas.json` updated in the same change with the reason
  in the commit message; `pnpm check:oracle-parity` and the fuzz seeds pass.
- Documentation: the CHANGELOG entry under the unreleased version, the binding
  READMEs when a public surface changes, and the feature-table row in
  `docs/specs/dialect.md`.
- Cross-item cases: every case in the item's `Cross-item cases` column whose
  partner item has already merged is part of this item's fixtures.

## Ground rules

These questions came up while sequencing and are settled; the dialect index
restates them as its ground rules. Where a module says otherwise, the item that
lands the behavior amends that module in the same pull request.

- Upstream tools define which features exist, not how they behave here. The
  module specifications define a common-case feature set drawn from Obsidian and
  Pandoc that is self-consistent on its own terms, and the repository's
  conformance fixtures are the oracle of record. Pandoc, Obsidian,
  remark-obsidian, and markdown-it-ins are evidence: a difference from them is a
  registered delta, never a rule change, and no gate requires byte-for-byte
  agreement.
- `VERSION` stays `3.0.0`. No 3.0 release exists, so every item here is part of
  the unreleased 3.0.0 line and nothing is deferred to a later major.
- Nothing is frozen while 3.0.0 is unreleased. The C kind enum, the wire kinds,
  the JNI and Wasm payload layouts, and the manifest order may change with any
  item, so no ordinal or position is reserved in advance.
- The canonical dump prints every field as it is, in canonical field order, with
  no inherited-versus-authored distinction. An item that adds a field extends
  the dump grammar in the same pull request.
- `scope` records original source. Anything with source has a scope and nothing
  else does: `Citation`, `Footnote`, and `TableCaption` are scoped because they
  are written, and a generated anchor is not.
- `TableCell.content` is `[Markup]`, the most general content model. Inline
  content stays inline, a table form whose cells hold blocks stores the blocks
  directly, and no cell is normalized to a `Paragraph`.
- There are no profiles, no umbrella switch, and no per-feature switch: the
  dialect is one language in which every feature is always on, and `X0`
  deletes `ParseOptions` and smart punctuation. The test tree keeps no layer
  selection either: the oracle gates parse the one language and register
  where it deliberately leaves an oracle's.
- Runtime modules and extension descriptors follow feature semantics and
  ownership. GFM, Obsidian, and Pandoc identify sources and oracle evidence;
  they do not group unrelated features into implementation boundaries.
- A comment is a `Comment` node and is never stripped: an HTML comment under the
  inherited grammar, and a `%%` comment. `stripHTMLComments` is removed,
  nothing strips anything, and a consumer that wants comments gone drops the
  nodes.
- The parser stores fenced-code info, language, and attributes as written. It
  does not lowercase, alias, or derive a language from a class; consumers
  interpret them.
- There is no emoji support. Automatic anchors apply the same steps to every
  scalar, the GFM emoji alias step is not part of the heading-anchor contract,
  and no item adds an alias table.
- The reference expansion bound is an invariant, not a decision. `M2` shares
  each winning destination across its occurrences in the C tree and redesigns
  the transports and decoders so a distinct destination is materialized once on
  every surface.

## Target inventory

This is the target shape of the model. Nothing about its order is reserved:
while 3.0.0 is unreleased, any item may renumber the C enum, the wire kinds,
and the manifest order.

### Markup kinds

| Kind                                                                                               | Target fields in canonical order                                                                                  | Change                                           | Item         |
| -------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- | ------------------------------------------------ | ------------ |
| `Document`                                                                                         | `content`, `metadata: Metadata?`, `footnotes: [Footnote]`, `specimens: [Specimen]`                                                         | changed                                          | `M4`, `M7`   |
| `Callout`                                                                                          | `variant: String?`, `collapsed: Bool?`, `title: [Markup]?`, `content`                                            | replaces `BlockQuote`                            | `M3`         |
| `Paragraph`, `ThematicBreak`, `HTMLBlock`, `FormulaBlock`                                          | as today                                                                                                          | unchanged                                        | —            |
| `Heading`                                                                                          | `level`, `content`                                                                                                | unchanged; anchors use the inherited field       | —            |
| `List`                                                                                             | `flavor`, `start`, `variant: OrderedListVariant?`, `delimiter: OrderedListDelimiter?`, `tight`, `items`               | changed                                          | `M5`         |
| `ListItem`                                                                                         | `marker: String?`, `content`                                                             | changed; `checked` removed                       | `M5`         |
| `CodeBlock`                                                                                        | `info`, `language`, `literal`, `fenced`, `closed`                                                                 | unchanged; `info` and `language` stay as written | —            |
| `Table`                                                                                            | `caption: TableCaption?`, `columns: [TableColumn]`, `head: [TableRow]`, `content: [TableRow]`, `foot: [TableRow]` | changed; `caption` arrives with its kind         | `M6`, `P11a` |
| `TableRow`                                                                                         | `cells`                                                                                                           | changed; `isHeader` removed                      | `M6`         |
| `TableCell`                                                                                        | `rowspan: Int`, `colspan: Int`, `content: [Markup]`                                                               | changed; inline or block content                 | `M6`         |
| `TableCaption`                                                                                     | `content: [Markup]`                                                                                               | new; typed field of `Table`                      | `P11a`       |
| `DirectiveBlock`                                                                                   | `name: String?`, `label`, `content`                                                                               | changed; attributes move to the inherited field, `name` nullable for the nameless container | `M7`, `P8` |
| `DirectiveLabel`                                                                                   | `content`                                                                                                         | unchanged                                        | —            |
| `DefinitionList`                                                                                   | `definitions: [Definition]`                                                                                       | new                                              | `P10`        |
| `Definition`                                                                                       | `term: [Markup]`, `content: [[Markup]]`, `compact: Bool`                                                          | new                                              | `P10`        |
| `Text`, `SoftBreak`, `LineBreak`, `Code`, `HTML`, `Formula`, `Emphasis`, `Strong`, `Strikethrough` | as today                                                                                                          | unchanged                                        | —            |
| `Link`                                                                                             | `dest: Destination`, `title`, `content`                                                                           | changed                                          | `M1`         |
| `Media`                                                                                            | `dest: Destination`, `title`, `dimensions: Dimensions?`, `content`                                            | changed                                          | `M1`, `M7`, `O9`   |
| `Directive`                                                                                        | `name`, `label`                                                                                                   | changed; attributes move to the inherited field  | `M7`         |
| `CrossLink` | `dest: Destination`, `label: String?` | new | `O1` |
| `CrossEmbedded` | `dest: Destination`, `label: String?`, `dimensions: Dimensions?` | new; transclusion separated from CrossLink | `O9` |
| `Mark`                                                                                             | `content`                                                                                                         | new                                              | `O2`         |
| `Comment`                                                                                          | `literal`                                                                                                         | new; block or inline by its parent edge          | `M0`         |
| `Cite`                                                                                             | `citations: [Citation]`                                                                                           | new                                              | `M4`         |
| `Insertion`                                                                                           | `content`                                                                                                         | new                                              | `I1`         |
| `Span`                                                                                             | `content`                                                                                                         | new                                              | `P5`         |
| `Superscript`, `Subscript`                                                                         | `content`                                                                                                         | new                                              | `P6`         |
| `BlockQuote`                                                                                       | —                                                                                                                 | removed                                          | `M3`         |
| `ReferenceDefinition`, `LinkReference`, `ImageReference`                                           | —                                                                                                                 | removed                                          | `M2`         |
| `FootnoteDefinition`, `FootnoteReference`                                                          | —                                                                                                                 | removed                                          | `M4`         |

Every kind also carries the inherited fields `scope: Scope`,
`anchor: String?` (`M7`), and `attributes: Attributes` (`M7`). A traversed
value carries `scope` only.

### Values that are not nodes

| Value                                                                               | Item and first producer                                       |
| ----------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| `Destination = url(String) \| cross(path: String, anchor: String?)`                 | `M1`; `cross` first produced by `O1`                          |
| `Dimensions(width: Int, height: Int?)` | `O9`; node-independent, first held by `Media.dimensions` |
| `Attributes(classes: [String], records: [Record])`, `Record(name, value)`           | `M7`; populated from directive syntax in `M7`                 |
| `CitationReferent = bib(key, mode: BibMode) \| footnote(id)`, `BibMode`             | `M4`; `bib` first produced by `P7`                            |
| `Citation(referent, prefix: [Markup], suffix: [Markup], scope)`                     | `M4`; scoped and traversed, not `Markup`                      |
| `Footnote(id, content: [Markup], scope)`                                            | `M4`; document-owned, scoped and traversed, not `Markup`      |
| `OrderedListVariant`, `OrderedListDelimiter`                                          | `M5`; values beyond the inherited forms first by `P9a`, `P9b` |
| `TableColumn(alignment: TableAlignment, relative: Double?)`                         | `M6`; `relative` first produced by `P11c`                     |
| `Metadata`, `MetadataValue`, `MetadataScalar`, `MetadataListItem` | `M7`; first produced by `O6`                                  |
| `ReferenceForm`                                                                     | removed by `M2`                                               |
| `DirectiveAttribute`                                                                | removed by `M7`                                               |

### One language

The dialect has no parse options, and neither does its test tree. `X0`
deletes the public `ParseOptions` and every internal layer selection with it:
the parser attaches every extension on every parse; the package fixtures,
oracle gates, and position audits parse through the installed `markdown-core`
CLI or the same facade entry; and an oracle's authority is which inputs it
judges, never how they are parsed. Where the dialect deliberately leaves an
oracle's language, `specs/oracles/*/deltas.json` registers the difference
against its exact inputs or as a projection the comparison applies; where the
engine has not caught up with the dialect's own rules, the oracle's `backlog`
names the item that closes the gap and must keep diverging until it does.
Each feature item marks its feature-table row `present` and registers the
divergences its syntax creates; a feature is public from the item that lands
its behavior, with no separate publication step.

## Stage 0 — groundwork

- [x] **S0 — Dialect specification.** Replace the Obsidian, Pandoc, Remark, and
      shared-contract specifications with the Markdown Core dialect: the index
      `docs/specs/dialect.md` and one module per feature under
      `docs/specs/dialect/`, each stating its grammar, model, fallback, scopes,
      oracle, and required cases, with the recognition-order
      tables, opacity list, failure rule, limits, and Unicode rules in the index
      and the source collisions in `docs/specs/dialect/conflicts.md`; resolve
      every finding of the audit; record in the `ParseOptions` table of
      `canonical-ast.md` that `X0` removes it; and retarget the plans, the oracle
      policies, and the topology audit. Each module states its rules with
      examples in the CommonMark specification's format, an input, a `.` line,
      and the expected dump in the target grammar that `canonical-ast-dump.md`
      reserves, so the item that lands a module's behavior adds the module's
      examples to the package fixtures byte for byte as its first fixtures.
      Specification only; no engine change. This is the pull request that
      carries this plan: tick it, and the audit's checklist with it, in that
      pull request once the specification work and validation are complete,
      before merge.
- [x] **X0 — Remove the option surface and every layer selection.**
      Delete `ParseOptions` from the C facade, the installed CLI, and the
      Swift, Kotlin, and ES bindings, so `Document.parse(source)` is the only
      entry point on every surface and the installed CLI takes no `--profile`,
      `-e`, or `--smart`; remove smart punctuation from the product parse, the
      `--smart` mode and its substitutions, so quotation marks, hyphen runs,
      and periods are stored as written; and delete the `ParseOptions` table
      of `canonical-ast.md`. Delete every internal layer selection with them:
      the CLI's `--profile` and `-e` shorthands, `spec_runner --feature`, the
      feature tables behind them, and the `*-option-gates` fixtures, so that
      the engine configuration is written once, every package fixture, oracle
      gate, and position audit parses the one language through the installed
      CLI or the facade entry, and a fence tag only classifies an example for
      the oracle corpora. Register in `specs/oracles/` each place the dialect
      deliberately leaves an oracle's language: GFM autolink literals and the
      text directive against cmark, keyed to the specification inputs, and
      HTML-comment stripping against cmark and cmark-gfm as a projection until
      `M0`. Fix the two collisions that running the shipped language
      through the gates exposed: a colon that an address follows belongs to
      the autolink scanner before the text directive can claim it, as the
      recognition order (A3 before A10) and cmark-gfm say, and the formula
      scanner's `\\]` and `\\)` closers fall to the base language when
      nothing opened them. Make
      `scripts/check-canonical-ast-fixtures.mjs` stop reading option fields
      from the manifest and validate the coverage vocabulary alone, failing on
      a case that still names an option. Exit: every existing fixture and
      canonical case is byte-identical apart from the smart-punctuation
      substitutions and the examples whose dump the one language changes, each
      regenerated and reviewed; no surface and no test parses a part of the
      dialect; and the corpus and fuzz gates judge the language that ships.
      Requires `S0`, which states the switch-less dialect.
  Completion audit (2026-09-08): removed the remaining internal option word,
  constants, liberal-HTML branch and scanner, extension-subset test helpers,
  and manually configured fuzz entries. The engine transaction always attaches
  the complete dialect before optional test instrumentation. The attachment
  audit rejects option constants and any second complete-dialect attach site.

- [x] **P0 — Pandoc evidence gate.** Add `oracle-pandoc` to
      `scripts/init-environment.sh`: `--install` fetches only the host archive
      named by `specs/oracles/pandoc/source.json` and verifies its SHA-256, and
      `--check` accepts only the exact 3.11 runner. Add one adapter that passes
      each `corpus.json` case's exact `from` string to the CLI with an empty
      data directory and requests JSON, with canaries for the version prefix,
      the `[1, 23, 1, 2]` API envelope, extension enable and disable behavior,
      UTF-8 input, and user-data isolation. Define one semantic projection per
      target concept, compare all 25 cases and register only actual missing
      features in a fail-closed `specs/oracles/pandoc/deltas.json` carrying both
      digests and the implementing item, and wire `check:pandoc-parity` into
      `check:oracle-parity`, the External parity CI job, and
      `scripts/audit-test-topology.sh`. Normal build and test commands still
      perform no network access. The adapter runs each case through the one
      parser, which recognizes every feature it already implements, and records
      the rest as not yet implemented.
      The comparison covers only the declared intersection and is evidence: a
      case where the specification chooses differently becomes a documented
      projection with a canary when its item lands, and no item changes a rule
      to match Pandoc. Requires `X0`, `S0`.
- [x] **I0 — Insertion oracle gate.** Pin `markdown-it@14.2.0` and
      `markdown-it-ins@4.0.0` as exact development dependencies with the
      integrity values recorded in `docs/specs/dialect/insertion.md`; add
      `specs/oracles/markdown-it-ins/` with a README, an input-only corpus
      replaying the pinned upstream cases plus the contract's composition cases,
      and a fail-closed `deltas.json`; add `check:ins-parity`, which compares
      `ins_open` and `ins_close` placement and nesting to `Insertion` after a
      canary requiring exactly one pair for `++inserted++`, to
      `check:oracle-parity`, CI, and the topology audit. Every case is a
      registered gap until `I1`. Requires `S0`, which creates the module that
      records the pins.

## Stage 1 — shared model, one consumer fact per pull request

- [x] **M0 — `Comment` replaces HTML comment nodes.** Add the `Comment(literal)`
      kind on every surface as the node for an inline HTML comment token and for
      an HTML block that opens with `<!--` and whose end line holds only
      whitespace after the first `-->`; `literal` is the bytes between `<!--`
      and `-->`, empty for `<!-->` and `<!--->`, and every other HTML block
      stays `HTMLBlock` as written. Remove `stripHTMLComments` wherever `X0`
      has not already removed it: the C option bit, the CLI flag, the facade
      field, and every binding option, so no
      comment is ever stripped and a consumer drops `Comment` nodes instead.
      Regenerate every fixture containing an HTML comment, add a canonical case,
      and delete the `stripHTMLComments` row of the `ParseOptions` table, if
      `X0` has not yet deleted the table, and amend the
      `HTML` and `HTMLBlock` rows of `canonical-ast.md`. Give a block comment
      the scope of its opener line through its closer line; the current HTML
      block position ends one line early for the comment form, as the comments
      module's example shows. Give an ATX heading the scope of its whole line,
      closing sequence included, as the base module's example shows; the
      current parser ends it at the content. Manifest states: `comment.placement.block`,
      `comment.placement.inline`. Requires `S0`.
- [x] **M1 — `Destination` on `Link` and `Media`.** Add the tagged `Destination`
      value with both branches and replace `Link.destination` and `Image.source`
      with `dest`; only `url` is produced until `O1`. New facade accessors
      reporting the branch and its strings replace
      `markdown_core_node_link_properties` and
      `markdown_core_node_image_properties`; the dump prints the value as is;
      the cmark, cmark-gfm, remark, and Obsidian projections read the real
      tagged value instead of wrapping a string. Manifest states:
      `destination.url.empty`, `destination.url.value`. Requires `S0`.
- [x] **M2 — Resolved reference links and images.** Resolve every successful
      full, collapsed, shortcut, and autolink form to `Link(dest=url(...))` and
      every reference image to `Media` inside the existing parser-owned lookup,
      and remove `LinkReference`, `ImageReference`, `ReferenceDefinition`, and
      `ReferenceForm` from every surface with their facade accessors;
      `markdown_core_node_association` narrows to the footnote kinds until `M4`.
      A resolved occurrence keeps its own scope and never acquires the
      definition's range; unresolved calls and invalid definitions keep the
      inherited literal fallback. Update the cmark, cmark-gfm, and remark
      projections (remark resolves mdast definitions inside the projection),
      rewrite `scripts/audit-reference-order-independence.mjs` and
      `specs/reference-resolution/` to compare resolved links, share each
      winning resource, destination and title together, across its occurrences
      in the C tree, redesign the JNI and Wasm transports and every decoder so a
      distinct resource is materialized once per surface, and re-derive
      `pathological_reference_expansion_bound` together with its transport and
      decoder counterparts on every surface around resource identity, so a long
      destination or title referenced many times is stored once and the bound
      holds on every surface. Manifest: the `reference.form.*` states are
      replaced by a case proving that a direct and a reference occurrence dump
      identically apart from scope. Requires `M1`.
- [x] **M3 — `Callout` replaces `BlockQuote`.** Rename the kind everywhere,
      expose `variant`, `collapsed`, and `title` on every surface, and give
      every `>` container `variant=null`, `collapsed=null`, and `title=null`
      with unchanged content and scope; no alias or wrapper survives. A present
      title holds at least one node, so its first node is its presence on the
      C facade and its count is its presence on the wire. The C node type,
      facade accessor, dump, bindings, walkers, and every fixture containing a
      quote regenerate. The Obsidian `universal-callout-container` delta stays
      as the general projection of mdast `blockquote`. Manifest states:
      `callout.variant.null`, `callout.collapsed.null`, `callout.title.null`.
      Requires `S0`.
- [x] **M4 — Citation and footnote model.** Replace `FootnoteReference` and
      `FootnoteDefinition` with inline `Cite(citations)`, the scoped
      `Citation(referent, prefix, suffix)` value, the complete
      `CitationReferent` union with `BibMode`, the document-owned `Footnote(id,
      content)` value, and `Document.footnotes` visited after `content` and
      ordered by scope start; `Citation` and `Footnote` are scoped traversed
      values outside the `Markup` union, and this item adds their value
      callbacks, dump form, and wire records. Inherited `[^label]` calls lower
      to a one-item `Cite` with empty affixes whose ID is the normalized label
      without the caret; repeated calls share one `Footnote`; a valid
      unreferenced definition remains a `Footnote`; a later definition of an id
      already defined remains a `Footnote` after the first, as the inherited
      grammar parses it. Remove `markdown_core_node_association` and
      add cite, citation, and footnote accessors; update the remark projection;
      only the `footnote` branch is produced until `P7`. Manifest states and
      orders: `citation.referent.footnote`, `citation.affix.empty`,
      `document.footnotes.empty`, `document.footnotes.populated`,
      `document.content-before-footnotes`, `cite.items-in-order`. Requires `M2`,
      `S0`.
- [x] **M5 — List and item facts.** Replace `ListItem.checked` with `marker:
      String?` (`" "`, `"x"`, and `"X"` under the inherited task-list rule,
      `null` otherwise) and expose `tasked` and `completed` only as derived
      binding conveniences; add `List.variant` and `List.delimiter`, populated as
      `decimal` with `period` or `parenthesis(closed=false)` for inherited ordered lists and
      `null` for bullets. Replace the
      task-list and list facade accessors, update the cmark-gfm and remark
      projections, and regenerate the list fixtures. Manifest states:
      `listItem.marker.null`, `listItem.marker.space`, `listItem.marker.value`,
      `list.variant.decimal`, `list.variant.null`, `list.delimiter.period`,
      `list.delimiter.parenthesis(closed=false)`. Requires `S0`.
      The revised citation model also exposes `Specimen(id?, start?, content,
      scope)`, `Document.specimens`, and the specimen referent and walk callback.
      Retire the reserved example list variant and item label. Definition
      ownership, copying and wire support land here; grammar remains in P9b.
- [x] **M6 — One table model.** Emit `Table(columns, head, content, foot=[])`
      with `TableColumn(alignment, relative=null)` from the existing pipe-table
      path, remove `TableRow.isHeader`, add `TableCell.rowspan` and `colspan` as
      `1`, and keep `TableCell.content` as `[Markup]` so inherited inline cells
      stay inline and later table forms store blocks directly, with no
      `Paragraph` normalization. Replace the table facade accessors, update the
      cmark-gfm and remark projections, keep the empty-cell positions from #191
      exact in the ledgers, give the content of a cell containing `\|` its
      authored coordinates, which the current path shifts by the removed
      backslashes, and regenerate every table fixture. `Table.caption`
      is not added here: a typed field cannot precede its kind, and the
      `TableCaption` kind cannot precede a producer, so `P11a` adds the field
      and the kind together. Manifest states and orders:
      `table.column.relative.null`, `tableCell.span.one`,
      `tableCell.content.inline`, `table.head-content-foot`. Requires `S0`.
- [x] **M7 — Universal fields and the one attribute operation.** Add the
      inherited `anchor: String?` and `attributes: Attributes` to every kind by
      turning the contract's single inherited field into an ordered set that the
      projection audit, the fixture checker, and the dump grammar understand;
      add `Document.metadata: Metadata?` with the metadata value types; add
      optional image dimensions as `null` (regrouped as `Media.dimensions` in O9). Add facade accessors for the
      anchor, classes, records, metadata fields, and dimensions, and carry the
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
      `{}` attaches as empty. Restrict directive names to the letter-first
      grammar of the directives module, so `12:30` contains no directive, and
      register the remark delta for non-ASCII letters. Register every resulting remark grammar difference
      (bare names, empty assignments, entity decoding, `-`, duplicate names) in
      `specs/oracles/remark/deltas.json`, make the remark projection compare the
      universal fields, rewrite the directive fixtures, retarget the
      `directive.attributes.*` manifest states to `markup.anchor.value`,
      `markup.attributes.classes`, `markup.attributes.records`,
      `markup.attributes.source-order`, and `escaping.attribute-value`, replace
      the `canonical-ast.md` directive-attribute section with a pointer to the
      dialect's attributes module, and regenerate every golden once. The
      Obsidian gate reads `metadata` from the dump's nested `Metadata` lines.
      Manifest states: `markup.anchor.null`, `markup.attributes.empty`,
      `document.metadata.null`, `media.dimensions.null`. Exit: the attributes
      and Remark attribute conformance cases pass, no second attribute tokenizer
      remains, and size-doubling valid, duplicate, malformed, and unclosed
      containers are linear. Requires `M0` through `M6`.

  M7 validation (2026-09-07): the 29-kind/13-surface projection audit and
  nine canonical cases pass; C correctness/conformance, ASan, UBSan, TSan,
  strict OOM, Swift, Kotlin JVM/Native/Android-host, ES Node/browser, all four
  oracle gates, and 400-input seed-1 fuzz runs for CommonMark/GFM/remark pass.
  The host release dry run builds and checks the C, Swift, npm and Maven
  artifacts; full cross-host aggregation remains the required CI check.
  Scanner work counts remain linear for size-doubling valid, duplicate,
  malformed and unclosed containers. Metadata and image dimensions also have
  synthetic value/transport tests while their syntax awaits O6/O9.

  Ledger review: retaining root dump fields exposes eleven previously
  unobserved root-level heading/footnote scope overlaps; the containment audit
  checks Document's separate owned sequences independently. No parser scope
  changed. Two Obsidian gap digests change because digit-first directive names
  are now literal. The expanded remark corpus registers the shared-attribute
  boundaries and an existing empty-task lazy-continuation difference. Original
  fixture inputs are retained; new module and boundary cases are appended.

- **Stage 1 exit criterion**, verified in the `M7` pull request: every surface
  compiles with exhaustive handling of the inventory kinds that exist so far,
  the projection audit proves kind and field parity, no fixture or document
  names a removed kind or field, no kind stores one semantic fact in two fields,
  and equivalent direct and reference links and images have identical semantic
  shapes.

## Stage 2 — Obsidian track

- [x] **O1 — Wikilinks and embeds.** Create the parser-owned cross-link inline
      extension and its reviewed attach-table position (before
      `table`; the extension must see `[` and `!` before inherited bracket
      handling), always on, and public from this item. One
      scanner recognizes `[[...]]` and `![[...]]`, splits path, optional anchor,
      and label while scanning, removes the `#` and `#^` punctuation, and builds
      `CrossLink(dest=cross(path, anchor), label)` or, for `![[...]]`,
      `CrossEmbedded(dest=cross(path, anchor), label, dimensions)` (separated
      into its own kind in O9), whose `label`
      is null only when no `|` was authored. Add the `CrossLink` kind on every
      surface as the first producer of `Destination.cross`, fixtures for the
      module's seven table rows, every malformed boundary, opaque contexts,
      scopes, allocation failure, and size-doubling `!`, `[`, `]`, `#`, `^`, and
      `|` runs, and a canonical case; remove the four `wikilink-*` gaps and keep
      the label-nullability and destination projections as general deltas with
      canaries. Teach the table boundary scanner the `\|` escape so a wikilink
      alias or embed size stays inside one cell while the inline scanner
      receives the logical pipe, in every table syntax that parses the cell, so the module's escaped-pipe cases hold in
      inherited pipe tables from this item; the inherited delimiter-row grammar
      is unchanged. Fixtures also cover escaped pipes in aligned and pipe-optional
      tables. An escaped wikilink pipe inside a simple, multiline, or grid table
      cell is a cross-item case owned by whichever of `O1` and `P11b`, `P11c`,
      or `P11d` merges later. The heading-text projection of `CrossLink` and `CrossEmbedded` in
      generated anchors is a cross-item case owned by whichever of `O1` and `P3`
      merges later. An attribute container following a complete `CrossLink` or `CrossEmbedded`
      staying text beside bracketed spans is a cross-item case owned by
      whichever of `O1` and `P5` merges later. Requires `X0`, `M7`.

  O1 validation (2026-09-07): 30 kinds across all 13 projection surfaces and
  ten canonical cases pass. The 38 cross-link package examples include all 18
  module examples, malformed boundaries, raw label/anchor states, all opaque
  contexts available at O1, inherited link/image/directive/footnote composition,
  aligned and pipe-optional tables, and authored UTF-8 byte scopes. Work-count
  gates cover 128–8192 repeated units and bounded scanning of both wikilinks
  and opaque formula bodies. Strict OOM sweeps every node and string allocation.

  C correctness/conformance, ASan, UBSan, TSan, Swift and its external consumer,
  Kotlin JVM/Native/Android-host and conformance, ES Node/browser and conformance,
  four oracle gates, and 400-input seed-1 fuzz runs for CommonMark/GFM/remark
  pass. `pnpm verify` and the host release dry run pass, including the C, Swift,
  npm, Maven and Android AAR artifacts. Full cross-host aggregation remains the
  required CI check.

  Review notes: five inherited package examples now recognize their double
  brackets (three CommonMark/GFM specification examples and two directive
  boundaries). The corresponding CommonMark and remark inputs have exact
  deliberate-difference entries. Four Obsidian wikilink gaps are retired, both
  general model projections have canaries, and the pending Properties-list gap
  digest changes because O1 recognizes its quoted wikilink before O6 claims the
  envelope. Position and reference-resolution ledgers remain unchanged. The
  table's existing escape contraction/source map is reused. Formula bodies now
  claim their bytes before later scanners can swallow a closer; malformed
  backtick pairs still release their bodies. Cross links have one always-attached
  feature descriptor; marks, comments, and footnotes retain their own semantic
  boundaries and do not share a source-family umbrella descriptor. Cross-item cases
  with syntax not yet landed remain owned by their later items.

- [x] **O2 — Highlights.** Add `=` to the shared delimiter stack
      with pairwise run matching (two signs per match, a leftover single sign
      is text) and the non-empty rule, local pairing, and opaque
      code, formula, comment, HTML-token, and wikilink bytes; add the
      `Mark(content)` kind, fixtures for formatted, adjacent, escaped,
      unmatched, triple, table-cell, and footnote-content bodies plus
      size-doubling equals runs, and a canonical case; remove the `highlight`
      gap and keep the content-model projection. Callout-title cases join in
      `O8`. The heading-text projection of `Mark` in generated anchors is a
      cross-item case owned by whichever of `O2` and `P3` merges later. Requires
      `O1`.

  Implementation notes (2026-09-07): `Mark` is a core delimiter rule at C5.
  It uses the same maximal-run scanner, pairing stack, inline-node construction,
  and source extents as emphasis and strong, with two signs per match and no
  rule of three. It has no payload beyond its content and universal fields, so
  the existing facade accessors and exports cover it without new symbols. All
  three bindings, their exhaustive and walking visitors, and both transports
  carry the new kind. Fifteen package cases include nine currently exact module
  examples; the canonical `marks` case also covers escaped table pipes and
  document-owned footnote content. The existing package and canonical goldens
  remain byte-identical. The `highlight` gap is retired and the formatted-body
  content-model difference has an executable canary. `%%` opacity, callout
  titles, generated heading anchors, and insertion composition remain with
  O3, O8, P3, and I1 respectively.

  Validation (2026-09-07): C correctness (70 tests) and conformance (2 tests),
  ASan, UBSan, TSan, strict OOM, Swift and its external consumer, Kotlin
  JVM/Native/Android-host and conformance, ES Node/browser and conformance,
  the four oracle gates, and 400-input seed-1 fuzz runs for CommonMark, GFM,
  and remark pass. The equals-run probes double from 128 through 8192 units
  and bound scanner bytes, opener comparisons, and child moves by input size.
  Position and reference-resolution ledgers remain unchanged. `pnpm verify`,
  the Kotlin JVM surface audit, and the host release dry run pass, including
  C, Swift, npm, Maven, and Android AAR artifacts. Full cross-host aggregation
  remains the required CI check.

  O2 recorded an unused `MARKDOWN_CORE_MAX_INLINE_DEPTH = 256` macro and a
  conflicting limit in the dialect index. The I0/I1 provenance audit resolved
  this on 2026-09-09: commit `53b9a2aa` introduced the constant as the capacity
  of a temporary traversal worklist for source-ownership checks, without
  changing the parsed tree. Commit `e4107e5e` removed that traversal but left
  the macro. The ME-9 specification audit in `2d9dc333` then promoted it into
  a delimiter syntax limit without a parser implementation. The unused macro
  and mistaken normative limit are removed. Delimiter nesting retains its
  existing uncapped semantics, protected by the nested emphasis/strong stress
  tests and the shared delimiter size-doubling probes.

- [x] **O3 — Comments.** Scan `%%...%%` from the shared cursor
      with a linear closer search, classify block placement when both delimiters
      occupy their own lines and inline placement otherwise, keep the body
      opaque, and emit the `Comment(literal)` kind that `M0` adds, never
      stripped. Add fixtures covering inline, standalone, multiline, empty,
      adjacent, escaped, unmatched, and Markdown-looking bodies, the syntax of
      every already merged extension inside a comment body under the opacity
      rule, an HTML comment beside a `%%` comment, and size-doubling percent
      runs, and a canonical case; remove the two `comment-*` gaps. A callout
      title that is one `%%` comment, whose `title` is non-null and holds one
      `Comment`, is a cross-item case owned by whichever of `O3` and `O8` merges
      later. Requires `O1`.

  Implementation notes (2026-09-07): `%%` is one extension descriptor,
  `comment`, attached between `formula` and `cross_link`, which places it at
  inline step A5 and block step 4 at once. The inline scanner consumes its body
  from the shared cursor and caches a failed closer search under a rule id of
  its own, `MARKDOWN_CORE_DELIM_RULE_COMMENT`, so a run of signs that never
  closes costs one scan of its suffix. The block form is the first block start
  whose grammar reaches past its own line, and it decides before it opens:
  `markdown_core_parser_lookahead_begin` in `blocks.c` walks the raw source
  after the current line and matches the open containers' prefixes through the
  one container-prefix operation `check_open_blocks` now shares
  (`S_container_prefix_matches`), with list flags saved and restored, a list
  item whose first child is the block about to be added accepting blank lines
  as it will then, and extension containers asked through a new
  side-effect-free `continues_block` hook that the directive extension
  provides. A candidate that fails consumes nothing, so its line is paragraph
  text and the parser never rewinds; a candidate that commits is a block the
  parser then reads line by line exactly as the lookahead saw it, and the
  core's `finalize` builds the block's literal because `Comment` is a core
  kind. A per-line resume cache keeps the lookahead linear: two failed
  candidates that reach one line have nested container chains, so the later
  one resumes from the state the earlier one recorded, and recorded blank runs
  are stepped over at once when the extra containers are lists and items;
  footnote nesting is bounded by `MAX_FOOTNOTE_DEPTH`, and directive containers
  cannot nest failing candidates because they add no prefix. The footnote
  continuation test now asks for a line-ending byte rather than an LF
  spelling, which changes nothing for `curline` and lets a lookahead line keep
  its own terminator.

  The module was amended in the three places its examples did not decide: the
  closer is the same fence line as the opener, trailing spaces or tabs
  included; the backslash escape takes one sign out of a run, so `\%%%a%%` is
  the text `%` and the comment `a`, stated by a new module example; and a
  block comment's scope includes the whitespace after either fence. A fence
  line that could continue a paragraph lazily is a block start, and a
  container's own closing line ends a candidate's scan. Forty-one package
  cases include all fifteen module examples in order, the block-boundary
  example printing `anchor=null` until `P3` generates the heading anchor, plus
  adjacent, escaped, unmatched, and percent-run forms, Markdown-looking and
  every-merged-extension bodies, comments inside every earlier opaque
  construct, an HTML comment beside a `%%` comment, marks and cross links
  beside comments, table cells with `\|`, footnote content, and every block
  boundary: fence whitespace and indentation, blank runs, nested quotes and
  items, failing candidates, lazy lines, Setext and thematic-break precedence,
  directive closers, and tables. The marks module's `==%%c%%==` joins the
  marks fixture, and the `comments` canonical case gains both `%%` forms. The
  two Obsidian `comment-*` gaps are retired: the oracle's comment removal is
  the registered `comment-removal` projection with a recognition canary, and
  the corpus gains a comment-only paragraph and a quoted multi-line body. The
  cmark corpus now selects the fixture's untagged HTML-comment examples; the
  `%%` examples carry the `comment` tag. Position and reference-resolution
  ledgers are unchanged. The callout-title composition stays with `O8`.

  Validation (2026-09-07): the projection audit covers 31 kinds over 13
  surfaces and 11 canonical cases. C correctness (74 tests, including the four
  new pathological cases and the strict OOM sweep over a nested-container
  comment corpus) and conformance (2 tests) pass, and so do the ASan, UBSan,
  and TSan presets. The api harness bounds the inline scanner's work over
  thirteen size-doubling shapes and the lookahead's visited lines plus matched
  prefix bytes over five nested-container shapes, from 16 to 128 levels deep.
  The four oracle gates pass: Obsidian over 20 inputs with 9 of 9 remaining
  gaps reproduced, CommonMark 703 of 703, GFM 81 of 81, remark 149 of 149; and
  400-input seed-1 fuzz runs for CommonMark, GFM, and remark agree. Position
  places, scope containment, inline sourcepos, and reference-order ledgers
  hold without an update. Prettier, eslint, clang-format 23.1.0, cmake-format,
  the extension inventory, special-character, attach-order, source-list, AST
  projection, canonical manifest, repository, and test topology audits pass.
  The Swift, Kotlin, and ES suites, the release dry run, and the audits that
  need those toolchains did not run in this environment, which has no Swift,
  Emscripten, or Android SDK; this item changes no binding source, and the
  shared canonical case gains only `Comment` lines every dumper already
  prints. Full cross-host aggregation remains the required CI check.

- [x] **O4 — Inline footnotes.** Recognize `^[content]` inside the shared bracket algorithm,
      ahead of superscript, producing one one-item `Cite` with a `footnote`
      referent and one document-owned `Footnote` whose content is the parsed
      inline body stored directly, with no synthesized `Paragraph`; assign
      `inline-N` IDs after every authored ID, de-collide with `-K`, and merge
      referenced and inline values in source order inside the one document
      footnote operation. The oracle is silent here, so product fixtures own
      escaped brackets, empty and unclosed forms, unresolved calls, nested
      citations, semantic cycles, deterministic IDs and visitation order,
      allocation failure, and adversarial `^`, `[`, and `]` runs. Requires `O1`.
  Implementation notes (2026-09-08): `^[` pushes an inline-footnote opener
  on the shared bracket stack; its matching close consumes no tail. The
  inherited and inline forms use one citation constructor. A successful inline
  close places its Footnote directly in the document value field, and both
  forms register in one parser collection. Finalization visits only those
  values, orders source starts with ten stable byte passes, reserves all
  authored ids, and assigns `inline-N` / `inline-N-K`. It completes document
  ownership and discards the index before consolidation and mutable extension
  passes. Those phases visit document footnotes through their live owner slots.
  Failed candidates never register. No public field, kind, export, or transport
  layout changes.
  Nonblank-body evidence visits disjoint consumed token ranges, and nested
  bodies are neither rescanned nor reparsed. Package coverage now includes all
  module examples plus malformed, nested, collision, semantic-cycle, opacity,
  link-boundary, table-map, and detached-field cases (35 cases total). The
  new canonical case and Swift/Kotlin/ES tests verify direct inline bodies,
  source ordering, and finite value visitation. P6 owns superscript composition.

  Validation (2026-09-08): C correctness (74 tests), conformance (2 tests),
  ASan, UBSan, TSan, strict OOM, Swift and its package consumer, Kotlin
  JVM/Native/Android-host and conformance, ES Node/browser and conformance,
  all four oracle gates, and 400-input seed-1 fuzz runs for CommonMark, GFM,
  and remark pass. The bracket probes double from 128 to 8192 units; authored
  suffix-collision sets double to 4096. Postprocess probes verify the collection
  records only committed notes and is discarded before mutable callbacks; a
  callback that deletes all footnotes verifies the lifetime boundary. Literal
  caret spans from 1 KiB to 1 MiB allocate exactly as ordinary text spans. Bare
  URL fixtures cover closing brackets, escapes, nested notes and label fields.
  Existing golden ASTs remain exact.
  Position-place and external-position ledgers and the reference-resolution
  ledger remain unchanged. The containment ledger adds three reviewed nested
  footnote overlaps: the values retain their original, enclosing source
  ranges after document ownership transfer. `pnpm verify` and the host release
  dry run pass; full cross-host release aggregation remains the required CI gate.

- [x] **O5 — Task markers.** Generalize the task-list scanner's marker from
      `[ xX]` to exactly one Unicode scalar followed by a structural separator,
      decoding at most the candidate marker. Fixtures cover the module's marker table, ordered and nested lists,
      tabs, vertical tabs, and form feeds as separators, a prefix at the end
      of its line as a non-task, empty and multi-scalar markers, missing
      separators, scopes, and long malformed bracket runs; remove the
      `custom-task-character` gap. Requires `O1`.

  Implementation notes (2026-09-08): task recognition runs once immediately
  after the inherited list algorithm creates an item, before choosing its
  first block. The task module decodes one candidate scalar, checks the
  closing bracket and required separator, owns the exact UTF-8 bytes, and
  consumes the entire SP/TAB/VT/FF separator run. Failed candidates do not
  search for a later closer or allocate marker storage. The old ASCII scanner,
  task extension descriptor, and duplicated item-continuation callback are
  removed; every item uses the inherited continuation rule. This also prevents
  a later paragraph from assigning or replacing an item's marker.
  Nineteen package cases include every module example byte for byte,
  one- through four-byte markers, combining scalars versus multi-scalar
  graphemes, punctuation, separators, malformed forms, ordered and nested
  containers, scopes, first-block decisions, and all already-landed opaque
  contexts. Only the two historical `[@]` goldens change, because those
  controls now express custom task states. The `task-markers` canonical case
  and Swift, Kotlin, and ES assertions verify real parses and derived
  completion. C tests verify input-buffer independence, strict allocation
  failure, and size-doubling malformed bracket and scalar runs with constant
  output-node counts and exact literal fallback.
  The Obsidian custom-task gap closes by agreement. Seven exact, digest-locked
  entries retain its deliberate grammar differences; cmark-gfm and remark
  register their ASCII-only and paragraph-first boundaries. A deterministic
  fuzz witness also fixes the requirement that a line ending cannot be a task
  separator before lazy paragraph content. Position and reference-resolution
  ledgers remain unchanged.
  C correctness/conformance, ASan, UBSan, TSan, Swift/macOS, Kotlin/JVM,
  Kotlin/macOS arm64, Kotlin/Android host, ES Node/browser and conformance,
  oracle parity, three 300-input deterministic differential fuzz runs (seed 1),
  `pnpm verify`, and the host release dry run pass. Full cross-host release
  aggregation remains the required CI gate.
  Review follow-up: the shared block cursor counts each completed Unicode
  scalar as one virtual indentation column while retaining byte offsets for
  scopes. Its byte advances compose across scalar boundaries, its column
  advances consume whole scalars, and tabs still expand or partially consume
  to the next four-column stop. This removes the ASCII-only assumption that
  made nested tab padding change a CodeBlock into a Paragraph for multibyte
  task markers. The regression joins both package and canonical fixtures;
  cursor partition tests and 1,728 marker-replacement/indentation combinations
  protect the general coordinate invariant.

- [x] **O6 — Fixed metadata fields and literal prose.** The user-directed
      contract of 2026-09-08 recognizes exactly `name`, `title`, `subtitle`,
      `time`, `date`, `authors`, `keywords`, `abstract`, `state`, and `comment`.
      `Metadata` exposes these ten optional values directly, with only the
      complete envelope's `scope`. Remove `Metadata.content`, the record
      wrapper, retained field order, per-field scopes and all source indexes.
      The [Properties module](../specs/dialect/properties.md) owns the grammar.
      Requires `O1`.

      Keep scalar and flat-list value semantics, including exact numeric text.
      `authors` and `keywords` accept single text, bracketed arrays and block
      lists on following lines; `authors: - Ada` is text `"- Ada"`.
      `abstract` and `comment` accept single-line text and bare `: |`
      indented prose, preserving internal blank lines with default clipping.
      Missing fields differ from present null scalars, empty text and empty
      lists. Ignore unnamed text, unknown fields, comments, `...`, invalid
      values and later duplicates; the first successful occurrence wins.
      Only an absent or unclosed envelope falls back to Markdown.

      The parser assigns valid members directly to their named destination;
      field presence supplies duplicate detection. Unsupported members are
      skipped once at their owned boundary. A bracketed value owns all lines
      until it closes, irrespective of indentation or field-looking contents;
      an unclosed collection consumes the remaining metadata payload up to
      the closing `---`. No JSON root objects, full YAML
      parser, anchors, aliases, tags, nested values, folding or literal modifiers
      are supported. Arrays remain field values. C, Swift, Kotlin/JNI/Native
      and ES/Wasm expose the same direct-field model and preserve ownership.
      The dump prints all ten fields in fixed model order, with no nested
      metadata records or visitor callbacks.

      Acceptance:

      - [x] Core recognizes ten fields, literal prose, first-successful
            assignment, whole-member recovery and original body coordinates.
      - [x] Every binding, public contract, dump and ABI snapshot exposes the
            direct fields; superseded collection and record APIs are removed.
      - [x] Fixtures cover missing/explicit-null/empty/list values, ignored
            input, duplicate recovery, literal indentation and owned cleanup.
      - [x] Complexity and OOM gates verify disjoint source decoding, flat-list
            scaling and punctuation-independent text allocation.
      - [x] The pinned YAML oracle compares only the selected valid grammar;
            unsupported syntax is not a missing feature.

  Status: direct-field replacement is implemented in PR #216. Both review
  findings are addressed: metadata builds no source index, and the root/package
  READMEs and changelog describe the direct-field model. Allocation tests from
  64 KiB to 1 MiB verify identical peak live bytes for equal-length plain,
  quoted, literal and list-item text with or without brackets. Fixtures verify
  source-order independence, absent versus explicit-null values, ignored root
  objects, and subsequent valid field lines. Further review corrections keep
  balanced collections opaque and skip an unclosed collection to the envelope's
  closing fence. A dash is a list marker only at the start of a following line;
  on the field line it remains ordinary scalar text.

  Direct-field host validation (2026-09-08): C correctness 76/76 and conformance 2/2;
  ASan, UBSan and TSan 76/76 each, including OOM sweeps; Swift tests, packed
  consumer and conformance; Kotlin JVM, macOS Native and Android host tests
  and conformance; ES Node/browser, packed consumer and conformance;
  `pnpm verify`; metadata/Obsidian parity (32 inputs); host release dry run,
  including ABI checks and Maven publication validation. Full Linux/macOS
  release aggregation remains in CI. Boundary corrections were revalidated
  with C correctness/conformance, all three sanitizer suites, shared conformance
  across Swift/Kotlin/ES, the metadata oracle, and `pnpm verify`.

- [x] **O7 — Block identifiers.** Attach `#anchor-id#`
      during block finalization through one operation for paragraph suffixes,
      structured-block follower lines with the required blank-line boundaries,
      and list-item suffixes, writing the identifier into the owner's inherited
      `anchor` without either `#` delimiter and removing the complete marker
      from visible content with no repair pass or offset side table; a second
      candidate on one owner is ordinary content,
      and the owner's scope still covers the identifier while child scopes end
      before it. Fixtures cover every placement, metadata-free callouts, nested
      lists, invalid characters, missing separation, escaped or missing
      delimiters, extra hash runs, the former caret spelling as ordinary
      inline content, ATX heading boundaries, duplicates, end of document,
      scopes, allocation failure, and long candidate sequences with linear
      scanning work.
      Identifier-like bytes inside a crosslink source form belong to this item
      because it requires `O1`. These cross-item cases are owned by whichever
      item merges later: the cross-extension reservation case of the anchor
      contract with `P3`, an identifier attached to a metadata-bearing callout
      with `O8`, and an identifier line after a table's caption attaching to the
      `Table`, once per table form, with `P11a`, `P11b`, `P11c`, and `P11d`.
      Requires `O1`.
- [x] **O8 — Callout metadata.** Evaluate `[!type]`, the
      optional `+` or `-` fold marker, and the inline title on the first content
      line of every `>` container inside the existing block algorithm, store the
      type as written in `variant` with matching left to consumers, remove the
      metadata line from content before body blocks finalize, keep unknown and
      custom types, leave invalid or misplaced markers as content, and nest
      through the inherited container recursion. Populate the `variant`,
      `collapsed`, and `title` fields that `M3` declared on every surface, changing no
      accessor or dump form; add fixtures for the module's table plus formatted
      titles, every built-in alias, nested combinations, lazy continuation,
      scopes, allocation failure, and adversarial depth, and canonical cases for
      `callout.variant.value`, `callout.collapsed.false`,
      `callout.collapsed.true`, and `callout.title.populated`. An identifier
      attached to a metadata-bearing callout is a cross-item case owned by
      whichever of `O8` and `O7` merges later, and a title that is one `%%`
      comment, non-null and holding one `Comment`, is a cross-item case owned by
      whichever of `O8` and `O3` merges later. Requires `O1`, `O2`.

  O8 implementation and host validation (2026-09-08): the existing quote-open
  path recognizes the first metadata line once. A callout owns its authored
  variant and optional collapsed state; its title uses the shared inline and
  owned-field phases. Body paragraphs are created when body text arrives,
  including immediate lazy continuation. The public accessor, transport,
  visitor, and dump forms are unchanged. All 15 module examples are copied
  byte for byte into 32 callout fixtures, with the four existing examples
  unchanged; the canonical case covers all four populated states and title
  visitation before content. A facade assertion preserves the native Setext
  end boundary before a following blank line. Existing scope ledgers and
  unrelated fixtures are unchanged.

  C correctness 77/77 and conformance 2/2 pass, as do ASan, UBSan and TSan
  correctness 77/77 each, including strict allocation-failure sweeps and
  adversarial quote-depth work bounds. Swift macOS, Kotlin JVM/macOS Native/
  Android host, and ES Node/browser tests and conformance pass, together with
  oracle parity, scope/place audits, `pnpm verify`, and the host release dry
  run for C, Swift, npm and Maven. Full cross-host release aggregation remains
  the required CI check.

- [x] **O9 — Media dimensions.** Parse the complete
      `W`, `WxH`, `alt|W`, and `alt|WxH` alt-label suffixes in the shared image
      construction path into `dimensions: Dimensions?`, keep the whole label as alt
      content on any malformed suffix. Apply the shared size grammar to embedded
      `CrossEmbedded.label`, retaining its raw prefix and exposing `dimensions`;
      ordinary cross-link labels remain raw.
      Fixtures cover every valid and invalid dimension form and formatted alt
      content. An image carrying both a typed dimension suffix and a `width` or
      `height` attribute record, each retained independently, is a cross-item
      case owned by whichever of `O9` and `P2d` merges later. Requires `O1`.

  O9 renames the canonical `Image` node to `Media` across the C kinds and
  accessor, Swift/Kotlin/ES models, visitors, decoders, dumps and public API
  inventories. The inherited `![...](...)` syntax does not infer the target
  media type; `CrossEmbedded` remains a workspace transclusion.

  O9 also groups the previously independent width/height fields into the
  node-independent `Dimensions(width: Int, height: Int?)` value, held by
  `Media.dimensions: Dimensions?`. Its C facade, binding models, transports,
  dump grammar, fixtures and API inventories change together; there is no
  legacy pair of image fields or Dimensions visitor callback. Embedded cross
  references are now `CrossEmbedded`, sharing the same dimension parser and value,
  with the generic C accessor
  `markdown_core_node_dimensions`; a size-only embed label stays present as
  an empty string. `CrossLink` has only `dest` and `label`; no node carries
  an `embedded` flag. Raw cross-link scanners and Media inline brackets each
  supply their own separator boundaries without reparsing or rescanning labels.

  Implementation notes (2026-09-09): the shared successful-image branch
  consumes dimensions after direct/reference resolution and before delimiter
  reduction. Each image bracket records its last ordinary-text pipe; escaped and
  opaque tokens and nested brackets keep their own boundaries. Bounded integer
  parsing checks complete raw suffixes, and retained alt nodes use the existing
  content-to-source map, including contracted table pipes. Dimensions belong to
  the occurrence while referenced destinations keep their shared identity.

  Package fixtures include every module example, numeric limits and malformed
  forms, all reference forms, formatted alt, opacity, nested images, table
  escapes and exact scopes. The shared `media-dimensions` case covers both
  produced dimension states on every binding. Size-doubling digit/pipe runs
  and nested image labels assert a linear work bound; strict OOM sweeps cover
  truncation and empty alt. Nine exact CommonMark inputs register the authored
  suffix difference; the exact Obsidian embed input registers label consumption
  as `embedded-dimensions`. Position and reference ledgers gain no exceptions.
  Validation: C correctness 77/77 and conformance 2/2; ASan, UBSan and TSan
  correctness 77/77 each, including strict OOM sweeps. Swift macOS, Kotlin
  JVM/macOS Native/Android host, and ES Node/browser tests and conformance pass,
  as do all four oracle gates, the three 400-case CI fuzz seeds, position and
  reference audits, `pnpm verify`, and the host release dry run for C, Swift,
  npm and Maven. Full cross-host release aggregation remains the required CI
  check. The dimension-attribute composition remains owned by later item `P2d`.

- [x] **O10 — Obsidian evidence closure.** Add the integration fixtures for
      the shared opacity and overlapping-delimiter invariants, OFM and CommonMark
      constructs between paired inline HTML tags, the dialect recognition order, task
      items carrying block identifiers, generic `CodeBlock` info/language
      preservation and literal-body opacity, and inline and display math; add
      canonical cases until every OFM kind, state, and order is covered; add deterministic fuzz seeds and pathological cases for
      delimiter runs, nested callouts, inline-HTML boundaries, escaped table
      pipes, long paths and headings, and repeated identifiers with structural
      bounds; audit every inline extension caller and delete obsolete skip
      tables and repair paths; empty `baselineGaps`; mark every Obsidian
      feature-table row `present`; document every Obsidian feature in the README
      and the binding READMEs. Requires `O1` through `O9`.

  CodeBlock evidence follows the [base language contract](../specs/dialect/base.md#code)
  for preserved info/language and opaque literal bodies. Consumers interpret
  those fields; no CodeBlock subkinds or per-language fixture checklist is
  introduced. Existing generic fixtures discharge these requirements.

  Completed 2026-09-09 on the merged O9 baseline. The
  [O10 evidence record](2026-09-09-obsidian-evidence-closure.md) maps each module
  to its fixtures and records the inline caller audit, shared ownership
  invariants, complexity/OOM coverage and validation. One new shared composition
  case runs on all four surfaces and joins the fixed fuzz corpus; HTML/comment
  boundary probes extend the existing delimiter test. C and all three
  sanitizers pass 78/78, C conformance 2/2, all host binding suites, four oracle
  gates, three 400-case fuzz seeds, static/position audits and the host release
  dry run pass. The remaining supported hosts and release aggregation retain
  their required PR CI gates. O10 adds no parser or public-model changes.

- **Obsidian track exit criterion**, verified in the `O10` pull request: the
  plan exit criterion of the Obsidian implementation plan holds on every public
  surface, with every Obsidian module always on in the switch-less dialect and
  no option, preset, or composed switch on any surface.

## Stage 3 — insertion track

- [x] **I1 — Insertion.** Implement inserted text: tokenize each plus run
      once into two-character units with the odd-run literal rule, apply the
      flanking rules without the rule of three, push eligible units onto the
      shared delimiter stack, nest rather than merge repeated units, and
      normalize an odd closer's spare `+` after its closing units; escapes,
      code, formula, comment, and HTML-token bytes are opaque and paired tags
      create no region. Add the `Insertion(content)` kind, fixtures replaying the
      pinned upstream cases plus the contract's crossed-delimiter, `CrossLink`,
      `Mark`, `Cite`, deep-nesting, allocation-failure, and size-doubling
      cases, and a canonical case; remove every `I0` gap. The `CrossLink` and
      `Mark` composition cases are cross-item cases owned by whichever of `I1`
      and `O1` or `O2` merges later, the `Cite` composition case belongs to `I1`
      because it reaches the citation model through `M7`, and `Comment` opacity
      follows the opacity rule with `O3`. The heading-text projection of
      `Insertion` in generated anchors is a cross-item case owned by whichever of
      `I1` and `P3` merges later. Requires `X0`, `I0`, `M7`.

  I0/I1 implemented together on the O10 baseline. The oracle replays all 16
  pinned upstream groups, all 11 normative examples and 27 composition inputs;
  all upstream/module inputs agree and 12 exact existing-dialect differences
  carry both digests. `baselineGaps` is empty. The shared core delimiter rule
  descriptor supplies minimum unit width and emitted kinds; plus runs use the
  same stack, reduction, OOM transaction and source extents as emphasis/marks.
  One run compactly represents its identical units: consuming its trailing
  opening pairs and leading closing pairs leaves an odd literal in the required
  middle position without a repair pass. Every public model, transport, dumper
  and visitor includes Insertion, with a shared canonical composition case.

  Core delimiter runs are classified before node allocation. Runs shorter than
  their rule's minimum width, or with neither flanking role, remain in the
  surrounding borrowed text slice. One source-offset-keyed lookahead carries
  the classification from text scanning to delimiter dispatch. Size-doubling
  allocation tests cover all four core delimiter characters, literal carets,
  and Unicode intraword underscores: known-literal spans allocate exactly as
  ordinary text of the same byte length, scan each delimiter byte once, and
  retain the exact literal and scope.

  Stack eligibility also keeps close-only runs literal when no earlier opener
  of the same rule survives. This decision reads the live opener counts rather
  than caching mutable state with source classification. Allocation/work tests
  cover close-only prose such as `C++ `, unrelated openers, and bracket cleanup
  for all four core rules. Matching tests preserve dual-role openers, multiple
  units in one opener run, and openers outside a completed bracket.

  Validation (2026-09-09, macOS arm64): `pnpm verify`, all external parity
  gates, source-position/reference ledgers and the JVM surface audit pass.
  C passes 79 correctness and 2 conformance tests; ASan, UBSan and TSan each
  pass the 79 correctness tests. Swift, Kotlin JVM/Native/Android-host and ES
  Node/browser correctness and conformance checks pass, as does the host
  release dry run with packaged consumers. Other release hosts remain covered
  by the Release dry run workflow.

  The O2 depth discrepancy is resolved by tracing and removing the mistaken
  specification limit, as recorded under O2. Deep size-doubling probes preserve
  the shared parser's uncapped nesting behavior. P3 still owns generated
  heading-anchor projection.

## Stage 4 — Pandoc track

- [x] **P2a — `inline_code_attributes`.** Attach a container that begins
      immediately after a complete closing backtick run to the `Code` node,
      excluding it from `literal` and including it in scope; whitespace prevents
      attachment and a malformed suffix leaves the code span unchanged. Fixtures
      and a canonical case; remove the `inline-code-attributes` gap. An explicit
      ID from this syntax reserved before heading synthesis is a cross-item case
      owned by whichever of `P2a` and `P3` merges later. Requires `P0`, `M7`.
- [x] **P2b — `heading_attributes`.** Attach a trailing container on ATX and
      Setext headings, after optional closing hashes, removing it from content
      and including it in scope; an invalid suffix stays visible. Fixtures cover
      compact, spaced, Setext, and malformed forms; remove the
      `header-attributes` gap. Requires `P0`, `M7`.
- [x] **P2c — `fenced_code_attributes`.** Accept a braced list in the opening
      info region of tilde and backtick fences and attach its classes and
      records as written; `info` and `language` keep the inherited contract over
      the bytes outside the list, nothing is lowercased, aliased, or derived
      from a class, and `numberLines` and its relatives stay inert records; a
      malformed list attaches nothing and does not reinterpret the body or
      closing fence. Remove the
      `fenced-code-attributes` gap. An explicit ID from this syntax reserved
      before heading synthesis is a cross-item case owned by whichever of `P2c`
      and `P3` merges later. Requires `P0`, `M7`.
- [x] **P2d — `link_attributes`.** Attach an immediate container after a direct
      link, image, resolved reference occurrence, or autolink. Implement the
      attributes contract's `merge(primary, inherited)` operation here with
      reference definitions as its first consumer. The C parser stores a
      definition's container in the reference resource introduced by `M2`; each
      occurrence retains its local declarations and reads inherited values
      through that resource. Apply the merge without changing occurrence scope,
      keeping inherited duplicates and registering Pandoc's `combineAttr`
      deduplication as an expected divergence of the Pandoc gate;
      extend `pathological_reference_expansion_bound` and its transport and
      decoder counterparts. Within each binding, decode inherited values once per
      definition using that language's native collection and ownership conventions;
      keep `width` and `height` unit strings as records. Audit every Link, Media, Heading, Code,
      CodeBlock, directive, and reference-definition caller and delete repair
      passes made obsolete by the shared operation. Two cross-item cases are
      owned by whichever item merges later: a link tail claiming the container
      ahead of a bracketed span with `P5`, and attributes authored on an
      implicit heading reference occurrence with `P4`. Remove the
      `link-and-image-attributes` and `pandoc-reference-attribute-merge` gaps.
      An explicit ID from this syntax reserved before heading synthesis is a
      cross-item case owned by whichever of `P2d` and `P3` merges later, and an
      image carrying both a typed dimension suffix and a `width` or `height`
      attribute record, each retained independently, is a cross-item case owned
      by whichever of `P2d` and `O9` merges later. Requires `P0`, `M7`.

  P2 completion audit (2026-09-09):

  - Binding representation is a language-specific design choice. Swift keeps
    value types and copy-on-write Arrays; Kotlin keeps immutable List snapshots;
    ES keeps ordinary readonly arrays. No binding retains a native handle after
    parsing. Definition values are decoded once within each binding, and an
    occurrence with local declarations constructs its merged native sequences.
    C and transport storage are linear in authored input. Binding construction
    is linear in input plus the native arrays explicitly materialized by local
    merges; public collection types are not replaced to impose a uniform
    physical-storage mechanism.
  - The shared recognition index serves immediate suffixes, trailing block
    containers and reference definitions. Heading envelopes reserve their own
    trailing container before inline attachment; an opaque inline body can
    consume source before that reservation commits. Scope comes from the
    existing source map, and definition inheritance never supplies scope.
  - P2b also fixes Setext finalization: the block ends on its underline, not
    the following line. The reviewed updates change only heading endpoints in
    13 existing fixture cases and remove ten containment/overlap ledger rows.
  - P0 observes five agreements, one exact `combineAttr` divergence, and nineteen
    gaps owned by later P items. The already-agreeing bare-name rejection case
    is an agreement rather than a fictitious baseline gap. Pandoc code language
    classes and final code newlines use documented representation projections.
  - Validation passed on macOS arm64: `pnpm verify`, C correctness and conformance, strict OOM,
    ASan/UBSan/TSan (79 correctness tests each); Swift and Kotlin JVM/Native
    correctness and conformance; ES Node/browser and conformance; all oracle
    gates; position and reference-order ledgers; 400 fixed-seed cases each for
    CommonMark, GFM and Remark differential fuzzing; and the host release dry run.
    The pinned Pandoc installer was exercised from a missing executable through
    download, SHA-256 verification, installation and the offline version check.
    Full Linux/macOS release aggregation remains in CI.

- [x] **P3 — `auto_anchors`.** Build one document anchor registry that reserves
      every explicit anchor from every enabled extension before synthesis, then
      generates GFM anchors in heading order from the per-kind text projection
      of the anchors module (`Text` and `Code` literals; the concatenated child
      text of formatting, `Link`, `Media`, and directive labels; one space per
      soft or hard line break; nothing for `HTML`, `Comment`, and a footnote
      `Cite`; `Formula.literal`), Unicode lowercasing, whitespace to `-` without
      collapsing, and the permitted-scalar filter, falling back to `section` and
      uniquifying with the smallest free `-N`. A generated anchor has no scope.
      Fixtures cover the module's cases, a projection case for every kind that
      exists when this item lands, `Formula`, `HTML`, `Comment`, `Media`, line
      breaks, and directive labels included, and large duplicate sets; remove
      the `gfm-auto-anchors` gap. Reserving an explicit anchor before synthesis
      is a cross-item case with every explicit-anchor producer that neither
      requires nor is required by this item, `O7`, `P2a`, `P2c`, `P2d`, `P5`,
      and `P8`, each owned by whichever merges later; the `P2b` heading and `M7`
      directive cases belong to this item. The heading-text projection of each
      kind a later item produces is a cross-item case owned by whichever of `P3`
      and that item merges later: `CrossLink` with `O1`, `CrossEmbedded` with `O9`, `Mark` with `O2`,
      `Insertion` with `I1`, `Span` with `P5`, `Superscript` and `Subscript` with
      `P6`, a bibliography `Cite` with `P7`, and `Cite` with a `specimen` referent with `P9b`.
      Requires `P2b`.
- [x] **P4 — `implicit_heading_references`.** Register a virtual reference
      definition for every heading with a final anchor, keyed by the authored
      label source after removing heading syntax, closing hashes, and trailing
      attributes and applying inherited label normalization, targeting `#` plus
      the final anchor; explicit definitions win, the first of duplicate labels
      wins, and every reference spelling resolves to an ordinary `Link` through
      the `M2` resolver in one order-independent finalization shared with
      specimen labels. Attributes authored on such an occurrence are a cross-item
      case owned by whichever of `P4` and `P2d` merges later. A complete cite
      beating the shortcut reference of a virtual definition registered for a
      heading such as `# @foo` is a cross-item case owned by whichever of `P4`
      and `P7` merges later. Remove the `implicit-header-references` gap.
      Requires `P3`.

  P3/P4 implementation audit (2026-09-09):
  - Headings register as their blocks close, already in source order, including
    inside footnotes. There is no final collection walk or heading sort. The
    ordinary inline cursor establishes actual heading-attribute ownership and
    writable raw labels before reference lookup; only a live bracket suspends
    that same cursor until declarations are complete. No prefix is reparsed.
    [Heading resolution](../architecture/heading-resolution.md) records the
    dependency argument, lifecycle, and complexity bounds.
  - One document registry reserves effective explicit anchors from every
    currently emitted producer and owned field during the existing inline
    completion walk, without another whole-tree traversal. Per-base suffix
    cursors live in the index slots and never
    restart; inherited definition anchors are hashed once per resource, not per
    occurrence. One generated Unicode 17.0.0 range table performs lowercase,
    whitespace replacement and filtering in one scalar lookup. The table
    regenerates byte-for-byte from pinned UnicodeData and the pinned runtime.
  - Each writable heading creates an ordinary reference definition with the
    existing M2 resource, including duplicate labels. Ordinary reference lookup
    applies the existing first-definition rule; heading parsing does not deduplicate. A final target is filled once before any postprocessor; bindings keep
    their native resource sharing without new transport fields. Tests cover
    forward references, explicit priority, duplicate labels, raw-label length
    boundaries, opaque/attribute overlaps, pending delimiter ownership, detached
    resource lifetime, and references throughout directive/callout/footnote fields.
  - All nine anchors-module examples appear byte-for-byte in
    `dialect-anchors.txt`, which contains 53 cases. A shared canonical case
    crosses every binding. Every byte changed in the ten pre-existing fixture
    files is confined to `Heading.anchor`; source scopes remain unchanged.
    Current cross-item cases with `O1`, `O2`, `O7`, `O9`, `I1`, `M7`, and
    `P2a`–`P2d` are covered. Future producers and citation conflicts remain owned
    by `P5`, `P6`, `P7`, `P8`, and `P9b` as listed above.
  - The Pandoc corpus now has 37 cases: thirteen agreements, seven exact dialect
    differences, and seventeen remaining feature gaps. Both P3/P4 gaps are
    retired. Added evidence pins global explicit reservation, Unicode simple
    lowercase/whitespace/filter behavior, and CommonMark reference adjacency;
    the multiline-table gap only updates its existing fallback heading anchor.
  - Host validation: C correctness 80/80 and conformance 2/2; ASan, UBSan and
    TSan correctness 80/80 each; Swift correctness, external consumer and
    conformance; ES Node, browser, packed consumers and conformance; Kotlin JVM
    and macOS arm64 correctness/conformance; `pnpm verify`; all six oracle gates;
    unchanged scope-containment, position-place and reference-order ledgers;
    and the credential-free host release dry run. Full Linux/macOS release
    aggregation remains in CI.
  - Seed 903 differential fuzzing agrees on CommonMark 400/400 and GFM 400/400.
    Remark agrees on 397/400; three occurrences of one task-prefix/LF difference
    produce byte-identical ASTs on main `8d9177fa` and this implementation. The
    task-list module excludes LF from its separator alphabet. This is existing
    evidence outside P3/P4, not a new heading discrepancy or a widened policy.
    CI seed 1 additionally exposed two P3/P4 harness omissions: heading-reference
    compositions were compared against cmark, and Remark compared a heading
    anchor fact its model cannot express. The independent CommonMark scope
    classifier now reports those compositions separately from comparisons;
    the shared Remark projection omits only `Heading.anchor`. Scope/projection
    tests preserve comparison of explicit references, opaque brackets, heading
    content/levels/attributes and other nodes' anchors. The full CommonMark
    fuzz witness is a product fixture and its reduced reference interaction
    agrees with Pandoc. CI seed 1 now passes: CommonMark compares 373/373 inputs
    and reports 27 outside-scope compositions; GFM and Remark compare 400/400
    each. Parser behavior is unchanged by this CI follow-up.
  - Local representative timing against main `8d9177fa` used three alternating
    runs, each with 15 measurements and three warmups. The median ratio across
    non-heading samples was 1.018 (range 0.965–1.041). Dense ATX/Setext samples
    changed from 0.330/0.176 ms to 0.768/0.496 ms, including new anchor and
    reference-declaration work. These are host measurements, not timing gates;
    doubling tests enforce the general work bounds independently.

- [x] **P5 — `bracketed_spans`.** Decide `[text]{...}` in the shared bracket
      stack: a valid link tail wins, a complete attribute container after the
      first balanced `]` produces `Span`, `{}` produces an empty-attribute
      `Span`, and an invalid container falls back without consuming the `{`. Add
      the kind, fixtures, and a canonical case; remove the bracketed-span
      and attribute-grammar gaps. The `Span` containing a `Cite` case belongs to
      `P7`, and a link tail claiming the container ahead of a span is a
      cross-item case owned by whichever of `P5` and `P2d` merges later. An
      explicit ID from this syntax reserved before heading synthesis is a
      cross-item case owned by whichever of `P5` and `P3` merges later. The
      heading-text projection of `Span` in generated anchors is a cross-item
      case owned by whichever of `P5` and `P3` merges later. An attribute
      container following a complete `CrossLink` or `CrossEmbedded` staying text is a cross-item
      case owned by whichever of `P5` and `O1` merges later. Requires `P0`,
      `M7`.
- [x] **P6 — `superscript` and `subscript`.** Add the single `^` and `~`
      delimiters through the delimiter engine with unescaped-whitespace
      rejection, `\ ` to a no-break space, literal empty-body fallback, `^[` and `~~`
      precedence. Add both kinds, fixtures, and
      canonical cases; remove the `superscript-and-subscript` gap and
      register the normative empty-body fallback as an exact deliberate
      difference in `empty-superscript-and-subscript`. X0 has removed the legacy
      double-tilde-only switch; this item removes single-tilde strikethrough
      from the extension, registering the cmark-gfm delta, so a
      single tilde is always a subscript delimiter. The `^[` precedence case
      with `O4` is owned by whichever item merges later. The heading-text
      projection of `Superscript` and `Subscript` in generated
      anchors is a cross-item case owned by whichever of `P6` and `P3` merges
      later. Requires `P0`, `M7`.

  P5/P6 validation (2026-09-10): all 36 kinds and 67 fields agree across
  the 13 projection surfaces; 28 canonical cases include populated and empty
  Span content, nested scripts and contextual no-break spaces. The package
  fixtures include every example of the bracketed-span, script and
  strikethrough modules, shared attribute forms, link/reference precedence,
  opaque tokens, escaped brackets, Unicode whitespace, line endings and exact
  scopes. Cross-item cases with P2d, O1, O4, P3 and P4 pass. M4's explicit bracket
  order (direct/reference tails, Span, cite, shortcut, footnote) is retained;
  stale local module text is synchronized with the owning table. The existing
  footnote/attribute input stays byte-identical and now demonstrates its P5
  Span, while plain footnote calls keep their inherited behavior. Bibliography
  composition remains assigned to P7.

  The bracket stack claims Span bodies through the same content transfer as
  links, images and inline footnotes. The shared delimiter engine matches
  scripts; it records raw-whitespace boundaries while scanning disjoint source
  slices. The existing inline-completion walk decodes tagged space-escape tokens
  after ownership is known and before heading projection, without scanning Text
  values for syntax. Size-doubling tests from 128 through 8192 units bound bracket, attribute and delimiter work for deep
  nesting, malformed suffixes, empty pairs, whitespace and mixed inline owners.
  Mixed bracket/script runs expose the obsolete per-`]` delimiter-stack scan:
  19 counter gates fail with that scan and pass after removing it. Opaque
  scanners claim whole tokens at their openers; the extension audit now pins
  the remaining `]` to the bracket procedure. Strict OOM tests sweep the new
  ownership and failure paths, including the literal image bang before a Span.

  All six historical P5 grammar inputs now agree with Pandoc; five registered
  gaps are removed, while bare-name rejection was already an agreement. P6's
  ordinary case agrees. Empty bodies, escaped spaces, Unicode whitespace,
  decoded TABs, maximal tilde runs and code opacity retain exact, explained
  differences under the normative module. The Pandoc corpus has 51 cases,
  27 agreements, 14 deliberate differences and 10 gaps for later items. The
  cmark-gfm ledger preserves all four historical single-tilde inputs. A
  minimized remark fuzz witness pins a Span exposed by a failed directive;
  remark fuzz excludes fragments containing this foreign Span syntax.

  C correctness (83 tests), conformance (2 tests), ASan, UBSan, TSan, Swift
  and its external consumer, Kotlin JVM/Native/Android-host with ABI checks,
  ES Node/browser and binding conformance pass. All six oracle gates and
  400-input seed-1 CommonMark/GFM/remark fuzz runs pass. Position, containment
  and reference-order ledgers remain unchanged. `pnpm verify` and the host
  release dry run pass; full Linux/macOS release aggregation remains a CI
  requirement. The Kotlin JVM surface ledger also records two pre-existing
  P2 internal helpers, `AttributesKt` and `DefinitionResource`; this change
  does not add them to the implementation or the public Kotlin ABI.

  Integration with P3/P4 (#225): Span, Superscript and Subscript participate
  in the heading text projection. A shared canonical case covers later explicit
  Span ID reservation, formatted heading text, decoded script spaces and forward
  references. Package cases also cover detached footnote/directive fields and
  Span precedence over an implicit shortcut. Deep nested Span/script headings
  retain linear bracket, delimiter and anchor work through 8192 levels. Three
  new Pandoc compositions agree; the fourth pins global reservation of a later
  Span ID. The original P4 occurrence-attribute input is retained and its new
  P5 Span output is recorded.

  PR #226 review follow-up (2026-09-11): owned directive labels now report
  ordinary raw whitespace to the enclosing script boundary through the shared
  inline parser. Each source buffer parses once; a heading suspends at a token
  with an owned label until forward references are available. The completion
  walk inherits script depth through owned fields while document-owned footnotes
  keep an independent context. Inline-field trailing spaces remain body content.
  Ten new package cases cover nested labels, raw and escaped whitespace, opaque
  content, local delimiters and failed outer candidates; eight fail before the
  fix. Two existing canonical cases gain these compositions and heading cases.
  Size-doubling counters and strict allocation-failure sweeps cover the new
  paths. Label source maps now reuse the enclosing source view, including line
  breaks and escaped table pipes; one existing table golden's Text endpoint is
  corrected from column 22 to 23 with its input unchanged. Allocation failure
  after extension consumption stops before any ordinary-text fallback.
  Validation passes for C correctness/conformance, ASan/UBSan/TSan, all host
  bindings and canonical cases, the six oracle gates, 400-input seed-1 fuzz
  runs, unchanged position/reference ledgers, `pnpm verify` and the host
  release dry run.

  Post-merge audit (2026-09-11): [all 100 PR #226 files were audited](2026-09-11-p5-p6-delimiter-audit.md).
  The follow-up replaces script boundary snapshots and the pending-field pointer
  with events on the shared delimiter stack, unifies parsed-container construction
  including Strikethrough, and adds isolated Pandoc whitespace agreements.
  The [delimiter architecture](../architecture/inline-delimiters.md) states the
  source-order, scope-reduction, lifecycle and complexity invariants.

- [x] **P7 — `citations`.** Recognize bare and braced keys, bracketed groups
      with semicolon items and prefix, mode marker, key, and suffix scopes,
      author-in-text keys with an optional bracketed tail, `-@` for
      `suppressAuthor`, and locator braces retained as suffix text. A bracketed
      candidate followed by a direct-link destination or reference tail belongs
      to that link; one followed by an attribute container belongs to the outer
      `Span`, which the bracket procedure tests first; a complete cite beats shortcut-reference lookup; and, as Pandoc
      resolves it, a bare `@key` with no bracketed tail whose key is a specimen
      label registered anywhere in the document is a `Cite` with a `specimen` referent, while
      `[@key]` and a bare key followed by a bracketed tail stay citations. This
      is the first producer of `CitationReferent.bib`. The
      `reset-citation-positions` class is documented here, and its heading
      fixture is an ordinary `heading_attributes` case in `P2b`. Remove the
      `bibliography-citations` gap. The heading-text projection of a
      bibliography `Cite` in generated anchors is a cross-item case owned by
      whichever of `P7` and `P3` merges later. A complete cite beating the
      shortcut reference of a virtual heading definition is a cross-item case
      owned by whichever of `P7` and `P4` merges later. Requires `P5`, `P9b`.
- [x] **P8 — Nameless container directives.** Open a `DirectiveBlock` with
      `name=null` on a line of three or more colons followed, after `{` or
      whitespace, by a braced attribute container or one unbraced class word,
      Pandoc's fenced-div spelling; a colon run followed
      immediately by a name stays a named container. Make `DirectiveBlock.name`
      nullable on every surface, close a nameless container through the one
      closer rule of the directives module, a bare colon run at least as long
      as the opener, and nest through the normal container stack. Add fixtures
      and a canonical case for the null name; register the Pandoc deltas for the
      closer rule; remove the `fenced-divs-nested` gap. A definition body
      ending at an enclosing nameless-container close is a cross-item case owned
      by whichever of `P8` and `P10` merges later. An explicit ID from this
      syntax reserved before heading synthesis is a cross-item case owned by
      whichever of `P8` and `P3` merges later. Requires `P0`, `M7`.
- [x] **P9a — `fancy_lists`.** Generalize the ordered-marker operation for
      decimal, alphabetic, Roman, and `#` markers with period, one-paren, and
      two-paren delimiters, the capital-period two-space rule, `i` and `I`
      disambiguation, same-variant continuation, a new list on a variant or
      delimiter change, and the nested-start restriction; `List.start` is always
      the first marker's value for every variant, so `startnum` has no counterpart,
      and a Roman numeral whose value exceeds the nine-digit decimal ceiling
      is ordinary text, the accumulation stopping at the ceiling so no run of
      any component can overflow the `int` that holds `List.start`.
      Remove the `fancy-list-and-startnum` gap. Requires `P0`, `M7`.
- [x] **P9b — specimen definitions and references (`example_lists`).**
      Implement [specimens](../specs/dialect/specimens.md) through the citation
      model: document-owned `Specimen(id?, start?, content, scope)` definitions
      and `Cite` items with `CitationReferent.specimen(id)`. The public model,
      native ownership, transports, dump and walking callbacks are available;
      P9b adds source recognition and document-wide registration. Preserve
      anonymous and duplicate definitions, first-label resolution, effective
      first-in-group resets, four-column continuations, and bounded numerals.
      No list variant, list-item label, separate reference node, or stored
      derived numbering is introduced. Add every module fixture and canonical
      coverage; retire the upstream `example-lists-and-reference` gap. The
      heading projection uses the existing citation projection with the id as
      its key spelling; test it with `P3`. Requires `M4`, `M5`, `P9a`.
  P9a/P9b/P7 implementation (2026-09-11): these items land together so the
  specimen definition registry exists before bibliography recognition. Ordered
  markers use one allocation-free classifier with committed-variant reading and
  bounded accumulation. Footnotes and specimens share definition registration,
  source ordering and ownership transfer. Citation keys, groups and author tails
  use the existing bracket procedure and delimiter matcher; deferred tails keep
  their parsed nodes in the AST and resume bounded ranges once their owner is
  known. Affixes use owned inline roots and all completion phases use an explicit
  stack. Heading projection and virtual-heading shortcut precedence are covered.

  All 30 normative examples are retained verbatim in the module fixtures, which
  now contain 36 citation, 14 specimen and 16 list cases. Three shared canonical
  cases cover populated referent families, modes, affixes, resets and list enum
  branches. The manifest now has 31 cases. Complexity tests exercise 8192-level
  citation dependency chains and numeral accumulation through the nine-digit
  ceiling, including overflow in M/C/X/I runs. Strict allocation-failure sweeps
  cover pending bracket ranges, affix roots and definition ownership. Kotlin/Native
  now checks field presence before decoding ordered-list values on bullet lists.

  The bibliography and fancy-list Pandoc gaps close by agreement. The example
  gap becomes an exact model difference: the product retains IDs and definitions,
  while Pandoc emits derived numbers and example List items. Nineteen new oracle
  cases preserve agreements and isolated dialect differences; the Pandoc corpus
  has 74 cases and seven unrelated gaps remain. Three historical golden changes
  follow citation recognition and the nested-start rule; their original inputs
  and exact oracle witnesses remain. Position and reference ledgers are unchanged.

  Validation passes C correctness/conformance, ASan/UBSan/TSan and strict OOM;
  Swift, Kotlin JVM/Android host/macOS Native and ES host tests and conformance;
  all six oracle gates; 400-input seed-1 CommonMark/GFM/remark fuzz runs;
  `pnpm verify`; and the host release dry run. The latter stages and verifies
  local packages only; release publication remains gated by R1.

- [x] **P10 — `definition_lists`.** Recognize a one-line term, an optional
      single blank line, and a first marker line by bounded non-consuming
      lookahead before paragraph fallback; feed each body's lines to the
      ordinary block parser in place; set `Definition.compact` from the term
      gap; append further bodies and definitions per the module's separator and
      boundary rules; and yield to a complete table candidate. Add
      `DefinitionList` and `Definition`, fixtures for every listed case, and
      canonical cases; remove the two `definition-list-*` gaps. Cross-item cases
      owned by whichever item merges later: caption precedence over term
      lookahead with `P11a` and again for each later table form with `P11b`,
      `P11c`, and `P11d`, and a body ending at an enclosing nameless-container
      close with `P8`. Requires `P0`, `M7`.
  P8/P10 implementation (2026-09-11): named and nameless directives share
  opener attributes, the open-container stack and one deferred closer decision.
  The innermost container owns its fence; an open code, HTML, comment or formula
  block retains its own lines. `DirectiveBlock.name` is nullable on every
  surface; inline directive names remain required. Definition terms use bounded
  prefix lookahead only at paragraph fallback. Bodies use the ordinary block
  parser, shared item padding and continuation, with private body roots exposed
  solely as ordered collections. The owned-inline walker includes terms in all
  completion phases. See [block containers](../architecture/block-containers.md).

  Module fixtures preserve the normative examples and add nested, empty, opaque,
  indentation, reference and container-boundary cases. One old normative fallback
  expected `:x` as Text; ordinary paragraph fallback still runs inline parsing,
  so that line correctly emits Directive(name="x"). Two inherited fenced-code
  golden scopes previously included a line outside their enclosing quote; losing
  the parent prefix now ends code on the preceding line, clearing two containment
  ledger rows without weakening the invariant. Two canonical cases cover the
  nullable name, all compact/body branches, P3 anchor reservation and the P8/P10
  closing boundary. The shared manifest has 33 cases and all 38 Markup kinds.

  The two definition-list and nested-div Pandoc gaps close by agreement. Exact
  differences retain fence width, global ID reservation and authored compact
  flags when Pandoc has no Plain/Para block from which to observe them. The
  oracle compares every term and ordered body and never drops the product flag.
  Its corpus has 86 cases and four later-feature gaps remain. Remark's named-only
  grammar retains eleven exact-input differences for nameless container cases.
  A reduced Remark witness records inherited quote/directive lazy continuation,
  verified unchanged against a rebuilt 9cbb83e5. Recombination also reproduced
  O5's registered newline task-separator boundary; its existing fuzz exclusion
  now covers zero trailing spaces. Caption/table precedence compositions remain
  owned by P11a–P11d as specified.

  Validation passes 86 C correctness and two conformance checks; full
  ASan/UBSan/TSan runs and strict OOM; Swift, Kotlin JVM/Android host/macOS
  Native and ES tests and shared conformance; all six oracle gates; seed-1
  400-input CommonMark/GFM/Remark fuzz runs; position/reference audits;
  `pnpm verify`; and the host release dry run. The final closer-state cleanup
  also passes the focused TSan facade, concurrency, directive, definition and
  fuzz checks. C export lists and Kotlin JVM/Klib ABI baselines include the
  planned public additions. Packages are staged locally; nothing is published.

P11 implementation and P12 evidence (2026-09-12): [delivery and validation
record](2026-09-12-pandoc-tables-and-compositions.md). All four table items are
implemented across C, Swift, Kotlin and ES. The 488-case composition diagnostic
and zero-gap Pandoc corpus preserve all remaining semantic differences. P12 is
complete for the selected feature inventory and the normative dialect. The
2026-09-12 scope clarification confirms that full Pandoc compatibility is not
an acceptance requirement; parser/consumer ownership remains unchanged.

- [x] **P11a — `table_captions`.** Recognize a `Table:`, `table:`, or `:`
      caption line as a table-candidate block start parsed in one lookahead with
      the table that follows it, releasing the bytes to paragraph parsing when
      no table follows, and claim a caption paragraph after a table only when
      it has no preceding caption. Between two tables, the caption belongs to
      the preceding table if that table has no caption; otherwise it remains
      available as the next table's preceding caption. Strip the marker into
      `TableCaption.content`, and extend `Table.scope` over both. Add
      `Table.caption: TableCaption?`
      and the `TableCaption` kind together on every surface, fixtures for
      before, after, both, between tables with and without an earlier caption,
      multiline, and empty captions, and canonical cases
      for `table.caption.null` and `table.caption.populated`. An identifier line
      after a table's caption attaching to the `Table` is a cross-item case
      owned by whichever of `P11a` and `O7` merges later. Requires `P0`, `M7`.
- [x] **P11b — `simple_tables`.** Establish column ranges from the dash
      separator line, derive alignment from header placement, accept the
      headerless closing-separator form, and end at a blank line or closing
      separator, under the block-start order of the tables module: a Setext
      heading beats every simple-table candidate, complete or not, a complete
      candidate beats a thematic break and a paragraph, a dash line that
      completes no candidate is a thematic break, and a code fence is never
      claimed. `TableColumn.relative` stays `null` for simple tables because
      Pandoc's reader gives them default column widths; `P11c` is the field's
      first producer. Remove the `simple-table-with-caption` gap. Caption
      precedence over definition-term lookahead for this table form is a
      cross-item case owned by whichever of `P11b` and `P10` merges later. An
      identifier line after a caption on this table form attaching to the
      `Table` is a cross-item case owned by whichever of `P11b` and `O7` merges
      later. An escaped wikilink pipe inside a simple-table cell is a cross-item
      case owned by whichever of `P11b` and `O1` merges later. Requires `P11a`.
- [x] **P11c — `multiline_tables`.** Recognize full-width and segmented dash
      boundaries, combine physical lines into logical rows separated by blank
      lines, populate `TableColumn.relative` from source widths, require the
      blank separator for a one-row table, and accept the headerless form.
      Remove the `multiline-table` gap. Caption precedence over definition-term
      lookahead for this table form is a cross-item case owned by whichever of
      `P11c` and `P10` merges later. An identifier line after a caption on this
      table form attaching to the `Table` is a cross-item case owned by
      whichever of `P11c` and `O7` merges later. An escaped wikilink pipe inside
      a multiline-table cell is a cross-item case owned by whichever of `P11c`
      and `O1` merges later. Requires `P11b`.
- [x] **P11d — `grid_tables`.** Parse `+`, `-`, `=`, and `|` boundaries with the
      column boundary set the union of the `+` positions on every horizontal
      boundary line, as the tables module states, `=` separators selecting
      head and foot, cell
      bodies through the ordinary block parser, missing segments as `rowspan`
      and `colspan` stored once in the upper-left anchor row, source-defined
      fully covered rows retained with `cells=[]`, and authored empty cells
      retained with `content=[]`. Keep coordinate expansion and layout in
      consumers; emit neither covered-coordinate placeholders nor layout-only
      rows. Use a compacted row frontier to validate connected source rectangles and
      cross-group spans, with authored alignment colons and widths. Temporary
      geometry is bounded by columns plus source rows and output cells. Ledger the row-span
      containment exception the tables module states in
      `specs/positions/containment.json` with its reason. Remove the
      `grid-table-block-cells` and `grid-table-row-and-column-spans` gaps. Grid
      widths populate `TableColumn.relative` through the producer `P11c`
      establishes. Caption precedence over definition-term lookahead for this
      table form is a cross-item case owned by whichever of `P11d` and `P10`
      merges later. An escaped wikilink pipe inside a grid-table cell is a
      cross-item case owned by whichever of `P11d` and `O1` merges later. An
      identifier line after a caption on this table form attaching to the
      `Table` is a cross-item case owned by whichever of `P11d` and `O7` merges
      later. Requires `P11c`.
- [x] **P12 — Selected Pandoc feature evidence closure.** Add composition fixtures for every
      selected feature beside every other, deterministic fuzz seeds and
      size-doubling cases for brackets, attributes, `@`, braces, carets, tildes,
      colons, numerals, and grids, and canonical cases covering the selected
      features' canonical kinds, states, and order. Close every missing-feature
      gap against the normative modules. Keep exact documented oracle
      differences for inherited language rules, selected-feature semantics and
      the canonical consumer model; a registered difference is not proof of
      implementation, and no projection may hide missing syntax or content.
      Document every selected feature in the README and the binding READMEs.
      Unselected Pandoc extensions, its rendering choices and full AST equality
      are outside this acceptance scope. Requires `P2a`
      through `P11d`.
- **Pandoc track exit criterion**, verified in the `P12` pull request: the Phase
  6 exit criterion of the Pandoc implementation plan holds, with every selected
  extension always on in the switch-less dialect and no preset of any kind.

## Stage 5 — release

- [ ] **R1 — Release.** Keep `VERSION` at `3.0.0`, write
      `docs/releases/3.0.0.md` listing the documented OFM subset and parser-only
      boundary, `Document.metadata`, resolved reference links and images,
      `BlockQuote` to `Callout`, the citation and footnote model, `checked` to
      `marker`, the unified table model, universal anchors and attributes,
      `Destination`, the exact Pandoc 3.11 pin and feature names without a
      monolithic preset, and inserted text; move the CHANGELOG section; run
      `pnpm release:check-version`, `pnpm verify`, and the release dry run; tag.
      Requires `O10`, `I1`, `P12`.

## Dependency table

Sizes are rough review-effort estimates, not schedules.

| Item   | Requires           | Size | Cross-item cases                                                                                                                                                                                        | Discharges                                                                                                                       |
| ------ | ------------------ | ---- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `S0`   | —                  | L    | —                                                                                                                                                                                                       | the dialect modules; audit A1 through C6                                                                                         |
| `X0`   | `S0`               | M    | —                                                                                                                                                                                                       | option registry serving both plans' option bullets                                                                               |
| `P0`   | `X0`, `S0`         | M    | —                                                                                                                                                                                                       | Pandoc Phase 0; Pandoc Phase 1 gate activation                                                                                   |
| `I0`   | `S0`               | S    | —                                                                                                                                                                                                       | insertion oracle setup                                                                                                       |
| `M0`   | `S0`               | S    | —                                                                                                                                                                                                       | Obsidian Phase 2 comments; removes `stripHTMLComments`                                                                           |
| `M1`   | `S0`               | M    | —                                                                                                                                                                                                       | Obsidian and Pandoc Phase 1 `Destination`                                                                                        |
| `M2`   | `M1`               | L    | —                                                                                                                                                                                                       | Obsidian Phase 1 reference normalization; Phase 5 projections                                                                    |
| `M3`   | `S0`               | M    | —                                                                                                                                                                                                       | Obsidian Phase 1 `Callout`                                                                                                       |
| `M4`   | `M2`, `S0`         | L    | —                                                                                                                                                                                                       | Obsidian Phase 1 citation model; Pandoc Phase 1 bibliography branch                                                              |
| `M5`   | `S0`               | M    | —                                                                                                                                                                                                       | Obsidian Phase 1 `marker`; Pandoc Phase 1 list values                                                                            |
| `M6`   | `S0`               | L    | —                                                                                                                                                                                                       | Pandoc Phase 1 table values                                                                                                      |
| `M7`   | `M0`–`M6`          | XL   | —                                                                                                                                                                                                       | Obsidian Phase 1 metadata, anchor, dimensions; Pandoc Phase 1 fields; Pandoc Phase 2 attribute operation and directive migration |
| `O1`   | `X0`, `M7`         | M    | `Insertion` containing `CrossLink` and `CrossEmbedded` (`I1`); heading-text projection (`P3`); container after a complete wikilink (`P5`); escaped wikilink pipe in a simple, multiline, or grid cell (`P11b`, `P11c`, `P11d`) | Obsidian Phase 2 wikilinks; Phase 4 escaped table pipes                                                                          |
| `O2`   | `O1`               | S    | `Insertion` containing `Mark` (`I1`); heading-text projection (`P3`)                                                                                                                                       | Obsidian Phase 2 highlights                                                                                                      |
| `O3`   | `O1`               | M    | callout title that is one comment (`O8`)                                                                                                                                                                | Obsidian Phase 2 comments                                                                                                        |
| `O4`   | `O1`               | M    | `^[` before superscript (`P6`)                                                                                                                                                                          | Obsidian Phase 2 inline footnotes and resolution                                                                                 |
| `O5`   | `O1`               | S    | —                                                                                                                                                                                                       | Obsidian Phase 4 task markers                                                                                                    |
| `O6`   | `O1`               | XL   | —                                                                                                                                                                                                       | Obsidian Phase 3 Properties                                                                                                      |
| `O7`   | `O1`               | M    | anchor reserved before synthesis (`P3`); identifier on a metadata-bearing callout (`O8`); identifier after a table caption (`P11a`, `P11b`, `P11c`, `P11d`) | Obsidian Phase 3 block identifiers                                                                                               |
| `O8`   | `O1`, `O2`         | M    | identifier on a metadata-bearing callout (`O7`); callout title that is one comment (`O3`)                                                                                                               | Obsidian Phase 3 callouts                                                                                                        |
| `O9`   | `O1`               | M    | typed dimensions beside a dimension attribute record (`P2d`)                                                                                                                                            | Obsidian Phase 4 media parameters                                                                                                |
| `O10`  | `O1`–`O9`          | M    | —                                                                                                                                                                                                       | Obsidian Phase 1 fixtures and oracle registration; Phase 2 caller audit; Phase 5; plan exit criterion                                          |
| `I1`   | `X0`, `I0`, `M7`   | S    | `Insertion` containing `CrossLink` and `CrossEmbedded`, `Mark` (`O1`, `O2`); heading-text projection (`P3`)                                                                                                                    | insertion contract                                                                                                           |
| `P2a`  | `P0`, `M7`         | S    | anchor reserved before synthesis (`P3`)                                                                                                                                                                 | Pandoc Phase 2 attachment sites                                                                                                  |
| `P2b`  | `P0`, `M7`         | S    | —                                                                                                                                                                                                       | Pandoc Phase 2 attachment sites                                                                                                  |
| `P2c`  | `P0`, `M7`         | M    | anchor reserved before synthesis (`P3`)                                                                                                                                                                 | Pandoc Phase 2 attachment sites                                                                                                  |
| `P2d`  | `P0`, `M7`         | M    | link tail ahead of a span (`P5`); attributes on an implicit heading reference (`P4`); anchor reserved before synthesis (`P3`); typed dimensions beside a dimension attribute record (`O9`)              | Pandoc Phase 2 attachment, merge, and caller audit                                                                               |
| `P3`   | `P2b`              | M    | anchor reserved before synthesis (`O7`, `P2a`, `P2c`, `P2d`, `P5`, `P8`); heading-text projection (`O1`, `O2`, `I1`, `P5`, `P6`, `P7`, `P9b`)                                                           | Pandoc Phase 2 heading registry                                                                                                  |
| `P4`   | `P3`               | M    | attributes on an implicit heading reference (`P2d`); complete cite over a virtual heading reference (`P7`)                                                                                              | Pandoc Phase 3 document resolution                                                                                               |
| `P5`   | `P0`, `M7`         | M    | link tail ahead of a span (`P2d`); anchor reserved before synthesis (`P3`); heading-text projection (`P3`); container after a complete wikilink (`O1`)                                                  | Pandoc Phase 3 spans                                                                                                             |
| `P6`   | `P0`, `M7`         | S    | `^[` before superscript (`O4`); heading-text projection (`P3`)                                                                                              | Pandoc Phase 3 superscript and subscript                                                                                         |
| `P7`   | `P5`, `P9b`        | L    | heading-text projection (`P3`); complete cite over a virtual heading reference (`P4`)                                                                                                                   | Pandoc Phase 3 citations and resolution                                                                                          |
| `P8`   | `P0`, `M7`         | M    | definition body ends at a nameless-container close (`P10`); anchor reserved before synthesis (`P3`)                                                                                                                    | Pandoc Phase 4 fenced divs                                                                                                       |
| `P9a`  | `P0`, `M7`         | M    | —                                                                                                                                                                                                       | Pandoc Phase 4 ordered markers                                                                                                   |
| `P9b`  | `P9a`              | M    | heading-text projection (`P3`)                                                                                                                                                                          | Pandoc Phase 4 specimens                                                                                                     |
| `P10`  | `P0`, `M7`         | L    | caption precedence over term lookahead (`P11a`, `P11b`, `P11c`, `P11d`); definition body ends at a nameless-container close (`P8`)                                                                                     | Pandoc Phase 4 definition lists                                                                                                  |
| `P11a` | `P0`, `M7`         | M    | caption precedence over term lookahead (`P10`); identifier after a table caption (`O7`)                                                                                                                 | Pandoc Phase 5 captions                                                                                                          |
| `P11b` | `P11a`             | M    | caption precedence over term lookahead (`P10`); identifier after a table caption (`O7`); escaped wikilink pipe in a simple-table cell (`O1`)                                                            | Pandoc Phase 5 simple tables and precedence                                                                                      |
| `P11c` | `P11b`             | M    | caption precedence over term lookahead (`P10`); identifier after a table caption (`O7`); escaped wikilink pipe in a multiline-table cell (`O1`)                                                         | Pandoc Phase 5 multiline tables                                                                                                  |
| `P11d` | `P11c`             | XL   | caption precedence over term lookahead (`P10`); escaped wikilink pipe in a grid cell (`O1`); identifier after a table caption (`O7`)                                                                    | Pandoc Phase 5 grid tables                                                                                                       |
| `P12`  | `P2a`–`P11d`       | M    | —                                                                                                                                                                                                       | Pandoc Phase 6; plan exit criterion                                                                                              |
| `R1`   | `O10`, `I1`, `P12` | S    | —                                                                                                                                                                                                       | both delivery sequences                                                                                                          |

## Working in parallel

- `M1` through `M7` are one open pull request at a time; each regenerates
  goldens that the next one rewrites again.
- `S0` is the specification pull request and proceeds in parallel with `X0`
  and `I0`. It precedes `M0`, `M1`, `M3`, `M4`, `M5`, `M6`, and `P0`, and every
  feature item follows it through `M7`, so no implementation item lands before
  the rules it implements.
- `P0` and `I0` touch only scripts and oracle policy and may land at any point
  before the first Pandoc feature item and before `I1`; their registered digests
  are re-registered by whichever model item changes them.
- After `M7`, the Obsidian, insertion, and Pandoc tracks are independent.
  Inside a track, items that edit the same engine file are serialized or rebased
  in order: `O2` through `O5` share inline integration points while retaining
  their own semantic operations; `O6`, `O7`, and
  `O8` edit block finalization; `O9`, `P11b`, `P11c`, and `P11d` edit the table
  path; `P2a` through `P2d` share the attribute callers; `P5`, `P6`, and `P7`
  share the inline bracket and delimiter code; `P8` and `P10` share block
  starts.
- The `Cross-item cases` column, together with the opacity rule, is the complete
  list of fixtures that wait for a second item neither of whose items requires
  the other: `Insertion` composed with `CrossLink`, `CrossEmbedded`, and `Mark`; `^[` before
  superscript; an explicit anchor reserved before synthesis, once per producer;
  the heading-text projection of
  each inline kind a later item produces; a complete cite over a virtual heading
  reference; an identifier on a metadata-bearing callout; a callout title that
  is one comment; an identifier line after a table caption, once per table form;
  an escaped wikilink pipe inside a simple, multiline, or grid cell; typed image
  dimensions beside a dimension attribute record; a link tail claiming a
  container ahead of a span; a container after a complete wikilink; attributes
  on an implicit heading reference; a definition body ending at a fenced-div
  close; caption precedence over definition-term lookahead, once per table form;
  and every comment and wikilink opacity case. Each is written by the later of
  its two items, and no item's `Requires` grows because of it.
