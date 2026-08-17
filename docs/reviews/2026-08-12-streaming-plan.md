# The streaming plan

**Date:** 2026-08-12 · **Baseline:** main = `4bf8c9e` · **Status:** adopted, governs milestones P0–P6

The complete implementation plan for the streaming-only redesign. This plan absorbs the
full conclusions of two rounds of adversarial verification, seven passes in total (four
against design D′, three against design S2). Every mechanism below is annotated with the
verification finding it comes from. The superseded D′ assessment ("anchor-window
reparse") is retained as decision history; this document replaces it as the plan of
record.

Requirements this plan serves (user-stated; they override the frozen specs):

- **R-A** — `document.concrete` / `document.semantic` are each reactive-ready
  (SwiftUI / Compose / React); path A is primary.
- **R-C (streaming)** — the hot path is LLM streaming: repeated `append`, each append
  producing a new tree. Edit scenarios tolerate full-parse latency. There is no
  incremental edit mode, ever (see D2). R-B (Commit/delta as a byproduct) is deleted
  together with delta and commit themselves (D4).

---

## 1 · Frozen decisions

Axiomatic. Changing any of these returns the work to the design layer; no deviation
during implementation.

**D1 — Unified mutation semantics: `edit` and `append` are two mutations under one
lifecycle rule.** Both: belong to the same chain, the same series; advance revision by
strictly +1 (**a per-chain plain counter; the shared series clock is deleted** — its only
reason to exist was fan-out, and fan-out no longer exists); **SUPERSEDE their
receiver**; and fail deterministically on a stale receiver (generation check). No
fan-out, no two lines of descent, no forking. The consumer's mental model is one
sentence: *"a mutation advances the chain, old handles die, decoded values live
forever."*

**D2 — `edit` is always full, and its parse IS the warmup.** No incremental edit is
built (anchors, resync, window diff, relative coordinates: none of it, at any stage).
`edit(text)` = full parse + whole-tree id matching (id continuity is preserved — editor
UI ForEach state survives whole-text replacement). On success **the successor stays
warm** (parser stopped at EOF) — continuing to append after a correction does not pay a
second parse. "Warm" is a constant property of the chain: every live head is warm; there
is no cold/warm document distinction.
> **Switch B** — if it is confirmed that no consumer needs id continuity across
> whole-text replacement, `edit` is deleted entirely (document only appends): the
> whole-tree matcher and the full-tree hash stamp die with it (parse 6–11% faster), and
> `Document(text)` opens a stream directly. A′→B can be flipped at any time; the reverse
> is expensive. Default is A′ (unified semantics, edit kept).

**D3 — `append` is a total method on Document; chained ownership.** `append(chunk)` is
legal on any live head. A superseded receiver supports only `free`; its node handles
become invalid the instant the next mutation begins (spelled out as UB-by-wording at
the C layer; neutered at the binding layer). Underneath is a reference-counted chain
owner (arena + warm parser + generation + the previous snapshot's remnants); handles
are thin shells; freeing a superseded handle is an O(1) refcount decrement. **Linear
history is runtime-enforced** (stale mutation errors), which is what makes the bindings'
destructive mirror legal on both paths. Honest naming: the chain owner is a
session-shaped memory layout without any observable session semantics (no open/commit
two-phase, no pending semantics; the projection is always a function of
`(text, options)`, pinned by the oracle). The old spec §8.2 streaming prohibition is
repealed. **The reversal of the previous plan must be stated explicitly:** "the edit
contract survives verbatim / TSan `shared_edit` survives" was overturned by the
consumer-consistency requirement — the concurrent-shared-receiver edit capability is
deleted and the gate rewritten to "serialized mutation + stale error"; the mechanism
had to be built for `append` anyway, `edit` joins the same rule, zero new machinery.

**D4 — Delta and Commit are removed entirely.** `(id, subtree-covering revision)` is
the whole update protocol. Bindings do revision-pruned decode: pruned after `append`
(append never moves bytes; adversarial pass S6 proved the warmup full parse also
satisfies this), full after `edit`. diff.c keeps id matching + revision stamping;
row emission is deleted.

**D5 — A failed `append` poisons the chain.** "The chain is done": subsequent appends
and queries fail, only `free` is legal; the caller holds the text, recovery = rebuild.
Isomorphic to the 2026-08-09 allocation-failure decision. The failure semantics of
`edit` / `document_new` are unchanged.

**D6 — The universal per-snapshot fallback = full parse of bytes-so-far.** Any shape the
fork mechanism does not cover takes a full parse for that snapshot, oracle-equivalent.
The heart lands construct by construct, the fallback always exists, and "never worse
than a full parse per append" is a hard promise.

**D7 — Deleting the ref_size budget is a hard precondition, with independent parity
evidence.** Three mechanisms make S2 unsound while the budget survives (the warm parser
never runs `finalize_blocks`' cap stamp; clone double-accounting creates phantom
budget consumption; the probe argument depends on answers being a function of
definedness only). If the parity corpus blocks deletion: restore the deleted engine's
uncap/restore/estimate protocol (`26045be^:incremental.c:2532-2605`) — one of the two,
decided now.

**D8 — The CLEAN_START mechanism is deleted whole.** Streaming never restarts the
parser. The grant/strip/transfer chain, the per-line clean computation, the node flags,
and the dead index go together.

**D9 — The sole author of identity is the frontier diff.** "The same node keeps the
same id" is only the common outcome of diff pairing, never fiat. Kind change =
retire+mint (setext/table/formula oscillation follows this); vanish-then-return mints a
fresh id (ids never resurrect); **every touched unit's child sequence goes through
diff.c's sweep — nothing is wholesale re-minted anywhere** (otherwise one flip destroys
every inline id in the mentioning unit). The frontier diff must use `diff_push`'s
original budgets and criteria, with settled same-object treated as pre-paired — this
guarantees provable equivalence to a full-tree diff of the two snapshots, and the
"same trees ⇒ same diff" doctrine survives.

**D10 — The old frozen specs are archived; the new contract is
`docs/specs/streaming-and-documents.md`.**

---

## 2 · Glossary

| Term | Definition |
| --- | --- |
| **settled** | A node truly finalized at the block layer. Subsequent appends do not change its block structure; its inlines may change only via flip-driven re-refine. |
| **open chain / spine** | The OPEN chain from root to the deepest open block; length = nesting depth. |
| **chain owner** | The refcounted native object: arena + warm parser (open chain on the tree, linebuf, line numbers, the two live definition tables, line_marks, diagnostics vector, total_size) + generation + the previous snapshot's frontier remnants and definedness sets. Snapshots are thin handles onto it. |
| **snapshot** | The document produced by one append. Correctness: canonical dump ≡ a one-shot parse of bytes-so-far (identity/revision excepted). |
| **parser-tail fork** | The mechanism that materializes a snapshot (§4.2): clone all OPEN nodes (root down, settled children shared), swap in forked parser scalars + a line_marks copy + a shadow diagnostics tail + the overlay table chain + shadow failure flags, run one `S_process_line` over a **copy of linebuf**, run `finalize_blocks`' unwind minus the max_ref_size stamp, refine the frontier, swap back. |
| **frontier** | The tentative subtrees this snapshot exposes (the fork's clones plus nodes created by the half line) + units newly settled this append + units re-refined by a flip. |
| **overlay** | Snapshot-scoped definition staging (produced by harvest inside the fork). The snapshot's definition set = live tables ∪ overlay; flip comparison is by normalized label and **never dereferences the owner** (which points at a tentative node that dies with the snapshot). |
| **flip** | A change in a label's definedness membership between adjacent snapshots' definition sets (per table). Winner/url/title/order changes are not flips. Both directions can only come from the overlay (live tables only grow during feed). |
| **probe list / re-refine** | As in the prior plan: 64-bit label hashes (hits AND misses, per table) recorded during the unit's own inline parse; on flip, a dense scan; hit units rebuild their inline domain under the current definition set, **the unit keeps its id and its child sequence goes through the diff sweep** (D9). |

---

## 3 · Final API and lifecycle

### 3.1 C layer

```c
/* Opens a chain. Produces the chain's first live head (warm: parser at EOF). */
markdown_core_document *markdown_core_document_new(
    markdown_core_string markdown,
    const markdown_core_parse_options *options,
    markdown_core_error **error);

/* Whole-text mutation. Full parse + whole-tree id matching; SUPERSEDES the
 * receiver (D1); the successor is warm (the parse is the warmup).
 * Same chain, same series, revision +1. Stale receiver → deterministic error. */
markdown_core_document *markdown_core_document_edit(
    markdown_core_document *document,
    markdown_core_string markdown,
    markdown_core_error **error);

/* Append mutation. Incremental (§4); SUPERSEDES the receiver; revision +1.
 * Stale receiver → deterministic error. Failure poisons the chain (D5).
 * Arbitrary byte splits are legal (mid-UTF-8 / mid-CRLF / mid-line). */
markdown_core_document *markdown_core_document_append(
    markdown_core_document *document,
    markdown_core_string chunk,
    markdown_core_error **error);

/* On a superseded handle this is the only legal call, O(1). Releasing the last
 * handle on a chain reclaims the chain owner (arena + warm parser + remnants)
 * as a whole. */
void markdown_core_document_free(markdown_core_document *document);
```

- **Threading (one unified sentence):** "A mutation (edit/append) is an exclusive
  operation on its chain: all access to any handle on the chain must happen-before the
  mutation or happen-after its return; two mutations must be externally serialized.
  Concurrent read-only access to live heads outside mutation windows is safe; decoded
  platform values are pure values, always safe." The concurrent-shared-receiver edit
  capability (pinned by the TSan `shared_edit` gate) is deleted; the gate is rewritten
  (§7).
- **series/revision (unified):** one chain = one series = one strictly-+1 revision
  line; edit and append are indistinguishable here; ids are never reused or
  resurrected; the shared series clock is deleted (`document.c:293-357`).
- **No finish, no cold documents:** every live head is complete and warm; warm state is
  held by the chain owner until the chain dies — a tail of O(largest leaf) memory,
  written into the contract.
- **options:** fixed when the chain opens, immutable within it; different options = a
  new chain (new series).

### 3.2 Bindings

- **No new types.** All three platforms' Document gains `append(chunk) → Document`.
  The ES streaming example simplifies to `doc = doc.append(data)` (no self-held
  accumulated string).
- **Superseded neuter:** after a successful append the receiver wrapper enters the
  superseded state — close/free becomes an O(1) refcount decrement, native access
  paths are severed (all three platforms' decoded values are already pure; node()/dump
  go through decoded state, verified not to touch native); the ES FinalizationRegistry
  registration target becomes the chain.
- **Revision-pruned decode + destructive mirror:** the mirror (id → platform value)
  travels with the chain; after an append, a pruned traversal from the new root —
  (id, rev) hits take the old value, the new mirror is produced by the traversal
  (vanished ids simply never enter it, zero leaks); legality = D3's runtime-enforced
  linear history. After an edit: full decode, mirror rebuilt. **A D6-fallback append
  (that snapshot took a full parse) needs no special flag:** append never moves bytes ⇒
  values for unchanged (id, rev) are byte-identical including scope, so pruning stays
  safe (adversarial pass S6 proved this). Under unified semantics there is no
  "cold-append warmup" — the only two full parses are `document_new` and `edit`, and
  the bindings decode fully at exactly those two points.
- **Kotlin wire MKC5:** delete the delta section; add the append payload: the C encoder
  accepts a baseline revision, subtrees with `rev ≤ baseline` emit a single
  `REUSE(id)`; edit payloads use baseline = 0. Kind numbering append-only;
  `checkKotlinAbi` passes one major break. ES gains the `es_document_append` bridge
  entry point.

---

## 4 · Engine mechanics

### 4.1 Invariants (all gated)

- **INV1 (oracle):** snapshot dump ≡ fresh parse of bytes-so-far.
- **INV2 (warm state bit-exact):** the warm-state fingerprint is equal before and after
  snapshot materialization (open-chain structure + flags, linebuf bytes, line numbers,
  both tables' contents, line_marks, diagnostics count).
- **INV3 (settled stability):** settled nodes change only via flip re-refine, and only
  in the inline domain.
- **INV4 (equal means identical):** between adjacent snapshots, equal (id, rev) ⇒ equal
  subtree dump. **Including VALUE bits:** guarded by the value-mirror gate (pruned
  decode ≡ full decode) — the only oracle that can see the `List.tight` defect class
  (dump and the revision mirror are both blind to it; adversarially proven).
- **INV5 (coordinates always true):** append does not move bytes; settled coordinates
  are valid forever; no relocation mechanism exists.

### 4.2 Snapshot materialization: the parser-tail fork (the heart)

"Clone the open leaf + recompute spine fields in place" was falsified by adversarial
review: the linebuf half line is processed as a real line by a fresh parse
(`blocks.c:1198-1201` flush → `S_process_line`), and it can open new blocks at any
depth, retype via setext/table, close the entire chain (triggering harvest / content
detach / definition-node insertion into the parent list / paragraph vanish), register
footnote definitions, and write `LAST_LINE_BLANK` down the whole spine. Moreover,
harvest inserts `ReferenceDefinition` into the **parent's** child list, and a failed
table probe permanently writes `TABLE_VISITED` on real nodes (one half-line probe =
never a table again). Therefore:

**Mechanism: the per-append parser-tail fork.**
1. Clone all OPEN nodes (path copy from root down; the clones' settled child pointers
   share the real nodes, read-only);
2. Swap in: the fork's `root`/`current`, copies of the parser scalars, **a copy of the
   line_marks array** (harvest and the table look-back read its contents), a shadow
   diagnostics tail (watermark), the overlay table chain, and shadow
   `oom/capture_lost/internal_error` flags;
3. Run one `S_process_line` over a **copy of linebuf** (the signature takes
   buffer+len; linebuf is not consumed);
4. Run `finalize_blocks`' unwind, **minus the max_ref_size stamp**;
5. Refine the frontier (inlines + per-unit postprocess for the clones and the
   half-line-created nodes; definition set = live tables ∪ overlay);
6. Swap back.

Cost: O(spine + open leaf + half line + frontier). The real `finalize()` /
`refine_blocks` are not modified by a single line — they are single-shot constructions
(refine frees the root on OOM, detaches it on success); the fork pipeline is parallel,
dedicated code. The plan states this explicitly.

- **Diagnostics:** an append-only vector on the chain + a per-snapshot watermark; fork-
  time diagnostics go to the shadow tail; a snapshot reads "main vector[0..wm) + shadow
  tail", discarded at swap-back. Diagnostics of flip-re-refined units: milestone P3
  moves directive diagnostics onto the unit node (following the `inline_concrete`
  domain-swap precedent), with document order assembled at query time — otherwise
  re-refine would re-record out of order into the shared vector.
- **VALUE field checklist:** tentative-written VALUE-bearing fields (today exactly:
  `List.tight`; any future spine-level VALUE field joins the list) must have revision
  driven by the frontier diff's field comparison — the fork satisfies this naturally
  (comparison happens between this clone and the previous snapshot's clone). The
  checklist is maintained as a checked list to keep future fields from reopening this
  hole.
- **The two O(children) terms — hash and tightness** (adversarial pass M4): recomputing
  root/upper-spine `subtree_hash` is O(top-level children) per append and has no
  consumer — **defer via a dirty flag**, recompute on demand when an edit fork or
  another hash consumer arrives (a stale hash affects only pairing quality, never
  correctness). List tightness folding is O(items) per append — maintain an
  **incremental tightness accumulator** on warm state (only the last item's blank state
  ever changes).
- **Failure domain:** every failure inside the fork goes to the shadow flags; snapshot
  failure ⇒ this append fails ⇒ chain poisoned (D5), so no warm-state restoration
  promise is needed; the shared delimiter engine must leave empty (`engine_begin`
  rejects a non-empty engine).

### 4.3 The per-append pipeline (canonical order; stage exclusivity is a hard rule)

1. Generation/poison check; append chunk to source.
2. **Feed:** the existing `parser_feed`; blocks truly closed during it are collected by
   a settled-list hook (real finalize runs normally; definitions enter the live
   tables).
3. **Flip set:** the definedness difference between (live ∪ overlay)ₖ and the chain's
   retained (live ∪ overlay)ₖ₋₁, per table (O(1) amortized: counts + order-independent
   hash; 99% of appends return immediately with no flip).
4. **Re-refine:** if the flip set is non-empty ⇒ probe-scan settled units;
   **excluding the frontier and this append's newly settled units** (the former are
   parsed by the fork itself under setₖ; the latter have no probe list yet — the
   3→5 ordering is load-bearing, not incidental); the unit keeps its id, its child
   sequence goes through the diff sweep (D9).
5. **Inlines for the newly settled:** **under live ∪ overlay** (the "paragraph closes +
   trailing half definition in one append" shape: a fresh parse runs inlines only
   after the EOF harvest — omitting the overlay here is an oracle divergence;
   adversarial pass F5's one-sentence pin) + per-unit postprocess + probe recording.
6. **Fork snapshot materialization** (§4.2).
7. **Frontier diff** (D9): previous snapshot's retained frontier vs the new frontier,
   with `diff_push`'s original budgets; settled same-object pre-paired; id
   transfer/mint/retire + revision stamping (stamp only what truly changed; touched
   ancestors' DESCENDANT semantics are carried by subtree revision). Tentative→real id
   hand-over: real closure happens on the original node, whose id is dead storage
   until this moment — this step pairs the retained clone subtree with the settled
   original and writes the id back onto the original.
8. **Publish:** revision+1; the chain retains the new frontier remnants + the new
   definedness sets, and releases the previous ones (via the arena freelist).

### 4.4 Honest behavior statements (the degradation ladder, written into the contract)

- **Steady-state churn = today's per-tick-edit churn, computed more cheaply.** A
  growing paragraph per tick: the trailing Text pairs positionally and keeps its id
  (TEXT-level change) + O(depth) ancestor revisions; late-closing emphasis and
  half-line setext oscillation retire+mint byte-for-byte identically to today's full
  path — inherited from the oracle, not introduced by S2.
- **A giant open paragraph (a prose wall with no blank lines) fully re-refines at parse
  speed every tick** — not memcpy speed (inline parse is parse speed; adversarial pass
  M5's correction). A 1MB open paragraph ≈ 4–10ms/tick. Only fences are memcpy speed.
  The old engine's inline seam (built for this, reverted after fuzz divergence) is not
  rebuilt; this shape is a written degradation.
- **The references-appendix shape** (consecutive `[n]: url` with no blank line = one
  growing paragraph) fully re-harvests every tick — a parse-speed Σ-quadratic; gated,
  and **no harvest prefix memo** (a patch-cycle seed; adversarial pass F7 named it).
- **A flip storm** (one label's definedness oscillating per tick × M mentions): O(the
  mentioning units) per tick — upper-bounded by the full-parse class, never worse.
  Flip-rate / fallback-rate counters are gated observables.

---

## 5 · Removal list

| # | Removed | Removed/changed alongside |
| --- | --- | --- |
| R1 | Public types and functions: `markdown_core_commit`, `markdown_core_delta`, `markdown_core_diff`, `markdown_core_diff_parts`, `delta_revisions/_diffs/_free` | The header's Delta section; both export tables; `delta.c` entirely |
| R2 | `document_edit` out-parameter shape → returns `document*` | All three bindings' call sites; `commit_compat.h` |
| R3 | diff.c row emission (`mark` / row writes); id matching + revision stamping stay | — |
| R4 | The three bindings' Commit/Delta/Diff/DiffParts, readDelta/decodeDelta, README path-B/C narratives, ES retire-driven eviction (replaced by mirror rebuild) | #98-era comments ("no reuse", "projection is a function of text") rewritten in the same batch to the c046972 boundary wording |
| R5 | Kotlin wire delta section (MKC4→MKC5) | `encode_delta` / `deltaBody` |
| R6 | The whole CLEAN_START chain (D8): flags, the five grant/strip/transfer sites, `line_began_clean/line_defs_only`, `clean_index` | — |
| R7 | Dead scaffolding: `edit_summary/pending`, the lookups type family + 11 prototype declarations with no definition, `definitions[].index`, the `if(false)` arm; four stale comments | — |
| R8 | The ref_size budget (D7): accounting, capping, `entry.size` | Parity determination first; if blocked, switch to the restore protocol |
| R9 | The archived definition table (`document->definitions`, zero readers); live tables always live on the warm parser | The `resolve_definition_owners` archiving call |
| R10 | The shared series clock (`document.c:293-357`, exists only for fan-out) → per-chain plain counter; the header's "two lines of descent" wording; the concurrent-shared-receiver edit capability and its gate assertions | D1 unified semantics; the bindings' series-salt semantics restated per chain |

---

## 6 · The contract document `docs/specs/streaming-and-documents.md`

Core wording (adversarially finalized, goes into the contract verbatim):

> "A document is the live head of a chain. A mutation — `edit(text)` replaces all
> text, `append(chunk)` appends bytes — advances the chain and supersedes its
> receiver: the successor reuses the receiver's structure, the receiver from then on
> supports only `free`, and its node handles become invalid when the next mutation
> begins. The two mutations are one rule: same chain, same series, revision strictly
> +1; mutating a superseded handle is a deterministic error; there is no forking.
> `append`'s result has the same tree, same dump, and same identity rules as
> `edit(text + chunk)` — the difference is only cost: append parses only what newly
> arrived. Every live head is a complete, warm document: its projection equals a fresh
> parse of all known bytes, and queries and traversal are unaware of the chain.
> Mutations must be externally serialized; concurrent reads outside mutation windows
> are safe; decoded values remain valid forever. There is no open, no close, no flush,
> no pending edit."

Remaining sections: the update protocol ((id, revision) as the entire semantics + there
is no delta); identity rules (D9 in full + the oscillation honesty statement + edit's
whole-tree matching preserving id continuity); failure (D5); the degradation ladder
(§4.4 in full, including "a correction costs one full parse — edit's parse is the
warmup"); a "differences from the deleted session" section (D3's honest naming + three
differences: no two-phase, no pending semantics, tables only grow with no reconciler);
Switch B's decision condition and consequences (D2); and the "things that do not
exist" list.

---

## 7 · Testing and gates

| Gate | Content | Guards |
| --- | --- | --- |
| `append_replay` (new, primary) | Drives the real `append()`: **token-sized, non-line-aligned splits** (3–8B strides) + random splits (mid-UTF-8/mid-CRLF/mid-line) + per-byte (inputs ≤ 2048 bytes, unsampled). Per append: dump ≡ fresh parse; double-walk against the previous snapshot (no id resurrection, revision monotone, (id, rev) ⇒ equal dump). **Per-line splitting is blind to the entire half-line mechanism class (independently confirmed by two adversarial passes) — non-line-aligned is the primary gate; per-line is only supplementary.** | INV1/3/4 |
| `value_mirror` (new) | On the append path: revision-pruned decode ≡ full decode, field-by-field value comparison. The only oracle that can see VALUE-bit defects (the List.tight class). | INV4 |
| `warm_fingerprint` (new) | Warm-state hash equal before and after snapshot materialization. | INV2 |
| `fuzz_appends` (new) | Randomly split streams over corpus + random bytes, full verification at every step; a flip round-trip fuzzer (half destination / unclosed title / definitions-only gaining prose / same label across tables / duplicate definitions / definitions inside containers). | INV1–4 |
| Targeted unit tests | The `TABLE_VISITED` half-line probe case (the adversarial reports' sharpest single case: `"a\|b\n"` + `"\|-\|"` + `"-\|\n"`); the map prepared-index add-after-prepare path (zero production executions on main; streaming makes it hot) + the sorted-fallback mode cost decision; the diagnostics watermark; oscillating identity (`"[x"` → `"]: /u"` → `"\ny"`). | — |
| Concurrency (TSan) | `shared_edit` rewritten to unified-mutation-rule gates: serialized mutation, deterministic stale-mutation errors, concurrent reads between mutations, decoded values safe across threads (D1 reversal; the original concurrent-shared-receiver edit capability is deleted). | The threading contract |
| `bench_append` (new) | Shapes (prose / nested list / fence / footnote-dense / giant single paragraph / references appendix / mixed edit) × sizes (100KB / 1MB / 10MB final) × **token-sized splits**; per-append p50/p99, whole-trace totals, peak RSS over 10k appends, cold/warm costs, flip traces. Ratio assertions + absolute values recorded in the migration baseline document; no timing under sanitizers. Binding arm: the existing ES/Kotlin edit benchmarks repointed at append. | §9 budget |
| `edit_mid` (informational) | Regression guard for the (full) edit path + records the background numbers behind the "no incremental edit" decision; adds behavior assertions for "the edit successor is warm; append does not re-warm". | D2 no-regression |
| goldens / parity / scope-sanity | Untouched (the snapshot tree is isomorphic to the full-parse tree). | — |
| Mutation discipline | Hand-mutate the new fork/flip/frontier-diff code; results go into a review document (repo convention). | — |

---

## 8 · Milestones

| # | Scope | Exit criteria |
| --- | --- | --- |
| **P0** Demolition and baselines (4 independent PRs) | P0.1 R1–R5 + the double-walk replay rework (one atomic SemVer major); P0.2 the `bench_append` skeleton, first measuring **today's** full-parse-per-tick baselines across shapes; P0.3 the D7 parity determination + R8; P0.4 R6/R7/R9 | Whole suite green; baselines archived; ABI/wire major each broken exactly once |
| **P1** Append API + binding pruning (engine still full-parse) | `append()` lands on all three layers, implemented as the D6 fallback (full parse + diff per append); chain owner + generation + the supersession contract effective from day one; binding pruned decode + destructive mirror + MKC5 (the warmup path already proven safe, S6); `append_replay` / `value_mirror` online | Every oracle green on the append path; **the bindings' per-tick O(changed) is delivered** (the platform-side dominant term dies first, zero engine risk); the API freezes here |
| **P2** First slice of the heart | Warm parser + settled-list hook + the fork mechanism: pure prose shapes (paragraph/heading/blank/thematic; no extensions, no definitions); `warm_fingerprint` online; fallback-rate counter | Prose fixtures green through the warm path on INV1–4 (token splits) |
| **P3** All constructs | Fence / list (tightness accumulator) / quote / indented / html / the footnote chain + directive/formula/table hooks + diagnostics-on-nodes + overlay harvest + the VALUE checklist | All canonical + spec fixtures green on the warm path; fallback rate on the 26 samples < the declared threshold |
| **P4** Probe/flip | Probe recording, flip detection, re-refine (D9 child-sequence diff); flip fixtures + fuzzer; the prepared-add special | Footnote-dense streams: flip-append cost independent of document size (ratio gate) |
| **P5** Identity closure | Pin the frontier-diff equivalence conditions (`diff_push` original budgets + in-place re-refine) + the oscillation case set + churn numbers archived; the deferred-hash dirty flag | INV4 fully online; a divergence gate (spot-checking warm-path vs cold-path identity agreement) |
| **P6** Wrap-up | The contract document, READMEs ×3, CHANGELOG major, D10 archiving, final baselines, claims audit | Every public claim executably verified (repo convention) |

P1 is the key de-risking step (adversarial task-6 conclusion): the binding-side O(K)
term is the dominant term at 100 appends/s and can be deleted safely under the
full-parse engine; the engine heart replaces the fallback slice by slice across P2–P4,
each slice covered by the oracle, rollback = that construct returns to the fallback.
The simplified variant "warm block layer + full re-refine every tick" was rejected:
rebuilding all inline nodes forces a full-tree diff to recover ids, at most a 1.7×
win, while still requiring the entire half-line mechanism — abandoned.

---

## 9 · Performance budget and the honest justification

```
append = C/B_parse                      [feed chunk]
       + (S+L+ℓ)/B_parse                [fork: clone spine + tentative refine
                                         (leaf + half line) — paragraphs at parse
                                         speed, fences at memcpy speed]
       + U/B_parse                      [newly settled inline + postprocess]
       + F_flip/B_parse                 [flip snapshots only]
       + T_frontier·c_diff              [frontier diff]
       + (touched+d)·c_stamp            [revision; hash deferred]
       + O(changed)·c_decode            [binding pruned decode]

No per-append term scales with total document size. The only size couplings:
L (giant open leaf), F_flip (storms), references-appendix harvest — all three
written into the degradation ladder and gated.
```

| Scenario (adversarially quantified) | Today / tick | This plan / tick | Who notices |
| --- | --- | --- | --- |
| 100KB prose, desktop, 100ms throttle | ~2–4 ms | ~30–60 µs | **Nobody** — both far under budget |
| 100KB dense (list/table), Android/WASM | ~6–45 ms | ~0.1–0.3 ms | Mobile: today already at/over frame budget |
| 1MB, desktop, no throttle, 50–100/s | core saturated | <1% of a core | Everyone |
| 10MB agent-log tailing | ~150–400 ms | ~0.1–1 ms | Everyone |

**The honest justification (goes into the contract; "append is generally
unaffordable" may not be used as the rationale):** at 100KB on desktop with a throttle,
today is already imperceptible. The real reasons are (a) the Android/WASM floors (dense
100–300KB already breaks the frame budget; the #100 concern), (b) streams ≥ 0.5–1MB
(code generation, agent transcripts), (c) removing the 100ms throttle for per-token
display, (d) power/heat (~100× fewer joules per stream on mobile). The thresholds
where streaming turns from nice to necessary: desktop native ≈ 1.5–4MB, WASM
≈ 0.3–1MB, mid-range Android ≈ 0.15–0.6MB. K3's growing-literal decode (a 1MB fence at
~0.5–2ms/tick + 1MB of string garbage per tick) is accepted for v1 and written down;
the v2 trigger = the fence binding arm of `bench_append` exceeding frame budget, the
v2 design = a stable-prefix hint on TEXT + segmented literal exposure.

---

## 10 · Risk register

- **K1 · Fork fidelity** (some construct cannot reproduce the fresh parse under the
  fork) — detection: `append_replay` (token splits) + the INV2 fingerprint; fallback:
  that construct takes D6, counted in the fallback rate; the plan does not fail on
  this.
- **K2 · New frontier-diff code** (#98 caught two defects in similar emission code) —
  detection: double-walk + value_mirror + fuzz; D9's equivalence conditions (original
  budgets, in-place re-refine) pinned dead by gate comments.
- **K3 · Growing-literal re-materialization per tick in bindings** — v1/v2 and the
  trigger fixed in §9.
- **K4 · Flip storms / references appendix / prose walls** — written into the
  degradation ladder; upper bound = the full-parse class; counters observable.
- **K5 · Memory** — high-water O(document + 2× largest leaf); per-class stranding of
  never-returned slabs, the warm remnant of O(largest leaf) until chain death: a
  10k-append RSS gate (the `binding_baseline` peak_rss precedent); the chained
  diagnostics vector prevents Σ-quadratic re-copies.
- **K6 · The map prepared-add path has never executed in production + the sorted
  fallback re-prepares per add** — targeted unit tests + mutation before P4 + the
  fallback-mode cost decision.
- **K7 · C-layer UAF through superseded handles** (bare node pointers cannot carry a
  generation check) — explicit UB-by-wording + binding neuter + hardened errors;
  accepted and documented, no pretense of prevention.

---

## 11 · Explicitly not doing

- Incremental edit (D1, permanent); anchors / resync / window diff / relative
  coordinates / base tables.
- Delta/Commit or any change-notification side channel (D4).
- The inline seam (intra-unit incremental inlines for giant paragraphs — built once,
  fuzz-diverged, reverted; not rebuilt).
- A harvest prefix memo (the references-appendix "optimization" = a patch-cycle seed).
- "Keep warm when the edit is at the tail" variants (a correction = one full parse by
  D2's warmup rule; no creep).
- Stream fork/clone; session two-phase; pending-edit semantics; parser
  answers/numbering; persistent structures / rope / extent sequences.
- Streaming-specific node types or a provisional AST (the snapshot tree is isomorphic
  to the full-parse tree).

---

## Errata

**E3 — WHAT SHIPPED INSTEAD (2026-08-17).** The D6 shape E2 left append
on — one full parse plus one whole-tree diff per tick — is gone. The engine
mechanism that replaced this plan's parser-tail fork is the LIVING TREE
(`2026-08-13-living-tree-plan.md`, landed as #104, #105 and #106): the
head's tree grows in place on every append, the previous projection is
retracted by a record and re-published, definitions flip exactly the units
that asked, and identity is object identity plus a pairing of the
re-derived tail against the tail it replaces. Nothing here about the D6
fallback, its ledger, the whole-tree diff or the whole-tree hash stamp
describes the shipped engine any longer; the identity contract this plan
states (an id names the same thing across an append, equal (id, revision)
means an identical subtree) is what the living tree keeps.

**E2 — THE RESHAPE (user ruling, 2026-08-13).** Edit and the parser-tail
fork (§4.2) are DELETED. The API is `new` + `append`, nothing else: a
whole-text edit is indistinguishable from constructing a new document, so it
is spelled that way — a new chain with a new series — and the warm path the
fork would have carried is not built. Append remains the D6 shape this plan
already shipped and measured (one full parse plus one whole-tree diff per
tick; the ~2x streaming win of P1 was measured on exactly that), so nothing
regresses. What the ruling buys: the fork's implementation had grown three
contract-edge mechanisms — a sibling SEAM (one `next` pointer shared between
the warm tree and every snapshot, borrowed per fork under the invalidation
window), a persistent PRESENTATION ROOT mutated in place per tick, and a
full-build→warm id handover sweep — all sound only by appeal to the
handles-die-at-next-mutation clause, all deleted unbuilt. P2's landed slices
(two-generation arena, settled sink, warm fingerprint, warm builder) served
only the fork and are abandoned with it; their design record survives in the
project memory for the day a warm path is wanted again. Consequences swept
through the tree on 2026-08-13: `markdown_core_document_edit` is gone from
the facade, exports and every binding; the replay harness is append-only
(`tests/support/append_replay.{h,c}`, chunk-boundary fuzz format); the
pathological and capture oracles drive appends; the diff's head-insertion
pairing configurations are unreachable through the public surface (an
append cannot mint a node ahead of unchanged bytes), which leaves the
subtree-hash value-pairing machinery wider than the surface needs — a
recorded candidate for later simplification, not touched here.

**E1 (found during P1, 2026-08-13).** §3.2's claim that a D6-fallback append
needs no flag because "append never moves bytes ⇒ values for unchanged
(id, rev) are byte-identical including scope" is **falsified**: a trailing
construct absorbs its terminating newline into its scope END without a
revision bump — position is not content by contract, so nothing stamps it
(`ThematicBreak 5:1..5:3` → `5:1..6:0` on appending a newline). The Swift
value_mirror gate caught it before it shipped, which is the gate doing
exactly its job. Consequences, all landed with P1: the bindings' prune
condition is **(id, revision, scope)** — sound for whole subtrees because
under append starts never move, ends only grow toward the old EOF, and a
grown descendant end forces every ancestor's end past it; the Kotlin wire's
`REUSE(id)` encoder emits only for nodes whose scope end lies strictly
before the predecessor's EOF (the encoder must be sound alone — a decoder
holding a REUSE record has no subtree to fall back to); and INV4's
"(id, rev) equal ⇒ equal subtree dump" is read with dump positions
excluded, which is how the C ledger always checked it.

---

*Method and evidence chain: six fact-finding investigations (all file:line verified) →
design D′ → four adversarial passes → the requirement decisions (streaming-only, no
delta) → design S2 → three adversarial passes (the fork mechanism, the identity
authorship rule, the chain owner, and the honest budget all come from this round) →
this plan. The deleted engine (`26045be^`) served throughout as the existence proof of
"what needs checking". The earlier D′ assessment ("anchor-window reparse") is retained
as decision history.*
