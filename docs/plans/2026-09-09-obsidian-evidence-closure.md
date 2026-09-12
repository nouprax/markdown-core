# Obsidian evidence closure (O10)

This record closes O10 of the [landing plan](2026-09-04-canonical-vnext-landing-plan.md)
on the O9 baseline, commit `9ab6dfee` (#219). The dialect modules define the
language; the pinned official help snapshot and executable oracles provide
external evidence. O10 changes tests and documentation, with no parser, AST,
binding, or wire-format change.

- [x] Verify the module evidence, shared invariants and validation below, then
      close O10 and the Obsidian implementation plan.

## Module and fixture ownership

This table is the OFM fixture manifest. Every example in each listed C fixture
belongs to the linked normative module, including its malformed and composition
cases. A composition also exercises the participating modules' shared ownership
rules. Paths in the C column are relative to
[`packages/markdown-core/tests/fixtures`](../../packages/markdown-core/tests/fixtures/README.md).
Canonical cases are enumerated by the existing
[`specs/canonical-ast/manifest.json`](../../specs/canonical-ast/manifest.json),
which all four public surfaces consume; no second case list drives execution.

| Normative module | C fixture files | Shared canonical cases |
| --- | --- | --- |
| [Properties](../specs/dialect/properties.md) | `dialect-properties.txt` | `properties`, `properties-empty`, `properties-forms`, `properties-lists`, `properties-boundaries` |
| [Cross links](../specs/dialect/cross-links.md) | `dialect-cross-links.txt` | `cross-links`, `destinations`, `media-dimensions` |
| [Marks](../specs/dialect/marks.md) | `dialect-marks.txt` | `marks` |
| [Comments](../specs/dialect/comments.md) | `dialect-comments.txt` | `comments` |
| [Footnotes](../specs/dialect/footnotes.md) | `dialect-footnotes.txt` | `inline-footnotes`, `references` |
| [Task lists](../specs/dialect/task-lists.md) | `dialect-task-lists.txt` | `task-markers` |
| [Block identifiers](../specs/dialect/block-identifiers.md) | `dialect-block-identifiers.txt` | `block-identifiers` |
| [Callouts](../specs/dialect/callouts.md) | `dialect-callouts.txt` | `callouts` |
| [Links and images](../specs/dialect/links-and-images.md) | `dialect-links-and-images.txt` | `references`, `media-dimensions` |
| [Tables](../specs/dialect/tables.md) | `dialect-tables.txt`; cross-link and media fixtures own their escaped-cell compositions | `structure`, `cross-links`, `media-dimensions` |
| [Formulas](../specs/dialect/formulas.md) | `elements-formula-github.txt`, `elements-formula-latex.txt`, `elements-formula-conflicts.txt` | `formulas` |
| [Base language](../specs/dialect/base.md) | `spec.txt`, `regression.txt` | `blocks`, `inlines`, `scopes`, `completeness` |

The new `obsidian-integration` canonical case composes Properties, HTML tokens
and live content between tags, comments and raw cross labels, overlapping mark
delimiters, failed cross-link fallback, callout titles, anchored task items,
resolved Media dimensions, escaped table pipes, generic CodeBlock opacity, and
inline/block formulas. Its source positions and public ownership edges are
reviewed in the shared golden. Existing module fixtures remain unchanged.

## Recognition and ownership evidence

The [dialect recognition order](../specs/dialect.md#recognition-order) owns
scanner precedence, bracket-tail precedence, delimiter reduction, attachment,
and email finalization. O10 verifies actual ownership and failure boundaries;
it does not require a Cartesian product of feature names.

| Invariant | Existing evidence and O10 reinforcement |
| --- | --- |
| Claimed source stays opaque | Cross-link fixtures cover code, HTML attributes/comments, formulas and raw labels. Comment fixtures cover every inline family plus opaque blocks. Footnote fixtures verify that opaque bodies register no notes. |
| Inline HTML tokens own only themselves | Marks and footnotes fixtures retain literal attributes and parse the text between paired tags. `obsidian-integration` carries this boundary through all bindings. HTML blocks retain the base block grammar. |
| Earlier ownership protects delimiters | Comment fixtures contain both comment-in-mark and mark-in-comment cases, crossed cross-link/comment openers, and HTML/percent-comment pairs. O10 extends the size-doubling mark probe with HTML-token and comment boundaries. |
| Failed candidates consume nothing | Cross-link malformed forms and footnote unmatched/nested cases retain later valid constructs. The integration case retains a Formula and Mark after an unfinished cross-link candidate. |
| Authored scalar state has one owner | Block-identifier fixtures cover task-item suffixes, nested containers, duplicate identifiers and reference-definition gaps. Callout titles, footnotes and metadata have their own ownership fields. |
| Table unescaping preserves syntax and source coordinates | Cross-link/media fixtures assert cell boundaries and contracted-pipe scopes. The integration case checks raw labels and embedded dimensions in adjacent cells. |
| Ordinary code labels stay descriptive | Base CodeBlock fixtures cover info/language extraction and literal bodies; the integration case includes an arbitrary label with OFM-looking body text. The formula module separately owns its explicit formula syntax. |

## Inline caller audit

The single attachment list is `elements/core-elements.c`. The inventory,
special-character, attachment-order and public-surface audits verify its callers
and the absence of a dialect selector. The source review covers every inline
descriptor and the core handlers:

| Caller | Shared mechanism and retained lifecycle |
| --- | --- |
| Core code, HTML, escapes and entities | `parse_inline` dispatches the base token handlers; their consumed source never enters a later extension. HTML token scanning creates no element-region suppression. |
| Cross links and embeds | One cursor scanner constructs CrossLink or CrossEmbedded, using the same reference fields and bounded dimension parser. Failed candidates leave the cursor untouched. |
| Percent comments | One opaque-close search with failure caching; the node retains the raw body. No comment stripping or later body repair. |
| Marks and strikethrough | Core marks and the strikethrough descriptor use the shared delimiter stack and typed delimiter rules. The parser's flanking table derives from descriptor declarations; strikethrough's declared transparency remains required. |
| Formulas | Shared opaque-close and delimiter operations own inline bodies. The documented block/sole-formula projection runs on the existing iterative postorder walker; it is not an OFM repair path. |
| Directives | Label boundaries are recognized once; deferred label content is parsed through the shared inline-field queue. Attributes use the shared attribute scanner. |
| Autolinks | URL/www recognition protects the owned run; inherited email finalization visits Text through the common tree walker, excluding existing link content. |
| Links, Media and footnotes | Shared bracket handling resolves tails, constructs occurrence-owned values, and registers footnotes before document finalization. It uses the existing reference map and field queue. |

No obsolete OFM-specific skip table, alternate parser, or repair callback remains
to delete. The shared character tables, formula projection and inherited email
pass have documented semantics and remain in place.

## Complexity, failure and external evidence

`tests/api/main.c` contains size-doubling probes `cross_link_linear_work`,
`mark_linear_work`, `comment_inline_linear_work`, `comment_block_linear_work`,
`inline_footnote_linear_work`, `callout_linear_work`,
`block_identifier_linear_work`, and `image_dimension_linear_work`. They assert
scanner/delimiter work bounds and semantic counts over failed and valid runs,
long paths/headings, repeated identifiers and nested containers. O10 adds
HTML-token and crossed-comment cases to the existing mark algorithm's probe.
The pathological runner also asserts deep container and cross-node structure.

The C fuzz target uses a fixed PRNG seed and fixed product corpus. O10 registers
`obsidian-integration`, `cross-links`, `block-identifiers`, and `callouts` in
that existing target, alongside the earlier per-feature inputs. The strict OOM
runner sweeps every allocation and requires terminal failure; O10 adds a mixed
HTML/mark/comment/callout/task/footnote/embed ownership input to that sweep.
Differential fuzzing keeps the existing CommonMark, GFM and remark seeds and
their explicit language boundaries.

The [Obsidian policy](../../specs/oracles/obsidian/deltas.json) has empty
`baselineGaps`. Its eight exact divergences are deliberate behavior, not missing
features; projections and canaries remain executable. Callouts, block
identifiers, inline-footnote recognition and dimensions retain product-owned
goldens where the selected oracle has no implementation. Properties use the
pinned YAML oracle only over the supported intersection.

The official help snapshot remains `obsidianmd/obsidian-help` commit
`d780d6b48a92ee6a150304b40ee888f322bf43bf`, read 2026-09-03, as recorded in the
policy. All OFM feature rows are `present`. The root README (also the Swift
consumer guide), Kotlin README and ES README describe the implemented subset
and its parser/consumer boundary. The unreleased 3.0.0 changelog records the
model migrations; no release is published by this closure.

## Validation

Completed on macOS arm64, 2026-09-09, using the repository-pinned toolchains.

| Check | Result |
| --- | --- |
| C `correctness` / `conformance` | 78/78 and 2/2 |
| `correctness-asan`, `correctness-ubsan`, `correctness-tsan` | 78/78 each, including strict OOM and new boundary probes |
| Swift `test:swift-macos`, `conformance:swift-macos` | Passed, including external package consumer and the new shared case |
| Kotlin JVM, macOS arm64 Native and Android host correctness/conformance tasks | Passed on all three host targets |
| ES `test:es-node`, `conformance:es-node`, `test:es-browser` | 32 Node tests, 26 conformance tests, browser ESM/Wasm smoke passed |
| `check:oracle-parity` | CommonMark, GFM, remark and Obsidian passed; 32 Obsidian inputs and 8/8 registered divergences reproduced |
| `fuzz:parity` with seed 1 and 400 iterations per oracle | CommonMark, GFM and remark each 400/400 |
| Position places, scope containment, inline source positions, reference order | All existing ledgers hold; no new exceptions |
| `pnpm verify` | Formatting, lint, contracts, Gradle model, version and all repository/CI/test/surface/package audits passed |
| `pnpm release:dry-run` | C, Swift, npm and Maven host artifacts and consumers passed; checksums generated |

The O10 PR's required CI gates cover the remaining supported hosts and full
release aggregation before merge. These local results do not claim an iOS
Simulator, Android device, Linux or Windows run. This follows the landing
plan's rule to mark an implementing item complete in its PR, before merge;
release publication remains separate.
