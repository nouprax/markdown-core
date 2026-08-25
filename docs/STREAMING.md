# Block-unit streaming

Owner ruling, 2026-08-25. This document is the whole plan. It replaces
`docs/RECONSTRUCTION.md`, which was deleted the same day for encoding a program
this ruling kills; recover any of it with
`git show 8d0910b:docs/RECONSTRUCTION.md`.

---

## 0. The ruling

> **The block is the minimal update unit.**

Not the line. Not the partial line. Not a "globally minimal edit".

**Why, and it is a property of the language rather than of this engine.** The
delimiter stack is not incrementally settleable: emphasis is matched only at the
end of a unit, so any granularity below the block either cannot reach the answer
at all, or reaches it by keeping an eager delimiter residue — and that residue
*is* an intermediate state that can be inconsistent. Since a large fraction of
stream boundaries require re-parsing past the local delimiter stack anyway
(§7 F7), sub-block granularity buys nothing and pays in inconsistency.

And it is what the consumer wants. In a reactive, self-contained consumption
model the consumer re-renders a unit; it has no use for half a paragraph. The
block is already the natural unit on the other side of the API.

---

## 1. What the ruling kills

Every one of these is dead and must not be reintroduced, including as an
"optimisation later":

| Dead | It was for |
|---|---|
| Pausing the parser at a line boundary | the line as the update unit |
| The incomplete trailing line / partial-line parse | ditto, one level finer |
| The resumable inline subject | making a per-line ask cost O(line) instead of O(open block) |
| A label → waiting-sites reverse index | making a definition's arrival edit only the sites that flip |
| "Each site flips at most once" as a bound | the same minimal-edit goal |
| Retraction / provisional node kinds / pending state | eager close-and-retract |

**Global minimal edit is dead twice over.** It was already deleted as a design
goal on cost grounds (the bound is `O(what you project)`, not `O(what changed)`);
the ruling deletes it again on a stronger ground — it conflicts with markdown's
non-local resolution, so pursuing it is pursuing something the language does not
have.

**What backward reach actually looks like.** Six mechanisms in this engine reach
backwards. Four of them stay inside the open spine, so they are free under this
ruling; two do not, and those two are the entire reason the AST is derived rather
than stored:

| Mechanism | Reach | Inside the open spine? |
|---|---|---|
| setext heading | 1 line | yes — the paragraph is still open |
| table retype (paragraph → table) | the paragraph | yes |
| formula block promotion | 1 line (`closed` lags deliberately) | yes |
| list tightness | the whole list | yes — written when the list closes |
| **link reference definition** | **unbounded** | **no** |
| **footnote definition** | **unbounded** | **no** |

---

## 2. The substrate that already exists

