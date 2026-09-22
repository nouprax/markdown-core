# Review of the last 15 commits and #354–#363

Review baseline: `5a154070`. The reviewed Git range is
`49310b75^..5a154070` (15 commits); the comparison parent is `49310b75^`.
Issues and pull-request discussions were read on 2026-09-21. This review treats
the changes as one parser lifecycle, including the rollback in #309, the
binding representations it restored, and the benchmark's changing corpus.

## Findings and repair

### P1: speculative input and consumed input disagreed about NUL

The ordinary source driver replaced NUL with UTF-8 U+FFFD, but table lookahead
retained raw NUL. A table could copy those raw bytes into a mapped cell, whose
driver then expanded them after the mapping had been established. Coordinates
could consequently resolve onto another line. The baseline Debug binary
aborted at `source_line == line`; Release hid that invariant violation.
Inline cells could also see the NUL as an end-of-input byte.

Differential testing found this independently of the issue proposals. The
failure was reproduced on the unmodified baseline and minimized; it also
reproduces after removing the malformed UTF-8 byte from the generated input,
so this is not dependent on violating the facade's UTF-8 precondition.

The input index now owns an immutable normalized view for each line that needs
NUL replacement. Driver and lookahead borrow the same view. Views remain valid
across index growth and other line normalizations, and end with the active
input. Properties continues to validate its original authored bytes. Tests
compare NUL with an explicit U+FFFD control across pipe, simple, multiline and
grid tables, LF/CR/CRLF, and blockquotes; the OOM sweep includes those views.

### P2: containment decisions were repeated or their failures ignored

Construction mixed a semantic predicate with ownership transfer. Block parent
selection and link/citation/footnote construction already checked containment,
then checked again during attachment. A stateful predicate could accept the
decision and refuse the commit. Several sites ignored the second result.
The ordinary inline dispatcher also ignored rejection of a detached token,
losing ownership of it. Email rewriting did not establish acceptance before
splitting the original Text.

There is now one callback-free splice. Public mutation validates ancestry and
containment before unlinking; parser callers with an established decision use
the same splice. Fresh built-in containers accept the transferred inline kinds
by their grammar contract. Sites without a proof retain checked attachment.
Email rewriting checks its parent before allocating or changing the Text.
A constructed token that cannot be attached is released and the transaction
fails. Tests exercise rejection, live allocation balance, and a predicate that
accepts only its first Link: one accepted decision must produce one Link.

The follow-up review closes the originally omitted construction boundaries:

| Construction boundary | Proof / retained decision |
| --- | --- |
| `blocks.c:add_child` | `block_parent_for` selects and validates the parent; commit does not repeat its policy. |
| Ordinary paragraph opening | Selects its parent before probing a detached identifier line, then calls the shared validated block constructor. It does not repeat the parent search. Exhausting the ancestry reports semantic refusal before allocating a child. |
| `inlines.c:markdown_core_inline_parse_inline` | Fixed inline owners admit their grammar's token kinds; Debug/ASan checks the shared built-in rule. A dynamic callback is still evaluated for each constructed token because hooks can change it and policies may depend on current children. This is a first decision, not a replay or a cached owner-wide answer. |
| `table.c:table_child` | The only attached pairs are fixed-element Table/Row and Row/Cell; caption construction has no parent. Both kind pairs and the element owner are asserted. |
| Table lead paragraph | The destination is the converted table's parent, not the table. Acceptance for a new paragraph sibling is decided before conversion/allocation. Refusal leaves the original paragraph intact. |
| Link/citation/footnote/formula/autolink rewrites | The existing recognition-time decision authorizes the commit; fresh built-in inline containers admit their transferred children. |
| Arbitrary mutation | Retains ancestry and containment validation before unlinking. The unused `attach_owned` wrapper has been removed; proven construction uses the shared validated splice. |

The shared splice checks pure built-in containment in Debug/ASan without
replaying dynamic callbacks. A subprocess regression deliberately attempts an
invalid attachment; existing stateful-policy tests require one accepted
decision to produce one commit. Fatal consumed-token rejection now retains a
separate error cause rather than being mislabeled as allocation failure.

