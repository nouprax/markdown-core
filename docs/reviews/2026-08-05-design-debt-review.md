# Design-debt review — coordinate spaces and the delimiter engine's chains

> **PARTLY SUPERSEDED — 2026-08-07.** This review's cure for the dual
> coordinate space is "M3 therefore eliminates the dual space from everything
> that persists: extent identities become the stored truth", and it quotes
> §7.2's "a domain-qualified ordinal over the aggregate sequence" — a phrase
> the contract no longer contains. §7.2's mechanism prescription was removed
> on 2026-08-07 and the landing that implemented it was reverted; see
> [`2026-08-07-requirement-audit.md`](2026-08-07-requirement-audit.md). The
> DIAGNOSIS — that two coordinate spaces are maintained per node and that this
> is where the fix friction came from — is not superseded, and neither is the
> delimiter-chain half. Only the cure and its action register are.

Date: 2026-08-05. Base: `main` at `9af16c9` (the merge of #92, closing M2.5).

Provenance: this review was triggered by the review round of the #90–#92
stack, in which the fix friction — reviewer findings, a fuzzer-exposed latent
crash, and the shape of the changes they forced — was decomposed cause by
cause. Two recurring structural causes account for most of it and are the
subject of this document; the rest is attributed in "Non-debts" below rather
than left as vague unease. Every claim below was
verified against the base commit (file references re-checked, the crash
reproduced at the pre-fix merge-base under UBSan, and the incident fixes are
in merged history). The purpose of the document is to separate what is real
design debt — with its complete evidence chain and an elimination path — from
what merely looks like debt, so neither is re-litigated from scratch later.

Verdict up front: both debts are eliminable to a stated, spec-mandated
residue. Neither requires abandoning the core model (one physical CST,
records on region nodes, region-relative coordinates), which every gated
slice to date has confirmed rather than contradicted. Two candidate
"debts" are recorded as non-debts with reasons.

## Debt 1 — two column spaces without a type boundary

### The two spaces

The engine carries two meanings of "column" in the same `int`:

- **Byte columns.** Node positions: `add_child`'s `start_column` argument is
  `first_nonspace + 1`, a byte offset plus one; a finalized `end_column` is
  the last line's byte length. Concrete records deliberately use byte
  offsets in the normalized line (`core/concrete_records.h`, the coordinate
  contract): records do not tab-expand.
- **Tab-expanded columns.** The block scanner's `parser->column`,
  `first_nonspace_column`, and `indent` implement CommonMark's tab-stop
  rule (a tab advances to the next multiple of four). The per-paragraph
  line marks store this expanded column (`core/parser.h:107`), and
  `S_content_position` (`core/blocks.c:538`) maps buffered content offsets
  into it.

Nothing in the type system, naming, or review rules separates the two. The
line-mark struct itself documents the split it bridges: "`column`
tab-expands, concrete records do not" (`core/parser.h:111-115`).

### Incident ledger

- **E1 (#89, shipped).** The table's look-back header capture needed record
  columns from buffered content, and expanded columns were the only thing
  the marks stored — so #89 added `byte_offset` to the line marks as a
  bridge field. The bridge worked only for the unpadded case (E2).
- **E2 (#90).** A lazy continuation whose matched prefixes stop inside a
  tab buffers stand-in spaces; the affine buffer→line map was off by
  pad−1 for every byte past them. The gates-first commit proved the
  shipped #89 look-back mis-mapped such lines (pipe columns landed one
  stand-in space to the right, one record escaping its line). Fix: the
  marks grew `pad` (`core/parser.h:123`) and the byte-exact map became the
  shared helper `markdown_core_line_mark_extent` (`core/parser.h:137`,
  impl `core/blocks.c:456`).
- **E3 (#91, reviewer finding).** The split-off table lead's `end_column`
  was computed from the mark's expanded column while every other node
  position is byte-based: a tab-prefixed lead reported `1:3..1:13` where
  the same paragraph unsplit reports `1:3..1:11`. Fixed by mapping the
  last byte through the extent helper.
- **E4 (standing, unfixed by design).** Three writers still persist
  expanded-space positions into the tree: `ReferenceDefinition` node
  positions from `S_content_position` (`core/blocks.c:665`, `:702`), the
  harvesting paragraph's rebased start after a harvest (`:792`), and the
  split table's re-dated start, which reads the header mark's column
  directly (`extensions/table.c:558-559`, whose own comment names the
  column tab-expanded) — while sibling nodes are byte-space: on
  tab-bearing lines one tree carries both conventions. This is left for
  the M3 unification rather than patched, because the fix belongs to the
  structure M3 replaces.

Three shipped defects and one standing inconsistency, all one cause: an
`int` that means two things, converted ad hoc at each boundary.

### Elimination path

The public contract already mandates the end state: "Nodes do not store
absolute source positions. Scopes are resolved on demand through the owning
document" (`docs/specs/canonical-ast.md:43`), and §7.2 of
`docs/specs/incremental-canonical-ast.md` defines extents as identity with
five explicit coordinate profiles. The C layer's per-node `line`/`column`
fields are implementation residue of the pre-CST engine, not contract.

M3 therefore eliminates the dual space from everything that persists:

1. Extent identities become the stored truth — and an extent "carries no
   coordinates by construction" (§7.2 defines `SourceExtent` as a
   domain-qualified ordinal over the aggregate sequence; a stored numeric
   range would go stale on every preceding edit, which is the whole
   reason extents exist). Byte offsets and line/column alike become
   on-demand resolutions through `Document.scope` under an explicit
   profile: "which column semantics" turns from an accident into a typed
   parameter.
2. The line marks lose their `column` field; `S_content_position` produces
   byte positions (or extent-relative offsets), unifying E4 — refdef and
   split-table columns on tab lines change once, under golden review.
3. The quarantine rule becomes checkable: **no structure that outlives the
   scan of one line may store a tab-expanded column.** Expanded columns
   remain scanner-local (`parser->column`, `indent`,
   `partially_consumed_tab`) because CommonMark defines indentation in
   columns — that residue is language-mandated and is the complete list.

Per the milestone rule ("For each rule a milestone adds, name the gate that
fails when it is violated"): the record side is already held by the capture
gates, which dereference every record against normalized line bytes; the
node side is held by M3's §14.3.2/§14.3.3 gates once positions resolve
through extents; the quarantine rule needs a structural audit (a review of
struct fields naming column semantics) added in the M3 slice that deletes
`line_marks.column`.

## Debt 2 — the delimiter engine's hand-maintained chains

### The structure

Delimiter records live in a relocatable arena where "every relationship is
an integer id (zero is null)" (`core/delimiter.h:11-14`). Each record
carries **seven** id fields (`core/delimiter.h:67-73`): the main chain
(`previous`/`next`), the per-lane rule chain (`previous_rule`/`next_rule`),
the open stack (`previous_open`), and two push-time snapshots used by
truncation (`push_previous_rule`, `open_top_before`). Ids solved the
arena-relocation problem the header names — growing the arena invalidates
no pointer — but ids without generations created its successor: truncation
(`truncate_to_mark`, `core/delimiter.c:681`) shrinks the live count and
later pushes **reuse the reclaimed indices**, so any surviving stale id is
a textbook ABA reference.

### Incident ledger

- **E5 (latent on main, fuzzer-found during the #90 round).** Truncation
  clamped the main chain's surviving tail (`tail->next = 0`) but never the
  per-lane rule chains: a surviving lane tail kept a `next_rule` naming a
  reclaimed record, and a later removal of that survivor wrote through
  `record_at`'s NULL for the stale id. Linux Release crashed; macOS
  Release happened to survive the same UB; UBSan pinpoints the write. The
  crash predates the stack (reproduced at the merge-base) and was reached
  only when the seeded parity fuzzer's recombined corpus shifted. Fixed by
  clamping every lane tail's `next_rule` (`core/delimiter.c:706`), with
  the crashing input pinned in `regression.txt`.
- **E6 (found fixing E5).** `lane_count` is set at `engine_begin`
  (`core/delimiter.c:336`) while `lanes` allocates on the first push
  (`ensure_lanes`, `core/delimiter.c:351`) — a unit that begins but never
  pushes truncates with `lane_count > 0, lanes == NULL`, and the first
  version of the E5 fix dereferenced that half-initialized state (caught
  by the OOM sweeps). The shipped fix guards on `engine->lanes`.
- **E7 (asymmetry as evidence).** The main chain's clamp already existed;
  the rule chains' did not. When an invariant is maintained by convention
  at each mutation site, each new chain re-rolls the dice.

### Elimination path

The crash class — stale ids dereferenced as UB — is structurally
eliminable; the chains themselves are kept deliberately.

1. **Generational ids.** Each arena slot carries a generation, bumped on
   reclaim; `record_at` validates it, so a stale id resolves to NULL by
   construction, everywhere, forever — not because every mutation site
   remembered to clamp. Cost: four bytes per record (or eight bits stolen
   from the 32-bit id — a unit's record count never approaches 2²⁴), and
   one compare on an already-loaded cache line per `record_at`.
2. **Mechanical invariant checking.** The validator already exists:
   `markdown_core_delimiter_engine_validate` (`core/delimiter.h:213`,
   impl `core/delimiter.c:812`), compiled under
   `MARKDOWN_CORE_DELIMITER_DIAGNOSTICS`, checks exactly this invariant
   set — every reachable id within the live count, doubly-linked
   coherence on both chains, every lane tail's `next_rule == 0`, `active`
   consistency — and the standalone runner already calls it at its
   checkpoints. That makes E5 doubly instructive: the checker that would
   have flagged the broken chain predates the bug's discovery; no test
   drove the truncate-then-remove sequence through it. The slice's work
   is therefore coverage, not construction — call the validator after
   every mutating operation in diagnostics builds, and add randomized
   operation-sequence cases to the runner. The acceptance criterion is
   mutation-shaped: removing any single clamp or unlink line must fail
   the validator suite, not merely the one pinned regression input.
3. **Lifecycle collapse.** `ensure_lanes` moves into `engine_begin` (lanes
   are parser-lifetime with retained capacity — the allocation happens
   once either way), deleting the half-initialized state of E6 and the
   `engine->lanes &&` guard it forced.

**Kept deliberately: the chains.** O(1) removal and skip is this engine's
reason to exist — it replaced the O(n²) delimiter walks of the ancestry,
and the `pathological_complexity_*` gates sit on that property. The
alternative (per-lane tombstoned arrays, no forward pointers to dangle)
was considered and rejected: tombstone skipping degrades under adversarial
run patterns, which is a bet against the complexity gates for a safety
property the generational ids already provide at negligible cost.

## Non-debts, recorded

- **The parity-registration ceremony** (deltas + input-keyed divergence +
  corpus fixture + fuzz-fragment exclusion + contract-doc row) is the
  fail-closed doctrine working, not friction to remove. In this one review
  round the machinery caught, respectively: a real latent engine crash
  (the fuzzer), an upstream-inherited semantic bug invisible to upstream's
  own suite (the title rewind), and dead guard arms (the coverage gate).
  Every step of the ceremony fired for a reason this cycle.
- **The weird-case inventory belongs to the inheritance, not the design.**
  The recurring "strange cases" of M2.5 divide cleanly. A lead's `\\|`
  unescaped twice and a rewound title kept in the reference map are
  inherited cmark-gfm defects that this project's fidelity goals forced
  into the open. The comment block deleted with its authored tail text
  was this repository's own pre-CST `strip_html_comments` option
  (baseline-era, no upstream counterpart, so no parity gate could have
  caught it), retired in #92 for parse-time classification. Each is now
  either fixed and registered as a deliberate difference or pinned by a
  conformance case; none originated in the CST model.

## Action register

1. **Engine hardening slice** (generational ids + the existing validator
   called at every mutation + lanes-at-begin): a standalone slice,
   independent of M3, proposed as the next engine change. Eliminates the
   E5/E6 class structurally.
2. **M3 named goal — coordinate-space retirement**: recorded in the
   delivery plan's M3 section alongside this review, so the extents
   milestone is held to deleting `line_marks.column` and unifying E4, not
   only to adding extents.
3. **Quarantine audit**: lands with the M3 slice that performs (2) — the
   review rule is stated above; its checkable form is part of that slice's
   gate work.
