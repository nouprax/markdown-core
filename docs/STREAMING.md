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
| The resumable inline subject | making a per-line update cost O(line) instead of O(open block) |
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
  [`markdown_core_parser_derive_tree`](../packages/markdown-core/core/blocks.c#L1711).
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
- **Feed is partition-invariant.** `stream_runner` gates it over `spec`,
  `regression` and `extensions` in both `--spec` (per line) and `--bytes` (per
  byte, i.e. *inside* a line) modes — **six `stream_*` rows**, green
  ([tests/CMakeLists.txt:247-258](../packages/markdown-core/tests/CMakeLists.txt#L247-L258)).
  The `streaming` ctest label carries 15 rows; the other nine are
  `projection_runner`'s, which feeds each document in one call and is never
  given `--bytes`, so they are the next bullet's gate and not this one's. This is orthogonal to
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

### F3 — there is no public streaming entry point at all  · VERIFIED · **CLOSED by T12/T14**

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

### F5 — there is no per-block change signal  · VERIFIED · ANSWERED by ruling, 2026-08-26 — none is owed

`derive_tree` returns a whole tree and re-derives unconditionally. Nothing stamps
a block with when it was last written or what map generation it was resolved
against. *(T3 has since stamped the CST block — for the cache's key, which is
the only consumer the stamp ever got: the ruling that kills Phase D, §5, says
the identity-keyed view derives "what changed" from the identity T2 mints and
is owed no classification.)*

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

### F8 — carried extension state is invisible to every oracle  · INHERITED · **CLOSED by T17**

`TABLE_VISITED`, `n_rows`, `n_nonempty_cells`, formula `block_delim`/`closed`,
directive `fence_length`/`closed`/`consume_line` are all load-bearing carried
state that no tree-equality gate can see. Stage 1's `opaque_copy_func` hook makes
them cross the clone, but nothing gates the *fields*. *(T17's gate does, from
the other side: the boundary derivation goes through the clone and the finish
projection does not, so a field that stops surviving the clone makes the same
closed block dump differently in the two — see `projection_carried_state_*`.)*

### F9 — the resident-memory bound is unmeasured  · INHERITED · **CLOSED by T16**

The split chose that every block **keeps** its content buffer for life. That is a
direct reversal of an earlier intent to release it at close, and the memory cost
of the choice was never measured. Under repeated projection it is the bound that
matters most. *(Measured 2026-08-26, `pathological_complexity_resident_memory`:
streaming 3.28 MB of paragraph blocks with a derivation every 64 feeds keeps
**8.3× of the bytes fed** resident at peak — the CST's content buffers, the
normalized source, the cache's one list per closed block, and the one live
projection a consumer holds. The bound is O(bytes fed); the gate trips at 24×.)*

### F10 — a suspected CST write does **not** reproduce  · ~~VERIFIED NEGATIVE~~ **WITHDRAWN by F21**

**Withdrawn 2026-08-25.** The write reproduces: the branch is reachable for an
empty ATX heading and for a directive label, and the corpus this finding
measured on had neither followed by an open multi-line block. F21 has the
mechanism, the witness and the fix. Kept as written below so the wrong
reasoning is on the record.


`markdown_core_parse_inlines` mints a content mark for any content-bearing block
that has none, and `markdown_core_parser_mark_content` appends to
`parser->line_marks` — i.e. a projection could write parser-owned CST state and
grow it once per projection. **It does not.** Measured over a real 826 KB document with
all six extensions and an *open* spine across 10 projections, and over every
fixture and benchmark sample: `line_marks_size` is flat. Table cells and
directive labels already receive their marks at build time through
`adopt_content_marks`, so the branch is unreachable. Recorded so it is not
re-investigated.

### F11 — a block IS identifiable, the field costs +0.7%, and the retype fork is rarer than the reference-definition one  · VERIFIED (D4 PoC)

D4 was ruled to be settled by a PoC rather than by argument. This is the PoC.
It is a working tree, not a proposal: `git worktree` at `ae0bb0b`, +59 lines
across five files plus one new runner, `ctest --preset correctness` **88/88**,
conformance **2/2**, all ten `projection_*` rows green, six `stream_*` rows
green, goldens unmoved.

**The candidate.** A `uint32_t block_id` on `markdown_core_node`, minted once
per block from a per-parse counter and carried wherever a block is re-created.
Four mint sites and two carry sites, and the corpus says that is all of them:

| site | file | mint or carry |
|---|---|---|
| every block the block phase opens | `add_child`, [blocks.c:1256](../packages/markdown-core/core/blocks.c#L1256) | **mint** |
| the root | `markdown_core_parser_reset` | **mint** |
| a reference definition | `S_new_reference_definition`, [blocks.c:999](../packages/markdown-core/core/blocks.c#L999) | **mint** |
| a table's lead paragraph | [table.c:381](../packages/markdown-core/extensions/table.c#L381) | **mint** |
| every derived node | `S_clone_block_node`, [blocks.c:1512](../packages/markdown-core/core/blocks.c#L1512) | **carry** |
| the formula promotion's replacement | `new_formula_block_from_literal`, [formula.c:747](../packages/markdown-core/extensions/formula.c#L747) | **carry** |

A retype that rewrites a node **in place** — setext at
[blocks.c:2366](../packages/markdown-core/core/blocks.c#L2366), paragraph →
table at [table.c:526](../packages/markdown-core/extensions/table.c#L526) —
needs no carry at all: the node object survives and the id rides along. Only
the formula promotion needs one, because it is the one "retype" that is
actually a `markdown_core_node_replace`.

**The gate holds.** `identity_runner` feeds a document one line at a time,
derives **twice** after every line, and asserts four things: no block carries
id 0, no two blocks in one derivation share an id, two derivations of one CST
name every block identically, and a dead id never comes back.

```
fixtures (8 files)   878 documents,  2,264 boundaries,   4,528 derivations   0 failures
real corpus          195 documents, 40,583 boundaries,  81,166 derivations   0 failures
```

**And the identity is CHUNKING-stable, which is stronger than the gate above
asked for.** A counter advanced as blocks open was suspected of depending on
how the bytes arrived rather than on the tree they produce — `add_child` bumps
for a block it then abandons ([blocks.c:1260](../packages/markdown-core/core/blocks.c#L1260)),
a declined table row mints and frees ([table.c:713](../packages/markdown-core/extensions/table.c#L713)) —
which would have made the ids a fact about the stream instead of about the
document. **Refuted.** Feeding the same bytes whole, then split at every line,
then **one byte at a time**, and comparing the final derived trees block for
block:

```
whole vs split at every line   960 cases   960 id-identical   0 bijection-only   0 broken
whole vs ONE BYTE at a time    960 cases   960 id-identical   0 bijection-only   0 broken
```

The mechanism is the one §2 already records: the counter is advanced only by
the block phase, and the block phase sees LINES — `parser->linebuf` has already
absorbed the partition. So block identity inherits feed's partition-invariance
for free, and the id is a fact about the document.

**And the gate can fail** — three carry points, each removed in turn against a
forced rebuild, with the restored binary byte-identical to the control:

| carry removed | what the gate says |
|---|---|
| `S_clone_block_node` | `zero-id=4905` on spec, `2703` on extensions |
| the formula replacement | `zero-id=2` on `extensions-formula-github` |
| the table lead mint | `zero-id=4` on extensions |

The failures are all `zero-id` rather than a wrong id, because a clone is
`calloc`'d: a lost identity **fails closed**, which is the failure mode to
want.

**The cost is +0.7%.** `sizeof(markdown_core_node)` goes 176 → 184: `type` and
`flags` exactly fill the four bytes before `extension`, so there is no padding
hole and the field is not free. Same `bench_runner` and same method as F1, 41
cases, **7 rounds**, trees alternated within every round, min of `median_ms`:

```
total   base=244.34 ms   head=246.10 ms   ratio=1.0072   (+0.7%)
median per-case ratio 1.0038     slower >5%: 2 of 41     faster >5%: 1
```

Three rounds first read +0.9% with median 1.014 and five cases over 5%, and
four more rounds collapsed it — `inline-em-worst` went 1.053 → 0.976 and
`inline-em-flat` 1.053 → 1.001. **Do not read a 3-sample >5% row as a
regression**; this is the third time that rule has paid. What survives at 7
samples is `adversarial_links@32768` at 1.104 whose siblings sit at 0.998 and
0.933, which is the exact signature F1 already recorded as noise for
`adversarial_links@16384`.

**The forks, with the frequencies that make them decidable.** The field is on
the shared node struct, so what follows is about blocks only; over 40,583 real
feed boundaries there are just **34 retypes**, and they are not where the
consumer churn is:

| event | real corpus | fixtures |
|---|---|---|
| births | 21,333 | 532 (spec) |
| **deaths** | **2,264** | 18 (spec) |
| retype paragraph → setext heading | 20 | 24 |
| retype paragraph → table | 14 | 7 |
| ...of those, **with a lead paragraph split off** | **0** | **0** |

Every figure in the fixtures column is **`spec.txt` only**, which is why its
lead-split cell is 0: all three lead splits in the corpus are in
`extensions.txt`. Across all eight fixture files the ratio is **3 of 28**, and
that is the number the fork is argued from below.

- **Fork 1 — paragraph → table with a lead split.** The paragraph node object
  survives as the TABLE, so it keeps the id and the lead paragraph — the text
  the reader saw *first*, and which did not change — is minted new. The other
  answer swaps them. **Frequency says this barely matters**: 0 of 14 real table
  retypes split a lead, 3 of 28 across all fixtures. Whichever way it is ruled,
  it is a handful of blocks per corpus.
- **Fork 2 — the formula promotion.** Carried, so the promoted block keeps the
  id of the paragraph or code block it was promoted from. This one is not
  really a fork: the promotion runs in **postprocess, i.e. inside the
  projection**, so at `derive_tree` it rewrites the clone while the CST keeps
  its PARAGRAPH. Every projection redoes the promotion, and a *mint* there
  would rename the block on every single projection — which is F4 restated. It
  must be a carry.
- **Fork 3 — and this one the PoC found rather than predicted.
  EVERY DEATH IS A PARAGRAPH.** 2,264 of 2,264 on the real corpus, 18 of 18 on
  spec. The mechanism is `finalize`'s PARAGRAPH arm
  ([blocks.c:1130-1134](../packages/markdown-core/core/blocks.c#L1130-L1134)):
  a paragraph whose content `resolve_reference_link_definitions` consumed
  entirely is **freed**, and a `REFERENCE_DEFINITION` with a fresh id is born
  in its place. So **typing a reference definition is birth → death → birth,
  not a retype** — and at 2,264 events it is two orders of magnitude more
  common than every retype combined. The fork: does the reference definition
  inherit the id of the paragraph it was carved out of? Under "identity follows
  the place the reader typed" it should, and today it does not.
  **And inheritance is well defined, which is the fact that makes this rulable**
  — a paragraph can hold several definitions
  ([blocks.c:1045](../packages/markdown-core/core/blocks.c#L1045) loops), so
  "inherit" could have been one-to-many. Measured, counting definitions born at
  a boundary where exactly one paragraph died:

  | definitions born | boundaries (real corpus) |
  |---|---|
  | **1** | **2,253** (99.5%) |
  | 2 / 3 / 6 / 7 / 8 | 6 / 2 / 1 / 1 / 1 |

  So a one-to-one inheritance rule covers 99.5% of the events outright, and the
  remaining 11 need only a tie-break — the first definition inherits, since it
  begins where the paragraph began, and the rest mint.

**What this gate cannot see, stated so the numbers are not read as more than
they are.** The event counts key on `node->type`, and two things change what a
consumer sees without changing it. `markdown_core_node_set_syntax_extension`
on a `LIST_ITEM` ([tasklist.c:105](../packages/markdown-core/extensions/tasklist.c#L105))
leaves the type alone while `markdown_core_node_list_item_checked`
([ast.c:477](../packages/markdown-core/extensions/ast.c#L477)) starts answering
a value where it answered nothing — the block's *content* moved, not its kind.
And a list's `tight` is decided at close by walking its descendants
([blocks.c:1207](../packages/markdown-core/core/blocks.c#L1207)), so an edit
inside one item changes what the list emits with no byte of the list itself
changing. **Neither is a hole in D4** — the node object survives in both, so
the identity is right — but both are traps for **T3's write stamp**, which
must therefore hook the field writes and not
`markdown_core_node_set_type`: the setext retype
([blocks.c:2366](../packages/markdown-core/core/blocks.c#L2366)) does not call
it either.

**What this does not settle.** The scheme is proved implementable, stable
across projections AND across chunkings, unique, total and cheap. Which of the
two nodes inherits in forks 1 and 3 is a statement about what a consumer should
see, and the frequencies above are the input to that ruling, not the ruling.
**Ruled 2026-08-26 in §4 D4**: the owner named the consumer — an
identity-keyed view, SwiftUI's `List`/`ForEach` — and both forks fell out of
"is this an element I already have?": fork 1 swaps (the lead keeps the id),
fork 3 inherits at death (the firstborn definition takes the paragraph's id)
and mints on survival. T2 and T5 carry the landed form and its gates.

### F12 — a feed re-parses the whole document today, and the re-parse and the projection's tail are ~88% of it between them  · VERIFIED · **table corrected**

Owner constraint, 2026-08-25: **cloning the whole CST on every feed is
accepted; re-parsing the document on every feed is not.** This measures which
one the engine actually does.

**It does both.** `S_project` runs `process_inlines`
([blocks.c:1334](../packages/markdown-core/core/blocks.c#L1334)), which walks
the whole tree and calls `markdown_core_parse_inlines` for every
content-bearing block. There is no per-block condition of any kind. So today a
feed re-parses every block in the document, every time.

**A feed is 55.4% of a full parse today** — 4.64 ms against 8.37 ms, over 195
real documents (1.27 MB), min of 7. That ratio is two end-to-end measurements
and stands.

**Its internal split had to be measured a second way, and the first way was
wrong.** This finding first split a feed with two PoC seams — `clone_only` and
`derive_tree_no_inlines` — taking `tail = noinline − clone` and
`reparse = derive − noinline`. **That seam cannot separate them**: with
`process_inlines` suppressed there are no inline nodes, so the tail's three
passes walk a skeleton with nothing in it, and the tail's real cost stayed
inside the re-parse bucket. The table read *re-parse 85.3%, tail 3.6%*. This
finding diagnosed exactly that failure mode for its own floor row a few
paragraphs down — *"it was pricing two things and calling it one"* — and did not
apply it here.

**The instrument is the per-pass timer** added for F13, which charges each pass
where it runs. Streaming 40 real documents one line at a time and deriving after
every line — what a feed actually does — 4,109 boundaries, three runs, spread
under 1%:

| | ms | of the feed |
|---|---|---|
| a feed today | ~79.8 | 100% |
| — the clone (and everything not below) | ~9.6 | **12%** |
| — **the inline parse** | ~34.5 | **43%** |
| — **the projection's tail** | ~35.7 | **45%** |
| &nbsp;&nbsp;&nbsp;· autolink `postprocess` | 17.0 | 21% |
| &nbsp;&nbsp;&nbsp;· `consolidate_text_nodes` | 12.3 | 15% |
| &nbsp;&nbsp;&nbsp;· `S_strip_html_comments` | 4.4 | 5% |
| &nbsp;&nbsp;&nbsp;· formula `postprocess` | 2.0 | 3% |

**So the clone is a ninth of a feed and the two whole-document passes are the
rest, roughly equally.** That is a stronger case for T18 than the corrected
table it replaces, not a weaker one: the tail was never a 3.6% residual.

**And almost all of that re-parse is redundant.** At every feed boundary,
comparing each block's whole projected subtree against what it was at the
previous boundary, over 4,353,444 block-observations:

| reuse rule | hit rate |
|---|---|
| upper bound — the block's projection is unchanged | **98.6%** |
| what an O(1) check can actually authorise — unchanged **and** no definition arrived anywhere | **90.1%** |

The second row is the one an engine can implement: a per-block write stamp (T3)
and a definition-map generation (T4), both O(1). The maps are global, so one
definition arriving invalidates every block at once — and that crudeness costs
**8.5 points, not the collapse F6's "no cheap middle" warns about for the
`contains [` filter.** 2,264 of 40,583 boundaries bumped a map.

**What reuse costs instead.** A reused subtree cannot be handed out twice, so
the question is what it costs to put a cached projection into the tree a feed
returns. Deep-copying a fully projected tree — inlines included, and **verified
byte-identical to the derived tree on all 195 documents, 0 mismatches** — costs
**1.66 ms, 2.38× cheaper than the re-parse it replaces**. Not cheaper still,
because the bytes are genuinely new: a census of the projected trees says
**92.4% of inline chunks are OWNED, not borrowed** (31,383 of 33,969; 93.9% of
the bytes), which is entity decoding, escape processing and smart punctuation
each producing bytes the source does not contain. So "share the block's content
buffer instead of copying" is not available.

**The two shapes — BUILT AND MEASURED, and the arithmetic was wrong.** Both
were first projected from the parts above; one of them is now measured and the
projection was not merely off, it had the sign wrong. The reuse was built into
`process_inlines` under exactly the O(1) rule (block content unwritten and no
definition arrived), and the whole corpus was then streamed one line at a time,
deriving after every line — what a `Session` actually does — once with the
re-parse and once with reuse:

```
195 documents, 40,583 feed boundaries, cache hit rate 91.3%
  TREES IDENTICAL AT EVERY BOUNDARY: yes, 0 documents differed

  re-parse every block           1203 ms    1.00x   (what a feed does today)
  reuse, cached subtree COPIED   1270 ms    0.95x   -- 5.6% SLOWER
  reuse, cached subtree SHARED    310 ms    3.89x   -- floor, see below
```

**The rule survived the adversarial corpus too.** The same comparison over
every fixture file, which is where the hard cases live — the definition
arriving after the lookup, the table retype, both formula promotions, the
directive grammars, smart punctuation:

| fixture | trees identical at every boundary |
|---|---|
| `spec` / `regression` / `extensions` | yes / yes / yes |
| `extensions-conflicts` / `extensions-directive` | yes / yes |
| `extensions-formula-github` / `-latex` | yes / yes |
| `smart_punct` | yes |

That is **not a proof** — the reuse predicate here is "the block's content size
is unchanged", which is weaker than the write stamp T3 would give it, and a
corpus is not a theorem. But the rule as stated has now been run rather than
argued, over every case this repository has.

**Copying a cached subtree does not pay, and that is the finding.** The
whole-tree copy above is 2.38× cheaper than the whole-tree re-parse, but a feed
does neither in bulk — it does one block at a time, and **most blocks are
small**. Parsing a short paragraph is a linear scan over a few dozen bytes;
copying its three or four inline nodes is three or four `calloc`s. At that size
the allocator costs more than the scanner, so per-block reuse-by-copy loses the
2.38× and then some. The 1.8× projected here before it was built was arithmetic
from a bulk measurement applied to a per-block operation, and it was wrong.

The 310 ms row is a **floor, not a working engine**: reuse is charged nothing
at all, so the trees it produces are wrong and are never compared. It measures
what a feed would cost if handing a cached subtree back were free — i.e. if the
cache were **shared** into the returned tree instead of copied into it. That is the shape worth building. **It is not worth 3.89×** — that row prices
the reuse and the tail together, and the built measurements are 1.45× (F12),
2.74× with a per-block tail (F13) and 2.95× with the declared filter (F15).

**The free path was never the problem, and the correction matters.** An earlier
draft of this finding called it a decision T9 had to make first. **Wrong.** The
tree a feed returns is a **borrow**, so nothing about who calls
`markdown_core_node_free` is at stake. What is at stake is a lifetime: a
consumer holding a borrowed tree while `feed` is called again, replacing the
projection it is holding. **That is what a reference count is for** — the
borrow keeps its nodes alive across the feed, and they are destroyed when the
last hold goes. Owner correction, 2026-08-25.

**Built, at subtree-holder granularity.** One counter per cached block, not one
per inline: `refs` on the holder,
[`S_free_nodes`](../packages/markdown-core/core/node.c#L178) releases a hold
instead of destroying and destroys at zero, and a derived block whose children
are borrowed **detaches them rather than splicing them into the free walk** —
which matters because the splice rewrites the sibling chain the cache shares.
Every node the parser builds has `refs == 0` from `calloc`, so every path that
existed before the cache is untouched.

**It taught two things, and the second is the important one.**

**(1) The store must COPY, not MOVE.** The first shape tried here moved the
block's freshly parsed children into the holder and let the block borrow them
straight back — and it corrupts, because the whole-tree tail runs *after*
`process_inlines` and rewrites the very children the cache now holds. Storing a
copy costs one copy per **miss** — under 1% of lookups at the measured hit rate
— and leaves the tree being built completely untouched.

**(2) Sharing wins 1.45×, not 4×, and the reason is the tail.** Measured over
40 real documents, 4,109 feed boundaries, hit rate 99.3%, each pass streaming
the document one line at a time and deriving after every line:

```
  re-parse every block            75.11 ms   1.00x   (what a feed does today)
  reuse by COPYING                76.13 ms   0.99x   -- slower, as measured before
  reuse by SHARING + refcount     51.81 ms   1.45x   <- trees identical on 39 of 40
  the same, tail suppressed       13.41 ms   5.60x   -- WRONG trees; prices the tail
```

**So the whole-tree tail is 38.4 ms — 74% of what a feed costs once the
re-parse is cached away.** `markdown_core_consolidate_text_nodes`,
every extension `postprocess_func` and `S_strip_html_comments` run over the
**entire projected tree on every projection**
([blocks.c:1687-1700](../packages/markdown-core/core/blocks.c#L1687-L1700)). In a stream it runs
at every boundary over the whole accumulated document, so it is **O(document)
per feed and no inline cache touches it**. The earlier "3.89× floor" in this
finding was measured with reuse charged nothing at all, which also emptied the
tree the tail had to walk — so it was pricing two things and calling it one.

**What that means for the plan.** Caching the inline parse is necessary and is
worth 1.45×; it is **not sufficient**. A feed that carries no term in the
document already fed — T15's bound — additionally needs the tail to be
per-block, or gated on the changed set. That is a task the plan does not
currently have.

**One defect stood here and F16 closed it.** Of 40 real documents, one —
`bail`'s readme at boundary 113 — diverged under sharing and not under copying,
and this finding recorded it as unexplained. **F13 observed it disappear** under
a per-block tail and inferred the cause; **F16 proved it with ASan**: the
whole-tree consolidation frees nodes the cache holds, so it is a use-after-free
rather than a wrong tree. Either way the conclusion is the same and it is
structural: **a per-block tail is a precondition of sharing, not an optimisation
on top of it.**

### F13 — the tail is as big as the inline parse, and per-block it collapses 14× — but it is also what makes sharing CORRECT  · VERIFIED

Owner question, 2026-08-25: with a Session and a finalize, should the
whole-document operations happen only at finalize, and should the rest of the
post-processing be per-block rather than per-document?

**Half the premise does not hold, and it matters.** Reference and footnote
resolution are **not** whole-document operations and are **not** in the tail.
They happen inside `markdown_core_parse_inlines` — the refmap lookup at
[inlines.c:1857](../packages/markdown-core/core/inlines.c#L1857), the footnote
set at [inlines.c:1724](../packages/markdown-core/core/inlines.c#L1724) — so
they are already per-block, and they are exactly the work the cache reuses.
There is no whole-document footnote pass; the one that existed was deleted
(D11, and [blocks.c:211](../packages/markdown-core/core/blocks.c#L211) still
carries the note). **And deferring them to finalize is ruled out by the API's
own semantics**: `feed` RETURNS the document, so it is a parse of the bytes fed
so far (§4, D1). A definition already fed but not yet resolved would make
`updated.semantic` show prose where the bytes say link. F7 sized that
disagreement at **27.40% of stream boundaries**, and it is the thing D1 was
dissolved to prevent.

**The other half holds, and the numbers are larger than expected.** The tail is
exactly four passes, and summed over one streaming run of 40 real documents
(4,109 boundaries) it is **as large as the inline parse it follows**:

| pass | ms | share of the tail |
|---|---|---|
| `process_inlines` | 35.56 | (not the tail) |
| **autolink `postprocess`** | **17.11** | **46%** |
| `consolidate_text_nodes` | 13.03 | 35% |
| `S_strip_html_comments` | 4.57 | 12% |
| formula `postprocess` | 2.16 | 6% |
| **tail total** | **36.87** | 100% |

Moving consolidate, autolink and the strip per-block — run on a block right
after its inlines are parsed, and **skipped entirely for a block whose
projection came from cache** — collapses the tail to **2.54 ms, 14.5×**, and of
what is left **2.02 ms is formula's postprocessor**, the one pass this PoC
deliberately left whole-tree.

```
40 real documents, 4,109 boundaries, hit rate 99.3%
  re-parse every block                82.23 ms   1.00x
  share + refcount, whole-tree tail   59.63 ms   1.38x   1 document differs
  share + refcount, PER-BLOCK tail    30.02 ms   2.74x   0 documents differ
```

Trees byte-identical at every boundary on all 40 real documents and on six
fixture corpora — `regression`, `extensions-conflicts`, `extensions-directive`,
`extensions-formula-github`, `extensions-formula-latex`, `smart_punct` — each
of which is where one of the tail's own constructs lives.

**And this is the finding that was not expected: the per-block tail is what
makes SHARING correct.** F12 left one unexplained defect — `bail`'s readme
diverging under sharing and not under copying — and it disappears here: **the
whole-tree tail reaches the shared cached nodes**, because it walks the returned
tree without knowing which of it came from a cache. With the tail per-block and
skipped on a hit, nothing touches a reused subtree again. *This finding inferred
that mechanism from the disappearance; F16 proves it with ASan.* Sharing and a per-block tail are not two independent
optimisations — **the second is a precondition of the first.**

**Three requirements the build turned up, each of which cost a crash or a wrong
tree to find.**

1. **The hook cannot live inside `process_inlines`'s `contains_inlines`
   branch.** An `HTML_BLOCK` has tail work and no inline content, so a tail
   hosted there never reaches it: **14 of 40 real documents kept an HTML
   comment** that the whole-tree pass strips. `CODE_BLOCK` is the same shape for
   the formula promotion. A per-block tail needs a hook over **every** block.
2. **It cannot free the block during the walk — at `ENTER` or at `EXIT`.** Both
   segfault. `EXIT` is the iterator's documented mutation point, but the walk is
   still standing on the node and reads it to advance. Candidates must be
   collected during the walk and acted on after it. The work stays bounded by
   the number of such blocks, not by the size of the tree.
3. **`S_strip_html_comments` cannot be called with a block that is itself a
   comment.** It frees `root` inside its own walk and then hands that freed
   pointer to `markdown_core_consolidate_text_nodes(root)` on the way out
   ([blocks.c:79-105](../packages/markdown-core/core/blocks.c#L79-L105)) — it is
   written for a root that survives. Per-block, **the predicate and the removal
   have to come apart**: ask during the walk, remove after.

**What is still whole-document after the move.** Formula's `postprocess`, at
2.02 ms of the remaining 2.54 ms, because it **replaces the block node in its
parent** ([formula.c:788](../packages/markdown-core/extensions/formula.c#L788))
and doing that to the node a walk is standing on is requirement 2 again. It is
the last whole-document term in the tail, and it is small — but it is not zero,
so **T15's "no term in the document already fed" still fails until it moves**,
by the same collect-then-act shape as the strip.

### F14 — formula's postprocess is THREE things, only one of them needs the projection, and moving is not the same as skipping  · VERIFIED

Owner question, 2026-08-25: why does formula have a postprocess at all — did we
invent a formula-only paragraph reshape, and can it be removed?

**It is not one thing.** `postprocess_node`
([formula.c:798](../packages/markdown-core/extensions/formula.c#L798)) has three
arms and returns after whichever it matches:

| arm | what it does | needs the block's inlines? |
|---|---|---|
| 1 · [formula.c:806](../packages/markdown-core/extensions/formula.c#L806) | a `FORMULA_BLOCK` copies its literal out of `node->content` and clears the buffer | **no** |
| 2 · [formula.c:820](../packages/markdown-core/extensions/formula.c#L820) | a `CODE_BLOCK` with info `formula` is replaced by a `FORMULA_BLOCK` | **no** |
| 3 · [formula.c:833](../packages/markdown-core/extensions/formula.c#L833) | a `PARAGRAPH` whose only child is a standalone `FORMULA` is replaced by a `FORMULA_BLOCK` | **yes** |

**Arm 3 is the reshape, and it cannot be removed — it is the semantics of a
display formula written on one line.** Deleting it turns `$$x+y$$` and
`\\[x+y\\]` alone on a line from a `FormulaBlock` into a `Paragraph` containing
a `Formula`. What pins it is **five executable gates**, not prose:

| gate | what moves |
|---|---|
| [extensions-formula-github.txt:97-100](../packages/markdown-core/tests/fixtures/extensions-formula-github.txt#L97-L100) | `$$x+y$$` |
| [extensions-formula-latex.txt:54-58](../packages/markdown-core/tests/fixtures/extensions-formula-latex.txt#L54-L58) | `\\[x+y\\]` |
| [specs/canonical-ast/formulas.ast:8](../specs/canonical-ast/formulas.ast#L8) | the cross-language conformance oracle — C, Swift, Kotlin and ES all read it |
| [tests/api/main.c:487-501](../packages/markdown-core/tests/api/main.c#L487-L501) | asserts the first child of `$$x+y$$\n` is `formula_block` |
| `specs/mdast-parity/deltas.json`, id `github-display-math-block` | a REGISTERED divergence; the gate fails when the two sides stop diverging |

**The prose contract does not pin it, and an earlier draft of this finding said
it did.** [canonical-ast.md:134](specs/canonical-ast.md#L134) constrains a
`FormulaBlock`'s *mode*, not which inputs produce one — and
[canonical-ast.md:69-70](specs/canonical-ast.md#L69-L70) leans the other way:
*"`Formula` may be `standalone` while remaining inside a paragraph."* The
citation was wrong; the conclusion survives on the gates above. The multi-line forms —
`$$` / `\\[` on their own line — never reach arm 3 at all: they open a real
`FORMULA_BLOCK` in the block phase
([formula.c:301](../packages/markdown-core/extensions/formula.c#L301)) and are
arm 1's business.

**But the pass does not have to be in the projection.** Arms 1 and 2 do not
read inlines, so they are block-phase work that is currently redone on every
feed:

- **Arm 1 can be DUPLICATED into `close_block_func` — it cannot be moved
  there.** A `FORMULA_BLOCK` carries the extension so the hook fires
  ([blocks.c:1123](../packages/markdown-core/core/blocks.c#L1123)), and a clone
  already receives the literal
  ([formula.c:154-164](../packages/markdown-core/extensions/formula.c#L154-L164)),
  so at close it is sound. **But it must stay in the projection as well**,
  because a formula block's `closed` lags one line by design
  ([formula.c:336](../packages/markdown-core/extensions/formula.c#L336), §1's
  table) — a derivation taken between the closing fence and `finalize` sees an
  open block whose literal comes from nowhere else. Removing it from
  `postprocess` regresses that boundary to `literal=""`, **and
  `ctest --preset correctness` stays 88/88 while it does** — a gate gap worth
  its own row.
  Two corrections to what this finding first said. Clearing `node->content` is
  **not** a memory saving: `markdown_core_strbuf_clear`
  ([buffer.c](../packages/markdown-core/core/buffer.c)) sets `size = 0` and
  leaves `ptr` and `asize` alone, so nothing returns to the allocator —
  `_free` would. And the close hook's own contract says it is for saying HOW a
  block closed, and that *"a close hook that changed the tree would be a second
  `postprocess` with a worse name"*
  ([markdown-core-extension-api.h:331-345](../packages/markdown-core/core/markdown-core-extension-api.h#L331-L345)).
  Putting a rewrite there needs that contract re-ruled first.
- **Arm 2 cannot use `close_block_func`, for two independent reasons.** The hook
  runs at [blocks.c:1123](../packages/markdown-core/core/blocks.c#L1123),
  **before** the switch that computes `as.code.info` at
  [blocks.c:1182](../packages/markdown-core/core/blocks.c#L1182) — so the info
  string it tests does not exist yet. And a core `CODE_BLOCK` has no
  `extension`, so the hook would not fire on it at all. Arm 2 needs a different
  block-phase home, not this one.
- **Arm 3 stays in the projection**, because the fact it tests is only knowable
  once that paragraph's inlines are parsed — but that is a **per-block** fact,
  not a whole-document one.

**Measured, and this is the part that matters: moving all three per-block keeps
the trees identical and does NOT make them cheaper.** Over 40 real documents
and 4,109 boundaries, with the three arms taken out of the whole-tree walk and
driven from a collect-then-act queue over leaf blocks: **0 documents differ**,
and formula's own cost goes **2.52 ms → 2.66 ms**. It did not move because the
queue runs for every leaf block on every feed **regardless of whether that
block's projection came from cache** — and arm 3 has to, because the clone
rebuilds the paragraph from the CST every time, so the promotion is redone even
when nothing changed. **Moving a pass per-block is not the same as skipping it**,
and only skipping buys anything. What arm 3 needs is its verdict memoised on the
CST block under the same key as the inline cache; arms 1 and 2 need to leave the
projection entirely, which is the paragraph above.

**A constraint the build turned up: the core cannot name an extension's block
type.** `MARKDOWN_CORE_NODE_FORMULA_BLOCK` lives in
`extensions/markdown-core-extensions.h`, which `core/blocks.c` does not include,
so a per-block postprocess hook cannot be written as "queue the formula blocks".
It has to be expressed in terms the core knows — **a BLOCK-class node whose
children are all inline**, i.e. a leaf block — which also keeps
`postprocess_node`'s own recursion bounded by the block, because a container is
never queued. That is a requirement on T18's hook, not a detail of this PoC.

**Independently corroborated.** A separate multi-agent pass over the same
question reproduced two of the three hazards F13 records, by building and
running rather than by reading: running the tail at a block's `ENTER` — where
`process_inlines` parses — corrupts the walk when formula replaces the node; and
`S_strip_html_comments` frees its own `root` and then dereferences it, witnessed
by a document whose top-level block is `<!-- -->`. It also **refuted** a hazard
worth having refuted: consolidation merges only siblings and no tail pass ever
moves a node to a different parent, so there is no cross-block text run and
per-block consolidation is sound.

### F15 — the missing mechanism is a declared TYPE-NAME filter on the descriptor  · VERIFIED

Owner ruling, 2026-08-25: the per-block hook should scan by block, and what is
missing is not visibility of the type — every block has a type name by
definition — but the ability to **filter by type name**.

**That is exactly right, and it is the fix for what F14 measured.** F14 moved
formula's three arms per-block and they got no cheaper (2.52 → 2.66 ms), because
the core had no way to say which blocks the extension wanted, so it queued every
leaf block on every feed. The core cannot name `FORMULA_BLOCK` — it lives in
`extensions/markdown-core-extensions.h` — but **both sides already share a
vocabulary**: `markdown_core_node_get_type_string`
([node.c:242](../packages/markdown-core/core/node.c#L242)) answers a name for
every node, asking the extension's own `get_type_string_func` first and falling
back to the core's table. `formula` answers `"formula_block"`
([formula.c:714](../packages/markdown-core/extensions/formula.c#L714)).

**The descriptor declares it, in the idiom it already uses — and the idiom is
NUL separation, not commas.** `.terminates_text` and `.dispatch` are sets whose
element is a byte, so a C string holds one directly
([syntax_extension.h:28-33](../packages/markdown-core/core/syntax_extension.h#L28-L33)).
Here the element is itself a name, so the container is the same string with the
element boundary written out — which makes each compare a plain `strcmp`, with
no tokenizer and no bounded span:

```c
.postprocess_blocks = "formula_block\0code_block\0paragraph",
```

walked as `for (p = set; *p; p += strlen(p) + 1)`. NULL means no per-block hook,
so an extension that declares nothing is unaffected. **The PoC the numbers below
came from used a comma-separated list**, which needs a bounded compare; the NUL
form is what the idiom asks for and it does not change what was measured.

**One member is not a name.** `"*inlines"` selects every block the parser's own
`contains_inlines` ([blocks.c:362](../packages/markdown-core/core/blocks.c#L362))
answers true for, and `*` cannot begin a type name so the two kinds cannot
collide. **Autolink needs it and a name list would be wrong for it**, for three
mechanical reasons: `table_cell` is a name minted by `table`
([table.c:849](../packages/markdown-core/extensions/table.c#L849)) and table
attaches last, so a name list would make autolink's block set depend on another
extension being present; `contains_inlines` is itself extension-extensible
(`contains_inlines_func`), so a frozen list would silently skip any future block
that grows inlines — a wrong tree nothing in the build would catch; and it is
the predicate the inline parse already uses at
[blocks.c:1334](../packages/markdown-core/core/blocks.c#L1334), so filter and
parse agree by construction rather than by maintenance.

**And it wants a NEW hook, not `postprocess_func` with a filter bolted on.**
`markdown_core_postprocess_func` returns a root, and formula's returns `root`
unconditionally after an arm may have freed it — safe today only because `root`
is always the DOCUMENT. A per-block call is handed a node that CAN match, so the
channel has to say what happened to the *node*:

```c
typedef void (*markdown_core_postprocess_block_func)(
    const markdown_core_syntax_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node **block   /* IN/OUT: reseat on replace, NULL on remove */
);
```

An out-parameter has one spelling per outcome — unchanged, reseated, gone —
where a return value cannot separate *removed* from *unchanged* without a
sentinel. This PoC instead ignores the return value, which avoids the
use-after-free but cannot express a removal.

**Two rules the dispatch must carry, and this PoC gets the second one wrong.**

1. **Re-resolve after a reseat.** A whole-tree pass sees whatever node occupies
   a place *when it runs*, so if extension *i* replaces the block, extension
   *j* after it must be re-matched against the NEW name — otherwise nothing
   declaring `formula_block` would ever see a formula block born from a
   `code_block`. This PoC queues `(node, extension)` pairs once and never
   re-resolves; the bug is latent only because formula is the sole declarer
   today.
2. **The dispatch belongs behind the cache check for passes that rewrite a
   block's CHILDREN — and must NOT be skipped for one that replaces the block
   NODE.** The cache holds a block's children, never its node: on a hit the
   clone still rebuilds the block itself from the CST, so a `PARAGRAPH` that
   arm 3 promotes is a fresh `PARAGRAPH` every projection. Skip its dispatch and
   `$$x+y$$` comes back as `Paragraph > Formula`, failing all five of F14's
   gates. So consolidate, autolink and the strip go behind the check — at F13's
   99.3% hit rate that makes their per-feed population the *miss set* rather
   than the document — while a node-replacing postprocess either stays in front
   of it or T9 additionally caches the block's projected identity so a hit can
   reproduce the promotion. **T18 must say which of those two it builds.** This
   PoC queues at the EXIT of every block regardless, which is why formula's own
   cost only fell from 1.98 ms to 1.53 and not to nearly nothing.

**Measured.** Same 40 real documents, 4,109 boundaries, **trees byte-identical
at every boundary**:

| | tail total | formula's own share |
|---|---|---|
| today, whole-tree | 35.19 ms | 1.98 ms |
| per-block, **filtered by declared type name** | **2.10 ms** | **1.53 ms** |
| *(F14's unfiltered run, a DIFFERENT run)* | *3.34 ms* | *2.66 ms* |

The first two rows are one run and are comparable. **The third is F14's and is
not** — its whole-tree baseline was 2.52 ms where this run's is 1.98, so
"worse than whole-tree" is a claim about F14's own run, not a cell in this
table. Every ms figure in F13–F15 is a single run of the streaming loop over the
40-document subset; the run-to-run spread on formula's share alone is about
0.5 ms, so **no delta under half a millisecond in these three findings is a
finding.**

The tail collapses **16.8×**, and formula's own cost lands *below* what the
whole-tree walk cost — which the unfiltered queue could not do. **The filter is
what turns "moved" into "skipped"**, which F14 named as the thing that matters
and could not deliver.

**The memo is keyed on the NAME, and that is the only correct key.** An earlier
draft of this finding said to memoise on the block's numeric type and to
special-case nodes carrying an extension, because
`markdown_core_node_get_type_string` asks the extension first. **That was both
over-complicated and incomplete.** Over-complicated because the reason for it
does not hold: resolving the name is a branch and a switch returning a string
**literal** — all twelve returns across the five extensions that define
`get_type_string_func`, and the core's own table — so it is not what costs
anything. What costs is comparing that name against a declared list once per
block per extension, and that is what a memo should hold. Incomplete because the
aliasing is not one extension's quirk:

| extension | same node type, different names |
|---|---|
| `tasklist` | a `LIST_ITEM` carrying it answers `"tasklist"` ([tasklist.c:16](../packages/markdown-core/extensions/tasklist.c#L16)); a plain one answers `"list_item"` |
| `table` | a `TABLE_ROW` answers `"table_header"` or `"table_row"` depending on `is_header` ([table.c:844-847](../packages/markdown-core/extensions/table.c#L844-L847)) |

**The name is a function of the NODE, not of the type.** Key the memo on the
name and both rows stop being special cases; key it on the type and both are
wrong. And because every implementation returns a literal, the key can be the
**pointer** — the steady state is a pointer compare, with a content compare only
on a miss.

Measured against the type-keyed version it costs nothing: tail 35.97 → 2.23 ms
and 40.56 → 2.12 ms over two runs, formula's own share 2.03 → 1.66 and
2.22 → 1.52, trees identical throughout. **Same speed, no special case.**

**Two hazards the dispatch must carry, both found by building it.**

- **`postprocess_func` can return a freed pointer.** `replace_with_formula_block`
  frees `oldnode` ([formula.c:788-791](../packages/markdown-core/extensions/formula.c#L788-L791))
  and `postprocess` then returns `root` unconditionally
  ([formula.c:857-858](../packages/markdown-core/extensions/formula.c#L857-L858)) — safe
  today only because `root` is always the DOCUMENT, which matches none of the
  three arms. A name-dispatched call is handed a block that CAN match, so **the
  dispatch must ignore the return value** (this PoC does), and must treat a node
  as possibly-replaced for any extension still holding it queued.
- **Order within a block is result-relevant; order across blocks is free.** The
  sequence per block must stay `parse_inlines → consolidate → each postprocess
  in attach order → strip`, and two reorderings change the tree silently:
  stripping before autolink turns `a<!-- x -->@b.com` into a link, and stripping
  before formula promotes `$$x$$<!-- c -->` to a `FormulaBlock`. Across blocks
  the order does not matter, and that is the theorem the whole decomposition
  rests on: consolidation merges only siblings, and **no tail pass ever moves a
  node to a different parent**.

### F16 — sharing while the tail walks the whole tree is a use-after-free, not a slow path  · VERIFIED

Owner correction, 2026-08-25: **a bench regression in an intermediate state is
not an argument about ordering.** It was being used as one here, twice. Worse,
the 0.80× that produced the argument prices *the per-block tail with no cache*,
which **is** a state the order passes through — T18 and T9 are separate
landings. What is wrong is using it to order them. Ordering is decided by the
use-after-free below; the transient regression sits in an unreleased
intermediate commit and is accepted, and T20 lands first and removes about a
quarter of it.

**What is real, and ASan proves it rather than inferring it.** Sharing a cached
subtree into the returned tree while the tail still walks the whole tree is not
a wrong tree — it is a **use-after-free**:

```
freed by   markdown_core_consolidate_text_nodes  (iterator.c:151)
           <- the whole-tree consolidation, merging adjacent TEXT nodes
use after  the cache store
```

On a cache hit the block's children ARE the holder's children, so the whole-tree
consolidation reaches them, merges them and **frees nodes the cache still points
at**. This is the mechanism F12 recorded as an unexplained divergence on one
document and F13 observed disappearing under a per-block tail; neither had
proved it. It is proved here, on a 114-line reduction of that document.

**So the tail goes first and the cache shares from the start.** The per-block
tail is correct on its own — with no cache present at all its trees are
byte-identical to today's over the 40-document real corpus and the six fixture
corpora it was run against (`spec.txt` and `extensions.txt` were not) — so
ordering it before the cache removes the hazard entirely, rather than staging a
copying hand-out that would be built and then discarded. That is the order in
§5. It is not free: the intermediate commit measures 0.80×, and that is
accepted rather than argued away.

**One thing found on the way is worth keeping.** `markdown_core_iter_new`
callocs ([iterator.c:15](../packages/markdown-core/core/iterator.c#L15)), which
is right once per document and wrong once per block per pass per feed — three
passes over a hundred-block document go from 3 allocations to 300. The struct is
already complete in the internal header, so the walk can live on the caller's
stack: adding `markdown_core_iter_init` and using it in consolidation, the
comment strip and autolink's postprocess is worth a quarter of the per-block
tail's overhead, with `ctest --preset correctness` still **88/88**. That is T20,
and it is independent of everything else.

**And one re-ordering does stand, on a different ground.** §5 put identity
(T2, T5) ahead of the cache. The cache keys on the CST block it hangs from plus
T3's stamp and T4's generation and **never needs an id** — identity serves the
consumer's change classification, which is a different axis. Putting it first
delays the only work that answers the owner's constraint on `feed`.

---

### F17 — a shared list can carry no parent, and a holder's list must own its bytes  · VERIFIED (T19)

Two things T19's gate found that the D4/F12 PoC could not, because its
measurement never had two derived trees alive at once: it freed each tree
before deriving the next.

**1. `parent` is per node, and a shared list is aliased by several.** The
PoC's hand-out rewrote each shared child's `parent` to the block it was
lending to (`c->parent = cur`). That is right for one live borrower and wrong
for two: the tree the consumer still holds and the tree the next feed returned
alias the same list, and the walk out of the OLDER tree's list climbs, via
`parent`, into the NEWER tree. That is the exact case §5's gate names — *a
borrow held across a feed that replaces it still reads* — so the PoC's shape
would have failed T19's own gate. The shape that holds: a shared list's nodes
have **`parent == NULL`**, and the iterator remembers the borrower it entered
through and climbs out to it
([iterator.h](../packages/markdown-core/core/iterator.h)) — one slot, because
a list is one level deep (a node in a holder's list is never itself a
borrower). `markdown_core_node_own` and `markdown_core_node_check` climb the
same way. Two consequences, stated so they are not later read as defects:
`markdown_core_node_parent` on a shared inline answers NULL, and an insert
beside one fails closed, having no parent to insert under — a borrower READS
a shared list and never writes it.

**2. A holder outlives the block it was filled from, so its list must own its
bytes.** The gate's first run failed 11 of 669 spec examples, 1 of 43
regression and 4 of 33 extensions, every one on the fourth assertion only —
the new tree stopped reading once the previous borrower was freed. The moved
inlines held chunks that borrowed the block's `content` buffer (F12's census
counted 7.6% of inline chunks BORROWED; this is where they point), and the
block died with its tree. `markdown_core_holder_take_children` therefore runs
`markdown_core_node_own` on the way in. **This is a requirement on T9's
store, not only on the gate's**: the store copies rather than moves, and the
copy has to produce OWNED chunks for the same reason — a copy that borrowed
the source block's buffer would fail identically the moment the derived tree
that block sits in is freed.

**Cost.** The holder is a struct, not a node — `mem`, the list's two ends,
`refs` — so `refs` lives on the holder and the node carries one pointer:
`sizeof(markdown_core_node)` 176 → **184**, where the PoC's node-holder shape
paid 16 (`refs` and `borrowed_from` on every node). Same `bench_runner` and
method as F1, 41 cases, **5 rounds**, head and `1ce6938` alternated within
every round, min of `median_ms`:

```
total   control=247.29 ms   head=247.10 ms   ratio=0.9992
median per-case ratio 1.0058     slower >5%: 0 of 41     faster >5%: 1
```

The one >5% is `adversarial_links@65536` at 0.929 with its @16384/@32768
siblings at 1.022 / 0.999 — the signature F1 and F11 both recorded as noise
for that family. The pointer is not free; at 5 samples it is under the
half-millisecond the run-to-run spread already occupies.

---

### F18 — the per-block tail is byte-identical at 101,000 boundaries, and two things it found  · VERIFIED (T18)

**The gate, run rather than argued.** `dump_boundaries` (in
`projection_runner`) feeds every document one line at a time, derives after
every line, finishes, and prints each tree's canonical dump — once with every
extension and once with the comment strip on, since the strip is the one
tail pass with a removal path of its own. The T20 build (`bd59279`, the
whole-tree tail) and the T18 build ran it over the same inputs and the
outputs were `cmp`-identical:

| input | dumps | plain + strip |
|---|---|---|
| `spec` | 4,586 | identical |
| `extensions` | 598 | identical |
| `regression` / `smart_punct` | 404 / 102 | identical |
| `extensions-conflicts` / `-directive` / `-directive-option-gates` | 68 / 350 / 8 | identical |
| `extensions-formula-github` / `-latex` / `-conflicts` / `-option-gates` | 96 / 80 / 32 / 28 | identical |
| real corpus, 195 documents | **81,556** | identical |
| second real corpus, 1,191 documents | 14,828 | identical |

`spec.txt` and `extensions.txt` were the two corpora §7 said the per-block
tail had never been run against; they are in the table. Correctness 91/91,
conformance 2/2, asan and ubsan 79/79, goldens unmoved.

**1. A NUL-separated name set ends with an EMPTY name, and the first build
did not.** `for (p = set; *p; p += strlen(p) + 1)` — F15's idiom — steps
past the last name onto whatever byte follows the literal, because a C
literal carries one NUL and the walk needs two. `"*inlines"` therefore walked
off the end into the neighbouring string in `.rodata`; the default build
compared garbage names, matched none, and produced identical trees anyway,
and **only ASan saw it**: `global-buffer-overflow` in `S_extension_wants`, 7
of 79 rows. The descriptors now spell the terminator out
(`"formula_block\0code_block\0paragraph\0"`) and
[syntax_extension.h](../packages/markdown-core/core/syntax_extension.h) says
why. A byte set needs no such thing, which is exactly why the idiom looked
complete. Recorded because a correct tree is not evidence that the walk that
built it stayed inside its bounds.

**2. `audit-ast-projections` has been red since `ae0bb0b`, and it was not
the engine.** Every gate row was swept for T18 and this one failed at the
T18 tree, at the T20 control, and at `ae0bb0b` — green at `8d0910b`. The
block-indented wrap put the C dump's last kind name on a line with no
trailing comma and the audit's regex wanted `,` or `}` after the quote, so
the row has read the C dump as one kind short for four commits. Fixed in
`e3565ad`, script only. F2's lesson, a third costume: a red row is a fact
about the row's instrument until it is checked against a tree known to be
green.

**Cost.** The one-shot path and the feed loop, head against `bd59279`, built
and run in one session. One-shot: same `bench_runner`, 41 cases, 5 rounds,
alternated within every round, min of `median_ms`:

```
total   control=249.00 ms   head=234.39 ms   ratio=0.9413   (-5.9%)
median per-case ratio 0.9944     slower >5%: 3 of 41     faster >5%: 11

deep_nesting@8192 / @16384 / @32768              0.743 / 0.722 / 0.710
large_document@128 / @256 / @512                 0.949 / 0.915 / 0.912
extensions@100 / @200 / @400                     0.955 / 0.994 / 0.962
adversarial_links@16384 / @32768 / @65536        0.968 / 0.973 / 0.981
inline-entity / block-fences / block-heading     1.068 / 1.067 / 1.062   (0.65 / 0.09 / 0.26 ms cases)
```

The feed loop, which is what a `Session` does — every document fed one line
at a time, a derivation after every line, a finish at the end — over the 195
real documents and the two largest fixtures, three alternated rounds each
the min of three, best of the nine:

```
control       head    head/control
real corpus   195 documents, 40,583 boundaries   1388.42 ms  1400.67 ms   1.009
spec.txt      669 documents,  1,624 boundaries      1.42 ms     1.57 ms   (+0.15 ms)
extensions.txt 33 documents,    266 boundaries      0.84 ms     0.87 ms   (+0.03 ms)

round-to-round, corpus:  control 1388 / 1394 / 1411   head 1401 / 1428 / 1431
```

**The one-shot path got faster, and the feed loop did not get slower.** The
whole-tree tail walked every node of the tree three or four times —
consolidation, autolink (which consolidated again on entry), formula, the
strip. Per block, a container with no inline content has no tail work and
is never queued, let alone walked — which is all of `deep_nesting`'s tree —
and a prose block is walked twice instead of four times, since autolink's
own consolidation is gone and the strip is off by default. The three rows
over 1.05 are the three smallest cases in the table, all under 0.7 ms at
5 samples, which F1 and F11 both ruled is not a reading. On the feed loop
the +0.9% sits inside the round-to-round spread and the two fixture rows
move by less than the half-millisecond §7 says is not a finding. **The
0.80× F16 predicted for this state did not happen**: that number priced a
per-block tail built on `iter_new` — an allocation per block per pass —
with no pre-filter and with autolink consolidating twice; T20 landed first,
as §5 ordered, and the queue only ever holds blocks with work. Per §5 none
of this is an argument about order: T9 is what removes the re-parse the
tail now runs beside.

---

**Amended 2026-08-26, on the landing review: the byte-identity held everywhere
but one place the corpora never looked.** A directive's CST-resident label is
inline-class, so the projection's walk never queues it, and its list missed
every content pass the whole-tree tail used to give it — an unmatched `*`
stayed three TEXT nodes, a `www.` never became a link — and the clone left
`ORIGIN` and a raw CST pointer on the label of every returned tree. The owning
block's tail now runs the label's passes (consolidation, the `"*inlines"`
hooks, the strip, in the block's own order, before the shared numbering), and
the clone enrolls only block-class nodes in the cache. The gate is
`projection_label_tail` — red on the unfixed tree on all three paths (finish,
derive, re-derive) — and the fixed dump is byte-identical to `origin/main`'s
for the label shapes the corpora lack. The same review hardened three gates:
the identity gate's second derivation runs cache-off (a hit aliased the first
tree's lists, so the inline half of its comparison was a node against itself),
`refmap_independence` dumps each tree before the next derivation (F22's
ordering rule, which only `projection_double` had), and the block collection
under the key and identity gates now descends into a labeled directive's
interior.

### F19 — the cache key is sound over 100,000 block observations, and what it would buy  · VERIFIED (T3, T4)

`projection_key` pairs every CST block with its derived block at every line
boundary (pre-order over blocks, strip off so the skeletons stay one to one)
and asserts two things: an unchanged key — the block's write stamp and both
map generations — means a byte-identical derived subtree, and a CLOSED
block's stamp never moves again. **0 stale keys and 0 closed-block writes**
over every fixture and both real corpora. Beside the assertions it counts
what the key would buy T9: of the block observations that revisit a block
seen at the previous boundary, the share whose key was unchanged is the
hit-rate ceiling, and of the changed, the share whose subtree had not in
fact moved is the key's imprecision — spurious, never wrong.

| corpus | boundaries | revisits | key unchanged | changed, of which spurious |
|---|---|---|---|---|
| `spec` | 1,624 | 2,846 | 666 (23.4%) | 2,180 / 416 (19.1%) |
| `regression` | 159 | 368 | 133 (36.1%) | 235 / 20 (8.5%) |
| `extensions` | 266 | 2,351 | 1,836 (78.1%) | 515 / 46 (8.9%) |
| **real corpus, 195 documents** | **40,583** | 4,332,113 | **3,880,072 (89.6%)** | 452,041 / **389,515 (86.2%)** |
| second real corpus, 1,191 documents | 6,223 | 31,835 | 20,826 (65.4%) | 11,009 / 1,915 (17.4%) |

The fixture ceilings are low because a fixture example is a few lines and
most of its revisits are of the OPEN block; the real-corpus row is the one
comparable to F12's 90.1%, which was the same rule measured with content
size in place of the stamp. The spurious share is two things: a definition
arriving bumps a generation and re-keys every block in the document (F6's
crudeness, sized at 8.5 points in F12), and the spine stamp re-keys every
open container on every line whether or not the line reached it.

**Amended 2026-09-01 (#163): the definitional crudeness is closed.** A
map's generation now takes part in the key only for a block whose stored
projection HAD SOMETHING TO ASK that map: the block's inline parse sets a
CONSULTED bit at the candidacy — a reference-form label in range of the
cap, a footnote call — whether or not the map answered or was even
non-empty when asked, the store copies the bits onto the holder, and
`S_cache_fresh` compares a generation only behind its bit. This is not
§1's dead label→sites index: no per-label state, no eager flipping, and
the failure direction is unchanged (a bit set without a lookup is a slow
feed, never a wrong tree). Isolated on a 2,000-paragraph stream with a
definition arriving every 20 blocks: misses 104,100 → **4,100** (25×,
hit rate 97.5% → 99.9%); the wall clock of that synthetic is
clone-dominated, which is #161's case, not this one's. The gate is
`projection_map_immunity`, mutation-verified red under a global key.

**Cost.** `stamp` fills the four bytes before `type` and pads to the next
eight: 184 → **192**. Same `bench_runner`, 41 cases, 5 rounds alternated,
head against the T18 tree `4b43e3e`, min of `median_ms`:

```
total   control=234.75 ms   head=235.80 ms   ratio=1.0045   (+0.4%)
median per-case ratio 1.0022     slower >5%: 1 of 41     faster >5%: 0

deep_nesting@32768 / @16384 / @8192              1.058 / 1.037 / 1.045
```

The one row over 5% has its siblings beside it and they agree, so it is
not noise: the spine stamp is O(depth) per line, and `deep_nesting` is a
document that is one spine 8,192 to 32,768 blocks deep. The block parser
already walks that spine on every line to match its containers, so the
class is unchanged and the constant is what the row reads. Real documents
are a few blocks deep and the rest of the table is inside the spread.

---

### F20 — the cache shares from the start, a feed stops re-parsing, and the clone is the floor  · VERIFIED (T9, T10, T11)

**The shape.** A CST block with inline content keeps its last projection on
a holder hung from the block (`link.holder`, flag `CACHE_OWNER`), keyed by
the block's write stamp (T3) and both map generations (T4). The hit is taken
**at the clone**, where the origin is in hand: `S_clone_block_node` aliases
the holder's list into the derived block (T19's borrow) and the derived
block never needs to find its origin again. A miss remembers the origin in
the same slot (flag `ORIGIN`) and, at the END of its per-block tail (T18),
**moves** its children into a fresh holder and borrows them straight back —
`take_children` owns the chunks that borrowed the block's buffer (F17). The
PoC copied on every miss because the whole-tree tail ran after the store
and rewrote what the cache held; with the tail per block nothing touches a
list after its store, so the move is safe and a miss costs one walk to own
its chunks. `finish` projects the CST in place (T1) and takes hits too: the
block is its own origin, and the cache's hold becomes the borrow's. One slot
carries all three meanings, so the node stays at 192 bytes.

**The first build failed three gates, and each was the gate doing its job.**
(1) `api_engine` and both `oom_sweep` rows segfaulted at `S_project`: the
store check at the end of the tail followed the strip, which had just freed
a comment `HTML_BLOCK` and left `node == NULL` — `0x7a` is `&NULL->flags`.
(2) `projection_refmap_independence` failed on 2 of 43 regression examples
and on spec: the projection against an EMPTY map did not hit, and then
STORED, because the store did not ask which map it had resolved against —
the next projection against the parser's own map served the empty map's
answer. A generation names a map, not a set of definitions; only a
projection against `parser->refmap` takes part now, on both sides.
(3) `pathological_complexity_projection_slope` read **11.15×**: served from
the cache, deriving the same CST again is the whole-CST clone and nothing
else, and the clone's ns/byte is a memory-hierarchy fact — **1.1 ns/byte at
64 KiB, 12.5 at 16 MiB**. With the cache off the gate reads 11.2 → 15.0,
1.339×, exactly what it read before T9. The gate is about the re-projection
being linear in what it projects, so it measures with the cache off; the
cached regime's bound is T15's, and the number says what it will find —
**at 16 MiB the clone is the floor**, and §6 already accepts it.

**Byte-identical at every boundary, cache on.** `dump_boundaries` under the
T3/T4 tree (`ffee53b`) and this one, `cmp`-identical over every input, plain
and with the strip:

```
spec 4,586   extensions 598   regression 404   smart_punct 102
extensions-conflicts 68   -directive 350   -directive-option-gates 8
extensions-formula-github 96   -latex 80   -conflicts 32   -option-gates 28
real corpus, 195 documents      81,556 dumps   identical
second real corpus, 1,191 documents   14,828 dumps   identical
```

**What a feed costs now (T11).** The feed loop — every document fed one
line at a time, a derivation after every line, a finish — three alternated
rounds each the min of three, best of nine; `cached` is this tree,
`nocache` this tree with the cache switched off, `control` the T3/T4 tree:

```
cached      nocache     control    control/cached   hit rate
real corpus   195 docs, 40,583 boundaries   454.95 ms  1416.82 ms  1402.51 ms     3.08x        90.9%  (2,507,041 / 250,481)
corp2       1,191 docs,  6,223 boundaries     6.91 ms     9.90 ms     9.80 ms     1.42x        69.9%
extensions.txt  33 docs,   266 boundaries     0.42 ms     0.87 ms     0.85 ms     2.02x        81.7%
spec.txt       669 docs, 1,624 boundaries     1.55 ms     1.56 ms     1.50 ms     0.97x        24.4%
```

**A feed over the real corpus is 3.08× cheaper, and the cache's bookkeeping
costs nothing when it is off** — `nocache` and `control` are the same number
to within the round-to-round spread. The hit rate is the 90.9% F12 projected
(its O(1) rule read 90.1%, F19's key 89.6%), and the 9% of misses carry
what is left: the open block re-parsed on every line, which §6 accepts, and
the whole document re-keyed each time a definition arrives (F19). `spec`
gains nothing because a spec example is two or three lines and its one
block is open at every boundary; `corp2`'s documents are short for the same
reason. The shape F13 measured on 40 documents — 2.74× with a per-block
tail and a shared cache — holds at full scale and is exceeded, because T20
and the pre-filter took the tail below what the PoC paid. What is NOT
removed is the whole-CST clone (§6): at 16 MiB it is the floor F20's slope
reading found, and T15 carves it out rather than folding it in.

**The one-shot path** pays the cache's bookkeeping and gets nothing back —
a one-shot parse projects once. Same `bench_runner`, 41 cases, 5 alternated
rounds against `ffee53b`, min of `median_ms`:

```
total   control=238.31 ms   head=236.10 ms   ratio=0.9907   (-0.9%)
median per-case ratio 0.9977     slower >5%: 0 of 41     faster >5%: 0
```

**Amended 2026-08-26, on the landing review (PR #119): the key had two
missing axes, both found by an automated reviewer and both confirmed.**
First, the extension set: an attach changes what a projection produces for
every block, closed ones included, and a closed block's stamp never moves —
so `attach` now advances `parser->extension_generation` and the holder
records and compares it, the same shape as a map's generation. Second, the
recording projection: the record-gated diagnostic rows (label-too-long, the
directive codes) speak from the inline parse a hit skips, so a mid-stream
derivation that filled the cache silenced `finish` — `S_cache_fresh` now
answers stale while `diagnostics_on` is raised, which costs nothing unless
diagnostics were retained and keeps `finish`'s in-place hits otherwise. The
gates are `projection_attach_invalidation` and
`projection_diagnostics_after_derive`, each with its vacuity guard: the
probe text must differ un-autolinked, and the control must raise rows to
lose.

**Amended 2026-08-31: the recording guard read the wrong flag and the cache
never hit through the public surface.** "Answers stale while `diagnostics_on`
is raised" conflated two states: RETENTION (a session raises the flag at its
first byte, Requirement 13, and it stays up for the session's life) and the
RECORDING projection (the sealing one — every `derive_tree` caller passes
`record_diagnostics=0`). `S_project` computed the effective state, but the
clone — where every hit is taken — runs *before* `S_project` and read the
retention flag bare, so for every session each feed's clone refused every
hit and re-parsed the whole document: hit rate 0 on the exact path Phase B
was built for, invisible to every gate because the runners never retain
diagnostics. Witness: 315 line-feeds of a 12 KB document made 160,800
`parse_inlines` calls; a 488 KB document fed line by line cost 56.7 s
against a one-shot's 8 ms. The fix moves the effective-state window to the
derivation's own boundary — `derive_tree` lowers `diagnostics_on` across
clone *and* projection when the derivation does not record, `finish` keeps
retention standing so the sealing projection still re-parses and its rows
still speak. Line-fed streams: 12 KB 30.5 → 2.9 ms, 98 KB 1,907 → 104 ms,
488 KB 56,663 → 2,727 ms (10.5× / 18× / 21×); the wire feed 5.9×.
`projection_diagnostics_after_derive` now carries the other half of the
invariant — retention alone must not refuse the cache: two derivations of
an unwritten CST under retained diagnostics must hit, asserted on
`parser->cache_hits` and red on the unfixed engine (0 hits, 4 misses).

### F21 — F10 was wrong: a projection minted marks into the parser's vector, and positions after it were wrong  · VERIFIED, FIXED (`e34cf20`)

F10 recorded that `markdown_core_parse_inlines` mints a content mark for a
content-bearing block that has none, appending to `parser->line_marks`, and
called the branch unreachable because table cells and directive labels
receive their marks at build. **Both halves were wrong.** Directive labels
receive no mark at build — `directive.c` never calls `mark_content` or
`adopt_content_marks` — and an ATX heading with no content after its marker
(`#`, `## `, `### ###`) records none either. Each such block minted one mark
on EVERY projection, so a mid-stream derivation grew the vector, and the
open block that took its next line afterwards found its run no longer
contiguous with the vector's end: `S_record_content_mark`'s contiguity
assert is compiled out, the new mark landed outside the run, and
`content_place` answered from whatever mark sat inside it — the first
block's. Witness: `cat00048.md` in the second real corpus, three empty
headings followed by `Foo *bar\nbaz*`, derived after every line: the
paragraph at line 11 dumped its inlines at **line 1, column 4** — the `## `
heading's content column.

**Found by T9's boundary A/B, and the cache had masked it.** Every fixture
corpus and the 195-document corpus were identical; `corp2` differed at
14,828 dumps, and the difference was the T9 tree being RIGHT: an empty
heading served from the cache is never parsed again and never mints, so
the paragraph's run stayed contiguous. A cache that changes an answer is a
defect by §6's last bound whichever answer is right, and the answer it
changed was a pre-existing defect of the projection. F10 measured
`line_marks_size` flat over one 826 KB document and the fixture files — a
corpus without an empty heading followed by a multi-line open block, which
is the shape the defect needs.

**The fix is that a projection is a read.** Every position a projection
needs is written into the nodes it builds while it runs, so the minted
marks are scratch: `S_project` cuts `line_marks_size` back to where the
parse left it. Two gates now see the mint — `refmap_independence`'s
fingerprint carries the vector's size, and `projection_key` asserts it
across every derive-then-feed boundary. Run against the unfixed engine
(`ffee53b`) with the new runner:

```
spec           refmap_independence: 3 examples WROTE the CST     projection_key: 3 boundaries grew
regression     refmap_independence: 0                            projection_key: 0
extensions     refmap_independence: 3 examples WROTE the CST     projection_key: 3 boundaries grew   (directive labels)
cat00048.md    projection_key: 1 boundary grew
corp2 (1,191)  projection_key: 52 boundaries grew

fixed engine (e34cf20): every row above green; corp2 dumps identical to the T9 tree's (F20)
```

Two things follow for the plan. F10's line in §2 — *a suspected CST write
does not reproduce* — is withdrawn; the write reproduces and is fixed.
And T9's A/B could only be run against a control that carries the fix,
which is why `e34cf20` sits between T3/T4 and T9 in the history and why
F20's numbers are taken against it.

### F22 — a borrower's list must not be entered, and `projection_double` could not see a shared list being written  · VERIFIED, FIXED (T9)

With F21 fixed on both sides, T9's A/B still differed on `corp2` — 204
hunks — and every one was a `DirectiveLabel` growing a child per boundary:

```
DirectiveLabel scope=5:10..5:18 children=1        <- control, every boundary
DirectiveLabel scope=5:10..5:18 children=2, 3, 4  <- T9, boundaries 7, 8, 9
    ├── Text scope=5:11..5:17 literal="smith04"
    └── Text scope=7:35..7:41 literal="smith04"   <- the duplicate, positioned by a stale mark
```

**The walk entered the cache.** `process_inlines` never descended into a
block's inlines because its lookahead was taken at the block's ENTER, before
the inlines existed; T18 wrote that down as a property of the walk. A hit
is different: the borrow is taken at the CLONE, so at ENTER the block's
children are already there — the cache's list — and the walk descended into
it. Inside it sat a directive's label: an INLINE-class node
(`MARKDOWN_CORE_NODE_DIRECTIVE_LABEL`) that `contains_inlines` nevertheless
claims, because its content is its own buffer and `directive.c` parses it
itself during the paragraph's inline pass. The walk saw a content-bearing
node that was not itself a borrower and parsed it — **appending into the
shared list**, once per projection, with positions read off a mark index
that F21's fix had already cut away. Fix: at the ENTER of a borrower the
walk queues the block (its name hooks must still run, F15 rule 2) and
resets to the block's EXIT, entering nothing.

**And the gate that owns this could not see it.** `projection_double`
derived twice and dumped both trees afterwards. Both trees alias the same
list, the second derivation wrote into it, both dumps carried the write,
and the gate agreed with itself. It now dumps each tree before the next
derivation runs, and `extensions-directive` — where the seven inline labels
in the fixtures live — joins every projection row. Run against the engine
before the walk was fixed: `projection_double` fails on `cat00684.md` and on
`extensions-directive`; after, both pass and boundary 8 of the witness holds
one `smith04`.

The general statement, for T12 and every later consumer of a borrowed
tree: **nothing walks into a borrowed list to write.** The iterator climbs
out of one to the borrower it entered through (F17); the projection's own
walk now never enters one at all.

### F23 — literal block-node sharing cannot satisfy the public navigation contract, and the clone's constant is what falls  · VERIFIED (#161)

**The owner's steer** (2026-09-01) re-ruled §6's "the clone is accepted and no
task removes it": per-feed throughput is the goal, everything else is means,
so #161 asked for the derived tree to SHARE closed block nodes and shrink the
clone to the open spine plus the changed set — engine-side O(open + changed).

**The literal shape is impossible against the API as ruled, and the proof is
short.** `markdown_core_node_get_next_sibling(const markdown_core_node *)`
is stateless: a physically shared node must answer "what follows me" the same
in every tree that contains it. On an append stream the answer near the tail
DIFFERS per feed — the last closed sibling's next is that feed's open clone —
so a node is shareable only when its next-target is itself a permanently
shared node. That induction has no base: `derived(c_i)` can be permanent only
after `derived(c_i+1)` is, and the tail always holds a fresher sibling, so
nothing is ever permanent and the shareable set is empty. This is the
persistent-list fact that a forward-linked list shares TAILS and an
append-only stream grows on the side the links point toward; it would invert
if navigation were `last_child`/`prev_sibling`, and it dissolves entirely for
a consumer that walks through the iterator, which already climbs through a
borrower (F17) — but the C surface exposes first/next, and D8 just ruled that
surface. Making Read iterator-only is an API break and therefore a D-question
for the owner, not a task. The promotion-memo half of the issue's shape (a
hit reproducing a name-hook's replacement without running it) falls with the
node sharing that would have carried it; F15 rule 2 stands.

**What falls instead is the clone's CONSTANT, in two measured cuts.**
Callgrind over the 98 KB line-fed stream (`session_feed` per line, hit-
dominated), Ir being load-independent:

- **The tail filter's memo scan** (F15's `(ext, name)` memo, walked per block
  per extension per projection) was 13.9% by itself, with `get_type_string`
  re-derived beside it per probe. The parser now keeps the declared sets as
  BITMASKS in extension-list order — one `"*inlines"` mask and a name-keyed
  table filled lazily, rebuilt when `extension_generation` moves — so a tail
  computes its offer set once and re-answers it only after a hook actually
  ran (F15 rule 1 unchanged). 882.0M → 809.6M Ir (−8.2%).
- **The skeleton's allocator round-trip** — one calloc per CST block per feed
  and the consumer's matching frees, the malloc family at ~33% — became a
  DERIVATION ARENA: one allocation sized off the block mint, bump-served
  zeroed nodes, pages returned in one motion when the last node dies. The
  tree stays SELF-CONTAINED (no handle on the root, nothing in
  `content.mem`): pages are 64 KiB-aligned and never outgrow the window, so
  masking a node's address names its page and the page its arena, and a live
  count born at one makes any free order safe, interior hook frees included.
  Two discarded designs are recorded at the definition (node.h): a root-slot
  handle dies to a legitimate borrow against an empty document; a mem-wrapper
  in `content.mem` escapes into frozen buffers that cache holders keep alive
  after the arena dies. Both were caught by existing gates (`borrow_across_feed`,
  `block_identity`), which is those gates doing their job. Pages are
  realloc'd, not calloc'd — zeroing the window upfront measured 43% of a
  feed — and a document under 128 blocks keeps the per-node path, where the
  page plus slop costs more than the callocs it replaces (the 12 KB stream
  regressed 54% before the threshold said so).

**Where it lands.** 882.0M → 553.5M Ir (−37%) on the 98 KB stream; wall
125.4 → 97.4 ms (−22%); 488 KB −14% wall; 12 KB and the 41-case one-shot
unchanged; ASan (with the arena poisoning every unhanded slot and forgotten
node), UBSan, the resident-memory gate and the feed benchmarks green. A feed
remains Θ(blocks) — clone walk, projection walk, free walk — with the
allocator term gone and the filter term collapsed; what Θ(blocks) still
buys and what an API re-ruling would buy beyond it now live in #161.

**Superseded the same day**: the owner ruled the API open (D9), and F24 is
the sharing this finding proved impossible against the OLD surface, landed
against the new one.

### F24 — siblinghood moved to the parent, the API followed, and a hit became the retained node itself  · VERIFIED (#161, D9)

**The obstruction dissolved, not overpowered.** F23's proof was never about
nodes: it was that an intrusive `next` writes a PER-TREE fact — what follows
me in THIS tree — into memory two trees share. D9 let the representation
say so: a derived skeleton parent now holds its children as a VECTOR
(`children.vec/count`, flag `CHILD_ARRAY`, overlaying `first_child/
last_child`), every per-tree fact lives in per-tree memory, and a shared
node answers no question that varies by tree. The CST and every inline list
stay intrusive — an inline list is shared WHOLE behind its holder (T19),
and its internal links never lie. The public surface followed: the
stateless first/next accessors became the by-value children cursor
(`markdown_core_node_children`/`markdown_core_children_next`, O(1) a step
on either shape), the internal iterator carries its own ancestry as an
explicit frame stack instead of climbing parents, and the wire, the dump
and the bindings' surfaces did not move (Swift's Markup builders switched
cursor in four mechanical sites; ECMAScript and Kotlin read the wire).

**Then the issue's literal shape landed.** The holder retains the derived
node it stored — malloc-shelled to outlive any tree's arena, flagged
`SHARED`, parentless and linkless, its list aliased as its children — and a
hit at the clone returns THAT node under one holder hold per requesting
tree: no allocation, no content retain, no parse, no tail, no entry by any
projection walk (F22 upgraded from never-write to never-look). Promotion,
strip, consolidation, numbering and the consulted bits are baked into the
stored projection: what F15 rule 2 re-ran every projection to REPRODUCE,
retention reproduces by identity — rule 2's re-run survives only where a
store never happens (a hook's replacement node carries no origin, so a
formula paragraph re-projects each feed, exactly as before). The free
walk's part in a shared entry is one holder release; the holder frees the
shell with the list when the last tree lets go, on whatever thread that
happens (the holds were already atomic).

**Three builds failed before the shape held, and each is written where it
fell**: the root-slot arena handle (a borrow clobbers it — the borrow gate
caught a holder freed as an arena), the mem-wrapper in `content.mem` (a
frozen buffer carried it into a holder that outlived the arena — the
identity gate caught the read), and the ARENA flag re-asserted past its
allocation (an enrolled miss took a malloc shell but kept the flag, and
`forget()` masked a malloc address into a fake page — the feed_bound gate
caught it). The `node_sharing` gate now pins the mechanism itself: two
derivations of an unwritten CST hand back POINTER-identical shared blocks,
dump byte-identically, and the survivor reads after either free order;
retention forced off turns it red.

**Numbers** (98 KB line-fed stream, same protocol as F20/F23; `main` before
this series = 882.0M Ir / 125.4 ms):

```
masks + arena (F23)        553.5M Ir   97.4 ms
vectors (substrate alone)      —      125.0 ms   (transitional: dual writes + all-fresh clone)
retention (this finding)   521.3M Ir   84.1 ms   -41% Ir / -33% wall vs main
488 KB                                2400 ms    -32% wall;  12 KB 2.49 ms;  one-shot unchanged
```

**The bound, stated honestly.** NODE work per feed is now O(open + changed):
a closed unchanged block costs no clone, no parse, no tail, no hook, no
free. What remains Θ(width) per feed is the REFERENCING pass — the vector
fill, one freshness check and one hold per closed top-level block, one
release at the free — i.e. the skeleton of the copy-out §6 already accepts,
at pointer-write cost; and the tail interrogation of per-feed-fresh
CONTAINERS (name-mask lookups per list item per feed — 17% of the remaining
profile), because phase 1 retains LEAVES only. Container subtree retention
— enroll closed containers, store their vectors of already-shared children
under one holder, key on the container's own stamp plus OR'd consulted bits
— collapses both terms for nested documents and is the next step in #161;
content-less leaves (code blocks, thematic breaks) enroll with it, so a
stored container never references arena memory.

### F25 — the width the projection still walked was mostly two defects, and they fall for −79%  · VERIFIED (#161, follow-up to F24)

Profiled on a rebuilt line-fed harness: `session_feed` per line over a 98 KB
stream whose unit is a heading + one-line paragraph — 2,500 stored blocks,
5,000 feeds, the flattest and therefore widest shape a document can take.
Callgrind Ir, load-independent; same session, same binary flags, A/B per
change.

**Defect 1 — the shared EXIT still answered the name rows (half the
stream).** The ENTER of a retained block skips its subtree, but
`skip_children` still delivers the block's own EXIT, and the EXIT arm
carried no SHARED test: every stored block re-answered `get_type_string`
and the name rows, queued, and re-ran its hooks as no-ops on every feed —
against F24's own "no tail" sentence and the revised contract. The no-op is
provable (a block a name hook would have replaced was never stored), so one
flag test replaces the interrogation: **3,819M → 1,742M Ir (−54%), wall 695
→ 440 ms; `S_run_block_tail` falls 25.2% → 0.2% of the profile.**

**Defect 2 — the projection walked the whole width to find the fresh set
it already knew.** The derive-path walk stepped ENTER, flag test, skip,
EXIT past every retained child — a quarter of the remaining instructions —
to locate exactly the blocks the clone had just BUILT. The clone now
records what it builds in a per-derive fresh list and the projection serves
that list; the walk is the finish path's alone (T1 hands in the CST, whose
borrowers need it). Parses run forward in clone order; the tail queue fills
backward, so a child still precedes its parent in the drain and the
replacement rule holds; sibling order flips, which F15 states is free.
**1,742M → 816M Ir (−53%), wall 440 → 310 ms; the iterator falls 25% →
0.6%.** Together: **−79% Ir on the width-heavy stream**, dumps and every
suite byte-identical.

**The bound, restated.** What remains Θ(width) per feed is the clone's
REFERENCING pass alone: `S_clone_block_node` (45% — the enrolled predicate,
one freshness check, one hold per closed block) and the clone-tree loop
with its vector fill (22%), plus one release per entry at the free (5%).
About 30 Ir per closed block per feed, every one of them a pointer-width
touch. Collapsing THAT needs the derived child-vector itself memoized —
and F24's "enroll closed containers" is not sufficient for it: the widest
streams are FLAT, their one container is the always-open document, so the
memo must cover an OPEN container's stable prefix (previous derivation's
vector + a child-list generation + OR'd consulted bits, holds owned by the
memo so a tree takes ONE hold), with the per-child pass kept as the
fallback wherever a generation or a consulted map moved. Container
enrollment then rides the same memo for nested shapes. That design is the
remaining #161 step; its ownership rules go through the same review the
frozen-projection surface earned.

### F26 — the stable prefix serves for one hold and a memcpy: −80% on what F25 left  · VERIFIED (#161, F25's remaining step, landed)

Same harness, same discipline (98 KB, 5,000 line feeds, 2,500 stored
blocks; callgrind Ir, same-session A/B per change).

**The memo, as landed.** The parser records the document's leading run of
SHARED top-level blocks once, after a projection stores its misses, and
the next derivation takes the run whole: one memo hold, one memcpy into
its own vector, per-child walk resumed after the run. The record trusts
only what it proves pair by pair — the CST child is **closed** (an open
block's stamp still moves, and an open paragraph can die at its close,
taken whole by a reference definition) and the derived entry is that
child's own retained projection, by holder identity. The anchor is the
last *recorded* child, never its successor (the successor can be the open
block that dies); freshness is one comparison per axis for the whole run
— extension generation always, each map generation only where some entry
consulted that map (#163), OR-folded at push, unconsulted axes refreshed
at each extension. Ownership is persistent structure by refcount: the
memo holds each entry's holder; a tree holds the memo once and carries
its **own** boundary (the memo's count keeps growing past it) beside the
hold in an arena-owned `memo_ref` — review-found: the boundary's first
home was the extension-owned `as.opaque`, where a document-selected name
hook's attach would have trusted the integer as a payload and the free
would have handed it to `opaque_free_func`; the arm now stays NULL and
the gate asserts it; the free
walk skips the run and returns the one hold; invalidation releases the
parser's hold and rebuilds while old trees keep the old memo — and the
old answer — alive. Every failure is absorbed the way the store absorbs
its own: slow feed, never a wrong tree. **808.9M → 227.2M Ir (−72%),
wall ~156 → ~56 ms.**

**The defect the memo exposed.** With the referencing pass gone,
`S_block_has_tail_work` was 27.6% of the feed loop: the derived document
is fresh every feed, and its `S_has_inline_child` walked all of the width
to learn, every time, that a document holds no inline child. A memoized
prefix needs no asking — its entries passed the record's proof, each a
closed top-level BLOCK — so the question starts at the consuming tree's
boundary, suffix still walked. **227.2M → 164.9M Ir (−27%), wall ~48
ms.**

Together **808.9M → 164.9M Ir (−80%)** on the width harness — cumulative
with F25, **3,819M → 165M (−96%), wall 695 → ~48 ms** — dumps and every
suite byte-identical (correctness 121, sanitizers 3 × 105, conformance
2). The `projection_child_memo` gate pins the mechanism in six acts, and
five planted defects were each caught: an open block recorded (a fed
line demonstrably lost), the hit ledger dropped, staleness ignored, the
prefix double-released (ASan), the boundary read from the memo's count
(LeakSanitizer).

**The bound, restated once more.** Θ(width) per feed is now the consume's
own memcpy (26% — ~3.4 Ir per closed block, a pointer copy) and the
vector count in `S_vec_open` (~2 Ir per block inside `derive_tree`'s
16%), which is the price of the fail-closed door at the consume: the
width is counted from the CST rather than trusted from the memo, so a
broken permanence invariant makes a slow feed instead of an overrun.
About 5–6 Ir per closed block per feed, down from F25's ~30. The rest of
the profile is allocator traffic (~22%) and the O(new) work itself. A
delta-shaped consumer (#162) sidesteps even the memcpy; within the
value-shaped API this is the floor worth having.

### F27 — retention reaches every closed block, and the memo reaches every open container: the feed is O(open + changed)  · VERIFIED (#161, closing round)

Three commits, each measured on the shape it exists for; callgrind Ir,
same-session A/B per change. The adversarial harnesses: the OPEN-LIST
stream (one tight list fed an item per line, 2,000 items, a derivation
per feed — the shape the document-only memo could not serve) and the
FENCE-MIXED stream (a code fence in every unit, 66 KB over 6,400 line
feeds — the shape whose fences capped every run).

**1. Every childless block enrolls — bare leaves, hooked or not.** A
code fence, a thematic break, an HTML block, an empty container, a
reference DEFINITION (kept as a node — the mdast model) never turned
SHARED: recloned every feed forever, and one at top level capped the
document's memo at its index. Three doors, each found by measurement:
the enrolled predicate drops its `contains_inlines` term (childless is
the shape the store's move honors; bare leaves enroll closed-only — a
cost line, their store saves only a clone — which also keeps the
always-open document out); the tail's store arm keeps a CHILDLESS block
too, because the formula extension declares `code_block` for its
promotion, so every fence ran a tail whose end then stripped the origin
unstored — enrollment alone measured +42% on the fence stream before
this arm, buying a malloc shell and a tail for nothing; and a sweep
after the tails stores what no tail visited, over a fresh list
compacted BEFORE any hook runs (the drain can free what it was handed;
the compacted set is exactly what it is never handed). A fence the hook
declined — or retyped in place — is the deterministic projection of its
origin under the stored key: the promotion memo T9's amendment asked
for. **Fence-mixed: 3,038.6M → 150.3M Ir (−95%), wall ~59 ms.**

**2. A closed container is one retained value.** The enrolled predicate
grows its container arm: a CLOSED container — no open descendant, since
the open spine is the rightmost path — enrolls with its children, and a
hit is the retained ARRAY node itself, the subtree never entered. The
sweep stores it (children first, its backward order) once every entry
is SHARED, under a holder keyed on the container's own stamp with the
consulted bits OR'd from the entries' holders: a definition's arrival
re-keys exactly the containers whose subtrees asked (#163 one level
up), and the per-child walk inside a stale one still serves every
non-asking child by identity — unless a hook's name reaches the
container, which the second review round below rebuilds whole. An
entry that cannot share — a
directive's arena label, a hook's fresh replacement — leaves the
container merely unstored. The retained node carries its holder in
`link` as a leaf's borrow does; the holder teardown gains the ARRAY arm
(the other arm's first/last writes would tear the vector through the
union). **Open-list: 1,297.5M → 304.4M Ir (−77%).**

**3. Every open container on the spine carries its own child memo.**
The document's memo was the depth-0 instance of a per-container fact,
so `doc_memo` became a small table of (CST container, memo) pairs —
pointer-keyed and swept each record against the live spine as landed;
indexed by spine depth since the third review round below — compared,
never dereferenced, so a container that closed or died at its close
just falls out. The clone consumes through one helper at the root AND at
every open-container descent (a run reaching the end of the child list
skips the descent whole); the record walks the rightmost path, pairing
each open CST container with the derived side's LAST entry, fresh
exactly because its source is open. One union collision surfaced as a
segfault: a container that JUST closed is an enrolled MISS (ORIGIN in
`link`) while its memo still stands — the store wins the transition
derive, and the record sweeps the dead memo. **Open-list: 304.4M →
85.7M Ir; the round's total on the adversarial shape 1,271.3M → 85.7M
(−93%), wall ~168 → ~28 ms.**

The flat harness is unchanged through all three (165.2M vs F26's
164.9M), the one-shot too (63.85M vs 63.91M); the tiniest-session shape
pays the table sweep's constant (~+2%, 65.1M → 67.1M on 650 six-line
sessions). The `container_retention` gate pins identity, consulted
invalidation, the open list's own boundary, and a directive across its
arena's death; `child_memo` grew the definition-node and hooked-fence
teeth; seven planted reverts across the three commits were each caught,
two by sanitizers, one by the crash it was built to prevent. **What
remains Θ(width) per feed is the document-level consume (F26's memcpy
and count) — every nested level now pays only for its OPEN containers —
which is #161's stated goal, O(open + changed), on every shape the
suite runs.** One shape the suite does not ship stands outside the
bound, named at the end of the second review round: a hook declared on
a CONTAINER name.

**The review round consolidated the store into ONE pass.** Codex worked
the landing over four findings, and the last one named the invariant the
first three were circling: the hook contract's own words are that a hook
acts on the block it is handed AND INSIDE IT, so nothing may freeze
until every hook that can reach a node has run — a child stored at its
own tail was SHARED by the time its ancestor's hook edited inside, and
the edit was a silently refused no-op (a wart that predated this round
for inline leaves). Every store now happens in one post-order walk of
the LIVE tree after the whole drain (`S_store_pass`): the live tree is
the liveness proof a pre-hook queue never had (what a hook freed is
simply not here — the queue-read use-after-free Codex predicted was
reproduced verbatim under ASan before the rework), post-order is the
container store's all-SHARED proof arriving in the right order, and the
walk skips retained subtrees and memoized prefixes whole, so it stays
O(built). The tail-site stores, the pre-hook sweep and its compaction
all fell to the one mechanism; a hooked container stores on its first
derivation and serves by identity from the second. The gate's acts five
through seven pin the counting hook, the removing hook (sanitizer-
judged), and the inside-edit landing. One deliberate carve-out closed
the round: a child under a hooked OPEN container defers retention -- its
ancestor's hook may still edit it on a later feed, and the third feed's
"remove the first item once three exist" must land, not silently miss a
frozen node -- so that subtree stays per-feed fresh exactly as before
this round, until the close bakes the hook's last word into the stored
whole (act eight, and act nine for a paragraph inside the closed first
item, which answers to the open hooked list above it just as the item
does).

**The second review round moved that decision from the store to the
clone.** Two findings arrived after the merge. **P1:** the carve-out was
asked per stored node by climbing the WHOLE parent chain, so a document
that is mostly depth paid depth squared — two derivations of 500, 1,000,
2,000 and 4,000 nested quotes measured 0.62, 2.49, 10.39 and 38.71 ms,
fourfold per doubling. The climb is gone, not made cheaper: the store
pass asks nothing about ancestors any more, because the clone walk
already STANDS on the chain and can carry the answer down. It decides a
`S_clone_mode` once per region — the outermost hooked container being
rebuilt — and every node under it is cloned in that mode. **P2:** a
hooked container that is REBUILT rather than served — its store refused
because a hook left a fresh child, or invalidated because a definition
re-keyed it — was handed its SHARED children, so the hook's edit inside
was the same silently refused no-op the first round had chased out of
the tail: the same CST projected `[5,3]` after one feed and `[3,5]`
after the next (acts ten and eleven, refused and invalidated). So the
three modes: RETAINS outside hooked regions (hits served, misses
enrolled); REBUILDS inside a closed hooked container being rebuilt (no
hit and no memo run served, still enrolled, stored after the hook's one
run); DISCARDS under an OPEN hooked container (nothing enrolls — arena
shells, nothing under it reaches a store — which is the first round's
carve-out with the question answered once instead of per node). A
store-side guard that blocked frames under an open hooked container was
built, measured and discarded: the open hooked list at 1,000, 2,000 and
4,000 items read 280, 1,383 and 6,075 ms with it and 440, 2,407 and
15,196 without, against 194, 985 and 4,097 for the clone-side decision
(the plain list: 5.5, 18.2 and 65.3). The `depth_slope` gate pins P1
the way the other slope gates pin their bounds: 1,000 against 8,000
levels deep, normalized 1.2–1.6x now against 16–17x with the climb.
The harnesses re-measure at 87.8M nested (the climb read 88.5M; the
round's pass before the carve-out, 85.7M — the rest is the clone's
name lookups at each container descent, +0.45%), with flat and
fence-mixed within a fraction of a percent (166.3M and 151.8M).

**The third review round made the table the spine.** One finding
after the second: the per-container memo table of commit 3 was keyed on
the container's pointer and read by a linear scan — the clone scanned
it once per open container it descended into, and the record swept it
once per entry with each sweep climbing the live spine — so a document
whose spine held a memo at every level paid depth squared twice over.
A STAIR (each open quote holding one closed paragraph before the next
open quote) at 100, 400, 1,600 and 6,400 levels measured its second
derivation at 0.03, 0.29, 7.1 and 117 ms, and its first at 20 ms at the
deepest, the record's own sweep. The table is now indexed by spine
depth: slot k names the spine container at depth k, the document at 0.
The clone counts the open containers it enters on the way down and
asks for a run by that index — one compare; a slot naming another
container is a miss, never a wrong tree — and the record retakes the
table level by level down `last_child`, truncating the suffix the spine
no longer holds (containers close leaf-first, so what left the spine is
always the table's tail). Scan and sweep are gone, not made cheaper:
0.01, 0.08, 0.29 and 3.9 ms on the same stair, 6.9 ms for its first
derivation, and 869 instructions per level at 1,000 and at 4,000 levels
(callgrind over steady-state derivations) — flat to four digits. The
`spine_depth_slope` gate pins it the way `depth_slope` pins the second
round: the stair at 500 against 4,000 levels, steady-state derivations
after one enrolling one, normalized under 3x — 1.5–1.9x now, the
remainder being the working set leaving the cache inside the ratio,
against 13–14x with the scan. The three rounds share one lesson, and
it is the design rule this section now states: every term was per-node
work that re-derived its CONTEXT by scanning or climbing a structure
sized by another dimension of the document — the parent chain per
stored node, the table per container, the spine per table entry — and
every fix was the same move, carrying the context down the walk that
already stands on it. A side table keyed by pointer invites the scan;
a table that mirrors the spine is read by the depth the walk already
counts.

**The cost of a container hook, stated honestly.** Declaring a
container's name costs that container's subtree its retention for as
long as the container is open or being rebuilt — the hook may act
anywhere inside it, and a SHARED node cannot be edited. Measured with a
no-op hook on `list`: a definition's arrival rebuilds the closed hooked
list whole, once per invalidation (402 misses at 200 items where 4
sufficed); a hook that leaves a fresh child in a closed list keeps the
list refused, so every feed rebuilds it — 2N+2 misses per feed forever
(402, 1,602 and 3,202 at 200, 800 and 1,600 items; the first round read
2 misses there, but paid Θ(N) holds per feed for the same shape); and a
hook on `document`, or on any container that stays open, is a full
rebuild per derivation. No shipped extension declares a container name
(the formula extension names `formula_block`, `code_block` and
`paragraph`; autolink names `*inlines`; the table extension retypes a
paragraph in place), so nothing shipped stands on the cliff, and the
`depth_slope` gate does not either. The durable answer is
sharing-aware editing — adoption taking a hold on the shared node's
holder and unlink releasing it, with copy-on-write for an edit deeper
than one level, which is a hook-API change D9 permits — and it is a
redesign of the hook contract, not a patch to this round: filed as
#168, with a SELF-vs-SUBTREE reach declaration on the descriptor as its
cheap half and the promotion of a hook's fresh child as its third.

---

## 4. Decisions — RULED, 2026-08-25

**The API shape is the ruling, and it answers most of §4 by dissolving it.**

```swift
let session = Session()
let updated = session.feed(chunk: String)
let concrete = updated.concrete
let semantic = updated.semantic
```

`feed` **returns the document**. There is no separate "ask", no snapshot
accessor, no handle held between calls, and no second answer available to
anybody. §4 as first written asked three questions that only exist once an
"ask" is invented alongside `feed`; the shape above never invents one, so D1,
D2 and D3 are not answered here — they are **dissolved**, and must not come
back in the vocabulary that produced them.

- **D1 — re-project, or serve stale? · DISSOLVED.** The return value of
  `feed(chunk:)` is the document after those bytes. *A parse of the bytes fed
  so far* is not a property this design chooses; it is what the signature says.
  There is no stale tree to serve, because there is no call that could serve
  one. F7's 27.40% therefore sizes nothing here: it is the price a cache would
  have to pay to change an answer, and §6's last bound already forbids that. The
  two gates that hold the line are unchanged and keep their meaning:
  `projection_double_*` (two projections of one CST are byte-identical, taken
  over the *open* spine) and `projection_refmap_independence_*`. Serving stale
  would have made the first one describe something the engine no longer does.
- **D2 — who owns the projected tree? · DISSOLVED.** `updated` is a **value**,
  under the rule the AST contract already states for every binding: *"AST values
  are immutable after construction and own their strings and collections. No
  value retains a C node, document, allocator, or WASM handle"*
  ([canonical-ast.md:32](specs/canonical-ast.md#L32)). All three bindings
  already work exactly this way — Swift frees the native document before `parse`
  returns ([Document.swift:132](../packages/swift-markdown-core/Sources/MarkdownCore/Document.swift#L132)),
  ES frees it in a `finally` ([parser.ts:54](../packages/es-markdown-core/src/runtime/parser.ts#L54)),
  and Kotlin serialises the tree to a wire buffer and frees the document before
  returning ([markdown_core_kotlin_bridge.c:398](../packages/kotlin-markdown-core/src/native/markdown_core_kotlin_bridge.c#L398)).
  The engine owns the CST; the consumer owns the value it was handed; nothing
  is shared **at the binding boundary**, which is why copy-on-return is the
  shape there. **One level down the question is real and it is T19's**: the C
  tree a feed returns is an engine-owned *borrow*, and a caller can still be
  holding it when the next `feed` replaces what it points at. That is a lifetime
  to extend with a reference count, not an ownership to negotiate with the
  consumer — the consumer-side answer stays "a value", which is what D2
  dissolves. Refcount / copy-on-return / borrow-until-next-ask as a *consumer*
  question was about a design this is not. What remains is a **cost**, not a decision: a feed
  copies out what it returns. That is measured at T11 and bounded at T15, and it
  is recorded as an accepted bound in §6.
- **D3 — per-node mark, or separate change list? · DISSOLVED, and what is left
  of it is D4.** A consumer holding `updated` from the previous feed and
  `updated` from this one needs exactly one thing to re-render a block: to know
  that a block in the second **is** a block it already has. That is identity, on
  the node. A separate change list is derivable from identity by the consumer
  and adds a second thing to keep in step with the first; a mark that is not an
  identity cannot be joined against the tree the consumer is holding. So there
  is no fork here: **the signal is identity carried on the block**, and the only
  open question is what identity is minted from — which is D4.
- **D4 — what is a block's identity minted from? · RULED 2026-08-26, and
  landed (T2, T5).** The mechanism was settled by PoC (F11): a `uint32_t`
  minted once per block, four mint sites, two carry sites, chunking-stable,
  unique, failing closed, costing **+0.7%**. What the PoC could not rule —
  which node inherits in forks 1 and 3 — is ruled by the owner naming what
  the id is FOR. **An id answers exactly one question, the consumer's: is
  this an element I already have?** The consumer is an identity-keyed view —
  SwiftUI's `List`/`ForEach` and its equation checks — so the id must hold
  still across streaming (the element the reader is watching never
  re-identifies as bytes arrive) and must tell apart two blocks with
  identical content in one document (which is why it is a mint and never a
  content hash). Everything else follows from that one sentence, and each of
  F11's events gets its answer from it rather than from which node object
  happened to survive:
  - **a retype keeps the id** — setext, paragraph → table, the formula
    promotion's carry: the reader's element was reinterpreted, not replaced;
  - **a split leaves the id on what the reader already had** — fork 1 is the
    SWAP: the table's lead paragraph, the text the reader saw first and which
    did not change, keeps the id, and the table leaves with the lead's fresh
    mint ([table.c](../packages/markdown-core/extensions/table.c),
    `try_inserting_table_header_paragraph`);
  - **a death bequeaths the id to the firstborn** — fork 3: a paragraph
    consumed entirely by reference definitions IS its first definition to the
    reader who typed it, so that definition inherits and later definitions
    from the same paragraph are births; the swap sits in
    `resolve_reference_link_definitions`, which also covers the
    emptied-at-the-setext-check arrival. A paragraph that keeps content keeps
    its id — the visible text is what the consumer is tracking — and its
    definitions are then all births, so the 99.5% one-to-one figure and the
    11 tie-breaks in F11 collapse into one rule.
  **Amended the same day, by the owner: identity is TOTAL over everything a
  `ForEach` can iterate, not blocks alone.** An inline cannot mint — it does
  not survive in the CST: every projection rebuilds it, and the cache shares
  what was built — so an inline's identity is its **pre-order ordinal among
  its owning block's inline-class descendants**, assigned at the end of the
  block's tail, after consolidation, the hooks and the strip have finished
  the list. The pair (block identity, ordinal) is unique in the document;
  the ordinal alone is unique within any sibling list a consumer iterates,
  which is the distinguishability a `ForEach` needs — two identical links in
  one paragraph are two ordinals. Stability is the parse's determinism: two
  projections of one unwritten CST number every inline identically (gated),
  a cache hit serves the very nodes the numbers were written on, and an
  append to an open block extends the trailing text run in place, leaving
  every earlier ordinal where the reader already had it. When the inline
  parse rebalances (a delimiter finally closes), the block's own bytes moved
  and its stamp says so; within it an ordinal is positional — the slot the
  consumer keys, not a resurrection, which is why the dead-id ledger tracks
  blocks alone. A nested block inside the walk keeps its mint — the one
  block that mixes child classes is the directive block, whose CST-resident
  label is inline-class and numbers in the directive block's namespace; the
  gate is what found that hole.
  Landed as **T2** (the mints and carries of F11, the two swaps above, and
  the ordinal pass in the per-block tail — the field is `identifier` on the
  node, one field, two scopes: the identity is the concept, the identifier
  is the value that carries it) and gated by **T5**: `block_identity` — total
  over every node, blocks unique per derivation, siblings unique everywhere,
  every node named identically by two projections of one unwritten CST, dead
  block ids never resurrected — over five fixture files (838 examples, 2,179
  boundaries, 22,521 node observations, 0 failures), and
  `block_identity_transitions`, which pins all eight ruled shapes.
  `sizeof(markdown_core_node)` is 184 → 192 — T3's stamp had already taken
  the padding hole F11 measured, so the field still costs the 8 bytes F11
  priced. Nothing in D4 is open.

  **Export addendum (owner ruling, 2026-08-27).** The identity leaves the
  engine, and the C side answers it WHOLE: the same numbering pass that
  assigns an inline's ordinal stamps the owning block's mint into a second
  field (`owner`, meaningful only on inline-class nodes — a block is its own
  owner — at a further 8 bytes on the node), so
  `markdown_core_node_identifier` returns the pair from a lone node the way
  `_scope` returns a scope, and no binding composes anything. Every binding
  `Markup` carries it as **`id: Identity`** — `(block, ordinal)`, the render
  key — and the dump leads every line with `id=block:ordinal`. The
  references grew the edge D4 existed to serve: `LinkReference`,
  `ImageReference` and `FootnoteReference` carry **`definition: Identity`**,
  the identity of the first definition of their label in document order,
  stamped at resolution from the reference map's entries — which now carry
  the registering definition's identity INSTEAD of an age, because mints are
  monotone in document order, so document order is on the value itself and
  both preparation paths fold duplicates to the smallest. The definitions
  rename their match key **`norm`** in the bindings (the reference kinds
  stop carrying it at all: theirs equals the winning definition's by
  construction), and the whole read crosses every boundary as one buffer:
  `markdown_core_document_wire`, the canonical BYTES beside the canonical
  TEXT, which the Kotlin and ECMAScript bridges wrap in a versioned MKC6
  envelope and decode in one pass.
- **D5 — the public surface · RULED: the shape above.** `Session`, `feed`
  returning the document, and the document's two total views `concrete` and
  `semantic` — which are the two the facade already publishes
  (`markdown_core_document_semantic`, `markdown_core_document_source` and its
  line index). Not `create/feed/ask/free`: there is no ask, and the document is
  a value the consumer already owns, so there is nothing for a `free` to take
  back. F3 stands — the C surface is 37 exported symbols today and `feed` is
  not among them, which is why `stream_runner` links the static engine
  ([tests/CMakeLists.txt:120](../packages/markdown-core/tests/CMakeLists.txt#L120)) —
  so T12 exports what this shape needs and `scripts/audit-public-surface.sh`
  gates the result the way it gates every symbol today.

  **Owner naming ruling, 2026-08-26 — the shape stands, the names moved.**
  D5's mechanics are unchanged; the 3.0 bindings spell them as: the session is
  the living **`Document`** (in an editor, the document IS the continuously
  fed thing); `finish` is **`seal`**, and sealing also releases the native
  shell — a sealed document is a closed one; the value a feed or seal returns
  is a **`Read`** — `{ semantic, concrete }`, the pair being closed over its
  own coordinate system; the tree's root node is **`Semantic`**, an ordinary
  `Markup` completing the `concrete: Concrete` naming symmetry (the C kind
  and the dump label keep `document`/`Document`); `Concrete.lineCount` and
  `lineStart` are **`lines`** and **`offset(line)`** — an offset indexes
  bytes, which a `Scope` never does. The bindings' one-shot entries are
  deleted: the whole-text parse is `Document(markdown).seal()`, a
  one-chunk stream, so feed/seal partition invariance is the only identity
  and the duplicate one-shot bridge paths die. The C facade keeps its own
  names and both entries; "the document" below refers to D5's returned value,
  today's `Read`.
- **D6 — bindings with C, or C first? · RULED: together, one release.** Swift,
  Kotlin and ES land the shape in the same release as the C entry point. Swift
  is the semantic canon. Each binding's conformance entry already exists
  (`conformance:swift-macos`, `conformance:kotlin-jvm`, `conformance:es-node`)
  and T14 extends the corpora rather than adding a channel.
- **D7 — the diagnostics requirement is DELETED · owner ruling, 2026-09-01.**
  Per-feed throughput is the goal and everything else is means (the owner's
  restatement of F12's constraint), and the diagnostic list was a self-imposed
  product requirement — no CommonMark or micromark obligation — whose recording
  rule was the one reason the sealing projection refused cache hits. Deleted
  whole: the list, both severity/code vocabularies, the recording flag and
  retention, every diagnose site (the parse behavior at each — the label cap,
  the directive fallbacks, the table rejection — is UNCHANGED; only the row
  emission goes), the three public accessors, the CLI's `--diagnostics`, the
  census and its audit, and `projection_diagnostics_after_derive` (whose
  invariant is vacuous with no retention to refuse the cache; reuse stays
  pinned by the key, borrow and boundary A/B gates). With retention gone,
  `derive_tree` loses its recording window and `finish` takes in-place hits
  like any projection.
- **D8 — the concrete view leaves the API · owner ruling, 2026-09-01, same
  session as D7.** The same throughput steer: a `Read` is `semantic` alone.
  Scopes stay counted against the normalized source — the definition is now
  stated normatively in the header and the bindings' `Read` docs — but the
  library stops handing the text back, so the per-feed source memcpy
  (`S_concrete_copy`), the per-line source accumulation and its line index
  (`parser->source`, `line_starts`, both retained for the document's life —
  part of F9's 8.3× resident bound), the wire's trailing concrete section,
  the `Concrete` value type in all three bindings, and the C accessors
  (`_source`, `_line_count`, `_line_start`) are deleted whole. The wire
  payload's layout changed, so the bridge envelope bumps MKC6 → MKC7 on both
  writers and both decoders. What a feed returns is now the shared tree and
  nothing else; what a stream keeps resident is the CST and the cache, not a
  second copy of everything fed.
- **D9 — 3.0 is unreleased, so any API change is acceptable before it · owner
  ruling, 2026-09-01, same session.** Ruled in answer to F23's D-question:
  the O(open + changed) feed needed literal node sharing, node sharing could
  not satisfy a stateless next-sibling accessor, and the owner removed the
  accessor's protection rather than the goal ("我支持高性能实现，我们根本没发布3.0，
  所以在发布前，什么API改动都可以接受"). What it bought, in order (F24): a
  derived container holds its children as a VECTOR — sibling order is the
  parent's fact, so a shared child answers no per-tree question — the public
  first/next accessors became the by-value children cursor
  (`markdown_core_node_children` / `markdown_core_children_next`; ECMAScript
  and Kotlin read the wire and are untouched, Swift's builders moved in four
  mechanical sites), and the holder retains the derived node itself, handed
  back shared on every hit. The wire, the dumps and every answer are
  byte-identical; only the C navigation surface changed shape.
- **The retained projection is FROZEN, at any depth and for content too ·
  review round on F24's landing, 2026-09-01.** Three findings completed the
  fail-closed surface. (1) A consumer's `node_free` of a shared child used
  to release the holder hold — but the parentless node cannot leave the
  vector that holds it, so the tree's own free walk released the same hold
  again and the holder died under the CST cache: free is now the refused
  no-op unlink already was, and the store flags the WHOLE stored subtree
  `SHARED` (once per store, allocation-free), so free/unlink/adoption fail
  closed on interiors and `S_can_contain` refuses shared parents at the one
  chokepoint every insertion shares. (2) Content is as frozen as structure:
  every setter, the extension setters, consolidation's merge and `unput`'s
  trim answer 0 for a shared node (`unput` also read `last_child` through
  the raw overlay — garbage on a vector container — and now takes the
  shape-aware accessor). The `node_sharing` gate runs the hostile sequence
  against a pre-mutation baseline dump and died under ASan at the stolen
  release before the fix. (3) The clone's "enrolled parents stay intrusive"
  arm was DELETED as unreachable: no enrolled type (paragraph, heading,
  table cell — `contains_inlines` claims the directive LABEL but `BLOCK_P`
  does not) admits skeleton children, so the arm, the `derive_malloc_depth`
  machinery and the ORIGIN descend branch served a shape the grammar cannot
  build — a suite-wide probe fired zero times — while its unconditional
  link writes would have double-freed a nested hit the day the shape
  appeared; asserts now hold that door, and container retention (phase 2)
  stores vectors of shared children, not intrusive lists. F15 rule 2's
  statement followed retention into `syntax_extension.h`, with the seal's
  carve-out found one round later: a DERIVE hit runs no tail pass at all —
  the retained node itself is the answer, node-level hook effects baked in
  — while the SEAL runs the NAME hooks exactly once per stored block,
  because finish hands back the CST shell borrowing the stored children
  and only the hooks can reproduce their node-level work (a retype, a
  level) on that shell; zero lost the cached mutation, and the historical
  double queue ran them twice. The children the hooks must not touch are
  frozen, which is what makes the seal's re-run safe. What keeps a
  replacing hook per-projection is the store its replacement never enters,
  not the dispatch. The hook_once gate counts all three paths and dumps
  the sealed tree against the hit derive's.
- **The stable prefix is a MEMO a tree consumes for one hold and a memcpy ·
  F25's remaining step, landed 2026-09-01 (F26).** The parser records the
  document's leading run of SHARED top-level blocks after a projection
  stores its misses — pair-proven (closed CST child, holder identity),
  anchored on the last recorded child, freshness one comparison per axis
  with #163's consulted gating OR-folded across the run — and the next
  derivation memcpys the run under ONE memo hold, its own boundary beside
  it in an arena-owned `memo_ref` (the extension-owned `as.opaque` stays
  NULL — review-found), per-child walk resumed after. Persistent structure by
  refcount: invalidation rebuilds while old trees keep the old memo and
  the old answer alive. The document's tail question then starts past the
  boundary (its O(width) inline-child scan was the next defect in the
  profile). The `projection_child_memo` gate pins six acts; five planted
  defects each caught, two by sanitizers. −80% Ir on the width harness on
  top of F25; numbers and the restated bound in F26.
- **Retention reaches every closed block and the memo reaches every open
  container · #161's closing round, 2026-09-01 (F27).** Every childless
  block enrolls (the tail's store arm keeps what a name hook declined to
  replace — the promotion memo T9's amendment asked for — and a
  pre-hook-compacted sweep stores what no tail visits); a CLOSED
  container is one retained value under a holder keyed on its stamp with
  consulted bits OR'd from its entries, failing closed on any entry that
  cannot share; and the document's memo generalized into a per-open-
  container table swept against the live spine, consumed at the root and
  at every open descent. Fence-mixed −95%, open-list −93% (wall ~168 →
  ~28 ms), flat and one-shot unchanged; details, the union-collision
  fix, and the reached O(open + changed) bound in F27.
- **Who edits a hooked subtree is decided ONCE, in the clone · #161's
  review rounds, 2026-09-01 (F27).** The first round consolidated every
  store into one post-order pass over the live tree after the whole
  drain, so nothing freezes before every hook that can reach it has run.
  Its carve-out — a child under a hooked OPEN container defers — was
  asked per stored node by climbing the parent chain, depth squared:
  38.7 ms for two derivations of 4,000 nested quotes (P1). And a hooked
  container REBUILT rather than served (its store refused by a fresh
  child, or invalidated by a definition) was still handed its frozen
  children, so one CST projected `[5,3]` after one feed and `[3,5]`
  after the next (P2). The second round moved the decision into the
  clone walk, which already stands on the chain: a `S_clone_mode` per
  region, the outermost hooked container being rebuilt — RETAINS outside
  it; REBUILDS inside a closed one (no hit and no memo run served, still
  enrolled and stored after the hook's one run); DISCARDS under an OPEN
  one (nothing enrolls) — and the store pass asks nothing about
  ancestors. A store-side frame guard was built, measured and discarded
  (280, 1,383 and 6,075 ms against the clone's 194, 985 and 4,097 on the
  open hooked list at 1,000, 2,000 and 4,000 items). The `depth_slope`
  gate pins P1 at 1.2–1.6x normalized (16–17x with the climb); acts ten
  to twelve of `container_retention` and a Debug-only assert at the
  hook's own call site pin P2. The price of naming a container is stated
  in F27, in §6, and in the contract's own words.
- **The spine memo table is indexed by depth · #161's third review
  round, 2026-09-01 (F27).** The per-container memo table was read by a
  linear scan — once per open container in the clone, once per entry in
  the record with a climb of the spine each — so a stair whose every
  level holds a memo paid depth squared: 117 ms for a second derivation
  of 6,400 levels. Slot k now names the spine container at depth k; the
  clone counts open containers on the way down and asks by index, the
  record retakes the table down `last_child` and truncates the suffix
  that left the spine. 3.9 ms on the same stair, 869 instructions per
  level at 1,000 and 4,000 (callgrind), the `spine_depth_slope` gate at
  1.5–1.9x normalized against 13–14x. F27 states the rule the three
  rounds share: context is carried down the walk, never re-derived by
  scanning a structure sized by another dimension.

---

## 5. The task list

**Ordered 2026-08-25 by the owner.** The refcount, then the per-block tail,
then the cache and the borrow. **The order is a correctness order** — each step
is what makes the next one able to be correct at all:

- **Nothing may be shared before the refcount exists**, because a consumer can
  be holding a borrowed tree when `feed` is called again.
- **Nothing may be shared while the tail still walks the whole tree**, because
  the whole-tree consolidation frees nodes the cache holds — a use-after-free,
  proved by ASan in F16. With the tail made per-block first, that hazard never
  arises and the cache can hand out shared subtrees from the start; no
  intermediate copying mode is built and then discarded.
- **Identity is not on this path.** The cache keys on the CST block plus T3's
  stamp and T4's generation and never needs an id; T2 and T5 serve the
  consumer's change classification, which is a different axis.

Speed is not an argument in this section. What each step costs is in F12–F16.

### Phase A — pay the measured debts  · needs no decision

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
- [x] **T19 — a holder reference count, so a borrow survives the feed that
      replaces it.** The tree a feed returns is a BORROW; what needs solving is
      not who frees it but a consumer still holding it when `feed` is called
      again. One counter per cached subtree holder, not one per inline;
      `markdown_core_node_free` releases a hold and destroys at zero, and a
      block whose children are borrowed detaches them rather than splicing them
      into the free walk. **Inert until something shares** — `calloc` gives
      `refs == 0` and every path that exists today keeps its behaviour — so it
      lands on its own. *Gate: correctness 88/88 unmoved, and a borrow held
      across a feed that replaces it still reads.* **Done 2026-08-25.** Built
      as `markdown_core_holder` ([node.h](../packages/markdown-core/core/node.h)),
      a struct and not a node: `hold` / `release`, where a fresh holder has no
      hold and its first release destroys it; `take_children` moves a block's
      own children in and OWNS their bytes (F17); `borrow_children` aliases the
      list under a childless block and holds; `markdown_core_node_free` on a
      borrower detaches the list and releases. Nothing in the engine takes a
      hold yet — `calloc` leaves `holder == NULL` on every node — so every
      existing path is unchanged. The gate is
      `projection_borrow_{spec,regression,extensions}` (`borrow_across_feed`
      in `projection_runner`), which plays T9's part with T19's primitives:
      feed a line, derive, move every leaf block's children into a holder and
      borrow them back — or borrow the PREVIOUS boundary's holder where the
      leaf's subtree dumped identically, so one list is aliased by two live
      trees — and assert at every boundary that the borrowed tree dumps as the
      plain derivation did, that the previous tree still does after the new
      borrow and again after the cache's release, and that the new tree still
      does after the previous borrower is freed; the parser runs on a counting
      allocator and every example ends at zero live allocations. 669/669 spec
      over 1,624 boundaries, 43/43 regression over 159, 33/33 extensions over
      266. Correctness 91/91, asan and ubsan 79/79 (rebuilt, per F2), goldens
      unmoved. Cost and the two things the gate found are F17.
- [x] **T20 — `markdown_core_iter_init`: a walk that does not allocate.**
      `markdown_core_iter_new` callocs
      ([iterator.c:15](../packages/markdown-core/core/iterator.c#L15)), which is
      right once per document and wrong once per block per pass per feed. The
      struct is already complete in the internal header, so the walk can live on
      the caller's stack. Correctness 88/88 unmoved. Independent of everything
      else; see F16 for what it costs. **Done 2026-08-25**: `iter_new` is
      `calloc` + `iter_init`; consolidation, the comment strip,
      `process_inlines` and autolink's postprocess hold the walk on their
      stack. Correctness 91/91, asan and ubsan 79/79 (rebuilt), goldens
      unmoved. Not benched on its own: on the whole-tree path it removes four
      allocations per projection, which no 5-round run can see; its measured
      value (F16) is on the per-block path T18 opens.

### Phase B — make a feed stop re-parsing  · needs no decision

**This is the phase that answers the owner's constraint** — cloning per feed is
accepted, re-parsing is not (F12). T18 takes the tail off the whole tree; T3
and T4 are the cache key; T9 lands the cache and shares.

- [x] **T18 — make the whole-tree tail per-block.** It comes first because
      nothing may be shared while the tail still walks the whole tree, and it is
      correct on its own: the trees it produces are byte-identical to today's
      with no cache present at all. Four things have to be right, each of them
      found by building it:
      **(a)** a hook over EVERY block, not just content-bearing ones — an
      `HTML_BLOCK` has tail work and no inlines, and a hook inside
      `process_inlines`' `contains_inlines` branch never reaches it (F13);
      **(b)** collect-during-walk, act-after — freeing the node a walk stands on
      segfaults at `ENTER` and at `EXIT` alike (F13);
      **(c)** `S_strip_html_comments` split into a predicate and a removal,
      because it frees its own `root` and then dereferences it (F13);
      **(d)** a declared **type-name filter** on the extension descriptor, so a
      block is offered only to an extension that named its type, with
      `"*inlines"` as the member for an extension that selects on inline content
      rather than on a block kind (F15 — the PoC hard-coded that case and did
      not build the member). Once T9 exists, a block served from cache is not
      offered to the passes that rewrite its CHILDREN; **a postprocess that
      replaces the block NODE must still run on a hit**, because the node is the
      one part of a hit the cache never serves (F15 rule 2). T18 states which of
      the two resolutions it builds. Formula's postprocess is
      three arms and only one of them needs the projection (F14).
      *Gate: byte-identical trees at every feed boundary over every fixture
      corpus and the real corpus.* **Done 2026-08-25.** `postprocess_func`
      is gone; the descriptor carries `postprocess_block_func(ext, parser,
      node **block)` — IN/OUT, reseat on replace, NULL on remove — and
      `postprocess_blocks`, a NUL-separated name set ending in an empty name
      (F18), with `"*inlines"` as the content member. `process_inlines`'s
      walk queues every block that has tail work at its EXIT on a queue the
      parser keeps across projections, and `S_run_block_tails` drains it
      after the walk: consolidate → each declaring extension in attach order,
      the name re-resolved after every call (F15 rule 1) → the strip, whose
      `HTML_BLOCK` arm asks the predicate and then frees, and whose inline
      arm is a walk rooted at the block that never frees its root (F13
      requirement 3). The memo is keyed on the name's pointer and lives on
      the parser. **The resolution of F15 rule 2 that T18 builds: a hook
      declared by NAME acts on the block node and runs on every projection,
      hit or miss; `"*inlines"`, consolidation and the inline strip act on
      the children and are skipped for a block whose `holder` (T19) is set.**
      So formula's arm 3 promotes `$$x+y$$` on a hit without any cached
      projected identity — the paragraph node is fresh every projection and
      the hook is what makes it a `FormulaBlock`. Autolink declares
      `"*inlines"` and no longer consolidates on entry (the core does, right
      before it); formula declares `formula_block`, `code_block`,
      `paragraph`, and its hook has no walk. Numbers and the two things the
      build found are F18.
- [x] **T3 — a write stamp on the CST block**, bumped whenever the parser writes
      that block (content appended, type retyped, closed). Half the cache key.
      It must hook the FIELD WRITES, not `markdown_core_node_set_type`: the
      setext retype writes `->type` directly
      ([blocks.c:2366](../packages/markdown-core/core/blocks.c#L2366)) and
      `tasklist.c:105` changes what a consumer sees without touching `type` at
      all (F11). **Done 2026-08-25.** `node->stamp` is the parser's **write
      clock** as it stood at the block's last write, not a per-block count —
      so a block born at an address another block died at can never read as
      unchanged. `markdown_core_parser_touch` is called at birth (`add_child`,
      the root, a reference definition, table's lead paragraph), at every
      `add_line` and content mark, at `finalize`, at the setext and table
      retypes and tasklist's item, **and over the whole open spine once per
      processed line** — which is what covers the writes the core cannot see,
      an extension's opaque state above all. The invariant that makes the
      spine stamp complete is that a CST block is written only while open,
      and open only on the spine; the gate below asserts it. The node grows
      176 → 184 → **192** (F19).
- [x] **T4 — a generation counter on each definition map**, bumped on every
      insert. O(1); flips nothing and walks nothing. Both maps reopen their
      preparation on insert already
      ([references.c:104-110](../packages/markdown-core/core/references.c#L104-L110)),
      which is where the counter goes. The other half of the cache key.
      **Done 2026-08-25**: `map->generation++` beside `map->prepared = 0`.
      Every insert counts, a duplicate label included — the first-wins fold
      happens at preparation, and a spurious invalidation is a slow feed where
      a missed one would be a wrong tree.
      *The gate for both is `projection_key_{spec,regression,extensions}`
      (F19): at every line boundary, every CST block is paired with its
      derived block and an unchanged `(stamp, refmap generation, footnote
      generation)` must mean a byte-identical derived subtree, and a closed
      block's stamp must never move again.*
- [x] **T9 — cache a closed block's derived subtree** on its CST block and
      **share it into the tree a feed returns**. T18 has already taken the tail
      off the shared nodes and T19 keeps a borrow alive across the feed that
      replaces it, so the hand-out shares from the start. The store copies once
      per miss; only the hand-out shares. This is also where T18's filter starts
      skipping a block whose projection came from cache. **Done 2026-08-25**,
      and one word of the plan changed on the way: the store **moves** rather
      than copies, because the per-block tail is what made the copy necessary
      in the PoC and T18 removed it. The hit is taken at the clone, the store
      at the end of the block's tail, `finish` hits in place, and the key,
      the slot and the flags are F20's. A runner turns the cache off with
      `parser->no_projection_cache`; the key gate and the borrow gate do,
      because each plays the cache's own part. Gate: F20's A/B, cache on.
- [x] **T10 — invalidate by generation**, per F6's rule: a cached projection is
      invalid when the map has advanced past the generation it was made at.
      Correctness must never depend on the cache — a wrong cache is a slow
      engine, not a wrong tree. **Done with T9**: the generation is half of
      the key every hit compares, F19 proved the key sound over 4.3 million
      block observations with the cache off, and F20's A/B says the trees are
      identical with it on. Nothing separate was left to build.
- [x] **T11 — measure the reactive loop** on the full corpus and price the
      copy-out a feed pays to return a value (§6). Every shape has been measured
      on a 40-document subset in F12–F16; T11 confirms them at full scale.
      **Done 2026-08-25**: the engine side is F20's table over the 195- and
      1,191-document corpora and the two largest fixtures. **The binding-side
      copy-out is NOT priced here** — no binding has a `Session` yet (T12,
      T14), so there is no copy-out to time; it is owed with Phase E and
      bounded at T15.

### Phase C — make a block addressable  · DONE 2026-08-26 (D4 ruled in §4)

D4's two inheritance forks were ruled by the owner's consumer model (§4 D4)
and the phase landed in the same session. **This is the consumer's axis, not
the feed-cost one** — Phase B did not wait on it.

- [x] **T2 — a stable id on the CST block**, minted at open, carried onto the
      derived node by the clone. **Done 2026-08-26**: `identifier` on the node,
      `block_ids_minted` on the parser, F11's four mint sites and two carry
      sites, plus the two ruled swaps — the table's lead paragraph inherits at
      the split, the firstborn reference definition inherits at the
      paragraph's death (both in §4 D4). The mint is advanced only by the
      block phase, so a projection never renames a block and the ids stay a
      fact about the document (F11's chunking argument, unchanged). Amended
      same day for the owner's totality requirement (§4 D4): inlines carry
      per-block pre-order ordinals in the same field, assigned by
      `S_number_inline_descendants` at the end of the block's tail — after
      every pass that shapes the list, before the cache stores it — and a
      block that owns a CST-resident inline construct (a directive's label)
      now takes a tail for exactly that assignment.
- [x] **T5 — gate:** two projections of an unwritten CST produce identical ids,
      and a retype preserves the id of the block it rewrites.
      *Closes F4.* **Done 2026-08-26**: `projection_identity_*` — five fixture
      files fed one line at a time, two derivations per boundary plus finish:
      no node without identity, no duplicate block id within a derivation, no
      duplicate among siblings anywhere, both derivations name every node —
      inline included — identically, a dead block id never returns (838
      examples, 2,179 boundaries, 22,521 node observations, 0 failures);
      `projection_identity_transitions` pins the eight ruled shapes — setext
      and table retypes keep the id, the lead split leaves it on the lead, a
      death bequeaths it to the firstborn definition, a surviving paragraph
      keeps it and births its definitions, the formula promotion carries it,
      and two same-content links in one paragraph are distinct and keep their
      ordinals across an append. The CST fingerprint in `refmap_independence`
      now carries the identity too, so a derivation that renamed a CST block
      would read as a write. The gate found the directive-label hole (a label
      under a directive BLOCK was reachable by no numbering pass) before the
      first full run was green.

### Phase D — the change signal  · KILLED 2026-08-26, owner ruling

**No change signal is owed, and none will be built.** The consumer is the
identity-keyed view D4 names — SwiftUI's `List`/`ForEach` and its equation
checks — and it never expected the parser to say what changed: it re-renders
an element when its identity is new and when its value stops comparing
equal, and it reads both off the trees it is already holding. D3's
dissolution said this and the phase did not hear it: *"a separate change
list is derivable from identity by the consumer and adds a second thing to
keep in step with the first."* The stamp pair T6/T7 would have carried out
was exactly that second thing — a classification the consumer would have to
trust instead of the equation it already runs. The phase encoded an
expectation the owner never had, and it dies whole:

- ~~**T6 — stamp each derived block** with the `(write stamp, map
  generation)` it was derived at.~~
- ~~**T7 — carry the stamp pair out on the returned block**, so a consumer
  classifies every block as new / changed / unchanged.~~
- ~~**T8 — gate:** the changed set is exactly the blocks whose bytes moved
  or whose resolution moved.~~ T8's falsifiability claim is not lost with
  it: the boundary A/B (F18), the key gate (F19) and the identity gates
  (T5) are the design's falsifiers, and each pins a property the engine
  actually owes. F5 is **answered** by this ruling rather than closed by a
  task: identity (T2, landed) is the entire interface to "what changed",
  and the write stamp and the generations stay what T3/T4 built them as —
  the cache's key, never the consumer's.

### Phase E — the public surface  · DONE 2026-08-26 (D5 and D6 ruled)

- [x] **T12 — export the streaming entry point** in C, carrying the shape §4
      rules: a session, `feed`, and the document's two total views. Whatever the
      C spelling, `scripts/audit-public-surface.sh` gates it against the header,
      the ELF version script and the Mach-O list together. **Done 2026-08-26**:
      four symbols — `markdown_core_session_new`, `_feed`, `_finish`, `_free` —
      and the header, the ELF version script and the Mach-O list agree at 41.
      The C spelling adds `finish`, the latitude this entry grants: the sealed
      document — list tightness, the finalize-minted definitions, the
      record-gated diagnostic rows — exists only at the last projection, so a
      session that could never end could never hand it over; after it the
      session refuses everything but `free`. `feed` returns the mid-stream
      projection as a VALUE: the tree shares the cache's lists under T19's
      holds (a document outlives the session that fed it — gated), the two
      views are copies (§6's copy-out made flesh), and its diagnostics are the
      rows recorded so far (§12.8 Q4 — the final projection's own rows speak
      at `finish`). Gate: `facade_native` — every canonical fixture fed in
      7-byte chunks seals byte-identical to the one-shot parse, diagnostic
      rows included — plus the session behavior checks in `facade.c`.
- ~~**T13 — the returned document carries the change classification** from
  T7.~~ Killed with Phase D (2026-08-26): `updated` carries identity, and
  identity is the whole signal.
- [x] **T14 — bindings** (Swift, Kotlin, ES) in the same release, and their
      conformance corpora. Swift is the semantic canon.
      *Closes F3.* **Done 2026-08-26**: `Session` in all three, the D5
      spelling, every document built by the binding's one-shot conversion
      path — Swift's `Document(copiedFrom:)`, Kotlin's one `deliver` payload
      writer over the MKC5 wire, ES's shared copy-out — and `feed` takes
      BYTES first (Swift `[UInt8]`, Kotlin `ByteArray`, ES `Uint8Array`,
      each with a `String` convenience), because the ruled 7-byte chunking
      splits UTF-8 sequences no string type can spell. Each binding re-runs
      the shared canonical-AST manifest streamed in 7-byte chunks inside its
      existing conformance channel (D6: the corpora extended, no new
      pipeline), asserting the sealed document against the same goldens and
      the concrete view against the one-shot parse. Validated locally:
      Kotlin end-to-end (jvmTest 24/24, Kotlin/Native linuxX64, the Android
      host suites, both ABI dumps regenerated, five JNI exports nm-verified);
      ES to the edge of the WASM build (strict tsc, eslint, prettier,
      clang-format, a host-cc -Werror compile of the bridge); Swift by
      line-level inspection against the header — CI's macOS, iOS, browser
      and emulator jobs carry the rest.
      **The es-node coverage ledger moved with it, and the growth is this
      change's, recorded here** (the migration document `policy.json` names
      is gone): the session tests are the first to march a `ParseError`
      across the wire, so `node-decoder.ts`'s `errorCode` executes for the
      first time and its two remaining defensive arms (`allocationFailed`,
      the unknown-code fallthrough) newly enter V8's branch count — 19 → 21
      with no protection eroded, the one increase the policy's rules allow.
      `session.ts` carries one unpinned branch, the constructor's
      allocation-failure throw: `instance.exports` is frozen, so no test can
      make `es_session_new` answer 0 — the same character as `native.ts`'s
      standing entry. Against those +3: `parse-error.ts`'s entry is deleted
      (the sealed-session refusal covered it to 100%) and `parser.ts`
      tightens to one branch, so the total unpinned surface SHRINKS — lines
      −6, functions −2, branches −1.

### Phase F — bounds and gates  · DONE 2026-08-26

- [x] **T15 — the reactive-loop bound as a gate:** the **projection** side of a
      feed is `O(open block + changed set)` and carries **no term in the
      document already fed**. Two terms are carved out and reported beside it
      rather than folded in, because both are accepted: the **whole-CST clone**
      (§6) and the binding-side **copy-out**. It needs all of Phase B: with T9
      alone the tail keeps a whole-document term, and with T18 alone there is
      nothing to skip. A fitted
      slope in document size fails and names the state being re-derived. The
      binding-side copy-out is bounded separately and stated, not hidden inside
      this one. **Done 2026-08-26**, as counters rather than clocks:
      `projection_feed_bound` streams 256 independent blocks, derives per
      feed, and asserts the per-feed **cache-miss delta** — exactly the blocks
      a projection re-parses, the cost F12 measured — sits FLAT (it is 1, the
      block the feed closed) while the hit delta grows with the document. The
      two carved-out terms are stated in its output, and the third — a
      definition's arrival re-keys the whole document (F19) — is asserted AS
      that term, so a change in its shape fails the gate instead of hiding.
      *It failed exactly so on 2026-09-01: #163 closed the term (F19's
      amendment), and the gate now asserts the tighter shape — an arrival
      re-keys no block that consulted no map.*
- [x] **T16 — measure resident memory** across a long stream, and state the
      bound that comes with every block keeping its content buffer for life.
      F14's first draft claimed clearing a formula block's content at close would
      reduce it; `markdown_core_strbuf_clear` returns nothing to the allocator,
      so that saving needs `_free` and is untested.
      *Closes F9.* **Done 2026-08-26**:
      `pathological_complexity_resident_memory` streams 3.28 MB of paragraph
      blocks with a derivation every 64 feeds and reads `ru_maxrss`: **8.3× of
      the bytes fed stays resident at peak** — the CST's content buffers, the
      normalized source, the cache's one list per closed block, and the one
      live projection a consumer holds. The bound is O(bytes fed); the gate
      trips at 24×. Numbers in F9.
- [x] **T17 — structural invariants over carried opaque extension state**, so a
      field that stops surviving the clone fails a gate rather than a golden.
      *Closes F8.* **Done 2026-08-26**: `projection_carried_state_*` joins the
      cloned boundary projection against the clone-free in-place finish on the
      identity T2 mints — a field that stops surviving the clone makes the
      same closed block dump differently in the two — run cache-on and
      cache-off over four corpora (510 closed blocks agree on spec, 294 on
      extensions, 8 on directive). A document whose finalize mints definitions
      is skipped structurally and counted; the boundary tree's final top-level
      block is exempt by the ruling (the formula promotion's `closed` lags one
      line, §1), so every block the stream has actually left behind is
      compared, and a corpus of one-block documents reads as tails, not as a
      vacuous pass.

---

## 6. Bounds this design accepts

Stated so they are not later mistaken for defects.

- **A feed costs O(open block)** — as a TARGET, and the engine does not meet it
  today. The open block is re-projected every time, because that is what "the
  block is the unit" means, and a consumer feeding many small chunks while one
  very large block is open pays that block's size on every feed. **That part is
  accepted**, and the machinery that would have flattened it (the resumable
  inline subject) is dead by §1.
  **What is NOT yet true is the rest of the bound.** `S_project` CONSUMES the
  skeleton it is handed — it says so at
  [blocks.c:1660](../packages/markdown-core/core/blocks.c#L1660) — and `finish`
  additionally resets the parser, so a `Session` whose `feed` returns a
  document can use neither: it must go through `markdown_core_parser_derive_tree`,
  which is **clone + project**. So as the engine stands, every feed clones the
  whole CST and a feed is **O(document)**, not O(open block). T1 bought the
  one-shot path out of that clone; the streaming path pays it on every feed,
  and F1 sizes a whole-CST clone at ~7% of a parse. **The clone is accepted and
  no task removes it**, which is why T15 has to carve it out rather than fold
  it in (below).
  **F12 puts numbers inside a feed**: the clone is ~12%, the inline parse ~43%
  and the projection's tail ~45%. The clone is the part the owner accepts;
  **the other two are what Phase B removes** — T18 the tail, T9 the re-parse,
  in that order.
- **A feed returns a value, and the copy-out is proportional to the document.**
  D5's shape hands the consumer `updated`, and every binding's AST is a value
  that retains no engine memory ([canonical-ast.md:32](specs/canonical-ast.md#L32)).
  So the engine-side bound above is not what the consumer pays: a feed also
  copies out what it returns. **This is accepted as the price of the shape**,
  it is measured separately at T11 and bounded separately at T15, and identity
  is what keeps the render side out of it — a consumer keyed on the ids
  re-renders only the elements whose value stopped comparing equal, so it pays
  the copy but not the render. (This clause first credited the change
  classification; Phase D's ruling killed it as never owed.)
- **The per-feed clone is GONE for what did not change; what remains
  whole-document is the referencing pass.** ~~NO task removes it~~ → F23
  (the old API forbade sharing; the constant fell instead) → D9 (the owner
  opened the API) → F24 (a hit is the retained node itself). NODE work per
  feed is O(open + changed); the Θ(width) that remains is the derived
  root's vector fill, a freshness check and a holder hold per closed block,
  and their release at the free — the skeleton of the copy-out this section
  already accepts — plus the tail interrogation of per-feed-fresh
  containers, which container retention (#161's next step, F24) collapses.
  **T15's bound stays stated over the projection only**, the referencing
  pass measured and reported beside it, the way the binding-side copy-out
  is.
- **A hook declared on a CONTAINER name costs that container's subtree
  its retention while the container is open or being rebuilt.** The hook
  contract lets a hook act on the block it is handed and inside it, and
  a SHARED node cannot be edited (the silent no-op of F22 is the failure
  mode), so the clone opens a region at the outermost hooked container
  it rebuilds and serves no hit and no memo run under it; under an OPEN
  hooked container nothing enrolls at all. An open hooked list is
  re-derived whole at every feed — 194, 985 and 4,097 ms across 1,000,
  2,000 and 4,000 items where the plain list reads 5.5, 18.2 and 65.3 —
  and a closed one is rebuilt once per invalidation, or once per feed
  while a hook keeps leaving a fresh child in it. No shipped extension
  declares a container name, and `markdown-core-extension-api.h` asks
  hooks for the leaf names they act on wherever those are enough. **This
  is accepted** until sharing-aware editing (#168) changes the contract;
  F27's second review round has the measurements.
- **A feed is not monotone.** A definition arriving later changes the projection
  of a block returned earlier. That block was never wrong: it was resolved
  against the map as it then stood, and CommonMark defines the earlier outcome
  as prose.
- **`finish` is O(document) once**, and equals a one-shot parse. That is the
  acceptance criterion and it is already gated.
- **No index, no back-pointer, no pending state.** The bound is
  `O(what you project)`. Phase B may make a feed cheaper; it may never make an
  answer different.

---

## 7. Provenance

**Measured in this session** (2026-08-25, `8d0910b`): F1's 41-case bench table,
F2, F3's export count, F4, F5, F10, correctness 88/88, the `gates.sh` sweep.
**T1's closing measurement** (2026-08-25): the three-way bench in F1, over
`f5edcdd` / `890e908` / the T1 working tree, built and run in one session.
**D4's PoC** (2026-08-25): F11 in full — the identity gate over 878 fixture
documents and 195 real ones, the three carry-removal experiments, and the
41-case 7-round bench against `ae0bb0b`. The PoC tree is a worktree at
`ae0bb0b` and is NOT committed; F11 is what survives it.

**A second costume for F2's trap, recorded here because it produced three wrong
readings before it was caught.** `cmake --build --target X` under Unix Makefiles
compares mtimes at SECOND granularity, so a source edited and rebuilt inside
one second is silently not rebuilt — and the gate then describes the previous
experiment's binary. Deleting the objects, the archives and the binary before
each rebuild, and fingerprinting the binary after it, is what made the
carry-removal experiments trustworthy: the restored binary's md5 is identical
to the control's.

**F12** (2026-08-25): the feed-cost split, the reuse hit rates, the
projected-tree copy cost and the owned/borrowed chunk census, all over the same
195-document corpus, using PoC-only seams (`clone_only`,
`derive_tree_no_inlines`, `clone_tree`) that are in the D4 patch and are not
proposed for landing as they stand.

**T9, T10, T11, F20, F21 and F22** (2026-08-25): the boundary A/B against
`e34cf20` over eleven fixture corpora and both real corpora, the three-way
feed loop, the slope readings with the cache on and off, and the 41-case
5-round bench. The binding-side copy-out is not measured; there is no
binding to measure. **A fourth costume for F2's trap, worn twice in one
session:** a control worktree moved between commits with `git checkout` and
rebuilt by target linked a core library from the previous commit (the
extension objects wanted `markdown_core_parser_touch`, the library had none),
and a runner source copied into it was not rebuilt at all — the "unfixed
engine" read every new gate green. Both were caught by `rm -rf` of the
build's objects before rebuilding and by `strings` on the binary for the
new gate's message; F21's evidence table is from the second run.

**T3, T4 and F19** (2026-08-25): `projection_key` over the three fixtures
and both real corpora, `sizeof`, and the 41-case 5-round bench against
`4b43e3e`.

**T18 and F18** (2026-08-25): the boundary A/B over eleven fixture corpora
and two real corpora (195 and 1,191 documents) against `bd59279`, the ASan
row counts, the 41-case 5-round one-shot bench and the feed-loop timings —
every number from the landed tree. The real corpora are the ones F12–F16
used; they live outside the repository, per `tests/corpora/README.md`.

**T19 and F17** (2026-08-25): the gate's first-run failure counts over the
three fixtures, `sizeof(markdown_core_node)`, and the 41-case 5-round bench
against `1ce6938` — every number in F17 is from the landed tree, not a PoC.

**F13, F14, F15 and F16** (2026-08-25): the per-block tail, formula's three
arms, the declared type-name filter and the ASan proof. **Every number in these
four is over a 40-document, 4,109-boundary subset of the same real corpus**,
chosen so the five projection modes could be run against each other in one
session; the correctness comparisons additionally ran over six fixture corpora.
`spec.txt` and `extensions.txt` have NOT been run against the per-block tail.
The PoC seams they use (`poc_set_reuse`, `poc_set_per_block_tail`,
`postprocess_blocks`, the holder refcount, `markdown_core_iter_init`) are in the
D4 patch and only `markdown_core_iter_init` is proposed for landing as it
stands.

**T2 and T5** (2026-08-26): D4's forks ruled from the owner's consumer model
and the identity field landed; amended the same day when the owner extended
the requirement to every element a `ForEach` can iterate — the inline
ordinal pass, the directive-label hole the extended gate caught on its first
run, and the re-run of every suite below with the amendment in. The gate
counts are in §5's T5 entry, alongside the
`ctest --preset correctness` sweep (red rows only among
`pathological_complexity_{valid,unclosed}_long_quoted_value`, wall-clock
gates this container also fails at the UNCHANGED head — measured
interleaved, base 2.32x–4.60x against head 1.84x–4.62x, both straddling the
bound, small-input times identical, and both pass standalone — which is
F2's shared-runner regime, not a regression), the
ASan and UBSan correctness presets 90/90 each, conformance 2/2, and
`sizeof(markdown_core_node)` read at 192 against 184 at head.

**Inherited from the Stage 1 design work and not re-measured**: F6's 36.9% /
23.0% / 7.2%, F7's 27.40% / 80.1% / 8-of-90 / 4.08%, F8, F9, and the
4-of-670 reference-loss figure in §2. All recoverable from
`git show 8d0910b:docs/RECONSTRUCTION.md`. **Re-measure any of them before it
decides a task**, because each was taken against a tree that Stage 1 has since
changed.