### P2: formula promotion classified semantic rejection as OOM

`replace_with_formula_block` returned NULL both when allocation failed and
when a parent rejected `FormulaBlock`; its caller abandoned the entire parse.
A code fence with info `formula` and an internal parent policy rejecting
formula blocks reproduces the problem.

Promotion now returns the finish protocol's distinct continue/consumed/failed
outcomes. Rejection preserves the original block, allocation failure fails the
transaction, and successful replacement commits once. Inline formula
construction likewise checks before allocating. These extension policies are
private engine test facilities; the installed facade still exposes one fixed
dialect, not configurable syntax.

### P2: source facts and transient storage had the wrong lifetimes

Root scanning, mapped-input indexing, lookahead, metadata geometry, table
geometry and temporary ordering storage did overlapping work with different
owners. The repairs establish three explicit lifetimes:

1. An active input owns one physical-line index. Geometry is compact; optional
   facts are stored only after grammar or normalization needs them and are addressed
   through the geometry entry. They do not duplicate geometry or form another
   line index. Both reset together when the active input changes.
2. The parse owns reusable table and source-order workspaces. Candidate
   completion resets used state; it does not release capacities. AST columns
   are copied into document-owned values at commitment.
3. Content maps are small borrowed values over parser-owned runs, rather than
   zeroed fake nodes. Every consumer was migrated, including consolidation,
   inline placement, attributes, citations, comments, table leads and email
   splitting.

Properties borrows plain keys, decodes quoted keys when necessary, rejects
unknown/duplicate slots before decoding their values, and preserves the next
member's classification discovered by boundary scanning. Reference prefix
removal is a persistent paragraph fact: a setext probe that consumed the
whole prefix still relocates content arriving later. The ordinary paragraph
path does not repeatedly rebuild unchanged geometry.

The buffer append operation also checks existing capacity before calling the
out-of-line grow function. Overflow and allocation failure semantics are
unchanged. A `strbuf_grow` call count is not a realloc count.

## Decisions against the issue proposals

- Retain #362's slab/cell ownership. Parser disposal must not invalidate a
  detached surviving subtree. Slab ownership is already independent of tree
  edges, with no process-wide mutable allocator state. Replacing it with a
  document-only arena would weaken that lifetime contract.
- Reject the dense rows-by-width region grid proposed in #356. Tall or wide
  sparse tables must retain a width-bounded frontier and output-sized closed
  regions. Reuse the existing stable radix ordering workspace instead; it is
  shared with headings and document definitions.
- Preserve semantic containment callbacks at decision boundaries. An assert
  must not invoke a callback again or erase a legitimate refusal.
- Keep metadata key classification in its decoder. It is a one-token grammar
  fact, not a property of every physical source line.
- Keep the existing attribute value representation. An optional pointer would
  introduce a different allocation and mutation lifecycle across all attribute
  consumers. The proposals do not establish a semantic requirement for that
  change, and corpus frequency alone is not such a requirement.
- Retain the one finish traversal and grammar-required table probes. No new
  whole-tree pass, input-size cutoff, small-table algorithm, or corpus-specific
  syntax gate is introduced.

The first integrated index layout stored all grammar fields on every physical
line. Allocation instrumentation exposed excessive metadata memory overhead;
the final representation separates compact geometry from lazily created
grammar facts under the same input owner. This is an ownership distinction,
not a cardinality-dependent algorithm.

## Follow-up: source frontier, space, and borrowed views

The driver now advances the physical-line frontier through a static inline
scanner in `blocks.c`. The external lookahead wrapper calls that same body.
Geometry shrinks from 20 to 12 bytes per visited line; optional NUL counts use
space in the existing 64-byte fact record. The exact capacity formula and
12–24x LF-only / 6–12x one-character-line amplification are documented in
`parser-input-storage.md`; short-line tests check space and scan work.
The bounded span walk finds NUL/CR/LF with portable word probes and bytewise
boundary resolution; it needs neither input padding nor aligned loads.
Its geometry excludes physical terminators, allowing the mutable grammar
line to be copied and LF-terminated in one reservation. There is no separate
scan algorithm selected by input size or corpus case.