Stage 1 landed as `8d0910b` (PR #117) and is on `main`. What it gives this plan,
verified in this session unless marked otherwise:

- **The CST/AST split.** The parser owns the CST — the block tree plus each
  block's content bytes plus the normalized source. The AST is
  `project(CST, refmap)`, derived on demand and never stored. The projection is
  one internal function,
  [`markdown_core_parser_derive_tree`](../packages/markdown-core/core/blocks.c#L1588).
  This is exactly the substrate block-unit streaming needs: a stable thing to
  re-project from.
- **`finish` is the last projection.** `finalize_document` closes the spine;
  `finish` is that plus a projection plus the seal. Since T1 the projection is
  taken **in place** on the CST — `finish` returns `parser->root` itself and
  the reset no longer frees it — while `derive_tree` keeps the clone for
  re-projection. Both run the same body (`S_project`).
- **Both definition maps accept insert-after-lookup.** Required for any
  mid-stream projection, and it fixed a shipped correctness defect independent
  of streaming (4 of 670 real `.md` lost 20 reference nodes from the *finished*
  tree). *Inherited measurement.*
- **A finished tree contains no open block.** Table cells and the header row
  close at build.
- **Diagnostics are CST-owned and speak at completion.** Only `finish`'s
  derivation records, so a mid-parse derivation stays silent about an open
  block's prefix.
- **Feed is partition-invariant.** `stream_runner` gates it over the whole
  fixture corpus in both `--spec` (per line) and `--bytes` (per byte, i.e.
  *inside* a line) modes — 15 streaming tests, green. This is orthogonal to
  update granularity and is exactly the property a byte-chunked consumer needs.
- **Gates that already hold the line:** `projection_double_*` (two projections
  of one CST are byte-identical, taken over the *open* spine),
  `projection_closed_*`, `projection_refmap_independence_*`,
  `pathological_complexity_projection_slope`,
  `regression_fallback_definition_after_lookup`.

`ctest --preset correctness` is **88/88**. Every row of `scripts/dev/gates.sh` is green
— once the presets are built, which is F2's trap.

---

## 3. Findings

Each is either **VERIFIED** here or **INHERITED** from the Stage 1 design work and
not re-measured. Inherited numbers are recoverable from
`git show 8d0910b:docs/RECONSTRUCTION.md`.

### F1 — `finish` pays for a clone it immediately discards: **+7.9%**  · VERIFIED · **CLOSED by T1**

`derive_tree` clones the block skeleton, projects onto the clone, and `finish`
then disposes the CST. Nothing can observe the CST after `finish`, so on the
one-shot path — which is what every binding, the CLI and every gate use today —
the clone is provably dead work.

**This cost had never been measured.** The `1.336×` figure carried in the Stage 1
commit is a *linearity slope* over 64 KiB → 16 MiB, not a before/after.

Measured here: `f5edcdd` (pre-Stage-1) vs `8d0910b`, same `bench_runner`, 41
cases, 3 runs each, min of medians.

```
total   pre=247.76 ms   head=267.27 ms   ratio=1.0787   (+7.9%)
median per-case ratio 1.0708     slower >5%: 25 of 41     faster >5%: 0
```

The cost sorts by **block density**, not by inline work — which identifies the
mechanism as the skeleton copy and nothing else:

| case | ratio |
|---|---|
| `deep_nesting@8192 / @16384 / @32768` | **1.525 / 1.512 / 1.476** |
| `block-list-flat` / `block-list-nested` | 1.298 / 1.254 |
| `block-heading` / `block-html` | 1.235 / 1.219 |
| `large_document@512` (5.6 MB) | 1.086 |
| `block-ref-flat` / `block-ref-nested` | 1.078 / 1.020 |
| `inline-em-flat` / `inline-newlines` | 1.012 / 1.003 |
| `adversarial_emphasis@16384` | 1.011 |

The reference-heavy cases are not outliers, so map re-preparation is **not** the
cost.

**Closed by T1** (2026-08-25). Same `bench_runner`, same 41 cases, same method,
three trees built and run in one session so the numbers are comparable:
`f5edcdd` (pre-Stage-1), `890e908` (Stage 1, before T1) and the T1 working tree;
3 runs each, trees alternated within every run, min of `median_ms` per case.

```
total   pre=256.64 ms   base=274.75 ms   head=257.04 ms
base/pre    1.0706 (+7.1%)   median 1.0618   slower >5%: 23 of 41   faster >5%: 0
head/base   0.9356 (-6.4%)   median 0.9319   slower >5%: 0  of 41   faster >5%: 24
head/pre    1.0016 (+0.2%)   median 1.0015   slower >5%: 0  of 41   faster >5%: 1
```

`base/pre` reproduces the +7.9% above; `head/pre` says the debt is paid in
full — the one-shot path is back at its pre-Stage-1 cost. The recovery sorts
exactly as the cost did, which is the mechanism confirmed a second time from
the other side:

| case | head/base | head/pre |
|---|---|---|
| `deep_nesting@8192 / @16384 / @32768` | **0.665 / 0.667 / 0.680** | 0.958 / 1.026 / 0.990 |
| `block-list-flat` / `block-list-nested` | 0.807 / 0.790 | 1.034 / 1.015 |
| `block-heading` / `block-html` | 0.817 / 0.816 | 1.016 / 1.006 |
| `large_document@512` (5.6 MB) | 0.939 | 1.008 |
| `block-ref-flat` / `block-ref-nested` | 0.967 / 0.990 | 1.003 / 0.976 |
| `inline-em-flat` / `inline-newlines` | 0.989 / 0.927 | 1.045 / 0.965 |
| `adversarial_emphasis@16384` | 0.965 | 0.981 |

A head-vs-base run taken earlier in the same session gave 0.9455 (−5.4%),
median 0.9472, 22 faster >5%, 1 slower >5%: `adversarial_links@16384` at 1.077,
a 4 ms case whose @32768/@65536 siblings sat at 0.986/1.012 and which came back
at 0.975 in the three-way run. Pooled over both runs (6 samples a side)
head/base is 0.9417 (−5.8%) with that case at 1.060, on the strength of one
fast base sample — noise, not a regression.

### F2 — WITHDRAWN: `audit-scope-containment` is not red  · CORRECTED

First recorded here as red on `main` with three `CLEARED` rows. **That was an
artifact of running the gate against a stale binary**, before
`cmake --build --preset default`. Rebuilt at `8d0910b` it is green — *9
registered rows still wrong, 4278 scanned, ledger holds.* Kept as a finding so
the trap is written down: `scripts/dev/gates.sh` builds nothing and says so in
its own header, so every row it prints describes whatever binary already sits in
`build/`, not the working tree.

### F3 — there is no public streaming entry point at all  · VERIFIED

The public C ABI is 37 exported symbols. `markdown_core_document_parse(const
uint8_t *source, size_t length, …)` takes the whole input. `feed`/`finish` are
not exported; `derive_tree` is internal. The facade has no chunked entry point,
which is why `stream_runner` drives the raw parser.

**Everything in this plan is invisible from outside the library until this is
built.**

### F4 — a block has no identity across projections  · VERIFIED

`S_clone_block_tree` allocates fresh nodes on every derivation. Two projections
of an unwritten CST are byte-identical in *content* (gated), but nothing lets a
consumer say "this is the same block I already rendered". Identity is the
precondition for a change signal.

### F5 — there is no per-block change signal  · VERIFIED

`derive_tree` returns a whole tree and re-derives unconditionally. Nothing stamps
a block with when it was last written or what map generation it was resolved
against.

### F6 — re-projection is O(whole document) by design  · VERIFIED mechanism, INHERITED numbers

No label → sites index was built (deliberately: the bound was ruled
`O(what you project)`). Inherited measurements over a 4.92 MB corpus: re-project
every closed block = 36.9% of a full parse; the crude "contains `[`" filter =
23.0%; the exact stale set = 7.2%. **The crude filter is nearly worthless** —
37% of the saving where the exact set gets 81%. There is no cheap middle.

### F7 — the disagreement window is wide, and "definitions live at the bottom" is false  · INHERITED

Line-weighted, **27.40%** of stream boundaries (9,543 of 34,832) hold at least
one site where "defer to the end" and "re-project now" differ. The window opens
at the **first binding** definition, median at **80.1%** of the document, and is
empty in only **8 of 90** real documents. Separately: **4.08%** of closed inline
blocks are stale at the end (1,688 of 41,384; 128 of 670 documents; worst
document 116 of 389).

This is the finding that sizes the cache. It is also the finding that says a
cache which serves stale closed blocks is answering a different question from
the one the consumer asked.

### F8 — carried extension state is invisible to every oracle  · INHERITED

`TABLE_VISITED`, `n_rows`, `n_nonempty_cells`, formula `block_delim`/`closed`,
directive `fence_length`/`closed`/`consume_line` are all load-bearing carried
state that no tree-equality gate can see. Stage 1's `opaque_copy_func` hook makes
them cross the clone, but nothing gates the *fields*.

### F9 — the resident-memory bound is unmeasured  · INHERITED

The split chose that every block **keeps** its content buffer for life. That is a
direct reversal of an earlier intent to release it at close, and the memory cost
of the choice was never measured. Under repeated projection it is the bound that
matters most.

### F10 — a suspected CST write does **not** reproduce  · VERIFIED NEGATIVE

`markdown_core_parse_inlines` mints a content mark for any content-bearing block
that has none, and `markdown_core_parser_mark_content` appends to
`parser->line_marks` — i.e. a projection could write parser-owned CST state and
grow it once per ask. **It does not.** Measured over a real 826 KB document with
all six extensions and an *open* spine across 10 projections, and over every
fixture and benchmark sample: `line_marks_size` is flat. Table cells and
directive labels already receive their marks at build time through
`adopt_content_marks`, so the branch is unreachable. Recorded so it is not
re-investigated.

---

## 4. Decisions owed before code

These change what gets built. None is answerable from the code.

- **D1 — Does an ask re-project a stale closed block, or serve it stale?**
  Re-projecting keeps the cleanest property this design has: *an ask is exactly
  a parse of the bytes fed so far.* Serving stale makes an ask a patchwork of
  parses of different prefixes, and F7 sizes the cost of that at 27.40% of
  boundaries. Recommendation: **re-project**, and let the cache be an
  optimisation that never changes an answer.
- **D2 — Who owns a projected tree, and how long is it valid?** If a closed
  block's derived subtree is cached on the CST block, the returned tree shares
  nodes with the cache and the consumer cannot simply free it. Three shapes:
  refcount, copy-on-return, or "the engine owns it and the consumer borrows
  until the next ask".
- **D3 — Is the change signal a per-node mark on the returned tree, or a
  separate change list?** A mark is simpler and costs one field; a list is what a
  consumer actually iterates.
- **D4 — What is a block's identity minted from?** Creation order is not
  document order. It must be stable across projections and across the CST edits
  that a retype performs (paragraph → table, paragraph → setext heading).
- **D5 — Does the public surface expose `create/feed/ask/free`, or a higher-level
  handle?** F3 says something must be exported either way.
- **D6 — Do the bindings get streaming in the same release as C, or does C ship
  first?**

---

## 5. The task list

Phases are ordered. Nothing in a phase starts before the decisions it names are
made.

### Phase A — pay the measured debts (needs no decision)

- [x] **T1 — `finish` stops cloning.** Give `finish` a non-cloning path that
      projects the CST in place and then seals the parser. H4's non-idempotence
      does not apply: this is the first and last projection of that CST.
      `derive_tree` keeps its cloning semantics for re-projection.
      *Closes F1. Gate: the 41-case bench before/after, plus correctness 88/88
      and every `projection_*` row unmoved.* **Done 2026-08-25**: the
      projection body is `S_project(parser, skeleton, refmap, record)`;
      `derive_tree` = clone + `S_project`, `finish` = `finalize_document` +
      `S_project(parser->root)` + ownership flip (`parser->root = NULL` before
      the reset, and the oom path frees whichever of `res` / `parser->root`
      still holds the tree). Correctness 88/88, conformance 2/2, asan and
      ubsan 76/76 each (rebuilt, per F2), all ten `projection_*` rows green,
      goldens unmoved. Numbers in F1.

### Phase B — make a block addressable  · needs D4

- [ ] **T2 — a stable id on the CST block**, minted at open, carried onto the
      derived node by the clone.
- [ ] **T3 — a write stamp on the CST block**, bumped whenever the parser writes
      that block (content appended, type retyped, closed).
- [ ] **T4 — a generation counter on each definition map**, bumped on every
      insert. O(1); flips nothing and walks nothing.
- [ ] **T5 — gate:** two projections of an unwritten CST produce identical ids,
      and a retype preserves the id of the block it rewrites.
      *Closes F4.*

### Phase C — the change signal  · needs D1, D3

- [ ] **T6 — stamp each derived block** with the `(write stamp, map generation)`
      it was derived at.
- [ ] **T7 — the ask takes the consumer's last stamp pair** and classifies every
      block as new / changed / unchanged.
- [ ] **T8 — gate:** for every fixture, feed to each block boundary, ask, and
      assert the changed set is exactly the blocks whose bytes moved or whose
      resolution moved. This is the gate that makes the whole design falsifiable;
      build it before Phase D.
      *Closes F5.*

### Phase D — the cache  · needs D2

- [ ] **T9 — cache a closed block's derived subtree** on its CST block.
- [ ] **T10 — invalidate by generation**, per F6's rule: a cached projection is
      invalid when the map has advanced past the generation it was made at.
      Correctness must never depend on the cache — a wrong cache is a slow
      engine, not a wrong tree.
- [ ] **T11 — measure the reactive loop:** feed a real corpus in block-sized
      chunks, ask after each, and report cost per ask with the cache on and off.
      **Decide from that number whether Phase D ships at all.** F6 says
      re-projecting every closed block is 36.9% of a parse; if the ask rate makes
      that acceptable, the cache is not worth its ownership complexity.

### Phase E — the public surface  · needs D5, D6

- [ ] **T12 — export a streaming entry point** in C: create, feed, ask, free.
- [ ] **T13 — the ask returns a document carrying the change classification** from
      T7.
- [ ] **T14 — bindings** (Swift, Kotlin, ES) and their conformance corpora.
      *Closes F3.*

### Phase F — bounds and gates

- [ ] **T15 — the reactive-loop bound as a gate:** cost per ask is
      `O(open block + changed set)` and carries **no term in the document already
      fed**. A fitted slope in document size fails and names the state being
      re-derived.
- [ ] **T16 — measure resident memory** across a long stream, and state the
      bound that comes with every block keeping its content buffer for life.
      *Closes F9.*
- [ ] **T17 — structural invariants over carried opaque extension state**, so a
      field that stops surviving the clone fails a gate rather than a golden.
      *Closes F8.*

---

## 6. Bounds this design accepts

Stated so they are not later mistaken for defects.

- **An ask costs O(open block).** The open block is re-projected every time,
  because that is what "the block is the unit" means. A consumer asking
  repeatedly while one very large block is open pays that block's size on every
  ask. **This is accepted**, and the machinery that would have flattened it (the
  resumable inline subject) is dead by §1.
- **An ask is not monotone.** A definition arriving later changes the projection
  of a block emitted earlier. That block was never wrong: it was resolved against
  the map as it then stood, and CommonMark defines the earlier outcome as prose.
- **`finish` is O(document) once**, and equals a one-shot parse. That is the
  acceptance criterion and it is already gated.
- **No index, no back-pointer, no pending state.** The bound is
  `O(what you project)`. Phase D may make an ask cheaper; it may never make an
  answer different.

---

## 7. Provenance

**Measured in this session** (2026-08-25, `8d0910b`): F1's 41-case bench table,
F2, F3's export count, F4, F5, F10, correctness 88/88, the `gates.sh` sweep.
**T1's closing measurement** (2026-08-25): the three-way bench in F1, over
`f5edcdd` / `890e908` / the T1 working tree, built and run in one session.

**Inherited from the Stage 1 design work and not re-measured**: F6's 36.9% /
23.0% / 7.2%, F7's 27.40% / 80.1% / 8-of-90 / 4.08%, F8, F9, and the
4-of-670 reference-loss figure in §2. All recoverable from
`git show 8d0910b:docs/RECONSTRUCTION.md`. **Re-measure any of them before it
decides a task**, because each was taken against a tree that Stage 1 has since
changed.
