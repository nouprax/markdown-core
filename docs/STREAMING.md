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
re-parse is cached away.** `markdown_core_consolidate_text_nodes_with_parser`,
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
freed by   markdown_core_consolidate_text_nodes_with_parser  (iterator.c:151)
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
  returns ([Document.swift:143](../packages/swift-markdown-core/Sources/MarkdownCore/Document.swift#L143)),
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
- **D4 — what is a block's identity minted from? · PoC DONE (F11); the two
  inheritance questions are OPEN.** The mechanism is settled and measured: a
  `uint32_t` minted once per block, four mint sites, two carry sites, stable
  and unique over 878 fixture documents and 195 real ones across 85,694
  derivations, failing closed when a carry is removed, and costing **+0.7%**
  with the node going 176 → 184 bytes. **F11 has the numbers and the method.**
  What a PoC cannot rule is what a consumer should SEE, and it turned up two
  questions rather than the one §4 first asked:
  **(a)** when a paragraph becomes a table and a lead paragraph splits off, the
  surviving node object is the TABLE, so it keeps the id and the unchanged lead
  text is minted new — or the two swap. Frequency: **0 of 14** real table
  retypes split a lead; 3 of 28 across fixtures.
  **(b)** the one the PoC found rather than predicted: **every death is a
  paragraph** — 2,264 of 2,264 real, 18 of 18 on spec — because a paragraph
  consumed entirely by reference definitions is freed and a
  `REFERENCE_DEFINITION` is born with a fresh id. Typing a definition is
  **birth → death → birth**, and at 2,264 events it dwarfs all 34 retypes.
  Should the definition inherit the paragraph's id? Today it does not.
  **Phase C** starts once (a) and (b) are ruled; nothing else in D4 is open,
  and Phase B does not wait on it.
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
- **D6 — bindings with C, or C first? · RULED: together, one release.** Swift,
  Kotlin and ES land the shape in the same release as the C entry point. Swift
  is the semantic canon. Each binding's conformance entry already exists
  (`conformance:swift-macos`, `conformance:kotlin-jvm`, `conformance:es-node`)
  and T14 extends the corpora rather than adding a channel.

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
- [ ] **T19 — a holder reference count, so a borrow survives the feed that
      replaces it.** The tree a feed returns is a BORROW; what needs solving is
      not who frees it but a consumer still holding it when `feed` is called
      again. One counter per cached subtree holder, not one per inline;
      `markdown_core_node_free` releases a hold and destroys at zero, and a
      block whose children are borrowed detaches them rather than splicing them
      into the free walk. **Inert until something shares** — `calloc` gives
      `refs == 0` and every path that exists today keeps its behaviour — so it
      lands on its own. *Gate: correctness 88/88 unmoved, and a borrow held
      across a feed that replaces it still reads.*
- [ ] **T20 — `markdown_core_iter_init`: a walk that does not allocate.**
      `markdown_core_iter_new` callocs
      ([iterator.c:15](../packages/markdown-core/core/iterator.c#L15)), which is
      right once per document and wrong once per block per pass per feed. The
      struct is already complete in the internal header, so the walk can live on
      the caller's stack. Correctness 88/88 unmoved. Independent of everything
      else; see F16 for what it costs.

### Phase B — make a feed stop re-parsing  · needs no decision

**This is the phase that answers the owner's constraint** — cloning per feed is
accepted, re-parsing is not (F12). T18 takes the tail off the whole tree; T3
and T4 are the cache key; T9 lands the cache and shares.

- [ ] **T18 — make the whole-tree tail per-block.** It comes first because
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
      corpus and the real corpus.*
- [ ] **T3 — a write stamp on the CST block**, bumped whenever the parser writes
      that block (content appended, type retyped, closed). Half the cache key.
      It must hook the FIELD WRITES, not `markdown_core_node_set_type`: the
      setext retype writes `->type` directly
      ([blocks.c:2366](../packages/markdown-core/core/blocks.c#L2366)) and
      `tasklist.c:105` changes what a consumer sees without touching `type` at
      all (F11).
- [ ] **T4 — a generation counter on each definition map**, bumped on every
      insert. O(1); flips nothing and walks nothing. Both maps reopen their
      preparation on insert already
      ([references.c:104-110](../packages/markdown-core/core/references.c#L104-L110)),
      which is where the counter goes. The other half of the cache key.
- [ ] **T9 — cache a closed block's derived subtree** on its CST block and
      **share it into the tree a feed returns**. T18 has already taken the tail
      off the shared nodes and T19 keeps a borrow alive across the feed that
      replaces it, so the hand-out shares from the start. The store copies once
      per miss; only the hand-out shares. This is also where T18's filter starts
      skipping a block whose projection came from cache.
- [ ] **T10 — invalidate by generation**, per F6's rule: a cached projection is
      invalid when the map has advanced past the generation it was made at.
      Correctness must never depend on the cache — a wrong cache is a slow
      engine, not a wrong tree.
- [ ] **T11 — measure the reactive loop** on the full corpus and price the
      copy-out a feed pays to return a value (§6). Every shape has been measured
      on a 40-document subset in F12–F16; T11 confirms them at full scale.

### Phase C — make a block addressable  · needs D4

D4's mechanism is settled by PoC (F11); its two inheritance forks are not.
Nothing here starts before they are ruled. **This is the consumer's axis, not
the feed-cost one** — Phase B does not wait on it.

- [ ] **T2 — a stable id on the CST block**, minted at open, carried onto the
      derived node by the clone. F11 has the mint and carry sites.
- [ ] **T5 — gate:** two projections of an unwritten CST produce identical ids,
      and a retype preserves the id of the block it rewrites.
      *Closes F4.*

### Phase D — the change signal  · needs T2 and Phase B

D1 and D3 are dissolved (§4): the signal is the identity D4 mints, carried on
the block, and a feed returns the document rather than answering a query.

- [ ] **T6 — stamp each derived block** with the `(write stamp, map generation)`
      it was derived at.
- [ ] **T7 — carry the stamp pair out on the returned block**, so a consumer
      holding the previous `updated` classifies every block as new / changed /
      unchanged by joining on identity and comparing stamps. No list, no query:
      the tree it was handed carries it.
- [ ] **T8 — gate:** for every fixture, feed to each block boundary and assert
      the changed set is exactly the blocks whose bytes moved or whose
      resolution moved. This is the gate that makes the whole design falsifiable.
      *Closes F5.*

### Phase E — the public surface  · D5 and D6 ruled

- [ ] **T12 — export the streaming entry point** in C, carrying the shape §4
      rules: a session, `feed`, and the document's two total views. Whatever the
      C spelling, `scripts/audit-public-surface.sh` gates it against the header,
      the ELF version script and the Mach-O list together.
- [ ] **T13 — the returned document carries the change classification** from T7.
- [ ] **T14 — bindings** (Swift, Kotlin, ES) in the same release, and their
      conformance corpora. Swift is the semantic canon.
      *Closes F3.*

### Phase F — bounds and gates

- [ ] **T15 — the reactive-loop bound as a gate:** the **projection** side of a
      feed is `O(open block + changed set)` and carries **no term in the
      document already fed**. Two terms are carved out and reported beside it
      rather than folded in, because both are accepted: the **whole-CST clone**
      (§6) and the binding-side **copy-out**. It needs all of Phase B: with T9
      alone the tail keeps a whole-document term, and with T18 alone there is
      nothing to skip. A fitted
      slope in document size fails and names the state being re-derived. The
      binding-side copy-out is bounded separately and stated, not hidden inside
      this one.
- [ ] **T16 — measure resident memory** across a long stream, and state the
      bound that comes with every block keeping its content buffer for life.
      F14's first draft claimed clearing a formula block's content at close would
      reduce it; `markdown_core_strbuf_clear` returns nothing to the allocator,
      so that saving needs `_free` and is untested.
      *Closes F9.*
- [ ] **T17 — structural invariants over carried opaque extension state**, so a
      field that stops surviving the clone fails a gate rather than a golden.
      *Closes F8.*

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
  it is measured separately at T11 and bounded separately at T15, and it is the
  reason the change classification exists — a consumer that re-renders only the
  changed blocks pays the copy but not the render.
- **The per-feed clone is a whole-document term and NO task removes it.**
  `markdown_core_parser_derive_tree`
  ([blocks.c:1711](../packages/markdown-core/core/blocks.c#L1711)) is
  `S_clone_block_tree` plus `S_project`, and the clone walks the entire CST and
  copies every block's content bytes. The owner accepts it, so **T15's bound is
  stated over the projection only**: the clone is measured and reported beside
  it, the way the binding-side copy-out is. A gate demanding "no term in the
  document already fed" without that carve-out could not pass however much of
  Phase B landed.
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

**Inherited from the Stage 1 design work and not re-measured**: F6's 36.9% /
23.0% / 7.2%, F7's 27.40% / 80.1% / 8-of-90 / 4.08%, F8, F9, and the
4-of-670 reference-loss figure in §2. All recoverable from
`git show 8d0910b:docs/RECONSTRUCTION.md`. **Re-measure any of them before it
decides a task**, because each was taken against a tree that Stage 1 has since
changed.