Properties carries an envelope index and reads geometry values. Table dash
slices are offsets with value access. Lookahead reacquires its fact record
after element callbacks. A forced-moving allocator exercises metadata and
table workspace growth; successful ASan runs alone are not the lifetime proof.

CI rebuilds the event base with the current benchmark harness, corpus,
references, compiler flags and runtime libraries. Every case at every scale
has a source-stage Ir budget of 1.02 times its base, independently of total
stage improvements. Raw base/current reports and failing rows are published
before the gate fails. `Required gates` depends on this reusable benchmark
workflow. Reference ratios, formal pairs, workload diagnostics and boundary
splits keep the interpretation established by merged PR #366.

## Re-review of `3119c35b` (2026-09-22)

[Claude's re-review](https://github.com/nouprax/markdown-core/pull/365#issuecomment-5763970631)
confirmed the seven earlier repairs and identified the following follow-ups:

- Table character access is now pure; loops charge scan spans, union-find
  charges visits on return, and source ordering records actual key/pass visits
  instead of a flat `16 * closed_count`. Existing adversarial complexity bounds
  remain in force.
- A real allocator-seam test warms and replays the existing non-committing
  caption/table query for pipe, simple, multiline and grid grammars plus a
  rejected grid. Replays allocate and release nothing while successful queries
  still rebuild geometry. Initial growth and AST construction legitimately
  allocate; the requirement applies to reusable query scratch.
- Source-column identity is checked in a shared inline wrapper. Only mapped
  input enters the mapping function, including from per-line finalization.
- The three remaining `parser->error |= attributes.oom` writes now use the
  first-cause helper. They were latent violations, not a reproduced live failure.
- The claimed public element-extension regression is based on an incorrect
  installation premise: `core/CMakeLists.txt` lists private build headers, but
  installs only `include/markdown_core.h`. The element API already describes
  itself as internal; it now explicitly requires returned inline tokens to
  satisfy built-in containment. Debug/ASan verifies that proof, and a dynamic
  policy is still evaluated once for each token. Release does not replay a
  proven built-in decision.
- Removed the unused checked-detached attachment wrapper and corrected the
  assertion/ownership comments. The three owned-root kind exclusions share one
  definition across built-in and dynamic decisions.
- NUL-bearing short lines now exercise fact capacity and normalized-view
  counts. The documentation already included NUL-bearing facts and explicitly
  excluded AST/allocator memory from its geometry bound. It now gives the full
  input-workspace formula for repeated NUL lines; the reported whole-process
  RSS is a different quantity and does not contradict that bound.
- The source-budget module now explicitly states that moving work outside the
  stage can evade this gate. Complete parse and outside-stage counts remain
  required report context, including the six-byte boundary document.

## Commit review ledger

| Commit | Main area examined | Conclusion |
| --- | --- | --- |
| `49310b75` (#309) | Rollback boundary, C facade and retained ownership, Swift indexed value storage, Kotlin native/JNI projections, ES result decoding, replacement benchmark | Preserve the restored public value contracts; inspect the final composition rather than reintroducing rolled-back representation experiments. |
| `4d64d13e` (#315) | First-byte declarations and finish skip contracts | Preserve descriptor order and conservative admission; source audits and grammar reach checks remain active. |
| `bc11ac06` (#316) | Stage boundaries, full-program versus stage accounting, profiling identity | Keep stage and lifetime costs distinct; local allocation counts are not instruction measurements. |
| `c0e3a90a` (#324) | Same-work corpus pair definitions | Interpret using the later #361/#364 corrections, not the original pair shapes. |
| `2ec15243` (#335) | UTF-8 progress, finish contracts, table gates, kind projection | Preserve fixed grammar and total scanner progress; shared input normalization closes the uncovered NUL composition boundary. |
| `f2bc2d16` (#337) | Linker allocator substitution and all owners | Preserve the three-symbol allocator seam; new reservations and normalized views participate in OOM injection. |
| `4592a1e1` (#338) | Actual Debug integrity activation and parity gates | Run Debug as a separate configuration; Release equivalence alone missed the historical mapping assertion. |
| `145b9522` (#339) | Lookahead accessor and per-visit work | Indexed reads stay inline; extension and allocation occur only when the relevant fact is absent. |
| `9fbe6565` (#342) | Inline hook projection | Preserve per-family descriptor order and existing hook-work invariants. |
| `6a2f3e88` (#344) | Attribute spelling/ownership, finish traversal, frontmatter geometry | Keep attribute semantics; remove the separate frontmatter geometry owner and repeated key classification. |
| `94231b2b` (#347) | Inline placement cursor and source runs | Keep bounded run lookup; represent the borrowed mapping explicitly without changing its algorithm. |
| `6cd9a7db` (#360) | Composed dispatch, finish, prefix gates, table commitment and release paths | Retain completed fixes; repair remaining containment decision/commit boundaries and input-view disagreement. |
| `8a140f43` (#362) | Slab alignment, cell reuse, records outside cells, detached lifetimes, failure cleanup | Retain the model and its slab boundary/reuse/lifetime tests; no global pool or document-only ownership. |
| `cfe8d0ea` (#361) | Seven corrected pairs, split-host measurements and identity | Use the corrected current corpus, including the subsequent attribute-member correction. |
| `5a154070` (#364) | Attribute-member pair and repetition proof | Preserve the member comparison and the explicit unpairable list remainder; current reach and pair audits pass. |

The binding review includes native-handle release, copied versus retained
values, resource sharing, iterative traversal/decoding, optional fields and
canonical field ordering. No public header, wire schema, binding model or
package version changes are required by this repair. Cross-platform projection
and source-list audits remain the integration gates.

## Issue disposition

| Reference | Disposition in this repair |
| --- | --- |
| [#354](https://github.com/nouprax/markdown-core/issues/354) | Slabs already landed in #362. Complete the proven decision/commit split and rejection cleanup; retain checks where needed and retain attribute values. |
| [#355](https://github.com/nouprax/markdown-core/issues/355) | One lazy physical input index for root and mapped inputs; shared lookahead facts, properties geometry and normalized line views. Remove mapped eager rescanning and redundant line clearing. |
| [#356](https://github.com/nouprax/markdown-core/issues/356) | Reuse table geometry/candidate/frontier/sort storage. Keep sparse regions instead of the proposed dense grid. Keep deterministic scan accounting. |
| [#357](https://github.com/nouprax/markdown-core/issues/357) | Borrow plain keys, preserve quoted decoding, reject slots early, share boundary classification. |
| [#358](https://github.com/nouprax/markdown-core/issues/358) | Already completed by #360. Preserve bounded numeral scanning and conditional memo release; existing adversarial tests pass. |
| [#359](https://github.com/nouprax/markdown-core/issues/359) | Persistent consumed-prefix fact and explicit content-map value, including all other mapping consumers. |
| [#360](https://github.com/nouprax/markdown-core/pull/360) | Treat as the integrated baseline, not pending work. Audit and preserve its dispatch/finish/gate invariants. |
| [#361](https://github.com/nouprax/markdown-core/pull/361) | Preserve the corrected measurements, with #364 and the proof-domain review in #366 applied. Rebuild both revisions on the current corpus; no comparisons against mixed corpus identities. |
| [#362](https://github.com/nouprax/markdown-core/pull/362) | Retain slab ownership and its regression coverage. Its measured improvement must not be claimed again for this change. |
| [#363](https://github.com/nouprax/markdown-core/issues/363) | Historical map measured primarily at `6cd9a7d`; distinguish confirmed costs from candidates and preserve its measurement caveats. Address shared ownership costs and unnecessary grow calls; do not assert an unmeasured new Ir ratio. |

## Initial verification snapshot (`5e37274`, before #366)

The measurements in this section were captured for the initial PR revision
`5e37274` against `5a154070`, using the old 268-document corpus and 20-byte
geometry records. They predate the follow-up's 12-byte geometry, input workspace
initialization and completed construction decisions. They are historical
diagnostics, not measurements of the current head. The PR description and
required CI benchmark publish the current 464-document comparison, rebuilt
against main with #366's proof and boundary contracts. Read its full parse-path
and outside-stage counts alongside the two stages; initial index reservation
is now part of parser initialization rather than the source stage.

The repair adds deterministic geometry, metadata rejection, workspace reuse,
stateful containment, rejected-token ownership, consumed-prefix and normalized
table regressions. Existing tests continue to cover sparse grids, deep owned
trees, slab reuse/detachment, source-map cursor bounds, stable ordering, and
failure at every allocation boundary.

The exact baseline was built from `git archive 5a154070`, separately from the
modified checkout. The generated `--scale 2` corpus has 268 files. Another
512 deterministic inputs combine three samples, change line endings, omit
final endings, add blockquote prefixes and inject NUL/invalid bytes. The latter
are robustness probes, not an expansion of the valid-UTF-8 public contract.
Of 780 comparisons, 778 preserve the baseline canonical dump byte-for-byte.
The two intentional NUL corrections match the baseline's explicit U+FFFD
control input byte-for-byte. The regression tests use valid UTF-8 plus NUL.

Validation is recorded for Release, Debug, ASan and UBSan (90 CTest cases per
configuration: 88 correctness and two conformance), the strict warning build,
46 SwiftPM tests, 120 script tests, source/ownership/projection audits, and
current corpus pair/reach audits. The CommonMark and GFM parity gates pass
714/714 and 97/97 inputs respectively under their registered dialect deltas.
Installed oracle checkouts are the repository-pinned cmark and cmark-gfm commits.

A local diagnostic substituted the same allocator seam in the baseline and
initial PR Release static libraries. It counted allocation/reallocation calls and
peak requested live bytes for parse and document disposal, excluding the input
buffer and canonical dump. Across those same 268 corpus files, calls decreased
from 4,423,051 to 4,129,242 (6.64%); final live bytes were zero for every file.
Representative scale-2 cases show both the benefit and the retained-index cost
of that initial snapshot:

| Corpus case | Allocation calls, baseline → initial PR | Peak requested bytes, baseline → initial PR |
| --- | ---: | ---: |
| `block-table-grid` | 28,807 → 8,872 | 4,931,098 → 5,052,138 |
| `block-table-simple` | 27,177 → 11,519 | 7,776,218 → 8,008,490 |
| `block-table-multiline` | 16,396 → 7,941 | 4,477,418 → 4,598,010 |
| `block-metadata` | 10,369 → 10,373 | 3,916,110 → 3,891,998 |
| `pair-metadataempty-dialect` | 5,358 → 26 | 211,487 → 246,946 |
| `block-list-flat` | 19,157 → 19,169 | 11,159,194 → 10,045,546 |
| `block-code` | 41 → 54 | 988,978 → 1,644,386 |

These are allocation diagnostics, not timings, RSS measurements, or a universal
memory improvement. The empty-metadata case is particularly explicit: removing
per-key allocation still retains physical geometry, so its peak rises by 17%.
Separating geometry from optional facts reduced that case from the intermediate layout's
869,514 bytes to 246,946. Code-only inputs also retain the new physical index;
the shown case grows by 66%. Optional facts are absent when no grammar or
normalization query needs them. Deterministic tests gate scan work, scratch reuse and ownership rather than
encoding these corpus-specific counts as implementation branches.

The host is macOS arm64, using Apple Clang 21 and Swift 6.3.1; the repository's
CI pins are not replaced by those local versions. Linux Callgrind was not run
for this initial local snapshot, so it claims neither a speedup nor a new
instruction ratio. Kotlin platform
tests and an ES/Wasm rebuild require host toolchains not present in this
session; static binding/projection review and Swift execution do not stand in
for those runtime matrix jobs. The full packaging/toolchain verification
matrix remains a CI responsibility.

See [parser-input-storage.md](../architecture/parser-input-storage.md) and
[node-storage.md](../architecture/node-storage.md) for the final ownership
contracts. GitHub issues were researched; this task does not post comments or
change their state.
