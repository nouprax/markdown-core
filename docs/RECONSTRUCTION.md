# Reconstruction

**This document governs.** Where it disagrees with any other document in this
repository, this one is right and the other is stale. Everything under
`docs/deprecated/` is archive: it describes engines and programs that either no
longer exist or have not been rebuilt yet. Nothing there is normative. A
deprecated document returns to `docs/` only when the step that makes it true
again has landed, and only by a deliberate commit that says so.

---

## 0. How to continue

Everything needed to pick this up cold is here. Nothing about the state of the
work lives outside this file.

**The work is on branch `reconstruct-from-1.0`.** `main` is untouched and still
carries the abandoned streaming program; do not build on it. The branch
`streaming-every-partition` holds that program's last 21 commits and is kept
only as a record.

### The state

| | |
|---|---|
| Branch | `reconstruct-from-1.0` |
| Landed | Steps 0 and 1, §4.0's re-ordering, and Stage 0a's 0a.0 through 0a.11 |
| Engine | byte-identical to `580d10c` (tag v1.0.3) **except** `core/main.c`, which gained `--profile` |
| `VERSION` | **`3.0.0`**, as of the owner ruling of 2026-08-21. There is no 1.0.4; see §4.10 and Q27 |
| Next action | **Stage 0a**, §4.2, at **0a.12** — 0a.0 through 0a.11 have landed, and **0a.15 now exists** (§2, §4.2.3) |

`--profile` is a named option set for the CLI, added because the restored parity
harness invokes it and the baseline had no such flag: `gfm` turns this
repository's own two extensions off so a parity run compares one language,
`gfm-extended` turns them on with the formula delimiters enabled. No existing
invocation parses differently, and the extension attach ORDER is deliberately
untouched — reordering `table` is a behaviour change that belongs to Step 3.

### Every gate, and how to run it

The sanitizer presets have their OWN configure and build; building `default`
does not prepare them.

```
cmake --preset default && cmake --build --preset default --parallel
cmake --preset asan    && cmake --build --preset asan    --parallel
cmake --preset ubsan   && cmake --build --preset ubsan   --parallel

ctest --preset correctness -j 8            # 67/67
ctest --preset correctness-asan -j 8       # 58/58 — SEE THE WARNING BELOW
ctest --preset correctness-ubsan -j 8      # 58/58 — SEE THE WARNING BELOW
node scripts/check-canonical-ast-fixtures.mjs   # 28 kinds, 47 fields, 6 cases
bash scripts/audit-public-surface.sh
node scripts/audit-extension-special-chars.mjs   # 4 extensions, 5 sentinels
node scripts/audit-extension-attach-order.mjs    # one attach site, table last (D15, added 0a.11)
node scripts/check-plan-graph.mjs                # 22 steps, 45 edges, acyclic
node scripts/fuzz-parity.mjs --iterations 300                   # upstream, 300/300
node scripts/fuzz-parity.mjs --oracle mdast --iterations 300    # KNOWN-RED, see below
node scripts/check-upstream-parity.mjs     # 816/816 vs cmark-gfm 0.29.0.gfm.13, 7/7 divergences
node scripts/check-mdast-parity.mjs        # 54/54, backlog 24/24 still diverging
node scripts/audit-scope-sanity.mjs        # 207 rows, only-shrink holds

# The three position oracles, landed at 0a.1 (§4.2.7). Each fails on a row
# APPEARING and on a row CLEARING, so a fix that moves one without recording it
# fails here rather than in review.
node scripts/audit-inline-sourcepos.mjs    # 0 rows registered, 68 scanned
node scripts/audit-scope-containment.mjs   # 58 rows registered, 3657 scanned
node scripts/audit-position-places.mjs     # 122 rows registered, 3929 scanned

# D9's pin. REGISTERED RED and it fails if a row STOPS reproducing, because
# deleting the budget clears both rows and costs 204.678x output growth.
node scripts/audit-reference-order-independence.mjs  # 2 rows, must stay red
```

**`22 steps, 42 edges` was stale**, and it is the second number in this table to
have drifted from what the command prints. `check-plan-graph.mjs` reads its edge
list out of *this file*, so the count moves whenever a section adds an arrow;
§4.13 added three. Anyone reconciling the gates should print the number rather
than trust the row — that is how D17 was found.

**A sanitizer preset with no build reports GREEN having run nothing.** With
`build/asan` absent, `ctest --preset correctness-asan` prints
`No tests were found!!!` and **exits 0**. Run the configure and build lines above
first, and treat a sanitizer run that does not report `58/58` as a failure
however it exited. This is a gate that cannot fail, which is worse than a gate
that is missing.

**`fuzz-parity` takes `--iterations`, not `--cases`.** An unknown flag is
silently ignored, so `--cases 5` runs the default 300 and prints `300/300`. Any
pin recorded with `--cases` measured the default and means nothing.

The upstream oracle needs a built cmark-gfm:
`scripts/init-environment.sh --install upstream-cmark`.

**Two checks are KNOWN-RED and owned, not forgotten** — the same pattern the
mdast backlog and D9's oracle use:

| Check | Why red | Owner |
|---|---|---|
| `scripts/audit-ast-projections.mjs` | Added at `26045be`; audits a kind/field table the baseline engine does not have. | Step 15 |
| `scripts/check-generated-scanners.sh` | Added at `8926594`; the baseline build has no re2c invocation or version pin (R9). | R9's experiment, then Step 3 |
| `node scripts/check-release-version.mjs --skip-swift` | **D17 is fixed and the 3.0.0 bump closed the rest**; what remains is **two** unexpected legacy tags — `codex-doc-pass-backup` and `pre-format-baseline` — which is repo hygiene, not engine state. Every version-drift, release-note and CHANGELOG assertion now passes. | release |
| `node scripts/fuzz-parity.mjs --oracle mdast` | 0/3 — the mdast oracle is red on every generated input, for the same reason the 23-entry backlog exists. CI runs both oracles; only the upstream one was listed. | Stage 0 close |
| `pnpm audit:ci`, `audit:source-lists`, `audit:ast-projections`, `format:es:check` | Not yet triaged by era (§0's rule). | 0a.0 item 5 |

**`scripts/` IS NOT ONE THING, and Step 0 got this wrong.** It was restored
from `main` wholesale. That is right for *infrastructure* — CI, environment,
build and release plumbing, which carry the Action SHA pins — and wrong for any
script that encodes a **contract about the engine**, because such a script
asserts a contract the baseline engine has not got yet. Two were caught this way
and restored to their baseline versions in 0a.0:
`check-canonical-ast-fixtures.mjs` (main's expects a `comment` field that HTML
comment classification only introduced at `9af16c9`) and `audit-public-surface.sh`.
Both are green at their baseline version and were red at main's. **When a gate is
red, ask which ERA it belongs to before assuming the engine is at fault.**

`timeout` is not on the macOS PATH; guard long runs with a background job and a
`kill`.

**The make-3.81 same-second mtime trap bites during MUTANT TESTING, not only on
a fresh checkout, and §2's `rm -rf build/` warning does not cover it.** Editing
a source file and rebuilding within the same second leaves the old object in
place, and the suite then reports the mutant passing. It produced two false
"this gate does not catch it" readings at 0a.6 alone, one of which nearly went
into the record as a missing gate. **Between an edit and a rebuild, `touch` the
file and `sleep 2`** — and confirm the mutant is live by running its witness
through the binary before trusting a green suite.

### The three standing rules

1. **No commit may leave `spec_commonmark` failing.** It is the cheapest oracle
   in this repository, and the previous attempt failed precisely because it
   broke the one-shot and then had nothing left to measure streaming against.
2. **The mdast backlog only shrinks, and only on purpose.** Its 23 entries each
   name the step that closes them; the gate requires each to *still* diverge, so
   a step that lands without deleting its own entries fails as loudly as a new
   divergence. Zero of the 23 close in Stage 0a, by design — the backlog
   measures distance to mdast's *model*, while the defects measure wrongness
   against the engine's own intent.
3. **A behaviour change regenerates its goldens in the same commit**, and every
   moved row is reviewed by hand and named in the commit message.

### What is already decided, and must not be re-opened

- **Q1–Q7** (§9) are settled, with their reasoning in §5.7 and §5.8. Q4 in
  particular is *both* `label` and `identifier`, on both node kinds, plus an
  exported fold — and the ecosystem argument that first suggested it was
  discarded as circular, so it must not be reintroduced as support.
- **Defects come before the port** (§4.0). Ten of eleven were each proved
  fixable on the untouched baseline. The old order rested on an untested claim
  that Step 3 must precede everything.
- **The CST needs no substrate** (§6).

---

## 1. What happened, and why this exists

The engine grew a session/incremental layer, then a delta layer, then a
streaming/append layer with a living tree, a publish/retract record, an inline
frontier and a two-tree shadow projection. That program failed, and it failed in
a way worth writing down, because the failure is instructive rather than
embarrassing.

The last attempt — branch `streaming-every-partition`, 21 commits — routed
`markdown_core_document_new` through the same projection machinery as
`markdown_core_document_append`. That is: **there was no working one-shot parse
left to measure streaming against.** The suite read 65 of 98, and
`spec_commonmark` reported 482 passed / 66 failed / 121 errored. Six defects
were found and fixed in that state, and every single one was an *aliasing* bug
between the two trees:

| Defect | What was aliased |
|---|---|
| the projection's free rewound the live parser | parser struct copy vs. live counters |
| the spine array was read after being freed | projection metadata lifetime |
| a live block's children named a freed copy | the child list, shared by pointer |
| `finalize` ran on an already-closed block | `current` crossing between trees |
| copies stayed spliced into the live tree while feeding | the two trees interleaved |
| a copy claimed the live block's chunk payload | ownership in the node's payload union |

The one that could not be fixed was the same thing again: the delimiter engine
takes **one unit at a time**, and a projection is a second unit. With one tree,
none of these can exist by construction.

Two conclusions carry forward, and they are the reason for the rest of this
document:

1. **Ask why the one-shot works before asking why streaming does not.** The
   cheapest oracle in this repository is "does `spec_commonmark` still pass?" It
   would have caught the very first commit that broke the one-shot. Every step
   below is gated on it.
2. **The feed is already lossless and already incremental.** `S_parser_feed`
   splits on line ends and calls `S_process_line` once per complete line, with
   the incomplete trailing line buffered in `parser->linebuf`. So after *n*
   appends totalling *L* bytes, the parser's state is identical to a one-shot's
   state after feeding those same *L* bytes. There is no gap. The only
   difference is the finish, and the finish's only new input is the held partial
   line.

---

## 2. The baseline, and its measured pin

The engine is reset to **`580d10c`** (tag `v1.0.3`, and the root of `main`).

The C engine under `core/`, `extensions/` and `include/` is **byte-identical**
across tags `v1.0.1`, `v1.0.2` and `v1.0.3` — the only difference under those
paths is the version string. Everything else separating those tags is scripts,
CI and bindings. So "go back to tag 1.0" and "go back to 580d10c" name the same
engine, and `580d10c` is the one in `main`'s history, which is why it is the one
used: later work can be cherry-picked forward rather than hand-copied across a
disconnected root.

Kept from `main`: `.github/`, `scripts/` and `AGENTS.md` — the operational
layer, including the Action SHA pins. Everything else is the baseline.

~~`VERSION` stays **1.0.3**~~ — **superseded by the owner ruling of 2026-08-21:
`VERSION` is `3.0.0` as of that commit.** The reasoning that made 1.0.3 right
still holds for the *engine* — it was byte-identical to 1.0.3, and the 2.0.0
major was bought with a session API that no longer exists — but it stopped being
right for the *tree* the moment 0a.2 moved parse output. **1.0.4 was never
available**: Q27 measured `check-release-version.mjs`'s ordering assertion to be
unsatisfiable at 1.0.4, because the tag `v2.0.0` exists and every tag must be
strictly below `VERSION` when its own tag is absent. 3.0.0 is the smallest
number that both is honest and leaves that gate reachable. §4.10 states what the
number does and does not oblige.

### The pin

Measured on the reset tree, not assumed. Steps 0 and 1 have landed, so this is
the full pin including the restored oracles:

```
correctness          65/65    100%
correctness-asan     57/57    100%
correctness-ubsan    57/57    100%
spec_commonmark      green

upstream parity      795/795 inputs agree with cmark-gfm 0.29.0.gfm.13
mdast parity          46/46 accounted for; 10/10 registered divergences reproduce
fuzz-parity          300/300 generated inputs agree (seed 1, 1213 fragments)
scope-sanity         207 unresolved rows, only-shrink ratchet holding
```

**Reproducing this pin requires `rm -rf build/` first.** A `build/` directory
left in the checkout can hold objects compiled from a different source state that
`cmake --build` will not rebuild — make-3.81's same-second mtime trap, where a
source and its `.o` both carry the checkout timestamp. With a stale `build/` the
tree reads **64/65**, `regression_commonmark` fails on example 24, and D10's
impossible position does not reproduce. Anyone measuring against this pin who
skips the wipe is measuring a different engine.

**The scope-sanity ledger counts THREE classes, not two.** It was extended on
2026-08-20 because a third shape was slipping through: **line zero with a
non-zero column**, such as `scope=0:0..0:13`. It is not a sentinel — not all
four coordinates are zero — and not a negative range — the end is after the
start — so both existing tests passed it as an ordinary position. There is no
line zero. It is written when a node is `calloc`'d and its start is never
assigned while its end is, which is what a synthesized replacement node does.
The single corpus row the new class caught is `Text scope=0:0..0:2
literal="123"`: a footnote **ordinal**, written by the very mechanism Step 9a
deletes. The ledger went 206 → 207 in the same commit.

**795/795 against upstream is not a coincidence.** At 1.0 this engine had not
yet diverged from cmark-gfm deliberately; every registered divergence in
`main`'s policy describes a fix made after 1.0. That is why the policies had to
be re-pinned rather than copied, and it is why the mdast gate — which compares
against a target the engine has *not* reached — is the one carrying a backlog.

### Stage 0 progress meter

The mdast gate carries a **reconstruction backlog**: inputs where remark is
right and this engine has not caught up. Every entry names the step that closes
it, and the gate requires each to *still* diverge — so a step that lands without
deleting its own entries fails as loudly as a new divergence.

```
    15  Step 7   — directive grammar conformance
     6  Step 9b  — the reference node model
     2  Step 6   — formula
     1  Step 10  — the split-off table lead
    --
    24  remaining
```

**This started at 23 and the one growth is recorded.** 0a.10 added D22's pin to
`extensions-directive.txt`, which is also mdast corpus, and the input diverges
for the PRE-EXISTING attributes-JSON and label-shape gap — not for anything D22
introduces. §4.2.3 authorised it in advance. The `Step 9a — definition
retention` row this list used to carry was never in the data: the entry it names
is registered under `Step 9b`, and §4.6 says which is right.

**When this list is empty, Steps 6, 7, 9 and 10 have landed — and that is all it
means.** The backlog is a parity progress meter, NOT Stage 0 acceptance. The last
step that closes a backlog entry is Step 10; Steps 11–15 close zero. Stage 0 is
accepted by §4.8's checklist, not by this number reaching zero.

**Stage 0a closes none of them, by design.** The backlog measures distance to
mdast's *model*; the sixteen defects are wrongness relative to this engine's own
stated intent. They are different axes, and expecting the defect stage to shorten
this list is the natural guess and the wrong one. The one thing that moves is the
attribution: `corpus.md:69` (`[^orphan]`) closes on definition retention, which
was measured to work at the untouched baseline and therefore belongs to **Step
9a**, before the CST — not behind it. See §4.6.

### The standing gate

> **No commit on this branch may leave `spec_commonmark` failing.** If a step
> legitimately moves spec output, the golden is regenerated in that same commit
> and every moved row is reviewed by hand and named in the commit message.

**And its corollary, which the first version of this plan did not draw.** A
golden regenerated while a defect is live *blesses* the defect: the reviewer's
only available answer is "unchanged from before, therefore fine", because the
defect's statement is four hundred lines away in §2 and names a step that has not
happened. That is how a golden comes to assert `Text scope=0:0..0:0` as expected
output — which `tests/fixtures/regression.txt:474` does today. **So: fix a defect
before any step regenerates a golden over it.** That corollary is the whole
argument of §4.

### What the baseline already has

A complete CommonMark + GFM engine with six extensions attached: table,
strikethrough, autolink, tasklist, formula, directive. Renderers were removed
before 1.0 (migration phase 9).

**Directive and formula support already exist** — 1,383 lines of
`extensions/directive.c` covering inline, leaf and container forms with
attribute parsing and JSON in both directions; 616 lines of
`extensions/formula.c` covering `$x$`, `` $`x`$ ``, `$$x$$`, `\(x\)`, `\[x\]`,
the block forms and the ```` ```formula ```` retype. So the two named
deliverables are **grammar conformance and defect correction, not new
features.**

Absent at baseline: sessions, incremental, delta, the source rope, node ids and
revisions, diagnostics, concrete records, the delimiter engine,
`ReferenceDefinition` nodes, `parser->line_marks` — **and every parity oracle.**

### Thirty-one defects live in the baseline

The first eleven were found by reading. **All eleven have since been built,
gated and reverted** on isolated worktrees at `8e76a94` — every claim below
about a line count, a moved golden row or a green suite is a measurement, not
an estimate. Doing that found five more (D12–D16). D17 was found reconciling
the gates and is fixed at 0a.0. D18–D24 were found restating the port list as
requirements, D25 while inventorying parser state for Stage 1, D26 while executing the
Q25 ruling — see §4.2.5 — and **D31 at 0a.6**, by un-gating D3 and reading what the
newly live code then reported. It is the only one of the thirty-one that this
programme created a witness for rather than inherited, and it is inherited too:
cmark-gfm reports the same wrong column. Every one of the fourteen that Q25 put to the test was
found **fixable on the untouched baseline**; none produced an architectural
dependency, and D9 remains the plan's only exception.

**All thirty-one are recorded here**, because a defect the plan does not name
is a defect the plan will re-derive later at full price — and because a list
split across three sections is a list nobody reads.

#### The index — every defect, its owner, and how it was confirmed

D18–D25 were each reproduced independently before being scheduled, on the tree
at HEAD, with the witness shown. The three marked **[verified here]** are the
ones whose witness is stated in this section rather than in the row.

| # | What is wrong | Severity | Owner | Confirmed |
|---|---|---|---|---|
| D1 | extensions fold `$ : }` and bytes `0x01`–`0x08` into `skip_chars`, killing CommonMark flanking | wrong-output | 0a.4 | built & reverted |
| D2 | `'}'` registered special, never consumed | wrong-output | 0a.4 | built & reverted |
| D3 | `adjust_subj_node_newlines` behind an option nothing sets | wrong-position | 0a.6 | built & reverted |
| D4 | `skip_chars[peek_at(...)]` read before the bounds test | latent | 0a.3 | built & reverted |
| D5 | title-rewind path writes the scanned chunk into the refmap | wrong-output | 0a.7 | built & reverted |
| D6 | `make_autolink` writes `title = ""` where nothing was written | wrong-output | 0a.7 | built & reverted |
| D7 | `make_autolink` omits `column_offset + block_offset` | wrong-position | 0a.6 | built & reverted |
| D8 | `try_opening_table_header` returns the parent on eleven non-opening paths | wrong-output | 0a.5 | built & reverted |
| D9 | reference resolution is order-dependent | wrong-output | **9a only** | 200 refs → 99 resolve, 101 do not |
| D10 | an undefined footnote call **loses source bytes** | data-loss | 0a.2 | `x[^a⏎b] tail` → `"x[^] tail"` |
| D11 | a nested duplicate definition **deletes a paragraph** | data-loss | 0a.2 | `"OUTER opens first"` in no node |
| D12 | `consolidate_text_nodes` drops `end_line` | wrong-position | 5 | built & reverted |
| D13 | autolink's `len==0` sentinel leaves a zero-length `Text` | wrong-output | 5 | built & reverted |
| D14 | the `"[^"` prefix rebuilt over decoded bytes | wrong-output | 9a | built & reverted |
| D15 | the CLI and the facade attach extensions in different orders | wrong-output | **fixed at 0a.11** | built & reverted |
| D16 | two more null/empty sites | wrong-output | 14 | built & reverted |
| D17 | shipped v1.0.3 declares `MARKDOWN_CORE_VERSION` = **1.0.0** | wrong-output | **fixed at 0a.0** | header vs `VERSION` |
| D18 | a paragraph whose leading definitions were consumed keeps the **definition's** line | wrong-position | 10 | `[a]: /1⏎text here` → `Text 1:1..1:9`, a column that does not exist on line 1 **[verified here]** |
| D19 | a link takes `start_line` from the **closing** bracket | wrong-position | 8 | `[a](/u "t⏎t2") tail` → `Link 1:1..1:14`, `Text 1:15..1:19` — both on a 9-character line **[verified here]** |
| D20 | strikethrough never sets `end_column` | wrong-position | 8 | `a~~` → `Text scope=1:1..1:0` |
| D21 | **a container directive's closing fence does not close it** | **content-attribution loss** | 7 | `:::note⏎body⏎:::⏎after` → `after` is pulled *inside* the block **and** reported at line 3 while it is on line 4 **[verified here]** |
| D22 | an extension consuming a span with a line ending cannot report it | wrong-position | 7 lands, 8 owns | `Directive 1:1..1:29` on a 28-character line; blocks Step 7's oracle |
| D23 | `S_insert_emph` takes the **whole** run's start column | wrong-position + overlap | 8, gated by 11b | `***a**` → `Text "*"` claims columns 1–3 and `Strong` also starts at 1: two nodes, one byte |
| D24 | `tasklist` decides `checked` by `strstr` over the whole line | wrong-output | **fixed at 0a.11** | `- [ ] see [x] below` → `checked=true` |
| D25 | a `FootnoteReference` label can be a **dangling pointer**, read on every lookup | **use-after-free** | 0a.2 | ASan: `heap-use-after-free`, READ of size 1 in `markdown_core_map_lookup (map.c:279)`, freed by `handle_close_bracket (inlines.c:1384)` |
| D27 | `parser->linebuf.oom` written at six sites and read at none | silent truncation (allocation failure only) | 3a, with A1 | §4.13.11, measured: 244 input bytes become 102 with `parser->oom == 0` |
| D28 | `extensions/formula.c` ignores `markdown_core_chunk_to_cstr`'s failure and keeps a **borrowed** pointer | **use-after-free** | **0a — UNSCHEDULED, see below** | §4.13.11, ASan: `heap-use-after-free`, READ of size 5 in `markdown_core_extensions_get_formula_literal` |
| D29 | `extensions/table.c:297` does not check `markdown_core_node_new_with_mem`, and `:305` dereferences NULL | **crash** | **0a — UNSCHEDULED, see below** | §4.13.11, SIGSEGV on `lead text⏎x | y` / `--|--` |
| D30 | `markdown_core_reference_create` commits an entry whose url or title was lost | wrong-document (allocation failure only) | 9a/11c delete it; §4.13.9 pins it | §4.13.11, measured on four refused allocations |
| D31 | a raw HTML tag that crosses a line ending ends **one column short of its own literal** | wrong-position | 8 | found at 0a.6 and pinned as a golden row: `a <b`⏎`c> d` gives `HTML scope=1:3..2:1` for a literal whose last byte is at `2:2`, while `a <b c> d` gives `1:3..1:7`, which covers it. cmark-gfm is wrong the same way |

**~~D25 also exposes a gate blind spot~~ — WRONG, and corrected at 0a.3.** This
paragraph said the `asan` preset allocates through the arena and therefore
*"cannot observe a use-after-free in node-owned memory at all"*. **The second
half is false.** The fixture runner does not go through `core/main.c`:
`spec_runner` → `ts_ast_parse` → `markdown_core_document_parse` →
`extensions/ast.c:113`, which calls `markdown_core_parser_new` with the
**default allocator**. `ctest --preset correctness-asan` runs the whole golden
corpus on malloc/free, and 0a.2 measured it catching D25: with the regression
example added and the fix reverted it reads **56/57** with a genuine
`heap-use-after-free`. **One ordinary regression example is a complete
memory-safety gate.** The blind spot is real but CLI-only — it covers
`markdown-core` itself and `dump_cli_runner` — and **Q12 is not a prerequisite
for anything in this stage.**

**D28 AND D29 ARE STAGE 0a DEFECTS WITH NO SUB-STEP, and §4.2.3's list does not
mention them.** §4.13.11 assigns both to Stage 0a — D28 *"ahead of Step 6"*, D29
*"ahead of Step 3"* — and §4.12 says every defect is fixed before any other task.
They are a use-after-free and a crash, so they are not deferrable on merit. But
§4.2.3 enumerates 0a.0 through 0a.14 and neither appears in it, because §4.13
was written after §4.2 and its four additions never made it into the sub-step
list. **They need a sub-step, and the natural place is a new 0a.15**, after the
fourteen and before the stage closes; both are extension-local and neither
touches anything the fourteen move. Decide it before 0a.11, which is the last
step that touches `extensions/table.c`.

**DECIDED, 2026-08-21, before 0a.11: 0a.15 exists and lands last.** §4.2.3
carries it. Both witnesses were reproduced first, on the tree at `f98fefe`, and
one clause of the sentence above is wrong: **neither defect is reachable without
an injected allocation failure**, so §4.13.11's *"two are live outside
allocation failure"* is a mis-statement — what separates D28/D29 from D27/D30 is
that these two are memory-unsafety while those two are silent wrong-document,
and that these two are *not* deleted by any later step's mechanism.

- **D29 needs a refused allocation and nothing else.** `lead text⏎x | y⏎--|--`
  parses cleanly at the baseline (`Paragraph` + `Table`, exit 0). Under a
  one-shot allocator sweep it SIGSEGVs at allocation **35 of 64**:
  `try_opening_table_block` → `try_inserting_table_header_paragraph` →
  `markdown_core_node_set_string_content(NULL, …)` → `markdown_core_strbuf_sets`
  reads `NULL + 8`. `EXC_BAD_ACCESS (code=1, address=0x8)`.
- **D28 needs one too, and §4.13.11's citation is exact.** The reachable
  spelling is the ```` ```formula ```` retype, not `\(…\)`:
  ```` ```formula⏎x+y+z⏎``` ```` under the same sweep gives ASan
  `heap-use-after-free`, **READ of size 5** in
  `markdown_core_extensions_get_formula_literal` at `formula.c:61`, freed by
  `markdown_core_node_free` under `replace_with_formula_block` at
  `formula.c:557`. The borrowed pointer is the *old code block's* literal.

**Landing last costs nothing, and that is measured rather than assumed.**
Neither defect has a golden expression — no fixture input reaches either path —
so §4.4's corollary (*fix a defect before any step regenerates a golden over
it*) does not apply to them, and 0a.11 through 0a.14 can neither introduce nor
mask them. **Their gate is a corpus addition to `case_oom_sweep`**
(`tests/runners/fallback_runner.c`), whose `FB_SWEEP_CORPUS` today contains
neither a paragraph immediately followed by a table nor a ```` ```formula ````
info string — which is why the only allocation-failure gate in the tree is blind
to both.

**Citations are `function` (`file:line`) pinned to `8e76a94`.** The function
name is the durable half: a landed fix shifts every line below it — deleting
D3's four-line guard moves D4 from `inlines.c:492` to `488` — and that exact
shift has already produced one false "the doc is off by four" correction. **Each
defect commit re-pins the citations that remain.**

| # | Defect | Severity | Fixable on the untouched baseline | Goldens moved | Seen by an existing oracle | Lands |
|---|---|---|---|---|---|---|
| D1 | attaching an extension kills CommonMark flanking | wrong-output | yes — **−2 lines** | 0 | corpus-blind only (mdast, +3 rows) | 0a.4 |
| D2 | `'}'` registered as special, never matched | latent + resource | yes — **−1 line** | 0 | **no** | 0a.4 |
| D3 | multi-line inline `Code`/HTML positions behind a dead option | wrong-position | yes — **−4/+1 lines** | 13 | **no** | 0a.6 |
| D4 | read at `input.data[len]` before the bounds test | memory-unsafety (latent) | yes — **1 line** | 0 | **no** — ASan and UBSan are blind *by construction* | 0a.3 |
| D5 | rewound title still written into the refmap | wrong-output | yes — **+2 lines** | 0 | yes, once registered | 0a.7 |
| D6 | autolink writes `title=""` where nothing was written | wrong-output | yes — **−1 line** | 18 + 1 assertion | **no** — all three parity oracles fold `""` to `null` | 0a.7 |
| D7 | `make_autolink` omits `column_offset + block_offset` | wrong-position | yes — **2 lines** | 0 | **no** — and upstream carries the same bug | 0a.6 |
| D8 | table's block opener swallows every later extension's | wrong-output | yes — **6 lines** | 0 | **no** — the corpus never co-enables two extensions | 0a.5 |
| D9 | the reference budget makes resolution order-dependent | wrong-output | **NO — genuinely blocked** | — | **no** | 9a; **pinned by two gates in 0a.8** |
| D10 | an undefined footnote call loses source bytes | **data-loss + memory-unsafety** | yes — **~10 lines** | 1 + 1 ledger | half, **and the fixture pins the defect** | 0a.2 |
| D11 | a nested duplicate footnote definition deletes a paragraph | **data-loss** | yes — **~9 lines** | 0 | **no** | 0a.2 |
| D12 | text consolidation carries `end_column` but not `end_line` | wrong-position | blocked by D13 | unmeasured | no | 5 |
| D13 | `set_sourcepos_from_range` returns early on `len == 0` | wrong-position | yes | unmeasured | the ratchet counts the row, not the cause | 5 |
| D14 | a failed footnote call rebuilds `[^` over decoded bytes | wrong-output | yes — **1 line** | unmeasured | no | 9a |
| D15 | the CLI and the facade attach extensions in different orders | wrong-output | yes | unmeasured | no | 3 |
| D16 | two more sites write `""` where nothing was written | wrong-output | yes | unmeasured | no | 14 |

**Ten of the eleven can be fixed now, in 74 lines of C across six files.** Only
D9 cannot, and §4.2 says what pins it in the meantime.

1. **Attaching `formula` or `directive` kills CommonMark flanking.**
   `create_formula_extension` (`formula.c:613`) and `create_directive_extension`
   (`directive.c:1380`) call `set_emphasis(ext, 1)`, which
   `markdown_core_manage_extensions_special_characters` (`blocks.c:504-518`)
   folds into `parser->skip_chars`, which `scan_delims` walks over. There is no
   option gate: **merely attaching the extension corrupts the base language.**

   ```
   $ printf 'foo:_bar_\n' | markdown-core --profile default
   └── Text literal="foo:_bar_"                 # one flat Text
   $ printf 'foo:_bar_\n' | markdown-core --profile gfm
   └── Text "foo:" + Emphasis → Text "bar"      # and this is what cmark-gfm says
   ```

   Bidirectional: emphasis is invented as well as lost, because the sentinel
   delimiter constants (`FORMULA_DELIM_*` = 1..4, `DIRECTIVE_LABEL_DELIM` = 8)
   reach `skip_chars` too, so `printf '\x01*.foo.*\n'` yields an `Emphasis` that
   CommonMark does not have. Measured footprint: over all 19,607 strings of
   length ≤ 5 over `{a } : $ * _ .}`, **186 (0.95%) parse differently** before
   and after the fix — identical 186 under `--profile default` and
   `--profile gfm-extended`, zero under `--profile gfm`.

   **Two corrections to the original statement.** (a) The set is `$ : }` plus
   bytes `0x01`–`0x04` and `0x08` — **not** `$ \ : ] }`.
   `is_core_special_character` (`inlines.c:1487-1504`) returns early for `\` and
   `]`, so `markdown_core_inlines_add_special_character` never sets them;
   `a]*.foo.*` emphasises correctly at the baseline. (b) There is a **third**
   `set_emphasis` site, `strikethrough.c:110`, which the original list omits.
   That one is inherited from cmark-gfm, behaves identically there, and **must
   be kept** — `~` has to stay transparent to `scan_delims` or upstream parity
   breaks. It is also what makes D4 reachable under `--profile gfm`.

   Do not transcribe the historical fix (`7c5025d`) forward. It split the list
   into `special_inline_chars` + `flanking_skip_chars`, which is the right
   shape, but deliberately kept the sentinels in the skip set on the stated
   grounds that they are "bytes that cannot appear in user text". **They can**:
   only NUL is replaced, so `0x01`–`0x04` and `0x08` are ordinary file bytes.
   The correct flanking-skip set for this engine is `~` and nothing else.
2. **`'}'` is registered as a special inline char that `match()` never
   consumes.** `create_directive_extension` (`directive.c:1377`) appends it;
   directive's `match` (`directive.c:1074-1086`) dispatches only `:` and `]` and
   falls through to `return NULL`. (The original cite, `directive.c:1379`, is
   the `set_special_inline_chars` call one line down.)

   Its *output* effect is entirely D1's: with D1 fixed, deleting the
   registration changes nothing across an exhaustive 37,448-case differential,
   because `markdown_core_consolidate_text_nodes` (`iterator.c:95-131`) runs
   before any extension postprocess and merges the split run back, carrying
   `end_column` forward. What is independently real is **memory**: on
   `'word} ' × 100000` at `--profile gfm-extended`, peak RSS is 45,432,832 bytes
   with the registration and 27,262,976 without — **+66%**, ≈181 arena bytes per
   `}` in the document, because the release CLI never reclaims what
   consolidation frees.
3. `adjust_subj_node_newlines` (`inlines.c:333`) is gated behind
   `MARKDOWN_CORE_OPT_SOURCEPOS`, which **nothing in the tree ever sets** —
   `git grep` finds exactly two hits, the guard itself and the `#define` at
   `markdown-core.h:560`. Multi-line inline `Code` and raw HTML therefore carry
   positions that are not places: `` before `code\nspan` after *emph* `` reports
   `Code scope=1:9..1:17` on a twelve-character line. `inlines.c:334-336`

   **The newly-live code inherits a sub-defect** the original statement does not
   name: it writes a *container-relative* end column, so ``> a `x\n> y` b`` gives
   `Code scope=1:6..2:1` where the truth is `2:3` (cmark-gfm is wrong the same
   way). One amended line — `inlines.c:343`, `node->end_column = since_newline +
   subj->block_offset;` — corrects it and moves **zero** additional rows. Take it
   in the same commit.
4. **A one-byte read outside the chunk.** `scan_delims` evaluates
   `subj->skip_chars[peek_at(subj, after_char_pos)]` before the bounds test, and
   `peek_at` is unguarded: `while (subj->skip_chars[peek_at(subj,
   after_char_pos)] && after_char_pos < subj->input.len)`. `inlines.c:492`

   It fires constantly — the ASan correctness suite executes it 3 times, a
   400,000-input sweep executed it 14,783 times — and **no sanitizer can see
   it**, measured, not assumed: 0 ASan reports, `poisoned=0` on all 14,783
   reads. The reason is an invariant, not luck: `markdown_core_parse_inlines`
   builds the chunk from a `strbuf`, and `strbuf` always keeps `ptr[size] ==
   '\0'` inside the allocation (`buffer.c:97,110,122,212,224,239`). The
   discarded value also cannot change the loop's exit, because `&&`
   short-circuits at `after_char_pos == len`.

   So today it is a contract violation and nothing more — **and it is one
   substrate change away from a real heap overread.** The strbuf invariant is
   written down nowhere near `inlines.c:492`. Step 11a's concrete records, or any
   future chunk that is a slice of a larger buffer, makes it live silently, with
   no gate to notice. That is the argument for fixing it while it costs one
   line.
5. On the title-rewind path, `markdown_core_parse_reference_inline` un-reads the
   title but leaves `title` holding the scanned chunk, which is then written into
   the refmap. `inlines.c:1749-1765`

   ```
   $ printf '[foo]: /url\n"title" junk\n\n[foo]\n' | markdown-core --profile gfm
   ├── Paragraph → Text literal="\"title\" junk"        # the bytes are prose
   └── Paragraph → Link destination="/url" title="title" # and also a title
   ```

   The spec's own example (`spec.txt:3236`) does not catch it because it has no
   `[foo]` reference, so the bad title never reaches a node.
6. `make_autolink` writes `link->as.link.title = markdown_core_chunk_literal("")`
   where nothing was written. `inlines.c:219`

   The contradiction is already pinned inside this repository's goldens:
   `extensions.txt:596` puts both spellings of one construct on one line, and
   `extensions.txt:667`/`:670` record `title=""` for the core autolink and
   `title=null` for the extension autolink three columns later.
7. `make_autolink` omits `column_offset + block_offset`. `inlines.c:221-222`

   It is the **only** site in `core/` or `extensions/` that turns a raw subject
   buffer offset into a column without adding both; every extension node maker
   goes through `markdown_core_inline_parser_get_column` (`inlines.c:1879`),
   which already includes them. The result is a child that escapes its parent:
   `> see <https://x.example/> ok` gives `Link scope=1:5..1:24` around
   `Text scope=1:8..1:25`. Inherited — cmark-gfm reports the same numbers — so
   **upstream sourcepos cannot be the oracle here.**
8. `try_opening_table_header` declines by returning `parent_container`.
   `table.c:325..457`

   The caller contract (`blocks.c:1444-1458`) is that non-NULL means "I opened a
   container": it assigns `*container` and `break`s, so **every extension
   registered after `table` loses its turn on that line.** The consequence is
   not cosmetic — enabling `table` changes the parse of input containing no
   table:

   ```
   formula alone,        'text\n$$\nx\n$$'  -> Paragraph + FormulaBlock literal="x"
   formula + table, same -> one Paragraph containing an inline Formula
   directive alone,      'text\n:::note\nbody\n:::' -> Paragraph + DirectiveBlock
   directive + table, same -> one Paragraph of 8 children
   ```

   The no-table output is this engine's own pinned intent. Attach order decides
   the victim: the CLI loses `formula`, the facade — **the path every binding
   uses** — loses both `formula` and `directive` (see D15). A 1,512-case matrix
   puts the reachable class at 376 cases (24.9%): any `$$`, `\[`, `::name` or
   `:::name` opener appearing as a paragraph continuation line with table
   co-enabled.

   **Correction: "eleven non-opening paths" over-counts.** There are eleven
   `return parent_container;` statements, but six are wrong declines with the
   node still a `PARAGRAPH` (`325, 329, 337, 354, 365, 372`), four run *after*
   `markdown_core_node_set_type(..., TABLE)` has succeeded and are
   allocation-failure paths that set `parser->oom` (`390, 401, 421, 432`), and
   one is the genuine opening path (`457`). **Fixing all eleven would be wrong.**
   `table` is also the only extension with this shape: directive and formula
   return `NULL` on every decline.
9. The reference expansion budget — `max(100000, total_size)` bytes of url+title
   summed over every *successful* lookup — makes whether a reference resolves
   depend on how many resolved before it. `blocks.c:799-806`, `map.c:307-309`

   A 1,422-byte document exhibits it, and the contamination crosses labels:
   `[b]: /short\n\n[b]\n` resolves; prefix it with an unrelated `[a]:` of a
   1,000-byte url plus 100 uses of `[a]`, and the identical `[b]` becomes
   `Text literal="[b]"`.

   **This is the one defect with no local fix**, and the reason is measured, not
   argued — see §4.2, step 0a.8.
10. **An undefined footnote call loses source bytes**, in the default profile,
    because footnotes are on by default (`core/main.c:133`). Thirteen bytes in,
    nine characters out:

    ```
    $ printf 'x[^a\nb] tail\n' | markdown-core --profile gfm
    Paragraph scope=1:1..2:7 children=1
      Text    scope=1:1..1:7 literal="x[^] tail"
    ```

    The `a`, the newline and the `b` are in no node, and the child fails to span
    its parent. Cause: the underflow guard at `inlines.c:1352-1356` on the raw
    column-arithmetic slice the `noMatch:` path takes. The same path emits
    impossible positions — `[^~~x~~] tail` yields `Text scope=0:0..0:13`.

    **A third symptom, worse than either, and not in the original statement:**
    `printf 'x[&#94;a] tail\n'` emits `literal="x[^\0\0\0\0\0] tail"` — five
    bytes read past the logical end of an owned entity-decoded `strbuf` and
    **materialised into the document**. The base pointer for the slice is the
    *following node's* chunk, which may be heap-allocated, while the length comes
    from column arithmetic over the input; the two coordinate spaces have nothing
    to do with each other. ASan is silent because the over-read stays inside the
    `strbuf`'s over-allocated capacity. That upgrades D10 from data-loss to
    data-loss *and* memory-unsafety, and the minimal fix removes it by
    construction by slicing `subj->input`.

    Nothing here needs the CST or `parser->line_marks`: the label's extent is
    two buffer offsets the function already holds (`opener->position`,
    `initial_pos`), and the opening line is already on `opener->inl_text`,
    written by `make_literal` at `inlines.c:112`. See §5.7 for the *shape* Step 9b
    then gives the failure — which is a different question from keeping the
    bytes.
11. **A duplicate footnote definition nested inside another deletes a block of
    content.** Definitions register on the iterator's `EXIT` event
    (`blocks.c:578`) — post-order close order — and the map's tie-break is
    registration `age` (`map.c:189`). The inner closes first and wins; the outer
    then has no reference:

    ```
    Ref [^dup].

    [^dup]: OUTER opens first

        [^dup]: INNER closes first
    ```

    `"OUTER opens first"` is in no node.

    **Correction: the cited free site is wrong.** The unreferenced drop
    (`blocks.c:668-671`) **never executes for this input** — proved with an
    instrumented build, where the only probe that fired was the map teardown.
    `sort_map`/`index_map` (`map.c:196-219, 247-263`) dedupe the loser out of the
    emission array before the emission loop ever reads it, and it is then
    destroyed at `blocks.c:683-684` (`markdown_core_unlink_footnotes_map` →
    `markdown_core_map_free` → `footnotes.c:11-13` → `markdown_core_node_free`)
    while its `parent` is still non-NULL, i.e. while it is still in the tree.
    This matters for the fix: **guarding the `!ix` branch does nothing**, and
    retention has to happen where node ownership is handed back.

    Moving registration to `ENTER` is necessary and **not sufficient**: measured,
    it just changes the victim — `"INNER closes first"` is deleted instead. See
    §5.8 and §4.2 step 0a.2.

    **This statement is about the nested case only, and the defect is wider.**
    Two definitions of one label at the *same* level lose the second one the
    same way — `[^dup]: FIRST` / `[^dup]: SECOND` resolves to FIRST and deletes
    SECOND. Nesting is only where the *resolution* answer also surprises. Both
    halves are fixed at 0a.2; see §4.2.8.
12. `markdown_core_consolidate_text_nodes` (`iterator.c:118`) propagates
    `end_column` from the last merged node but never `end_line`, so any merged
    run crossing a line end reports the wrong end line. This is why D10's fixed
    `Text` still reads `1:1..1:7` where `1:1..2:7` is right. The one-line fix is
    **blocked by D13**. → Step 5, with the iterator contract.
13. `set_sourcepos_from_range` (`extensions/autolink.c:176-181`) early-returns on
    `len == 0` after `clear_sourcepos`, leaving split fragments at `0:0..0:0`.
    With D12 applied that zero propagates into a neighbour: `extensions.txt:804`
    goes from `Text scope=59:1..59:0` to `59:1..0:0`. (The existing `59:1..59:0`
    is itself an impossible range the ratchet already counts.) → Step 5, before
    D12.
14. The failed-footnote-call path rebuilds the prefix as a hard-coded `"[^"`
    (`blocks.c:631`) while recognition ran on the **decoded** text
    (`inlines.c:1321`), so with D10 fixed `[&#94;a]` reconstructs as `[^#94;a]`.
    The one-line closure — test the raw byte `subj->input.data[opener->position]
    == '^'` — was measured green, but it **changes which inputs are footnote
    calls** (`[\^a]`, `[&#94;a]` stop being calls). That is a policy move, not a
    repair. → Step 9a, with §5.7.
15. **The CLI and the facade attach extensions in different orders**, so the
    CLI's default language is not the facade's. `attach_option_extensions`
    (`main.c:84-89`) attaches `directive` *first*, before table; the facade
    (`extensions/ast.c:126-131`) attaches it *last*. Every binding goes through
    the facade. Combined with D8 this decides which extension is silently
    disabled, and any reasoning about precedence that uses the CLI as its model
    is wrong for the bindings. → Step 3, which fixes the order into a static
    table and must therefore **decide** it rather than inherit one of the two.
16. **The null/empty rule is violated at two more sites than D6.**
    `chunk_clone` (`inlines.c:1300`) always allocates, so a NULL-data source
    chunk becomes a non-NULL `""` on the resolved-reference path; and
    `markdown_core_parse_reference_inline` writes
    `markdown_core_chunk_literal("")` for a definition that has **no** title at
    all (`inlines.c:1755`). Together with D6 that is three sites for one rule,
    which is why the rule has to become structural rather than be fixed three
    times. It is also why D5's fix reads `title=""` rather than `title=null`
    until this lands. → Step 14.

---

## 3. The roadmap

Three stages, in order. **Do not begin a stage before the one above it is
finished**, and in particular do not collapse Stage 1 into Stage 2 — that
collapse is what forced the two-tree shadow design last time.

### Stage 0 — Reconstruct

**Stage 0a first: fix the defects on the untouched baseline.** Ten of the eleven
known live defects need no architecture at all — seventy-four lines of C across
six files, all of it built, gated and reverted before this was written. They go
first because every later step regenerates goldens, and a golden regenerated over
a live defect blesses it. The eleventh, D9, cannot be fixed before Step 9a; it is
pinned by two gates instead. §4.0 gives the verdict, §4.2 the order.

Then re-apply, onto the 1.0 baseline and by hand, exactly three things:

1. directive support (grammar conformance),
2. the formula fix,
3. CST and diagnostic support.

Nothing else. The port list is §4.1.

### Stage 1 — Make the parser pausable at line boundaries

#### The flow, stated before anything else

> **When line N+1 arrives, lines 1…N have already been processed. If the
> parser's state has been preserved completely, handling line N+1 requires no
> re-processing of those N lines whatsoever — the flow simply continues.**

That is the whole of it, and everything in this stage is measured against it.
Any design that re-walks, re-derives, re-parses or re-copies work proportional
to the document already fed is off-model, however correct its output. The
performance criterion T(document) = Σᵢ T(line i) is not an additional
requirement layered on top; **it is what "the flow continues" means when you
time it.**

Two things follow, and both are measured rather than hoped:

- **The block phase already does this.** Per-line feed cost is flat in *i* (252
  → 210 ns by decile over 20,000 lines) and line-at-a-time agrees with one-call
  on 13,566 line-boundary prefixes. Nothing about lines 1…N is touched when line
  N+1 arrives. The flow is already continuous; the state is already preserved.
- **`finish` is the only thing that breaks it**, and not because reading is
  inherently expensive. It is because the engine **defers work it could have
  done when the work first became possible.** A block's inlines can be parsed
  the moment that block closes; deferring every block's inlines to the end is
  what turns a per-block cost into a whole-tree pass. Measured: 4.48 / 12.22 /
  31.53 ms at 5k / 20k / 80k lines, dead linear.

So the refactor this stage names is not "make the close incremental". It is
**do each block's work in the line that closes it.** Then nothing is ever
re-processed, by construction rather than by optimisation, and what remains at
the end is only the open spine — O(depth), which is not a function of the
document.

**Why this stage exists, stated first because it is not a milestone for its own
sake.** Line-by-line append is the forcing function for the constitution: *the
parser must preserve every piece of state that incremental parsing requires.*
Choosing the line as the unit is what makes that demand systematic — it is small
enough that no state can hide, and it admits no partial-line special cases to
hide behind. The deliverable is line-by-line append; the *point* is a parser
whose state is complete, explicit and resumable. A Stage 1 that shipped
line-by-line append without that analysis would have missed the whole
instruction.

A major refactor, therefore, whose goal is a property of the **parser's state
machine**, not of the tree: every piece of state the parser carries is explicit
and preserved, so the data flow can be paused at a breakpoint after each line, a
snapshot taken as the current output, and then continued.

**At a line boundary there is no held partial line at all.** Stage 1 therefore
has *zero* partial-line complexity. Every problem that wrecked the previous
attempt — running a held line into a copy, un-running it, the delimiter engine
seeing a second unit, an inline scan straddling a boundary — does not exist here.
That is why the line comes first and the partial line comes second.

#### Acceptance — two criteria, both hard

**1. Correctness, measured against the external oracles.** The hard line is
agreement with the implementations that define the language — cmark-gfm for the
CommonMark and GFM surface, remark/mdast for the model — not agreement with this
repository's own goldens. A golden can be regenerated into agreement with a
defect; an external oracle cannot.

> For every partition of the input **on line boundaries**, the resulting tree
> must equal a one-shot parse of the same bytes — and that one-shot parse must
> itself still satisfy every parity gate.

**2. Performance: the cost of a document is the sum of the cost of its lines.**

> For a document of *l* lines, **T(document) = Σᵢ T(line i)**.

Equivalently, and this is the form that makes it testable: **the cost of
appending line *i* does not depend on *i*.** Feeding the ten-thousandth line
costs what feeding the first line costs, modulo that line's own length. There is
no term proportional to the document so far, to the tree so far, or to the number
of lines already fed.

This is the original instruction, restated for the line: *for any partition with
Σ chunks = L, Σ o(chunk) ≈ o(L)*.

**What this rules out, and why criterion 2 is not optional.** Criterion 1 alone
is satisfied by cloning the tree and finishing it after every line — structurally
equal on every partition, and O(l²) overall. That is not a hypothetical: it is
the shape the previous program spent months inside. Criterion 1 says the answer
is right; criterion 2 says the parser is actually incremental rather than
re-deriving the answer each time. **Neither alone is Stage 1.**

#### The gate

A per-line timing series over documents of growing size, asserting that the
per-line cost is flat in *i* — not that the total is "fast", which any constant
factor can fake. The series is the artifact: a fitted slope indistinguishable
from zero passes, and any positive slope in *i* fails and names the state being
re-derived. Total wall time against a one-shot of the same bytes is reported
alongside as a sanity check, but the slope is the gate.

#### What Stage 1 owes before it starts

**The inventory is done — it is §11**, and it changed the shape of the stage.
Measured: the line loop already satisfies both criteria at HEAD (13,566
line-boundary prefixes agree with a one-call parse, and per-line feed cost is
flat in *i*). What is missing is that **there is no way to read the tree without
ending the parse**, and the close is linear in the document — so calling it per
line is the quadratic cheat. Stage 1 is therefore a much narrower problem than
"make the parser resumable": it is *make the tree readable at a line boundary,
without ending the parse and without paying the document.* See §11.5 and §11.7.

The analysis was the first deliverable, not a preliminary: **an
inventory of every piece of parser state**, each classified as carried across a
line boundary, derived on demand, or genuinely per-line and discardable. Anything
that cannot be classified is the finding. That inventory is what tells Stage 0's
refactors what they must preserve — which is why Step 3's driver is stated as
*a paused parser is a plain struct* rather than as tidiness.

Six things the API must also settle, and they are design decisions rather than
discoveries: the public append and snapshot surface; who owns a snapshot and how
long it stays valid once more lines are fed; whether equality is required after
every prefix or only at the end; failure and OOM behaviour mid-stream; whether
the bindings participate in Stage 1 or only after it; and the allocation bound
that accompanies the time bound.

### Stage 2 — The incomplete trailing line

Only once Stage 1 is proven. The buffering is already solved at the byte level
(`parser->linebuf`); the remaining problem is *speculatively parsing* the
partial line and being able to un-parse it. That is a smaller and better-posed
problem than the one previously attempted, and it is deliberately last.

---

## 4. Stage 0 port list

Difficulty: **[CP]** cherry-picks cleanly · **[CX]** picks with conflicts ·
**[HW]** must be hand-written because the original is entangled with dropped
machinery.

### 4.0 The verdict: defects first, and the old order was wrong

> *"Should you not adjust the reconstruction plan to let us fix all the defects
> before any tasks?"*

**Yes — for ten of the eleven, and the earlier plan asserted a dependency it had
never tested.**

Every one of D1–D8, D10 and D11 has now been applied to the untouched baseline
with **no other step landed**, built, run against every gate in the repository,
and reverted. Together they are **fewer than forty lines of C across six
files** — `blocks.c`, `inlines.c`, `map.c`, `directive.c`, `formula.c`,
`table.c`. Nothing about the
architecture was in the way. What was in the way is smaller and more
uncomfortable: **four of the eleven have no gate at all**, and the plan had
scheduled them behind steps that would have regenerated goldens over them.

**D9 is the single genuine exception.** Its budget is not a safety measure that
happens to be order-dependent; it is the only thing standing between a resolved
reference and superlinear output, because resolution *copies the destination
into the node* (`chunk_clone`, `inlines.c:1299-1300`). Deleting it was measured
on a 1 MiB adversarial input:

| | dump size | wall | peak RSS |
|---|---|---|---|
| with the budget | 16,424,146 B | 0.10 s | 134,742,016 B |
| without | 68,745,508,944 B | 156.81 s | 84,684,455,936 B |

4,186× the output, 1,568× the time, 628× the memory, from one megabyte of
input. Three local alternatives were built and all fail: a per-entry constant
cap is order-independent but admits the same bomb; charging each entry once is
unbounded; interning the destination has no owner at the baseline that outlives
the refmap. **D9 is fixed by deleting the copy, which is Step 9a's model
change, and by nothing smaller.**

**D9's interim mitigation, which is not "leave it bleeding":** step 0a.8 lands
its two missing gates *while the defect is still live*, so the damage is stated,
bounded and watched. An order-independence oracle (for any reference R, the tree
under R must be identical whether or not unrelated resolved references precede
it — `[b]: /short\n\n[b]\n` versus the same two lines behind an unrelated 1 KB
definition) is registered as a **known-red** case naming Step 9a, exactly as the
mdast backlog does. An output-size bound in `complexity_runner.c` asserts that a
resolved-reference document's dump stays within a stated multiple of its input —
that one is **green today**, and it is what stops the naive deletion at Step 9a.
A 68 GB blowup is currently invisible to every gate in this repository; after
0a.8 it is not.

Five defects (D12–D16) were found *while* proving the other eleven. D12 and D13
go to Step 5, D14 to Step 9a, D15 to Step 3, D16 to Step 14 — each to the step
that was already going to touch that code, so none of them lengthens Stage 0a.

### 4.1 The requirement list

**The difficulty grading is gone, and so is the legend above §4.0.** `[CP]`/`[CX]`/`[HW]` answered *"how hard is this hunk to move"*, and under §4.9 nothing is moved. Each row below states what must be **true of the engine** when the step is done, and what must already be **true** before it starts — not what must be merged. Sizes are rough new C, net where deletion dominates; gate and binding lines are counted separately in the notes.

Seven defects that this restatement found by measurement are numbered **D18–D24** and listed in §4.1.7. Each has an owner step in the table. They belong in §2 and are recorded here only because the rows reference them.

| # | What must be TRUE when it is done | New C | Depends on — facts, not merges |
|---|---|---|---|
| ✅ **0** | The engine is byte-identical to `580d10c` except `core/main.c`, which carries `--profile`. | — | — |
| ✅ **1** | Every oracle that can judge a behaviour change exists in the tree and has been re-pinned against the **baseline** binary. | ~1,400 script | 0 |
| **0a** | The seventeen §2 defects are fixed, or pinned by a named gate with a named owner, on an engine nothing else has touched. **§4.2 stands unchanged** — it was derived on this tree, not ported. | 39 + ~180 gate | The two position oracles took their first reading on the *unfixed* tree (0a.1). |
| **2** | Every C source the build compiles is a `clang-format` fixpoint, and the config makes **braces mandatory on every `if`/`else`/`for`/`while`/`do` body**. `scripts/format-c.sh --check` is the gate. | 0 new · 1 config line · 2,393 diff lines | 0a has landed and each defect commit re-pinned its own `file:line` citations (R13). No other work is in flight in `packages/markdown-core/` (R17). |
| **3a** | The engine has **one allocator model**: `markdown_core_mem`, supplied per parser, defaulting to `calloc`. There is no process-global scratch allocator, no `core/arena.c`, and no re-parse retry in `table.c`. The Release CLI allocates and frees exactly as the library does, so `#if DEBUG` in `core/main.c` collapses to one path. | 0 new · **−140** | `extensions-conflicts.txt` exists (0a.5) and re-proves D8 after the retry path it patched is deleted (R14). `node scripts/audit-source-lists.mjs` **runs** (it throws at HEAD). |
| **3** | An extension is a `static const` descriptor in a fixed compile-time table. It is not registered, not looked up by name, and carries no mutable state. A parser records *which* extensions are on as a bitmask and **cannot express an order** — the order is the table's, and `table` is last. A descriptor declares **three** byte sets (terminates-text, dispatch, flanking-transparent), not one list. A delimiter names its **rule**, not a byte. Node types and node-flag bits are compile-time constants. There is no process-global mutable state anywhere in the extension path. | **+500 / −535** | D1, D2 fixed (0a.4) so the descriptor author transcribes a correct source. D8 fixed (0a.5). The tree is a format fixpoint (2). One allocator (3a). The source-list audit runs. |
| **3b** | `markdown_core_node_append_child` / `_prepend_child` / `_insert_before` / `_insert_after` / `_set_type` refuse any link that would make a node its own ancestor — **always**. `markdown_core_enable_safety_checks` does not exist. | ~25 | None beyond "the tree builds". See **Q13**: this may be a §2 defect, not a refactor by-product. |
| **15A** | **One** machine-readable AST contract lives in `docs/` (normative) and **one** audit checks all six projection surfaces against it — C header, C dump, Kotlin bridge + decoder + model, ES bridge + export list + decoder + model, Swift model + dumper, and the canonical-AST manifest — and it is **green**. | 0 C · ~500 JSON+script | Nothing under `docs/deprecated/` is normative *and* no executable policy file still points there. |
| **5** | The iterator's event contract is **total** (every node gets `ENTER` and `EXIT`; `S_is_leaf` is gone). Its mutation rule names *nodes*, not events: only the node whose `EXIT` is current may be freed. A subtree operation stays inside its subtree. **No zero-length `Text` node exists in a finished tree, and no node carries `0:0..0:0` as a stand-in for "no bytes".** A merged run's scope is the union of what it merged, line **and** column. One function computes a position from a byte range. | ~200 | D3 and D7 fixed (0a.6) so merged positions are merged from correct operands. D10's replacement node carries a start line (0a.2). |
| **6** | **Deliverable #2.** Attaching `formula` is the *only* gate — the two delimiter options do not exist. Five inline forms, four block forms, and one padding rule: one leading and one trailing space-or-line-ending is stripped from an inline formula's body when the body is not all whitespace. | ~60 · deletions across 18 files | 3. D1 fixed (0a.4), else one oracle row stays red and must be named as 0a.4's. |
| **7** | **Deliverable #1.** The directive grammar of micromark-extension-directive 4.0.0 and mdast-util-directive 3.1.0, applied to **code points**: name rules, one/two/three-colon forms, `#`/`.` shorthand, `class` accumulation, last-value-wins elsewhere, and **degradation** — a malformed label or attribute block leaves the directive standing and the punctuation as prose. `DirectiveLabel` is a visible node whose scope spans its brackets. A container's closing fence **closes it and every block open inside it** (D21). A directive that consumes a span containing a line ending leaves the subject's position honest (D22). Attributes are an ordered key/value sequence; the JSON round-trip is deleted. | ~530 written · **+150 net** | 3. 15A (this is the first step that changes the node inventory). 0a.6's newline-adjust mechanism is live, or Step 7 lands it (D22). |
| **10** | For any block node with a content buffer and any byte offset within it, the engine can name the **source line and column** of that byte. Every node synthesized from a content offset carries a position that is a place: the split-off table lead, its inline children, the recovered header row and cells, and any paragraph whose front was consumed (D18). The lead keeps its authored spelling. | ~110 | **Nothing.** Every mechanism exists at the baseline; both consumers run while the marks would be live. |
| **9a** | A footnote definition is a block node at the byte where its `[` was written, in the container it was written in, and it **stays there**. No pass runs after the parse that moves, reorders, drops or re-parents any node. Every definition the author wrote is in the tree. The reference map never owns a node. A reference carries **the label the author wrote**; numbering is derived, not stored. A `[…]` is a footnote call only if it opens with a **raw** `^` and the document defines that label; otherwise the brackets take the ordinary unmatched-`[` path and nothing frees children core already built. | **+90 / −290** | 0a.2's D10 fix, so a reference's label is sliced from the parser's own buffer. |
| **11a** | A parse produces, beside the tree, a **concrete record set** in which every block-level byte of the normalized source is owned by exactly one node, in exactly one of three roles (`MARKER`, `CONTENT`, `DISCARDED`). Three laws hold over every corpus: **L1** the regions on a line tile it exactly; **L2** every region lies inside its owner's scope and descendants lie inside their ancestor's `CONTENT`; **L3** concatenating the regions in order reproduces the normalized source byte for byte. The document **retains** that normalized source and its line index. A region may be *refined* — split, never moved, never deleted — which is how extensions capture without breaking L1. | ~600 + ~350 gate | 0a (an L3 gate written over the unfixed engine would encode D10/D11's loss as expected). 5 (no node without source bytes). 10 (the content-to-source marks 11a retains — **Q22**). |
| **8** | **The inline position model.** An inline node's position is a *projection* of the byte range it covers, not a counter each handler maintains: one `seek` primitive, one newline index, offsets stored on the node, and one constructor for a delimiter run. `adjust_subj_node_newlines`, `count_newlines`, `subj->column_offset`, `subj->block_offset` and the three hand-written `make_delimiter_text` copies cease to exist. Subsumes D3, D7, D12 and D19/D20/D23 by construction. | **+330 / −245** | 3 (rules exist). 6, 7 (the grammars are settled, so the extensions are rewritten once). 11a (the retained `CONTENT` records are what make the projection exact on continuation lines). |
| **9b** | One reference model for both kinds. A link reference definition is a **node** at the byte where its `[` was written. Five kinds carry an **association**: `label` as authored, `identifier` as the match key, neither derivable from the other. A reference holds **no destination** — resolution is the consumer's, and is derivable as "group by identifier, first in document order". The map holds no resource, so D9's expansion budget has nothing to charge and is deleted. The dump and the facade speak one vocabulary (`label=`, not `id=`). | **+450 / −180** C | 9a (the tree is source-ordered and the winner is derivable from it). 10 (a harvested definition needs a source position and the surviving paragraph needs rebasing). 15A. |
| **11b** | Every byte of every block's `CONTENT` region is owned by exactly one inline node or by the block itself, and inline records are expressed in **source** coordinates, not content coordinates. Delimiter runs, brackets, escapes, entities, destinations, titles and smart-punctuation substitutions are all `MARKER`; the text between is `CONTENT`. | ~500 + ~200 gate | 11a. 8 (a position is a projection of a range, so the lift has one answer, not two). |
| **11c** | A reference definition and a footnote definition own their source bytes, so the block partition is total for real documents. A definition that lost a duplicate-label contest keeps its bytes. | ~150 | 9b (a node exists to own them). 11a (refinement exists and cannot move a boundary). |
| **12** | The public surface presents **one parse under two total views** — `document.semantic` (policy applied, may omit bytes) and `document.concrete` (the normalized source, its line index, and every node's regions; omits nothing) — and states the law that binds them: every byte is in exactly one region and every region has exactly one owner, so the pair is complete. The concrete view survives being copied into value types and the handle being freed. | ~400 | 6, 7 (the surface is not renamed twice). 11b, and 11c for definition-bearing documents. 15A. |
| **13** | **Deliverable #3.** A parse produces an ordered list of diagnostics — `(severity, code, scope, message)` — and one law governs them: **a lost diagnostic is not a lost parse.** For every input and option set the semantic tree and the concrete records are byte-identical with diagnostics on and off; if the buffer cannot be allocated the parse still returns a complete document with a truncation marker. Its converse is equally normative: **a parse failure is not a diagnostic** — `markdown_core_error` means there is no document, and it carries no scope. | ~430 + ~150 gate | 7 (its 51 oracle examples are the enumeration of degradation cases). 12 (a scope is resolvable without a node handle). |
| **14** | `null` means "the source did not write this"; `""` means "the source wrote it and it was empty". The distinction is **structural**: an optional field cannot be assigned a value that does not state whether it is present, so a write site that does not say so **does not compile**. No transformation and **no read** collapses it. The facade folds nothing. | ~150 C + bindings | 9b (the optional field set is complete). 12 (the accessors are the target accessors). 15A. |
| **15C** | The 3.0 release obligations: one contract, all seventeen defects closed or carried with a registered gate, both deliverables measured against the 96 whitelisted examples with every staleness recorded, every §4.8 gate green and non-vacuous, release plumbing pointing at live paths, and `check-release-version.mjs` passing with no `--skip-*`. | ~150 + notes | 12, 13, 14. |

**15B is not a step. It is a standing rule**, and it belongs in §0 beside the other three:

> **4. A change to the node inventory, to a field's name, type, nullability or category, to an enum's members, or to the dump grammar lands its contract edit, all six projections, the manifest's coverage requirements and the regenerated `.ast` goldens in the same commit as the engine change.** No commit may leave `audit-ast-projections.mjs` red.

**Totals.** ≈ **4,560 lines of new C**, against the old port list's ≈ 7,600 — the requirement list is roughly three thousand lines smaller than the port list, and §4.1.2 says where every one of those lines went. Add ~1,400 lines of new gate script and ~2,000 lines of binding work **distributed across Steps 7, 9b, 12, 13 and 14**, not batched at the end (§4.1.4).

**`VERSION`.** ~~"`VERSION` moves to 1.0.4 at the close of Stage 0a"~~ — **settled 2026-08-21 by owner ruling: `VERSION` is `3.0.0`, taken early.** Q27's measurement is why: at `VERSION=1.0.4` the ordering assertion in `check-release-version.mjs` is **unsatisfiable**, because the tag `v2.0.0` exists and the script requires every existing tag to be strictly less than `VERSION` when `v$VERSION` is absent. The stated cost of going to 3.0.0 early — the release-notes file must exist from that commit — was paid at the bump, and the gate now fails on the legacy tags alone. Do not adopt a version whose only job is honesty and whose effect is to make a gate permanently unreachable; that was the argument against 1.0.4, and it is the argument for 3.0.0.

---

### 4.1.2 What is deleted, and why

Six things. Each existed because a commit existed, or because a constraint existed that the owner has since removed.

**1. The `[CP]`/`[CX]`/`[HW]` column, and the legend at the head of §4.** §4.9 already voided the grading; the legend outlived the table it introduced. Delete both.

**2. The struck `~~4~~` row.** Its content is Stage 0a. A struck row is a note about how the plan changed, and §4.0 already carries that note in prose. Delete the row.

**3. Step 4h — "the extension attach order".** Q9 decides the order and Step 3 makes an order **unexpressible by a caller**. A row with no size, no gate and no deliverable is a reminder, and reminders belong in the step that acts on them. Delete the row; Step 3's requirement carries it.

**4. Step 8 as a decision fork.** See §4.1.3. The number is reused for the requirement that survives the fork, deliberately, so that §4.5's instruction to *"re-run and re-read those four gates by name at Steps 3, 8 and 11"* stays true: the new Step 8 rewrites exactly the flanking-adjacent code those four gates guard.

**5. "Step 8 carries four syntax fixes" (§4.4, §4.7).** A port artifact. Under Q8 the only admissible source of a syntax requirement is `specs/oracles/`, and every syntax requirement in those six files belongs to Step 6 or Step 7. The two hardest arbitration cases in the directive oracle — `:note[See [plain] text.]` and `[Go :badge[beta]](/roadmap)` — **already produce the oracle's structure at HEAD**, measured. The delimiter machinery is not what is wrong with the directive grammar. Amend §4.4's sentence about which steps regenerate `spec.txt` accordingly.

**6. Step 15 as a single step at the end**, and **Step 12's "ABI break window"**. The window is gone by Q10 (§4.1.4). Step 15 is deleted as a trailing step and replaced by 15A (early, before the first surface-changing step), 15B (a standing rule) and 15C (release). The argument is empirical and is already sitting in the tree: `audit-ast-projections.mjs` reports **16 Swift-only failures and zero Kotlin or ES failures** — that is not, as §0 records, "a kind/field table the baseline engine does not have"; it is **one binding a full era behind the other two**, and the gate that says so is parked with the owner "Step 15". *Deferring the bindings is how the drift happened.* There is no longer a batching argument to weigh against that: the surface breaks at Step 7 (attribute type, `DirectiveLabel` as a 29th kind), at 9b (three kinds, `label`+`identifier`, `id=`→`label=`), at 12 and at 13 regardless, and §4.4's own duplicate-golden argument applies verbatim one level up — a batch at the end regenerates six `.ast` files, four `TreeDumper`s and the coverage manifest for the second time.

**Considered for deletion and surviving conditionally: Step 2.** Measured at HEAD: `sh scripts/format-c.sh --check` **exits 0**, and `.clang-format` contains no `InsertBraces` line. The tree is already a fixpoint of the current config, so the step as written — *"run `clang-format`"* — is a **no-op**, and the 1,296 lines §4.3 attributes to it are an observation about a historical commit, not a requirement. Its only possible content is adopting one invariant: **braces on every conditional body**. That invariant is worth having here for one reason that survives the closed history — Stage 0a and Steps 3–14 consist very largely of adding a statement to, or removing one from, an existing conditional body (§2's own defect list: *"adds one line inside the successful rewind"*, *"plus four lines in `blocks.c:625`"*, *"an 8-line sweep before `blocks.c:675`"*), and in a braceless body "add one line" and "change the control flow" are the same edit and look identical in review. **If the owner declines the rule (Q11), Step 2 is deleted outright, because there is nothing else in it.**

**Nothing else is deleted.** Every remaining row is a live requirement measured on this tree; none of them exists because a commit exists.

---

### 4.1.3 Step 8 answered: the inline phase needs a position model, not a delimiter engine

**The verdict: no. A unified delimiter engine as a distinct ~1,100-line component is not warranted, and the fork is not a fork.** The step splits cleanly in two, and the two halves belong in different places.

**Half one is a *declaration* problem, and it belongs in Step 3's descriptors.** One `llist` — `special_inline_chars` — is read by five consumers with five different meanings: `markdown_core_manage_extensions_special_characters` folds it into two byte tables; `try_extensions` uses it for cursor dispatch; `get_extension_for_special_char` uses it for **delimiter-tag ownership**; `find_extension_opener_for_special_char` and `bracket_takes_close_bracket` use it for `]` arbitration; `handle_backslash` uses it to disable a core optimisation. That is D1's and D2's root, and 0a.4 fixes the symptom by deleting three calls while the shape that produced them survives. Four facts make the split non-optional, all verified at HEAD:

- `core/inlines.c:780` calls `extension->insert_inline_from_delim(...)` with **no NULL check**, on an owner derived from a *byte* by first-registration order. `autolink` registers bytes and supplies no such hook. It is one `push_delimiter` call away from a NULL dispatch.
- `openers_bottom` is declared `bufsize_t openers_bottom[3][128]` and indexed `openers_bottom[closer->length % 3][closer->delim_char]` with `delim_char` an `unsigned char` that the **public** `markdown_core_inline_parser_push_delimiter` accepts unconstrained. `openers_bottom[2][200]` is offset 456 into a 384-element array. Dense rule ids size the array correctly by construction.
- The sentinel delimiter tags (`FORMULA_DELIM_*` = 1..4, `DIRECTIVE_LABEL_DELIM` = 8) are ordinary file bytes. Only NUL is replaced by the feed. §2 already says *"they can appear in user text"*; deleting them from `skip_chars` at 0a.4 stops the flanking corruption and leaves them in `special_chars`, where a literal `0x01` still splits text runs and still dispatches. **Only removing the concept closes it.**
- Owner lookup is an O(extensions × bytes) linked-list walk executed once per closer in `process_emphasis`. A rule pointer on the delimiter makes it one load.

**Half two is not about delimiters at all. It is a position model**, and that is what the new Step 8 is. Measured at HEAD, none of these are in §2's sixteen: `a~~` under `--profile gfm` yields `Text scope=1:1..1:0` on a three-byte input; the unmatched-backtick literal is placed one column right; a link with a multi-line label or title takes its start line from the **closing** bracket and contains a child that starts before it; and an extension that consumes a span containing a line ending cannot report it, which displaces every later node in the paragraph — that one **blocks Step 7's own oracle** (`:note[label]{title="one\ntwo"} tail` must be `Directive 1:1..2:5`; HEAD says `1:1..1:29`, a column that does not exist on line 1). Baseline reading over the three fixture files at `--profile gfm-extended`: **78 of 1,928 inline nodes carry a position that is not a place.** 0a.1(a)'s planned oracle reads 13, over inline `Code` and `html_inline` only. The class is six times larger than the oracle scheduled to watch it.

**What the fork would have cost, and bought.** A `core/delimiters.{c,h}` hosting the rule table, the stack and the matcher is ~450–600 lines, not 1,100 — the 1,100 measured a hunk, and hunks are no longer moved. The real cost is not lines: the delimiter stack lives on the `subject`, and so do `pos`, `input`, `line`, `refmap`, `last_bracket` and the flanking tables. Moving the stack out means exposing `subject` to a new module or duplicating its state — **"a second unit" is the precise shape of the failure §1 records six times.** What it buys is separate testability and a place to hide the public `delimiter` struct, and both are available later, for free, once a rule table exists.

**One thing is decided here and must be stated as an invariant, because it is the property that would have been traded away for configurability nobody asked for:** the engine keeps **one delimiter stack, one matcher, first-closer-wins**. Every interleaving the three extensions admit was tested — `$a *b* c$`, `*a $b* c$`, `$a *b$ c*`, `~~a $b~~ c$`, `$a [b$ c](/u)`, `*a [b* c](/u) d*`, `a~~b~~c~~d~~e`, `$a$b$c$`, `$$a$$b$$` — and every result is well-formed and non-crossing; all three extensions already re-check their own opener/closer compatibility inside `insert_inline_from_delim`. A single stack scanned once is what buys non-crossing output for free, and it is exactly the property the previous streaming program destroyed when it introduced a second unit. **A later step may not add a second stack.**

**Consequence for the CST.** 11b's stated dependency on Step 8 was the "one-funnel" property — one path through which every inline node is born. That funnel **already exists**: `make_literal` and `make_simple` are the two makers and every extension goes through `markdown_core_inline_parser_get_column`. What does not exist is a correct extent for emphasis, which `S_insert_emph` builds *after the fact* by re-parenting, and which is one function holding the two numbers it needs in local variables. So 11b's dependency on 8 survives, but for a different and smaller reason — **a position must be a projection of a range, or the lift has two answers** — and R2's experiment is still worth running with its subject changed to that question.

---

### 4.1.4 The dependency graph, and the check

The previous table carried a 9b↔11 cycle through several revisions because the arrows were read one row at a time. They are now stated once, machine-checkable, and checked.

```
0  →  1  →  0a  →  2   ─┐
                  →  3a ─┴→ 3 ─┬→ 6 ─┐
                  →  3b         ├→ 7 ─┼→ 8 → 11b ─┐
                  →  5  ─┐      └──────┘   ↑      ├→ 12 → 13 ─┐
                  →  10 ─┼→ 11a ─────────┘  │      │        →  15C
                  →  9a ─┼→ 9b ──→ 11c ─────┘      └→ 14 ────┘
     1 → 15A ─────┴──────┘
```

Edge list (`step: [what must already be true]`):

```
0:[]            1:[0]           0a:[1]          2:[0a]        3a:[0a]
3:[0a,2,3a]     3b:[0a]         15A:[1]         5:[0a]        6:[3]
7:[0a,3,15A]    10:[0a]         9a:[0a]         11a:[0a,5,10] 8:[0a,3,6,7,11a]
9b:[9a,10,15A]  11b:[11a,8]     11c:[9b,11a]    12:[6,7,11b,11c,15A]
13:[7,12]       14:[0a,9b,12,15A]  15C:[12,13,14]
```

**The check is executable, and it reads the edge list above rather than a copy
of it:** `node scripts/check-plan-graph.mjs`. It resolves every named dependency
to a real step and runs a white/grey/black depth-first walk that reports the
grey-on-grey *path* if one exists, rather than merely announcing that a cycle
does. Run on the list above: **22 steps, 45 edges, acyclic** — 42 when this was written; §4.13 added three arrows and §0's copy of the number went stale before anyone re-ran it, which is the argument for the check reading the list rather than a copy. It is in §0's gate
list, so an arrow that moves without the graph being re-checked fails. The critical chain is `0 → 1 → 0a → 3a → 3 → 7 → 8 → 11b → 12 → 13 → 15C`, depth 10, and it runs through Step 8 — which is a second reason not to let Step 8 be a 1,100-line fork.

A valid linear order, verified against the edge list rather than asserted:

```
0  1  0a  2  3a  3  3b  15A  5  6  7  10  9a  11a  8  9b  11b  11c  12  13  14  15C
```

**Four arrows changed, and each is a claim that can be falsified:**

| Was | Now | Why |
|---|---|---|
| `9b → 11a` | **struck** | No claim in 9b needs a concrete record. Every byte it stores is available at parse time; the record is 11c's job, and 11c depends on 9b. This is the arrow that made the graph look cyclic. |
| `10 → 9b, 11a` | `10 → nothing` | Every mechanism Step 10 needs exists at the baseline, and both its consumers run while the marks are live. 10 is a *prerequisite* of 9b, not a consequence: a harvested definition node has no position without it, and the paragraph it came from cannot be rebased. |
| `11b → 8` | kept, re-argued | Not the funnel (which already exists) — the projection. See §4.1.3. |
| `3 → 2` | kept, plus `3 → 3a` | The arena removal deletes the code path holding D8's fix, so it goes first and discharges R14 there, and Step 3's table work then runs against one code path instead of two. |

---

### 4.1.5 What changes because the target is 3.0

Q10 removes a constraint the plan was shaped around. Four things move and three risks shrink or die.

**The ABI window is gone as a *goal*, and survives as a *method*.** R4 read "six independent ABI breaks, unbatched" as a risk to be mitigated by batching them into one release. There is no release to batch into. What survives of R4 is one afternoon's discipline — *write the target public header first, as one diff against the baseline's 232 lines* — which is now good practice rather than a gate. Step 12 is retitled **"The two views"** and loses its second half.

**R16 disappears entirely.** It said "Stage 0a moves parse output before Step 12's ABI window", and it exists only if 1.0.4 is a release. It is not. What is left of it is not a risk but a question — whether `VERSION` should move at all, given that the release-version gate is unsatisfiable at 1.0.4 — and that is **Q27**.

**R11 stops being a risk and becomes a build-time assert.** Option-struct layout across three bindings was a hazard because a break was expensive. With the surface free, the bridge asserts are the mechanism, and they fail loudly at build time in the same commit that changes the struct (15B).

**The option surface shrinks, and every deletion is an application of one rule — attachment is the only gate.** `MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS` and `_LATEX_FORMULA_DELIMITERS` (Step 6), `MARKDOWN_CORE_OPT_DIRECTIVE` (Step 7), `markdown_core_enable_safety_checks` (3b), `markdown_core_register_plugin` and the whole runtime-registration surface (3), `markdown_core_get_arena_mem_allocator` and the arena entry points (3a), `markdown_core_parser_feed_reentrant` (11a, **Q28**), and `markdown_core_error_get_scope` (12 — `has_scope` is never set to `true` anywhere, so the function is unconditionally dead). The public `delimiter` struct, annotated *"Exposed raw for now"* since 1.0, is hidden behind accessors (3).

**The facade changes deliberately, in five named places**, each in the step that owns it: `markdown_core_document_root` → `_semantic` plus `_concrete` (12); `markdown_core_node_footnote_id` deleted in favour of `label` + `identifier` on five kinds (9b); the directive label-hiding accessors deleted and `DirectiveLabel` made visible (7); directive attributes retyped from a JSON `String?` to a key/value sequence (7); optional strings given an explicit presence bit instead of a NULL sentinel (14). The dump renames `id=` → `label=` (Q5, 9b).

**The bindings follow per commit, not in one batch.** ~2,000 lines distributed across Steps 7, 9b, 12, 13 and 14 — four times the old estimate of ~500, because "the bindings" is six lockstep surfaces and not three model directories. That figure is itself the argument against batching: 2,000 lines of mechanical cross-language edit in one commit is unreviewable.

**The release gates are off the critical path until 3.0, and there are seven of them, not three.** Two are era skew from Step 0's wholesale `scripts/` restore (`audit:ci` wants 40-hex Action SHA pins that `.github/` predates; `audit:source-lists` **throws** on a missing `packages/swift-markdown-core/Package.release.swift`), one is two minutes of formatting on restored files (`format:es:check`, three real files), one is a second unexpected legacy tag (`pre-format-baseline`, which §0 does not name beside `codex-doc-pass-backup`), one is the ordering assertion of Q27, one is the release-notes path — hard-coded to `docs/deprecated/releases/$(cat VERSION).md` in five places, so publishing 3.0 would publish from the archive — and one is `audit:ast-projections`, which **is not era skew at all** but the live Swift drift of §4.1.2. That closes §0's fifth known-red row.

---

### 4.1.6 What the design now owes — ledger entries Q11 onward

**Status, for all of these: PROPOSED.** They were produced by restating the port
list as requirements, which exposed decisions the borrowed code had been making
silently. Each carries a recommendation, and a recommendation is not a decision.
They are listed in §9 with their statuses, and the four that are genuinely the
owner's — **Q14, Q24, Q25, Q26** — are called out there. The rest are
engineering calls that stand unless contradicted, and they become SETTLED when
the step that consumes them lands with the recommendation carried out.

Restating a port as a requirement exposes the decisions the port had already made for us. Each is recorded here so it is tracked rather than rediscovered mid-step. They belong in §9's table; recommendations are the restatement's, not the owner's.

| id | Question | Forced by | Recommendation |
|---|---|---|---|
| **Q11** | Does the repository adopt `InsertBraces: true`? | 2 | **Yes** — it is the whole content of Step 2 (measured: `format-c.sh --check` is already green). Footprint 2,393 diff lines across 36 files, 561 of them in `core/` + `extensions/`. **If no, delete Step 2.** Land the neutrality gate with it: normalized-disassembly equality, measured 29/29 objects identical. |
| **Q12** | Is the arena deleted, or made parser-owned? | 3a | **Delete.** Measured: ~7% CLI-only parse win, **+10–16% peak RSS**, `abort()` on allocation failure inside a library with a careful sticky-OOM discipline, total sanitizer blindness on the binary the parity oracles drive, and a demonstrated **480-byte leak in a parser that never asked for it** (a global `A != NULL` makes an unrelated default-allocator parse take `table.c`'s retry branch). Parser-owned is impossible without a document-owned lifetime model this engine does not have. Output-neutral: 7,251 comparisons, 0 differences. |
| **Q13** | Is the cycle check unconditional — and is it a defect or a refactor by-product? | 3b | **Unconditional**, and *"the shipped library makes `b->parent == b` on request while the test that denies it flips a flag nothing else flips"* reads exactly like D1–D16. Measured cost: unmeasurable on four workloads, 10.7% on one already-pathological path the engine takes 36 seconds to parse. **The owner may re-file it into Stage 0a; that is the only change to 0a this restatement would ask for besides Q25.** |
| **Q14** | One knob per extension, or two? | 3, 6, 7 | **One.** Attachment is the language. Delete `MARKDOWN_CORE_OPT_DIRECTIVE` and both formula delimiter options; keep formula's `dollar`/`latex` **sub-grammar** selection only if a use is stated, and today none is. |
| **Q15** | What is the **inline** dispatch precedence? | 3 | Q9 settles the *block* order (`table` last) and says nothing about inlines — `table` has no inline hooks at all. `autolink` and `directive` both claim `':'`, and first-non-NULL wins today. **Recommend: table order is also inline order, `autolink` before `directive`** (a bare `:` far more often begins a URL), stated in the commit and pinned by a fixture. **MEASURED AT 0a.11 AND THE COLLISION HAS NO WITNESS**: moving `directive` to first or to the middle changes 0 of 12 hand-built candidates and 0 of 4,000 random `:`/URL/attribute documents (§4.2.17). The recommendation stands as a tie-break; it must not be shipped as a fix, and **a fixture cannot pin it until an input exists that distinguishes the two** — finding one, or recording that none does, is Step 3's. |
| **Q16** | Are extension node types and node-flag bits re-assigned as fixed constants? | 3 | **Yes**, in a fixed enum decoupled from the table order. They are internal — the shipped export map is 32 read-only facade symbols — and conflating "attach order" with "type numbering" is precisely what makes today's globals order-dependent. |
| **Q17** | Is an inline node's position a projection of a stored byte range? | 8 | **Yes**, and store the pair — two `bufsize_t` on the inline node. This is what makes D12 *unexpressible* rather than fixed, and it is the concession that makes 11b cheap. |
| **Q18** | Which inline-math padding rule? | 6 | **micromark-extension-math's**: strip one leading and one trailing space-or-line-ending, interior untouched. Not CommonMark's code-span rule, which also converts interior line endings. No oracle example separates them; three independent reasons do (the oracle's own prose cites micromark; the mdast gate compares `Formula.literal` against remark on every corpus input; a formula body is handed to KaTeX). **And it applies to the `\(…\)` / `\[…\]` forms too** — no oracle row covers that; pin it with two new ones. |
| **Q19** | Are directive attributes sorted in the model, or only in the dump? | 7 | **Sorted in the model.** After class-accumulation and last-value-wins the list *is* a map; source order is meaningful only inside `class`'s accumulated value, which is already a string. Two orders is how a third order appears in a binding. |
| **Q20** | Are character references decoded in directive attribute values? | 7 | **Decode** (one call to the existing `houdini_unescape_html_f`), pin `:n{a=&amp;}` → `a="&"`. If declined, it must be a *registered* divergence in `deltas.json`, not silence. |
| **Q21** | Does a reference definition box itself, or only its resource? | 9b | **Only its resource.** Measured on this machine: `chunk` 16, `association` 32, `definition` 64, `reference` 40, widest existing union arm (`markdown_core_code`) **40**. `{association; resource *}` is 32+8 = **40** — the union does not grow, the association stays inline and uniformly readable for all five kinds, and the label can never be lost to a failed box allocation. |
| **Q22** | Does the content-to-source map have **one** owner? | 8, 10, 11a | **Yes, and this is the sharpest thing the restatement found.** Three steps independently proposed a mechanism for one fact: Step 10's per-line parse-time marks, Step 8's newline index, and 11a's `CONTENT` regions. **Recommend: 10 produces it, 11a retains it, 8 projects through it, 11b tiles it.** Three implementations of one fact is the disease this plan names in five other places. |
| **Q23** | Does the document retain the normalized source? | 11a | **Yes** — one append-once buffer, 1× the input, plus 4 bytes per line of index. §6's verdict ("nothing replaces the substrate") is true of the *rope* and silently assumed the bytes survive; they do not (`parser->curline` is cleared per line, `linebuf` freed at finish, `source` borrowed). The alternative is re-implementing the normalizer — including `markdown_core_utf8proc_check`'s replacement policy — byte-identically in Swift, Kotlin and JS. **This is a §6 amendment, not just a Step 11a decision.** |
| **Q24** | Is the concrete view opt-in? | 12 | **A parse option defaulting to `true`.** Cost is ~2.5–3× input resident. The gate that makes it safe: the semantic dump must be **byte-identical** with the option on and off, over every corpus. An option that changes the parse is a second engine. |
| **Q25** | Do D16's two site fixes move into 0a.7? | 14 | **Owner call, because Stage 0a is otherwise closed.** Measured: 58 golden rows carry `title=""`; 18 are D6's; the remaining **~40 are D16's** `chunk_clone` path, and under the current schedule they are regenerated by nine steps with the reviewer's only available answer being "unchanged, therefore fine". Moving them is ~6 lines and resolves D5's stated tension in the commit that already has the defect statement in hand. If it does not move, Step 14 moves 40 rows; if it does, Step 14 moves **zero**, which is the right shape for a step whose deliverable is an invariant. |
| **Q26** | Do `Link.destination`, `Image.source`, `ReferenceDefinition.destination` stay optional? | 14 | **No — required.** Q7 already rules a definition's destination required; §5.1 rules that a reference carries none. Once 9b splits `LinkReference` out of `Link`, an inline link's destination has no reachable null except allocation loss, which Q7 answers with the failure bit. |
| **Q27** | Does `VERSION` move to 1.0.4 at all? | 15C | **SETTLED 2026-08-21: no — it went to `3.0.0` early**, the second of the two options this row offered. Measured: the ordering assertion in `check-release-version.mjs` passes at `3.0.0` and **fails at `1.0.4`**, because `v2.0.0` exists and the script requires every tag to be strictly less than `VERSION`. `3.0.0-dev` fails the `stableSemver` assert. The stated cost — a release-notes file from that commit — was paid; see §4.10. |
| **Q28** | Is `markdown_core_parser_feed_reentrant` deleted? | 11a | **Yes.** Zero in-tree callers, and it re-enters line processing with bytes that are in no source line — unrepresentable under L1. Keeping an entry point whose only purpose is to inject bytes no position can name, in the step that establishes that every byte has a position, is carrying a contradiction forward for no consumer. |
| **Q29** | Does `mode` survive on `Code`, `CodeBlock`, `Directive`, `DirectiveBlock`? | 15A | **No** — delete it from those four, keep it on `Formula`/`FormulaBlock` where it is genuinely variable. Both decoders prove the point: Kotlin and ES hard-code the constant and one of them then *asserts* the constant it just synthesized, and the Kotlin wire format does not transmit it. A field whose value is implied by its type is ceremony four surfaces must keep in step. |
| **Q30** | Do the bindings spell child edges typed (`content`, `items`, `label`, `header`, `rows`, `cells`) or flat (`children`)? | 15A | **Typed.** Kotlin and ES already do; Swift's flat `children` is what forces `labelCount: Int?`, forces `Table.init` to filter rows by `isHeader` and `preconditionFailure` if the count is not one, and forces `children: [any Markup] = []` onto eleven leaf kinds. Two of three bindings and the contract already assume it. |
| **Q38** | Does the empty `Text` node D13 removes become a registered divergence from cmark-gfm? | 0a.14 | **OPEN.** Upstream emits the node too, so removing it costs one normalizer projection, one `NORMALIZED_DELTAS` name and one `deltas.json` entry. Measured at §4.2.3. Owed by the commit that lands D13. |
| **Q39** | `[foo]: <>` resolves to `destination=null`, not `destination=""`. Is that right, when the destination WAS written and was empty? | 0a.7 | **TAKEN 2026-08-21, at 0a.7: yes, on consistency grounds, and the limit is stated.** `markdown_core_clean_url` folds a zero-length destination to `CHUNK_EMPTY` before it ever reaches the map — the same fold `clean_title` does — so `<>` is indistinguishable from *no destination* by the time the reference path sees it, and the inline path already answers `[a](<>)` with `destination=null`. Making `chunk_clone` preserve absence made the two paths agree. **This is consistency, not correctness:** a rule that truly separates "written and empty" from "not written" requires the folds to stop, which is Step 14's structural job, and this row is the one input in the corpus that will move again there. It is one row, `spec.txt` example 169. |

---

### 4.1.7 Seven defects the restatement found — §2 additions, D18–D24

Recorded here because §2's own rule is that a defect the plan does not name is a defect the plan re-derives later at full price. D17 is taken (the version macro, fixed at 0a.0). None of these changes Stage 0a; each goes to the step that was already going to touch that code.

| # | Defect | Severity | Witness | Owner |
|---|---|---|---|---|
| **D18** | A paragraph whose leading reference definitions were consumed keeps the **definition's** start position, so every inline child reports the definition's line. | wrong-position | `[a]: /1\ntext here` → `Text scope=1:1..1:9`; truth `2:1..2:9`. Upstream has it identically (`cmark-gfm --sourcepos` agrees), so upstream cannot be the oracle. Invisible to every gate: containment holds, not a sentinel, not negative, not line-zero. | **10** |
| **D19** | `handle_close_bracket` takes a link's `start_line` from the **closing** bracket and never adjusts for newlines, so a link with a multi-line label or title has a wrong line *and* contains a child that starts before it. | wrong-position | `[a](/u "t⏎t2") tail` → `Link 1:1..1:14`, `Text 1:15..1:19`; truth `1:1..2:6`, `2:7..2:11`. `core/inlines.c:1411`. | **8** |
| **D20** | `strikethrough`'s `match` sets `start_column` and never `end_column`, so the calloc'd `0` survives consolidation whenever the run ends the paragraph. | wrong-position | `a~~` under `--profile gfm` → `Text scope=1:1..1:0`. Three bytes, the default GFM profile, and every parity oracle blind because none compares positions. | **8** |
| **D21** | **A container directive's closing fence does not close it.** `directive_block_matches` marks `closed` and consumes the fence but returns 1, so the container and every block open inside it stay open; the next non-blank line is taken as a lazy paragraph continuation, pulled into the container, and recorded on the wrong line. | **content-attribution loss** | `:::note⏎body⏎:::⏎after` → one `Paragraph 2:1..4:5` whose third child is `Text scope=3:1..3:5 literal="after"`. Inside a block quote it moves `after` into the quote. A blank line after the fence hides it. The formula block is unaffected (it is a leaf with no open children). | **7** |
| **D22** | An extension that consumes an inline span containing a line ending cannot report it: `markdown_core_inline_parser_set_offset` does not advance the subject's line counter, so **every later node in the paragraph is displaced**. | wrong-position | The oracle case `:note[label]{title="one⏎two"} tail` requires `Directive 1:1..2:5`; HEAD says `1:1..1:29` and `Text 1:30..1:34` — columns that do not exist on line 1. **Blocks Step 7 outright.** | **7** lands the primitive; **8** owns the model |
| **D23** | `S_insert_emph` gives an emphasis node the start column of the **whole** delimiter run: it shortens `opener_inl->as.literal.len` from the end (`inlines.c:843`) and then assigns `emph->start_column = opener_inl->start_column` (`inlines.c:875`), while `handle_delim` had spanned the entire run. | wrong-position + overlap | On `***a**` the leftover `Text` and the `Strong` both claim the run's first byte — two nodes, one byte. Correct value: `opener_inl->start_column + opener_num_chars`. **11a's L1 gate detects it mechanically.** | **8**, gated by **11b** |
| **D24** | `tasklist` decides `checked` by searching the **whole line**: `strstr((char *)input, "[x]") \|\| strstr((char *)input, "[X]")` (`extensions/tasklist.c:88`), while `scan_tasklist` matched only at `parser->first_nonspace`. | wrong-output | `- [ ] see [x] below` reports `checked=true`. May be the same thing as the pending upstream delta `tasklist-checked-marker` — check before re-deriving. | **3** (the descriptor rewrite touches it) |

Two further findings that are not new defects but change what an existing item means. **The content→source column map is wrong whenever a continuation line's stripped prefix differs from the block's first line** — `make_literal` uses a per-node constant `block_offset`, so `"> foo\n*bar*\n"` reports the emphasis at column 3 (truth 1) and `"> foo\n>bar *baz*\n"` at column 7 (truth 6). That is the **general case of which D7 is one instance**, and Q22's single map is what closes it. And **there are three producers of zero-length `Text` nodes, not one**: D13 names `autolink`'s `postprocess_text`, but `markdown_core_node_unput` (core, `inlines.c:1925`) empties a literal and leaves the node spliced in, and consolidation merges runs of empties into an empty. Corpus footprint, measured: `Text scope=0:0..0:0 literal=""` on **36 golden rows**, and **16 rows carry a negative range**, 12 of them ending at column 0 — including `tests/fixtures/extensions.txt:804`, which pins `Text scope=59:1..59:0` as *expected*. Step 5 owns all three.

---

### 4.1.8 Where the whitelisted oracles are stale

`specs/oracles/README.md` states the rule: *where an example's expected output disagrees with what this engine should produce, this engine is right and the example is stale — say so in the commit.* These are the places, collected once so each is a deliberate divergence rather than a surprise mid-step. Measured by running all 96 examples through the HEAD engine: **43 of 43 formula examples and 2 of 53 directive examples already reproduce exactly**; of the 49 directive examples that move, 18 are spelling-only, 29 are grammar or position, and 2 are D1's.

| Where | What it says | What this engine will do | Why |
|---|---|---|---|
| `extensions-directive-option-gates.txt` — prose | *"These examples attach the directive extension without enabling its parser option."* | Keep both inputs and both expected blocks; **rewrite the prose**. | **Vacuous as wired**: the ctest entry passes no `--option`, the examples carry no tags, and `spec_runner` starts from `ts_ast_options_none()` — so the extension is **not attached at all**. The oracle cannot distinguish "attached, option off" from "not attached", which is exactly the gap D1 lived in for eleven releases. Under Q14 there is only one knob; do not build a second to satisfy a comment. |
| `extensions-directive.txt` — prose above `:shortcut{#identifier}` | *"HTML-style `#id` and `.class` shortcuts are outside this extension's generic key-value grammar and remain ordinary Markdown text."* | Implement the shorthand; **delete the sentence**. | It contradicts its own expected output, which shows both recognized. The expected block is authoritative; the sentence is a leftover from the fixture the oracle was extracted from. |
| `extensions-directive.txt` — prose above `:ordinary[label]{…}` | *"`id` and `class` are ordinary keys. Like every repeated key, their last value wins while their first source position is retained."* | Implement `class` accumulation and last-value-wins for everything else; **delete both clauses**. | Stale on both halves. The expected output shows `class="red green blue"` — `class` is the one key that does *not* take the last value — and "first source position is retained" describes a per-attribute position that no dump field and no proposed accessor exposes. Do not build an API to justify a sentence. |
| `extensions-formula-github.txt` — `foo$_bar_` · `extensions-directive.txt` — `foo:_bar_`, `a}_b_` | (expected output correct) | Green before Steps 6 and 7 start. | **Not stale — misattributed.** These are **D1's and D2's** rows, closed at 0a.4 by deleting three lines. Steps 6 and 7 must not claim them; if either lands before 0a.4, the row is listed as known-red naming 0a.4. |
| `extensions-formula-github.txt` — prose | frames the dollar and fenced forms as *"a surface recognized by the `formula` extension"* | Add one sentence: **attachment is the only gate**. | Cosmetic, but under Q14 the two delimiter options cease to exist and the prose currently implies otherwise. |
| `extensions-formula-option-gates.txt` — title and framing | written against two option knobs | Retitle to *"the formula extension is not attached"*; the five expected blocks stand **unchanged** (measured byte-identical with no options). | Same one-knob correction. Note that `extensions-formula-conflicts.txt` is *not* affected and `extensions-formula-github.txt`'s attached-but-inert case is genuinely distinct — do not collapse them. |
| All six files — positions | positions reflect fixes scheduled separately | Derive positions from this engine at each step; the oracle's positions are a **cross-check**, not a golden. | The README says so, and two specific classes prove it: the directive's `Directive 1:1..2:5` needs **D22**, and `DirectiveLabel scope` spanning brackets inclusive needs the label node to be visible — the baseline's hidden label spans the content only and its empty form is a *negative* range. |
| `extensions-directive.txt` — the 18 spelling-only rows (`attributes=[…]`, `DirectiveLabel`) | new dump vocabulary | **Adopt them verbatim.** | **Not stale.** They are the surface change, and `scripts/lib/mdast-oracle.mjs` already sorts remark's attributes and compares the rendered bracket form — the gate was written against that exact spelling before this branch existed. |
| `extensions-directive.txt` — the `:a-[]` / `:-a[]` / `:_a[]` prose | records that an earlier version of the example was wrong and how it was found | **Keep verbatim.** | It is the only place where the leading-`-`/`_` rule's provenance is written down, and the rule reverses baseline behaviour (which produces directives named `-a` and `_a`). |

**One oracle-adjacent gate must move with Step 6 and is easy to miss:** `scripts/check-mdast-parity.mjs`'s self-test canary currently asserts `literal=" mid "` — the *unpadded* answer — with a comment naming Step 6 as the flip. An oracle whose canary asserts the defect is an oracle that has been told to expect it. Step 6 flips the assertion, moves `github-backtick-math-padding` and `inline-display-math-across-lines` from `pendingExpectedDivergences` back into `expectedDivergences`, and **deletes** the two `baselineBacklog` entries that close by leaving the mdast corpus — the gate fails loudly on a backlog entry that stops diverging.

---

Scratch artifacts (outside the repository): `/private/tmp/claude-501/-Users-donz-Repos-GitHub-markdown-core/19b6648c-2779-4f7c-bddf-acfaf7c2be6b/scratchpad/dag.mjs` and `dag2.mjs` — the acyclicity check and the linear-order verifier of §4.1.4, runnable with `node`. Repository unmodified; `git status` clean.

# Replacement for §4.2, and the edits the ruling forces

*Drop-in prose. §4.2 replaces `docs/RECONSTRUCTION.md` lines 1046–1193 in full; the consequent edits follow, each keyed to its section. The edge list has been run through `scripts/check-plan-graph.mjs` and the linear order through a verifier — both results are stated below.*

---

### 4.2 Stage 0a — the defect stage

**Owner ruling, 2026-08-20 (Q25):** *"Fix all defects before start any tasks."*

The ruling was executed, not paraphrased. The fourteen defects §2 had assigned to Steps 3, 5, 7, 8, 9a, 10 and 14 — D12, D13, D14, D15, D16, D18, D19, D20, D21, D22, D23, D24, D25 — were each put to the test that settled the first ten: **applied to the untouched baseline with no other step landed, built, run against every gate in the repository, and reverted.**

**Fourteen tested. Fourteen fixable. Zero produced an architectural dependency.** D9 remains the only exception in the plan, and its exemption is still the measured one: its budget is the only thing between a resolved reference and 68.7 GB of output from 1 MiB of input, because resolving a reference copies the destination into the node; it is fixed by deleting the copy, which is Step 9a's model change, and by nothing smaller. It is pinned, not fixed, at 0a.8.

Two of the fourteen *looked* like dependencies in §2 and were not. **D12 "blocked by D13"** is a sequencing constraint between two defects that are now both inside this stage — not a step dependency, and the two land in one commit. **D22 "7 lands the primitive, 8 owns the model"** was an ownership label, not a blocker: the primitive is twenty lines in `core/inlines.c` and needs nothing Step 7 provides. Two more — **D14 "that is a policy move, not a repair"** and **D16 "the rule has to become structural"** — were arguments about *desirability*, and the ruling is precisely a decision about desirability. Both are now measured to be repairs: see the verdict rows.

#### 4.2.1 The fourteen, tested

| # | Verdict | Evidence, measured on the untouched baseline | Lands |
|---|---|---|---|
| **D12** | **FIXABLE-AT-BASELINE**, in the same commit as D13 and no other | The one-line fix *alone* turns `extensions.txt:804`/`:809` from `59:1..59:0` into `59:1..0:0` — a strictly worse row that **every gate in the repository passed** at the time this was written: 65/65, 795/795, 46/46, canonical green, ledger 207 unchanged, because `endLine < startLine` keeps it in the same `negative` bucket it left. **That clause expired at 0a.1**: `audit-position-places.mjs` reads a live parse and reports the three rows moving to line zero, re-measured at 0a.2 (§4.2.8). With D13 and D10 landed it has **no witness at all**: 4 hits over the 860-example corpus, every one through an operand with no position; 0 hits over 40,000 random inputs filtered to merges where both operands are positioned. It is a real defect (the assignment is plainly missing) that is unobservable on this engine, and it must not be sold as fixing anything measurable | **0a.14** |
| **D13** | **FIXABLE-AT-BASELINE**, by removing the node, not by respelling the position | Option A (§2's wording — give the empty fragment an honest empty range) was built in two cuts and **rejected on measurement**: every sentinel row it removes returns as a negative row, because a closed `(line, column)` interval cannot express an empty range; `extensions.txt` negative goes 10 → 36 (narrow) or 38 (wide) and `specs/scope-sanity/ledger.json` forbids growth in either class, so A cannot land without raising the ratchet, which defeats the ratchet. A-narrow also does not clear its own class — producer (2) still emits `0:0..0:0`. Option B is 24 lines across `core/iterator.c` and `extensions/autolink.c`: 106 rows changed, net **−46**, ledger **207 → 169**, and every gate green **after** one registered upstream divergence (Q38) | **0a.14** |
| **D14** | **LANDED 0a.9.** Fixable at the baseline | **§2 is wrong twice, and two of this row's own numbers went stale.** Re-measured composed with 0a.2 at §4.2.15: **360** of the 432 move, not 252, and the NUL and invalid-UTF-8 rows are **0** and **0**, not 162 and 90 — 0a.2 removed the heap bytes before this commit ran. The original reading: It reproduces on the untouched tree with no D10 fix: `x[\^a] tail` → `literal="x[^a]] tail"` (backslash lost, `^` invented, `]` doubled) and `x[&#94;a] tail` → `literal="x[^\0\0\0\0\0] tail"`. And the "policy move, not a repair" objection does not survive measurement: at the baseline **no** escaped or entity-spelled call ever resolves, because the column arithmetic makes the lookup key `n]` or `\0\0\0\0\0`, never `n` — verified with `a[\^n]` + `[^n]: note`, which drops the definition before *and* after. The narrowing removes broken behaviour only. 432-case matrix (6 caret spellings × 8 labels × 3 tails × 3 definition contexts): 252 move; the baseline emits **invalid UTF-8 on 90 of them and NUL bytes on 162** — heap bytes materialised into a document. One condition, bounds-tested before the subscript. **Zero golden rows** | **0a.9** |
| **D15** | **LANDED 0a.11.** Fixable at the baseline | Over all 2,744 ordered triples of 14 significant lines, **414 (15.1%) parse differently through the CLI than through the facade**; after one shared attach path, 0. The 809-input fixture corpus shows 0 CLI-vs-facade differences, which is why no oracle sees it — **and no corpus ever could**, because every fixture runs through the facade and so can see only one of the two orders (§4.2.17, mutant D). **Re-measured at 0a.11 with D8 fixed: the two old orders disagree on 4, not 414; the 414 is a baseline-era reading and was not reproduced, because reproducing it means reverting 0a.5.** `markdown_core_core_extensions_attach(parser, mask)` walking one ordered table with `table` last (Q9), declared **without** `MARKDOWN_CORE_EXPORT` so the export map and `audit-public-surface.sh` are untouched. The CLI's `-e NAME` lever must route through the same bit table or the hole is still open — **at 0a.11 it was deleted instead: no name outside the table can reach the registry, so the by-name path was unreachable code holding a second attach site open.** **Zero golden rows moved; three examples added, and a new structural audit, because the by-construction claim had no gate** | **0a.11** |
| **D16** | **FIXABLE-AT-BASELINE** | 40 rows — 37 `spec.txt`, 3 `extensions.txt` — cross-checked independently here: the corpus carries **58** `title=""` rows (54 spec + 4 extensions), 18 of them D6's, and 58 − 18 = 40 exactly. **Mechanism correction:** `markdown_core_clean_title` already folds a zero-length title to `CHUNK_EMPTY`, so `inlines.c:1755` is **behaviour-neutral today**; the entire visible defect is `chunk_clone`, which `calloc`s `len+1` unconditionally and turns the refmap's NULL back into `""`. Take both anyway — `chunk_clone` alone leaves 1755 asserting "written and empty" for something never written, which is the exact tension 0a.7 was told not to resolve | **0a.7** |
| **D18** | **FIXABLE-AT-BASELINE** | 10 rows, one file (`spec.txt` examples 177, 179, 184, 185), every one of them the **golden being wrong**: example 185 pinned `Text "===" scope=1:1..1:3` for text on line 2 and `Link scope=2:1..2:5` for a link on line 3. The fix counts `\n` in the prefix `resolve_reference_link_definitions` drops, which is sound because `markdown_core_parse_reference_inline` only returns after `skip_line_end` succeeds — so the dropped prefix always ends on a line boundary. Putting it in the helper covers **both** consumers (`finalize` and the setext path); the setext one is why example 184 moves. Verified under block quotes, list items, stacked and multi-line definitions, CRLF, and the all-consumed case | **0a.12** |
| **D19** | **FIXABLE-AT-BASELINE** | 1 row (`spec.txt` example 518, which pinned `Link scope=1:1..1:25` on a 14-character line 1). +14/−1 at the `match:` label, reusing the file's own `count_newlines` and reproducing `handle_newline`'s exact `column_offset` convention. Two further witnesses separate the defect's halves: `[a\nb](/u) tail` gives a **link that begins after its own child** with no newline counting involved, and `*[a](/u "t\nt2")* tail` displaces every later node in the paragraph. Do **not** guard it on `MARKDOWN_CORE_OPT_SOURCEPOS` — that is D3, nothing sets it, and the guard would make the fix a no-op | **0a.12** |
| **D20** | **FIXABLE-AT-BASELINE** | One line. 3 rows in `extensions.txt` (568, 582, 584), each a negative range becoming a real one; ledger 207 → 204 via `--update`, the gate's sanctioned path. The `extensions_gfm` red was the **golden** being wrong | **0a.12** |
| **D21** | **FIXABLE-AT-BASELINE** | +54/−14 across three files, including one new extension-API constant. **Two smaller candidates were built and discarded**: returning 0 on the fence line does not fix it (`check_open_blocks` does not close unmatched blocks — the lazy branch still fires, because `parser->blank` is false), and advancing past the line end to make it read as blank silently changes list tightness. Full-corpus differential — every example in all ten fixture files × two profiles, 11,180 dump lines — moved **exactly two lines**, both in the golden that pinned the defect, plus one row in `specs/canonical-ast/structure.ast` that the task's gate list does not cover. **External confirmation:** mdast parity goes 46/46 → **48/48 with the backlog unchanged at 23** — remark, driven by the normative grammar Step 7 will port, produces the same tree as the fixed engine. 4,000 directed random documents through the ASan build, 0 failures | **0a.10** |
| **D22** | **FIXABLE-AT-BASELINE**, and it does **not** need Step 7 | Make the primitive honest (`markdown_core_inline_parser_set_offset` advances the subject's line counter over a consumed span) and let directive's two sites read the end back from the subject instead of computing `start_column + len - 1`. Result is exactly the oracle: `Directive 1:1..2:5`, `Text 2:6..2:10`, agreeing byte-for-byte with a softbreak control at the same indent under two newlines, CRLF, block quote and list item. `set_offset`'s other three callers are unaffected. **Zero existing golden rows — the defect is completely unpinned**, which is the finding; the pin was added and proved to bite | **0a.10** |
| **D23** | **FIXABLE-AT-BASELINE**, and take the complete cut | §2's named one-liner (`emph->start_column += opener_num_chars`) gives the right `Strong` and **leaves the defect's other half open**: the leftover `Text "*"` still claims columns 1–3, so the two nodes still overlap, and the mirror case `**a***` is untouched. 31 rows. The complete cut is 4 more lines in the same function — the opener keeps its leading bytes, the closer its trailing ones — and every case becomes byte-exact and non-overlapping: `*****a*****` → `Emphasis 1:1..1:11 > Strong 1:2..1:10 > Strong 1:4..1:8`. **57 rows** (spec 45, regression 11, extensions 1). Taking the one-liner buys a second golden churn later | **0a.13** |
| **D24** | **LANDED 0a.11.** Fixable at the baseline | +7/−1. The re2c rule is `("[ ]"\|"[x]"\|"[X]")spacechar+`, so a non-zero `matched` guarantees `matched >= 4` and the read at `first_nonspace + 1` is in range. **Zero existing golden rows.** It is confirmed to be the pending upstream delta `tasklist-checked-marker` — and activating it **cannot be done by editing JSON alone**: `check-upstream-parity.mjs` keys `expectedDivergences` by input and fails any entry not reachable in the corpus, and the registered input `- [ ] call me [x] later` was in no fixture at all | **0a.11** |
| **D25** | **FIXABLE-AT-BASELINE**, and it is **one hunk with D10's byte half** | Reproduced three ways; the ASan stack is §11.4's witness byte for byte, and `map.c:279` / `inlines.c:1384` still resolve exactly. The fix is one expression — the length was never the bug, the **base pointer** was: the old code borrowed *the following node's* literal, the fix borrows `subj->input + opener->position + 1`, and on one line the arithmetic is provably identical when the node after `[` borrows `subj->input` at `opener->position`, which is exactly what a decoded entity breaks. **Zero golden rows on the fix alone**, so its fixture is mandatory, not optional evidence | **0a.2** |

**Two of the fourteen carry a decision that is not the implementer's.** D13 needs `empty-text-node` registered as a deliberate divergence from cmark-gfm, because upstream emits the empty node too (**Q38**). D16's spec example 169 flips `[foo]: <>` from `destination="" title=""` to `destination=null title=null`, and the destination *was* written (**Q39**). Both are in §4.1.6's ledger; neither may be taken silently inside a defect commit.

**These were written as Q31 and Q32 and those ids were already taken** — §11.8's inventory claims Q31–Q36, and §11.8's Q32 (snapshot ownership) is answered and cross-referenced from Q35. Renumbered to **Q38** and **Q39** at 0a.7, which is the commit that had to take the second one. 0a.0's row says it took both; it did not, and neither appeared in the ledger.

#### 4.2.2 The stage

Ordered by four rules, in this precedence: **(1)** an oracle's first reading is taken on the *unfixed* tree, so the position oracles land alone and first; **(2)** memory-unsafety, lifetime and data loss before wrongness; **(3)** a defect that makes another defect's gate vacuous lands **after** that gate has proved its mutant kill; **(4)** anything whose statement changes under a later step lands before that step.

| # | Lands | Defects | C lines | Goldens moved | New gate | Cost |
|---|---|---|---|---|---|---|
| 0a.0 | Gate reconciliation | D17 | 1 | 0 | four items, all verified; the two decisions (now Q38, Q39) were NOT taken here — Q39 was taken at 0a.7, Q38 is still owed by 0a.14 | ½ day |
| 0a.1 | **Three** oracles, no engine change | — | 0 | 0 | inline-sourcepos · scope-containment **incl. sibling non-overlap** · **position-is-a-place** | ~200 script lines, 1 day |
| 0a.2 | Lifetime and data loss | D10, D11, **D25** | ~28 | 1 + 1 ledger | 3 regression examples, 1 upstream **model** delta, 2 `expectedDivergence`s | 1½ days |
| 0a.3 | The out-of-bounds read | D4 | 1 | 0 | a debug assertion the existing ASan/UBSan presets trip | 1 hour |
| 0a.4 | Extension registration | D1, D2 | 3 (all deletions) | 0 | 3 mdast corpus rows, 3 engine examples, 1 structural invariant | ½ day |
| 0a.5 | Cross-extension interference | D8 | 6 | 0 | `extensions-conflicts.txt`, 2 examples — **and its order-independent form** | ½ day |
| 0a.6 | Positions behind a dead option | D3, D7 | 7 | 13 | 2 regression examples; oracle (a) goes 13 → 1 | 1 day |
| 0a.7 | The null/empty rule, all three sites | D5, D6, **D16** | ~11 | **58 + 1 assertion** | 1 regression example; activate `refdef-title-rewind` | 1 day |
| 0a.8 | **D9 pinned, not fixed** | — | 0 | 0 | order-independence oracle (**registered red**), output-size bound (green) | ½ day |
| 0a.9 | Footnote-call recognition | D14 | 1 | 0 | 3 regression examples + the caret matrix reduced to a fixture | ½ day |
| 0a.10 | Directive close and span | D21, D22 | ~88 changed | 2 + 1 `.ast` | 3 directive examples, 1 mdast `baselineBacklog` entry; mdast 46 → 48 | 1½ days |
| 0a.11 | One attach path, one marker | D15, D24 | ~48 | 0 | CLI/facade equality test; activate `tasklist-checked-marker`; **convert 0a.5's gate** | 1 day |
| 0a.12 | Positions that are not places | D18, D19, D20 | ~24 | 14 | 2 regression examples; oracle (c)'s reading drops | 1 day |
| 0a.13 | The overlap class | D23 | 6 | 57 | oracle (b)'s sibling half goes red on revert | 1 day |
| 0a.14 | The empty-`Text` class | D12, D13 | 24 | **106 changed, net −46** + 1 assertion | ledger 207 → 169; `empty-text-node` projection + delta | 1½ days |
| | **total** | **24 fixed, 1 pinned** | **~250** | **252 + 2 assertions** | **~20 examples, 7 oracles/invariants** | **~13 days** |

**The stage grew from 39 changed lines to ~250, and almost all of the growth is two mechanisms rather than many expressions.** D21's block-close semantics is 68 lines and D15's shared attach path is 40; the other twelve defects together are ~140. Eleven files are touched: `core/blocks.c`, `core/inlines.c`, `core/iterator.c`, `core/markdown-core-extension-api.h`, `core/main.c`, `extensions/ast.c`, `extensions/autolink.c`, `extensions/directive.c`, `extensions/strikethrough.c`, `extensions/table.c`, `extensions/tasklist.c`, plus one new `extensions/core-extensions.c`.

#### 4.2.3 The sub-steps

**0a.0 — reconcile the gates before touching the engine.** No engine change except D17. Four items, all verified; the first three stand as written (repoint `specs/`, restore the two era-skewed checkers, D17's one-line version macro, register the known-reds with their owner steps). **A fifth item is added by the ruling:** take Q38 and Q39 (written here as Q31 and Q32, ids that were already taken) *here*, before any commit needs them, so that no defect commit smuggles a divergence decision. **This did not happen** — Q39 was taken at 0a.7 in the commit that needed it, and Q38 is still owed by 0a.14.

**0a.1 — the oracles that must exist before any position is touched. LANDED; the readings are in §4.2.7, and three of the numbers below were wrong.** There are **three**, because the ruling brought two whole classes of position defect into the stage that the original two oracles cannot see.

- **(a) Inline sourcepos vs upstream** — unchanged. Every inline `code` and `html_inline` position from the pinned `cmark-gfm --to xml --sourcepos` against our dump over all 671 `spec.txt` examples. **Baseline reading: 13.** After D3: 1 (spec example 200, the pre-existing content-offset-as-column class).
- **(b) Scope containment, extended to siblings.** For every node, `parent.start ≤ child.start` and `child.end ≤ parent.end` — **and no two siblings overlap**. The sibling half is new and it is what makes D23 falsifiable: D23's defect is `Text "*"` and `Strong` both claiming column 1, which is not a containment violation at all. It cannot be an upstream comparison; upstream has D7, D18, D19 and D23 too.
- **(c) A position is a place** — new. For every dumped scope, the line exists in the input and the column is within that line's length. This is the oracle that watches D18, D19, D20 and D22, none of which trips (a), (b), the sentinel/negative/line-zero ratchet, or any parity gate. **Its baseline reading is already measured and recorded in §4.1.3: 78 of 1,928 inline nodes over the three fixture files at `--profile gfm-extended` carry a position that is not a place.** Stage 0a must drive that number down and name what is left; the residue is the continuation-line column class, which is Step 10's.

Three oracles, ~200 lines of Node, no new dependency. **Every one of them takes its first reading on the unfixed tree.**

**0a.2 — D10, D11 and D25: the lifetime and data-loss defects, first among the fixes. LANDED; §4.2.8 records what it moved.** D10 is two hunks (the label taken from buffer offsets; `fnref->start_line` from the opener) plus four lines in `blocks.c:625` positioning the `calloc`'d replacement node. D11 is one word — `EXIT` → `ENTER` at `blocks.c:578` — plus the 8-line sweep before `blocks.c:675`; neither hunk alone is enough, measured. **D25 is the same expression as D10's byte half**, and that is why it lands here rather than anywhere else: if D10 lands as written, D25 is fixed *by accident*, which is exactly what §11.4 warned about. Name it, with the `&Hat;` witness and its own fixture.

**Correct §4.2's own underflow argument while writing this hunk.** The previous text said the branch is entered only when `input[opener->position]` is the `^`. **That is false, and `&Hat;` is the counterexample** — the branch tests the *decoded* first byte. The sound argument is that the branch requires `literal->len > 1 || next->next`, so the label span is ≥ 2 bytes and the length is ≥ 0. `subj->input` is `parent->content`, freed only in `markdown_core_node_free`, so it outlives `process_footnotes` — the same lifetime every `make_str` literal already relies on.

**The differential moves three classes, not two:** the entity caret (`x [&Hat;abcdefghij] y` — UAF and arena garbage → real bytes), the label crossing a line end (`x[^a\nb] tail` — `x[^] tail` → `x[^a\nb] tail`), and the **escaped** caret (`[\^abc] x` — `[^abc]] x`, a manufactured `]` → `[^^abc] x`). The third is not memory-unsafe and is not named by any existing defect; it is closed here and its *language* half — that neither spelling should open a footnote call — is 0a.9's.

The ratchet composition stands and must still be verified in the 0a.6 commit: D10 turns `regression.txt:474`'s sentinel into a real position (17 → 16) and D7's continuation-line fixture adds one `SoftBreak` sentinel (16 → 17).

**0a.3 — D4. LANDED; §4.2.9 records it.** Unchanged: swap the operands, and gate it with an assertion under `#ifndef NDEBUG` that the existing sanitizer presets trip on `a *~~`.

**Correct §2 and §11.4 here.** They say *"the CLI allocates through the arena, and so does the `asan` preset"*, and *"H12 makes it invisible even under `--preset asan`"*. **The second half is false.** The fixture runner does not go through `core/main.c`: `spec_runner` → `ts_ast_parse` → `markdown_core_document_parse` → `extensions/ast.c:113`, which calls `markdown_core_parser_new` — **the default allocator**. `ctest --preset correctness-asan` runs the whole golden corpus on malloc/free, and it was measured to catch D25: with the regression example added and the fix reverted, `correctness-asan` reads **56/57** with a genuine `heap-use-after-free`. What the arena hides is only the CLI and `dump_cli_runner`. Q12 is not a prerequisite for anything in this stage, and 0a.3's note should say *that*.

**0a.4 — D1 and D2, before Step 3 writes the descriptors. LANDED; §4.2.10 records it.** Unchanged.

**0a.5 — D8. LANDED; §4.2.11 records it, and OBLIGATION 2 IS ALREADY PAID.** The six `return parent_container;` → `return NULL;` and both "do not take" warnings stand. **What changes is the gate, and the reason is 0a.11.** With `table` last — which is Q9, and which 0a.11 implements — D8's block-open symptom is **unobservable through both product entry points**: measured with D8 *unfixed*, the independence property over 1,728 no-table documents goes 375 failing → 0 on D15's fix alone. So the `extensions-conflicts.txt` fixture framed as an end-to-end parse reads *"0 passed / 2 failed at baseline, 2 passed / 0 failed with the fix"* **only while the attach order is still wrong**. Two obligations follow, and both are mandatory:

1. **0a.5 lands before 0a.11**, and its commit records the 0/2 → 2/2 mutant kill against the baseline order.
2. **0a.11's commit converts the gate to an order-independent form** — a direct test that `try_opening_table_header` returns `parent_container` on a decline, or an extension registered *after* `table` in `tests/api/main.c` — and re-proves the mutant kill after the reorder. A gate that goes vacuous the moment a later defect is fixed is not a gate; §4.5's "four defects invisible to every oracle" warning applies to D8 twice over.

**0a.6 — D3 and D7. LANDED; §4.2.12 records it, and it found D27.** Unchanged, including the `inlines.c:343` amendment for the container-relative end column and the instruction to name the end-column-zero class in the ledger's `purpose` field. **Two corroborations arrived independently.** First, un-gating `OPT_SOURCEPOS` alone turns five spec examples red for a *second* reason: `adjust_subj_node_newlines` writes `node->end_column = since_newline`, which is **0** when the span's last byte is a newline, and never adds `block_offset` — that is the same class the existing text calls "a fourth class the ledger does not classify", and the `inlines.c:343` amendment is what answers it. Second, the family is wider than the extension API: the *core* has D22's bug wherever it consumes a newline-bearing span (`` `x\ny` tail`` → `Text " tail" scope=1:6..1:10`; CommonMark 500 likewise). Under Q14 the `MARKDOWN_CORE_OPT_SOURCEPOS` bit is deleted outright at Step 3; 0a.6 removes its only live use, so that deletion becomes trivial rather than a behaviour change.

**0a.7 — D5, D6 and D16: the null/empty rule at all three sites, in one commit. LANDED; §4.2.13 records it, and Q39 was taken here.** The previous text said *"note the tension and do not try to resolve it here"* and named `inlines.c:1755` as a third violation to be left standing. **Q25 reverses that instruction**, and the reversal is what makes this the cheapest commit in the stage rather than the most awkward:

- D6 deletes one line (`make_autolink`'s `title = chunk_literal("")`), 18 rows.
- D16 is two hunks, ~8 lines: `chunk_clone` returns `CHUNK_EMPTY` for a NULL-data source, and the no-title branch records absence. 40 rows.
- D5 then sets the rewind path's `title` to exactly what its own neighbour sets — which, with D16 in the same commit, is **absence**, not `""`. D5's fix therefore reads `title=null` from the start and its new regression example is written once, correctly, instead of being written as `""` and rewritten at Step 14.

58 rows move, plus one assertion string in `tests/api/main.c:1076`. **No parity oracle can ever police D6 or D16** — `scripts/lib/upstream-cmark.mjs:174` maps `title:""` to `"null"` before comparing, on all three of them — so the golden dump is the only mechanism in this repository that can hold the fix down, which is exactly why the 58 rows are regenerated *once*, deliberately, in a commit whose subject says so. The `spec_commonmark`/`extensions_gfm` reds that appear before regeneration are **the goldens being wrong**: they assert `title=""` for a title nobody wrote.

**One row needs a hand decision and must be named in the commit** rather than passing as "unchanged in kind": spec example 169, `[foo]: <>`, goes `destination="" title=""` → `destination=null title=null`. The destination *was* written and was empty, so `""` is arguably right. It flips because `markdown_core_clean_url` folds length-0 to NULL, the same fold `clean_title` does, and the fix makes the reference path agree with the inline path, which already answers `[a](<>)` → `destination=null`. That is consistency, not correctness; a rule that truly separates the two requires the folds to stop, which is Step 14's structural job. **Q39, taken at 0a.7.**

The mdast note must also be amended in this commit: `specs/mdast-parity/corpus.md:243` still diverges correctly after D5, and its note must say the title defect is fixed and only the node model remains — otherwise the next reader re-derives D5.

**0a.8 — D9 pinned. LANDED; §4.2.14 records it.** Unchanged. No engine change; two gates and the statement of the defect recorded beside `map.c:307`.

**0a.9 — D14, the footnote-call recognition rule. LANDED; §4.2.15 records it.** One condition at `inlines.c:1321`, testing the **raw** source byte with the bounds test before the subscript (D4's lesson). It lands after 0a.2 because it amends the entry condition of the branch whose slice 0a.2 rewrites, and the pair must be re-measured composed. Zero golden rows; the gate is three regression examples (escaped, entity-spelled, and a spelling with a matching definition present) plus the 432-case matrix reduced to a fixture. **This discharges half of Step 9a's raw-`^` clause** — the "opens with a raw `^`" half. The other half, "and the document defines that label", stays 9a's, because it is a model question about what a failed call becomes (§5.7, Q2).

**0a.10 — D21 and D22, the directive pair. LANDED; §4.2.16 records it.** D21 adds one constant to the extension API:

> `MARKDOWN_CORE_BLOCK_CLOSED` — returned by a `markdown_core_match_block_func` when the input is the container's own closing line. The parser closes the container and every block still open inside it, ends the container at this line, and stops processing the line.

The core half extracts the existing "ends on the line being processed" arithmetic out of `finalize` byte-identically and threads a `should_continue` out-param through `parse_extension_block`, exactly as `parse_code_block_prefix` already does for fenced code. Other extensions are inert: `table`, `formula` and `tasklist` return only 0/1, so `matched != CLOSED → return matched != 0` is the old code. **One lifetime invariant a reviewer must keep true:** `S_set_end_to_current_line(parser, container)` runs *after* `finalize`, which frees a node only in its `PARAGRAPH` case, and the node reaching this path always carries an extension-minted type. ASan cannot police that — the CLI is the arena path — so it is stated here and asserted in the commit.

D22 makes `markdown_core_inline_parser_set_offset` honest and has directive's two sites recompute their end *after* the consume, which keeps every OOM path returning `NULL` without having moved the subject.

Three rows move — two in `extensions-directive.txt` example 16 (the inner `:::spoiler` was ending at the **outer** `::::` fence's line: the golden was wrong) and one in `specs/canonical-ast/structure.ast`, which the `conformance` preset catches and which no defect statement predicted. D22's pin is a new example, and because `extensions-directive.txt` is also mdast corpus, it needs one `baselineBacklog` entry closed by Step 7 (the pre-existing attributes-JSON and label-shape gap, not anything D22 introduces): backlog 23 → 24.

**0a.11 — D15 and D24, the two Step 3 was holding. LANDED; §4.2.17 records it, and the `-e NAME` lever was DELETED rather than routed.** D15 is one shared attach path plus routing the CLI's `-e NAME` lever through the same bit table, without which "impossible by construction" is false. The order is Q9's — `table` last — so Step 3 inherits a decided *and implemented* order and its job becomes making the order **unexpressible**, not choosing it. D24 is one read of the byte `scan_tasklist` already matched.

**Activating `tasklist-checked-marker` requires a corpus addition, and §4.1.7 does not say so.** The registered input was in no fixture; `check-upstream-parity.mjs` fails any `expectedDivergences` entry not reachable in the corpus. Add the example (`extensions.txt`'s task-list section, or `regression.txt` per `refdef-title-rewind`'s precedent — both are parity corpus), then move the entry out of `pendingDeltas`/`pendingExpectedDivergences`. And convert 0a.5's gate here, per the obligation above.

**0a.12 — D18, D19 and D20: positions that are not places.** 14 rows in two files, every one the golden being wrong. The three are independent — different files, disjoint rows, verified from a scratch reconfigure — and land together because they are one class and one oracle reading. Two honest limits belong in the commit message, not discovered later:

- D18 corrects the **line**. `start_column` is deliberately untouched and is right wherever the remaining first line has the same stripped prefix as the definition's line, which is every corpus case plus block quotes and list items. Where the prefixes differ, the residue is the content→source column class that exists with no reference definition in sight (`a\n  *b* tail` → `Emphasis 2:1..2:3`, truth `2:3..2:5`). That is Q22/Step 10's.
- D19's example 518 ends at `2:10` where the true source column is `2:12`; the two-column shortfall is the continuation line's stripped leading spaces — the same class, visible with no link present.

**0a.13 — D23, the overlap class.** The complete cut, four lines beyond §2's one-liner. 57 rows, hand-checked against the source columns before regenerating; CommonMark 426 (`foo******bar*********baz`) becomes `Strong 4..18 > 6..16 > 8..14` with the tail `Text` at `1:19`, against the golden's `4..21 / 6..21 / 8..21 / Text 1:13`. It lands after 0a.12 and separately from it because 57 rows is the largest single regeneration in the stage and it deserves its own review. It does **not** interact with 0a.14: `S_insert_emph` already frees a delimiter node whose literal is spent (`if (opener_num_chars == 0) markdown_core_node_free(opener_inl)`), so the emphasis path creates no empty `Text` for D13 to clean up.

**0a.14 — D12 and D13, the empty-`Text` class, last.** It is last because it *removes* rows, and how many it removes depends on which empties exist — which 0a.12 and 0a.13 both affect. Option B, 24 lines: consolidation drops a `TEXT` node that owns no bytes and takes a merged run's end only from an operand that owns bytes (the `len > 0` guard is what makes the D12 line safe, since an empty operand may still be absorbed by a merge); autolink's `postprocess_text` stops leaving an empty prefix or an empty tail. Producer (3) — consolidation merging a run of empties into an empty — becomes unreachable by construction.

**106 rows change, 60 are replacements, 46 disappear.** The 46 are cross-checked here against the corpus: exactly 46 `Text … literal="" children=0` rows exist, of which **36 are the `0:0..0:0` sentinel** (30 `extensions.txt`, 6 `spec.txt`) and 10 carry honest positions — the base-language empty a hard break's stripped spaces leave. The other 60 are `children=` counts on `Paragraph`/`Link`/`TableCell`/`Strikethrough`. One hand-written assertion, `tests/api/main.c:1166`, **pins the defect** as expected output, the same shape as D10's; it is updated in this commit. The ledger goes **207 → 169** by `--update`, all shrink.

**Two things this commit must state.**

1. **The upstream red is the oracle, not the fix, and clearing it is a decision.** `check-upstream-parity.mjs` reads 784/795 without it: **cmark-gfm emits the empty text node too**, verified directly, and 11 corpus inputs diverge (8 autolink, 3 hard-break/shortcut-reference). This is the history already recorded in `specs/mdast-parity/deltas.json` under `empty-text-node` — *"suppressing it here failed `scripts/check-upstream-parity.mjs`, which is how this was classified as a shape delta rather than a defect"* — reproduced. Registering it costs one normalizer projection (drop an empty-literal `Text` from **both** sides in `scripts/lib/upstream-cmark.mjs`), one `NORMALIZED_DELTAS` name and one `deltas.json` entry of kind `deliberate-difference`: **795/795, green.** That is **Q38**, and §4.1's Step 5 row should have said so from the start rather than leaving it to be discovered at Step 5. The `specs/mdast-parity/deltas.json` `empty-text-node` entry goes half-stale in the same moment and is updated here.
2. **The free happens at `ENTER`, and that is legal today for a reason Step 5 removes.** `TEXT` is a leaf, so `S_is_leaf` suppresses its `EXIT` and `markdown_core_iter_next` has already computed `iter->next` past it. When Step 5 makes the event contract **total**, every node gets an `EXIT` and the rule "only the node whose `EXIT` is current may be freed" makes this free illegal. Step 5 must move it, or state that a leaf's `ENTER` *is* its `EXIT` for mutation purposes. Write that into the commit and into Step 5's row.

**0a.15 — D28 and D29, the two memory-unsafety defects §4.13 added after this list was written.** Added 2026-08-21, before 0a.11, per §2's standing instruction; the reproductions and the argument for landing it *last* are in §2 beside the defect index. Neither fix moves a golden row and neither is reachable without an injected allocation failure, so it does not interact with 0a.12–0a.14's regenerations.

- **D29** (`extensions/table.c`, `try_inserting_table_header_paragraph`): check `markdown_core_node_new_with_mem` before `markdown_core_node_set_string_content`. Two neighbours travel with it and are named in §4.13.11 — the `!paragraph_content` path frees the lead paragraph and returns without setting `parser->oom`, and the failed-insert path frees the node with `mem->free` instead of `markdown_core_node_free`, leaking its content buffer.
- **D28** (`extensions/formula.c`, `set_formula_literal_bytes`): `markdown_core_chunk_to_cstr` returning NULL must be a failure, not ignored — a borrowed chunk that could not be copied outlives its buffer at all three sites (`:154` via `make_formula_node`, `:523` via `new_formula_block_from_literal`, `:550` via `postprocess_node`).
- **The gate is `case_oom_sweep`'s corpus** (`tests/runners/fallback_runner.c`): add a paragraph immediately followed by a table, and a ```` ```formula ```` info string. The sweep's contract — *each injected failure must either surface as a failed parse or leave the output byte-identical to the control* — is already the right assertion; it simply never sees these two shapes. Prove the mutant kill by reverting each fix and reading the sweep, and say which of the two neighbours the sweep can and cannot see.

#### 4.2.4 Nothing failed to reproduce — but eleven statements are wrong

**Every one of the fourteen reproduced on the untouched baseline.** There is no defect here that §2 invented. There are, however, eleven statements in §2, §4.1.7 and §4.2 that measurement contradicted, and each is a correction to make rather than a defect to schedule.

| Where | What it says | What is true |
|---|---|---|
| §2 row D14 | it needs D10 fixed, and the fix is "a policy move, not a repair" | Reproduces at the baseline unaided; and no escaped or entity-spelled call ever resolves there, so the narrowing removes broken behaviour only. Both clauses go |
| §2 row D12 | "blocked by D13" | Blocked by producer (2), `S_update_text_sourcepos` (`core/inlines.c:1898-1922`), which is **not** the site D13 names. And after D13 and D10 it has no witness in 860 fixture examples or 40,000 random inputs |
| §2 item 13 / §4.1.7 | D13 is `set_sourcepos_from_range`'s `len == 0` | That site produces **31** of the 36 sentinel rows; `markdown_core_node_unput` produces the other 5 — including `extensions.txt:804`, the very row §2 uses to argue D12 is blocked. §4.1.7's three-producer note is the accurate description and should replace the §2 row. §4.1.7 also under-counts the *sites*: there is a **fourth** producing `Text scope=0:0..0:0` with a non-empty literal, D10's `calloc`'d replacement node (`core/blocks.c:620`, `regression.txt:474`) — four sites, three of them zero-length |
| §2 item 16 | D16 is two sites, `chunk_clone` and `inlines.c:1755` | `markdown_core_clean_title` already folds length-0 to `CHUNK_EMPTY`, so 1755 is behaviour-neutral **today**; the entire visible defect is `chunk_clone`. Take both, but for the reason stated in 0a.7, not this one |
| §2 / §11.4 | *"the CLI allocates through the arena, and so does the `asan` preset"*; *"H12 makes it invisible even under `--preset asan`"* | False for the fixture path. `extensions/ast.c:113` uses the default allocator; only `core/main.c:238` uses the arena. **One ordinary regression example is a complete memory-safety gate for D25.** The blind spot is CLI-only, and Q12 is not a prerequisite |
| §4.2 0a.2 | underflow is impossible because "the branch is entered only when `input[opener->position]` is the `^`" | Unsound — `&Hat;` enters the branch with `&` at that byte. Substitute the ≥2-bytes-of-label argument |
| §4.1.7 row D18 | the witness is a `Text` node | The **paragraph itself** carries the wrong start, and through the setext path so does a `Heading` (spec 184). Any oracle for D18 must assert the block position, not only the inline one |
| §4.1.7 row D19 | truth is `Link 1:1..2:6`, `Text 2:7..2:11` | Arithmetically wrong. Line 2 is `t2") tail`: the `)` is the 4th character and ` tail` occupies 5..9. The newline consumes one linear column too, so the subtraction is 10, not 9 — truth is `Link 1:1..2:4`, `Text 2:5..2:9`. Engine-after-fix and hand arithmetic agree. **Fix the row before it becomes a test expectation** |
| §4.1.7 row D21 | "a blank line after the fence hides it" | It hides the *sibling-attribution* half only. `:::note/body/:::/(blank)/after` still emits `Paragraph scope=2:1..3:3` — a paragraph whose extent covers a line none of its children touch. The extent half is never hidden. And with the trailing line outside a block quote, `after` is pulled inside **both** the quote and the directive and recorded at a column that does not exist on its line |
| §4.1.7 row D22 | "**7** lands the primitive; **8** owns the model" | 0a.10 lands the primitive. Step 8 still owns the model |
| §4.1.7 row D24 | "may be the same thing as the pending upstream delta — check before re-deriving" | It is. And activating the delta needs a **corpus addition**, not a JSON edit |
| §4.2 0a.5 | the gate reads "0 passed / 2 failed at baseline" | True only until 0a.11, and **0a.11 measured it going vacuous**: with the same D8 mutant the fixture reads 4 passed / 0 failed while `api_engine` still fails on two named assertions. §4.2.17 |

#### 4.2.5 One new defect the ruling exposes: D26

Counting the sentinel rows to size 0a.14 turned up a class nothing in §2 names. The corpus carries **188** `scope=0:0..0:0` rows, not 36:

```
    133  SoftBreak
     37  Text          (36 empty-literal — D13's; 1 non-empty — D10's replacement node)
     17  LineBreak
      1  Paragraph     (the footnote-ordinal paragraph, deleted by Step 9a)
```

`handle_newline` (`core/inlines.c:1443-1460`) creates its break node with `make_simple`, which `calloc`s, and **never assigns a start or an end** — while `nlpos`, `subj->line` and `subj->column_offset` are all in hand two lines earlier. So Stage 0a closes 37 of the 188 and Step 9a closes 1; **150 rows have no owner**, and Step 5's requirement — *"no node carries `0:0..0:0` as a stand-in for 'no bytes'"* — is unreachable without them.

> **D26 (proposed, unmeasured).** `handle_newline` gives `SoftBreak` and `LineBreak` no position at all. Severity: wrong-position. 150 golden rows. **It must be put to the same test as the other fourteen before it is scheduled**; on the evidence it is a two-line fix and belongs at 0a.12, but it is listed here as a §2 addition with an unmeasured budget, not as a sub-step with a stated one. If it passes, 0a.12's golden movement grows by up to 150 rows and the ledger's sentinel budget falls by the same.

That is the ruling working as intended: the discipline of proving each defect fixable at the baseline is also the discipline that finds the ones nobody had counted.

#### 4.2.6 What Stage 0a now moves, and why §4.4's argument gets stronger

**252 golden rows, 2 hand-written C assertions, and one `.ast` row** — against 32 + 1 in the previous stage. By file, from the independent measurements:

| Sub-step | Defects | `spec.txt` | `regression.txt` | `extensions.txt` | other |
|---|---|---|---|---|---|
| 0a.2 | D10 | — | 1 | — | + 1 ledger row |
| 0a.6 | D3 | 13 | — | — | |
| 0a.7 | D5, D6, D16 | 54 | — | 4 | + 1 assertion (`main.c:1076`) |
| 0a.10 | D21, D22 | — | — | — | 2 `extensions-directive.txt`, 1 `structure.ast` |
| 0a.12 | D18, D19, D20 | 11 | — | 3 | |
| 0a.13 | D23 | 45 | 11 | 1 | |
| 0a.14 | D12, D13 | ~6 removed + replacements | — | ~30 removed + replacements | + 1 assertion (`main.c:1166`) |
| | **total** | | | **252 rows** | **net row count −46** |

`specs/scope-sanity/ledger.json` goes **207 → ~166**, every movement a shrink: −1 (D10) +1 (D7's fixture) −3 (D20) −38 (D13). That is the largest reduction in the ledger's history and it is taken under the ledger's own rule with no recorded exception.

**Three caveats, because the 252 is a sum of independent measurements and not yet a composed one.**

1. **Four pairs must be re-measured composed, once:** D13 × D20 (both concern consolidation carrying a zero end position, and the agent's `--update` runs overlap in `extensions.txt`'s negative bucket), D13 × D23 (both touch emphasis examples in `spec.txt`), D6 × D16 (expected additive — 18 + 40 = 58, cross-checked against the corpus's 58 `title=""` rows, but prove it), and D14 × D10/D25 (same branch, same function). D18 × D19 is already verified additive: 10 + 1 = 11 rows and nothing else.
2. **The composed number is expected to be ≤ 252**, never more, because a row fixed twice is counted twice here.
3. **Corpus growth:** ~20 new examples, of which most land in files that are also the upstream-parity corpus, taking that gate from 795 inputs to roughly **813** with **four** registered `expectedDivergence`s active (D5, D10's byte retention, D25's entity caret, D24's tasklist marker) and `deltas` going 4 → 6 (D11's footnote-model rule, D13's `empty-text-node`). Measure it once at the close of the stage rather than per commit.

**Does 252 change §4.4's argument that regenerating once beats regenerating repeatedly? It strengthens it, on both halves.**

*The duplicate-work half.* Under the old schedule these same 252 rows were regenerated by Steps 5, 7, 8, 9a, 10 and 14 — and regenerated *again* whenever an earlier step touched the same file first. Step 14 alone was going to move 40 rows for `title=""`, ten steps after the defect could have been closed for eight lines. Step 8 was going to move 71 position rows behind six other steps. Step 5 was going to move 106. Under this schedule **Step 5, Step 8 and Step 14 each move zero**, and 5 and 8 gain something better than a smaller diff: an acceptance test that says *the rewrite moves no golden row*, which is the only falsifiable form of "subsumes it by construction".

*The blessed-golden half — and this is where 252 is a much stronger number than 32.* §4.4's corollary is that a golden regenerated while a defect is live **blesses** the defect, because the reviewer's only available answer is "unchanged from before, therefore fine". The stage now unpins **five** goldens that currently assert a defect as expected output — `regression.txt:474` (`Text scope=0:0..0:0 literal="[^~~is~~1]"`), `extensions.txt:804`/`:809` (`59:1..59:0`), `extensions-directive.txt` example 16 (an inner fence ending at the outer fence's line), `tests/api/main.c:1076` and `:1166` — plus a sixth, `specs/canonical-ast/structure.ast`, which no defect statement predicted and which only the `conformance` preset catches. Each of those flips from defending the defect to killing it. **A corpus that asserts six wrong answers is not a corpus that got 32 rows more expensive to regenerate; it is a corpus that cannot be used to review anything until it is corrected.**

*The counterweight, stated honestly.* 252 rows is a lot of hand review, and the standing gate requires every moved row to be reviewed and named. That is why the stage is fifteen commits and not one: the largest single regeneration is 106 rows (0a.14) and the next is 58 (0a.7) and 57 (0a.13), each in a commit whose subject is the defect and whose reviewer has §2's statement in hand. The alternative is not "fewer rows"; it is the same rows, spread across eight steps, with no statement in hand at any of them.

#### 4.2.7 0a.1 landed: what the three oracles read, and where §4.2.3 was wrong

`scripts/audit-inline-sourcepos.mjs`, `scripts/audit-scope-containment.mjs` and
`scripts/audit-position-places.mjs`, one ledger each under `specs/positions/`,
~330 lines of Node, no new dependency, no engine change. All three run in CI in
the job that already builds both parsers.

**The readings, taken on the unfixed tree.**

| Oracle | Registered | Scanned | §4.2.3 predicted |
|---|---|---|---|
| (a) inline sourcepos vs upstream | **12** | 68 inline `Code`/`HTML` over 669 `spec.txt` examples | 13 |
| (b) containment + sibling overlap | **58** | 3,555 parent/child and sibling relations | — |
| (c) a position is a place | **131** | 3,814 scopes, 178 deferred to the scope-sanity ratchet | 78 of 1,928 |

**They are ratchets on a SET, not a budget.** Each records the exact rows that
are wrong, keyed by the input and the node's index path from the document, and
fails on a row appearing *and* on a row clearing. A count cannot distinguish a
fix that cleared twelve rows from one that cleared twelve and introduced one,
and that is not hypothetical — see the D3 measurement below. `--update` is taken
in the commit that moves the behaviour, whose message names the rows.

**Three corrections to §4.2.3.**

1. **(a) reads 12, not 13.** Every one of the twelve is the same shape: our side
   reports a span that crossed a line ending as though it had not, the start
   always agrees, and the end names a column on the start line. Spec example 200
   — the `content-offset-as-column` residue §4.2.3 predicts as the survivor — is
   **not among them**; it currently agrees with upstream. So the sequence is not
   13 → 1. It is measured below.
2. **(c) reads 131 over the whole fixture corpus, not 78 over three files.** The
   difference is definitional and the definitions are now written down rather
   than implied: columns are **bytes**, lines are split the way `S_parser_feed`
   splits them (`regression.txt` carries a CRLF), the corpus is every example in
   every `packages/markdown-core/tests/fixtures/*.txt` at `--profile
   gfm-extended`, and **a coordinate on line zero is not counted here** because
   `audit-scope-sanity.mjs` owns it. Restricted to inline nodes the reading is
   **59 of 2,242**, which is the number §4.2.3's sentence was reaching for; the
   gate prints that split on every run.
3. **§4.2.3 says (c) watches D20 and lists it as untouched by "the
   sentinel/negative/line-zero ratchet".** D20's symptom `Text scope=1:1..1:0` is
   *both* a zero column and a reversed range, so the two ratchets do overlap on
   it — and the overlap is load-bearing rather than redundant. They **interlock**:
   giving the end a real column at or after the start clears both, while zeroing
   the node clears (c) and *grows* the sentinel budget, which fails there. Say
   the two things separately and neither fix can hide behind the other.

**Nine families, and the census is in `specs/positions/places.json`'s `purpose`.**
58 rows are a block's end naming column 0 of a line that exists (H14's
neighbour, §11.4; **no step owns it**); 19 are the content-offset-as-column class
on a continuation line (Q22/Step 10); 19 are a span that crossed a line ending;
9 are the synthesized table cell at `L:0..L:0` and 6 more the split-off table
lead (requirement 10); 9 are the unmatched-backtick literal placed one column
right (§4.1.3, Step 8); 6 are an inline end column left at 0 (D20's shape); 3
are D18's consumed-definition line.

**Two rows name a class no defect in §2 names.** A footnote definition starts at
the column *after* its marker, which does not exist when the marker ends the
line: `[^footnote]:` is twelve bytes and the definition is reported at `19:13`.
It is the same idea as the end-column-zero class — a coordinate used to mean
"just after" rather than "at" — and the same shape turns up again the moment D3
lands (below). It is recorded here rather than minted as a defect number,
because the cure is one decision about what a zero-width position is spelled as,
and that decision belongs with the end-column-zero class, not beside it.

**Falsifiability was proved by two throwaway experiments, applied to the
baseline, measured, and reverted — and both are pre-measurements the steps that
own them should not have to repeat.**

- **D3 (0a.6), un-gating `adjust_subj_node_newlines` alone, no `block_offset`
  amendment:** (a) goes **12 → 0**, not 12 → 1. (c) goes **131 → 121**: thirteen
  rows clear and **three appear**, all three the same shape — ``` `` ```
  followed by a newline gives `Code scope=1:3..3:0`, whose *start* is one past
  the end of a two-byte line and whose *end* is upstream's own column zero. (b)
  moves by **nothing**, which corroborates §4.5's claim that containment cannot
  see D3. `spec_commonmark` fails, as §2 item 3 predicts.
- **D23 (0a.13), correcting `S_insert_emph`'s four columns:** (b) goes **58 →
  44** — exactly the fourteen rows the ledger attributes to D23, none appearing —
  while (a) and (c) move by **nothing**. The three oracles are independent in
  practice and not only by argument.

**`class` is analysis; `closedBy` is measurement.** Only the rows with a measured
owner carry one. Every other family's owner is deliberately `unassigned`: the
step that moves the rows proves the attribution *by moving them*, and the gate's
`CLEARED` list is that proof. That is cheaper and more honest than 131 hand
guesses, and it is the same discipline §4.2.1 applied to the fourteen.

**One thing 0a.1 did not fix, named so it is not re-derived.** The mdast
backlog's `corpus.md:69` entry still says `Step 9b` while §2's progress meter
and §4.6 both say `Step 9a`; the gate prints `6 Step 9b` where §2 prints `5` and
`1`. The total is 23 either way, so no rule is broken — but §2's table and
`specs/mdast-parity/deltas.json` disagree about one row's owner, and §4.6 says
which is right. It is a one-field edit and it belongs to whichever commit next
touches that file. Related: `audit-scope-sanity.mjs` is in §0's gate list and is
**not in CI**, which the three new oracles now are.

---

#### 4.2.8 0a.2 landed: three defects, one golden row, and two things nobody predicted

**What is in the engine.** Three hunks, and the shape §4.2.3 specified:

- `core/inlines.c`, `handle_close_bracket`'s footnote branch — the label is a
  slice of `subj->input` between `opener->position + 1` and `initial_pos - 1`,
  replacing a slice of the *following node's* literal whose length came from
  column arithmetic. **That one expression is D10's byte half and D25 entire**,
  and the underflow guard it replaces is gone rather than corrected: the branch
  requires a decoded `^` plus either a second decoded byte or a further node,
  and decoding never lengthens a span, so the source span is at least two bytes
  wide. The assertion says so under `NDEBUG`.
- the same branch — `start_line` from the opener and `end_line` from the closing
  bracket, instead of both from the closing bracket. A call whose label crosses
  a line ending had a start column on one line and a start line from another.
- `core/blocks.c`, `process_footnotes` — registration on `ENTER` (D11), four
  lines positioning the `calloc`'d replacement `Text` from the call it replaces
  (D10), and the sweep that hands a still-parented definition back to the tree
  before the map frees everything it names (D11).

**Where the sweep went, and why it is not where §4.2.3 says.** It is *inside*
`if (map->prepared)`, after the emission loop — not before the teardown. Before
emission it would NULL the node the emission loop is about to append; outside
the `prepared` guard it would retain every definition in a document that has no
footnote references at all, which is **definition retention and belongs to Step
9a**, not here. Placed where it is, the rule reads exactly as intended: *the map
frees only what emission removed from the tree.* An unreferenced definition is
still dropped, and `text\n\n[^unused]: hi\n` still yields one paragraph.

**One golden row moved**, and it is the row §4.5 nominated as the gate:
`regression.txt` example 24, `Text scope=0:0..0:0` → `1:1..1:10`. That golden
asserted the defect; unpinning it is the fix. `spec_commonmark` and
`extensions_gfm` moved nothing.

**Four regression examples and four registered divergences.** The corpus goes
795 → 799 inputs, `deltas` 4 → 6 (`footnote-call-label-bytes`,
`footnote-duplicate-definition`) and `expectedDivergences` 0 → 4. Every one is
a place upstream is worse: it loses `a`, the newline and `b` from `x[^a\nb]
tail`; it emits an **empty text node** for `x [&Hat;abcdefghij] y`, losing the
whole paragraph; it manufactures a `]` it never read from `[\^abc] x`; and it
destroys `OUTER opens first`. `scope-sanity` goes **207 → 206**, the exact
shrink §4.2.3 predicted.

**Mutant kills, all three proved by reverting and rebuilding.**

- D10/D25's byte half: `correctness-asan` reads **56/57** with
  `heap-use-after-free`, `READ of size 1` in `markdown_core_map_lookup`
  (`map.c:279`), freed by `handle_close_bracket` — the §11.4 witness, character
  for character. The fixture runner allocates through malloc, so the repository's
  own ASan preset is the gate; §4.2.3's correction to §2 and §11.4 on that point
  is confirmed.
- D11 with registration reverted to `EXIT`: `regression_commonmark` red,
  `OUTER opens first` and `INNER closes first` both present but the wrong one
  winning.
- D11 with the sweep removed: `regression_commonmark` red, `INNER closes first`
  gone. **Neither hunk alone is enough**, exactly as §2 measured.
- `leaks --atExit` on the nested-duplicate input: `0 leaks for 0 total leaked
  bytes`, which is the ownership half §4.5 asks for.

**Two things neither §2 nor §4.2 predicted.**

1. **The same-level duplicate changes too, and §2's D11 statement covers only
   the nested one.** `[^dup]: FIRST` / `[^dup]: SECOND` used to resolve to FIRST
   and *delete* SECOND; SECOND is now retained. It appears in the tree **before**
   the winner, because emission moves a resolved definition to the document tail
   while a retained loser stays where it was written. That ordering is a
   consequence of the existing `footnote-definition-placement` model and not a
   new decision; it disappears when Step 9a stops moving definitions.
2. **0a.2's own fixture is a D12 witness, and §4.2.4 says none existed.**
   §4.2.4 states D12 "has no witness in 860 fixture examples or 40,000 random
   inputs" after D13 and D10. It has one now: `x[^a\nb] tail` retains its bytes
   and the consolidated run reports `1:1..1:7` — column 7 on a four-byte line 1,
   because `markdown_core_consolidate_text_nodes` takes the merged end *column*
   from the last operand and leaves the end *line* at the first's. That is one
   new row in `specs/positions/places.json`, recorded and attributed.

   **And measuring the obvious fix is why the row is worth having.** Adding
   `cur->end_line = tmp->end_line;` beside the existing `end_column` line does
   clear it — `1:1..2:7`, correct — **and moves three other rows to LINE ZERO**:
   `<http://foo.bar/baz bim>` goes `Text 1:1..1:0` → `Text 1:1..0:0`.

   **§4.1's D12 row already said this**, and the credit belongs there: *"the
   one-line fix alone turns `extensions.txt:804`/`:809` from `59:1..59:0` into
   `59:1..0:0` — a strictly worse row that every gate in the repository passes
   … ledger 207 unchanged, because `endLine < startLine` keeps it in the same
   `negative` bucket it left."* What is new is only that the clause **"every
   gate in the repository passes"** stopped being true at 0a.1:
   `audit-position-places.mjs` reads a live parse and reports the move, which is
   the interlock §4.2.7 describes doing the job it was built for. **0a.14 owes
   more than the line**, and it owes a re-reading of both ratchets together.

**Citations re-pinned.** `inlines.c` node-free loop `1384` → `1395`;
`blocks.c` registration `578` → `586`, replacement node `625` → `646`, the `!ix`
drop `668` → `683`, teardown `683` → `711`. **D14's `inlines.c:1321` is
unchanged** — 0a.9 amends the branch condition, which sits above this commit's
hunk — and `map.c` is untouched, so D9's `map.c:307` stands.

---

#### 4.2.9 0a.3 landed: D4, and the sanitizer gate is stronger than §2 said

**Seventeen lines, one file, zero moved rows.** `scan_delims`'s forward flanking
scan tested `subj->skip_chars[peek_at(subj, after_char_pos)]` before
`after_char_pos < subj->input.len`. The operands are swapped, and the read now
goes through one inline helper whose only other content is
`assert(pos < subj->input.len)`. The assertion is the gate: `Release` carries
`-DNDEBUG` and the `Asan` and `Ubsan` build types do not, so the two sanitizer
presets execute it and the shipped binary does not.

**The mutant kill.** Reverting the operand order takes `correctness-asan` and
`correctness-ubsan` from 57/57 to **55/57** — `regression_commonmark` and
`fuzz_smoke`, both `Subprocess aborted` — and `printf 'a *~~\n' | markdown-core
--profile gfm` prints `Assertion failed: (pos < subj->input.len), function
flanking_skip_at`. That is §4.5's stated witness, reproduced.

**Nothing else moved**, which is the point: the discarded value cannot change the
loop's exit, because `&&` short-circuits at `after_char_pos == len`. Goldens,
both parity gates, the fuzzer, the scope-sanity ledger and all three position
ledgers read exactly what they read before.

**The correction 0a.3 owes, made in §2 and §11.4.** They said the `asan` preset
allocates through the arena and therefore cannot see a use-after-free in
node-owned memory. It does not: `spec_runner` → `ts_ast_parse` →
`markdown_core_document_parse` → `extensions/ast.c:113` calls
`markdown_core_parser_new` with the **default allocator**, so
`ctest --preset correctness-asan` runs the whole golden corpus on malloc/free.
0a.2 measured it — 56/57 with a genuine `heap-use-after-free` when D25's fix is
reverted. The arena blind spot is real and is **CLI-only**. **Q12 is not a
prerequisite for anything in Stage 0a.**

**And the reason D4 is worth a commit is unchanged and worth restating**, because
its own gate says nothing about today: the read is in bounds today only because
`markdown_core_parse_inlines` builds its chunk from a `markdown_core_strbuf`,
and a strbuf keeps `ptr[size] == '\0'` inside its allocation. That invariant is
stated in `buffer.c` and nowhere near `inlines.c`. Any chunk that is a slice of
a larger buffer makes the read live, silently, with **0 ASan reports over 14,783
executions** — measured. The assertion is what keeps the ordering from drifting
back once the invariant no longer holds.

---

#### 4.2.10 0a.4 landed: three deleted lines, and §2's 186 is confirmed

**Three lines out of the engine.** `markdown_core_syntax_extension_set_emphasis(ext, 1)`
from `extensions/directive.c` and `extensions/formula.c`, and the `'}'`
registration from directive's `special_inline_chars`. Strikethrough's
`set_emphasis` stays: `~` has to remain transparent to `scan_delims` or upstream
parity breaks, and it is the only byte that belongs in that table.

**§2's measurement reproduces exactly, and its alphabet needs writing down.**
Over all **19,607** strings of length ≤ 5 over `{a, '}', ':', '$', '*', '_',
'.'}`, **186** parse differently before and after — 0.95%, §2's number to the
digit. **The set has `}` as a member and no space**; read the other way, with a
space in place of the `}`, the same experiment gives **130**. The set notation
`{a } : $ * _ .}` admits both readings and one of them is wrong, so it is spelled
out here. Under `--profile gfm`, which detaches both extensions, the same 19,607
inputs give **0** differences, which is what makes this a statement about
attaching an extension rather than about the extensions themselves.

**D2 alone is 0 differences over 37,448**, measured with D1 already fixed, at
both `--profile default` and `--profile gfm-extended` — §2's claim, reproduced.
Deleting the `'}'` registration changes no output at all.

**D2's gate is a source audit, and the first attempt at a runtime one was
measured to fail.** §4.5 calls for "a structural invariant: every registered
`special_inline_chars` byte is dispatched by `match_inline` or is a sentinel
`< 0x20`". The obvious runtime form — count a paragraph's inline children before
consolidation, using the low-level parser — **does not work**, because
`markdown_core_parser_finish` itself calls `markdown_core_consolidate_text_nodes`
(`core/blocks.c:1697`). There is no API in this repository that returns an
unconsolidated tree, so the split a stray registration causes is invisible
everywhere. That test was written, run with `'}'` re-registered, measured to
pass anyway, and deleted rather than kept.

`scripts/audit-extension-special-chars.mjs` reads the source instead: it
collects every `(void *)'x'` and `(void *)SENTINEL` appended to `special_chars`,
resolves each sentinel through its `#define`, finds the file's `match_inline`
hook through its own registration, and requires every registered byte ≥ 0x20 to
appear in a `character == 'x'` comparison inside that function's body. Both
mutants die: re-adding `'}'` fails it, and raising a `FORMULA_DELIM_*` sentinel
into printable range fails it too. It runs in CI beside `audit:surface`.

**Gates.** 3 rows in `specs/mdast-parity/corpus.md` — `foo:_bar_`, `foo$_bar_`,
`a}*.foo.*` — with the kill measured: **46/49 with the fix reverted, 51/51 with
it**. 3 engine examples, one in `extensions-formula-option-gates.txt` and two in
`extensions-directive.txt`, which are also upstream-parity corpus and take that
gate 799 → **802/802**. **Zero golden rows moved**, as §2's table predicts.

**One thing the sentinel argument makes concrete.** §2 says the delimiter-tag
sentinels `0x01`–`0x04` and `0x08` are ordinary file bytes a user can type, and
the audit's output now states them: `formula.c: '$' '\' 0x01 0x02 0x03 0x04`,
`directive.c: ':' ']' 0x08`. Deleting them from `skip_chars` — which is what
this commit does — stops the flanking corruption; they remain in
`special_chars`, where a literal `0x01` still splits a text run and still
dispatches. **Only removing the concept closes that**, and it is Step 3's.

---

#### 4.2.11 0a.5 landed: six declines, and obligation 2 is already paid

**Six `return parent_container;` became `return NULL;`** in
`try_opening_table_header` — the six §2 identifies as wrong declines with the
node still a `PARAGRAPH`. The four allocation failures *after*
`markdown_core_node_set_type(..., TABLE)` succeeds and the one genuine opening
path are untouched, and both "do not take" warnings are now comments at the
sites rather than only in this document: one at the function head stating that
a decline is NULL and why the other five returns differ, one at the retype
marking where the meaning changes.

**The defect reproduces through the facade and not through the CLI**, which is
worth writing down because it cost time. `core/main.c` attaches `table`
unconditionally, so "formula alone" is unreachable from the command line, and it
attaches `directive` *first*, through `attach_option_extensions`, so the CLI
cannot show the directive half at all. `extensions/ast.c` — the path every
binding uses — attaches only what is enabled, in the order table,
strikethrough, autolink, tasklist, formula, directive. That is D15, and it is
also what makes the fixture below possible.

**The gate reads 0/2 at the baseline and 2/2 with the fix**, which is obligation
1. `packages/markdown-core/tests/fixtures/extensions-conflicts.txt`, two
examples, registered as `extensions_conflicts` and taking `correctness` from
65 to 66:

```
text            with `formula table`     was: one Paragraph holding an inline Formula
$$                                       now: Paragraph + FormulaBlock literal="x"
x
$$

text            with `directive table`   was: one Paragraph of 8 children
:::note                                  now: Paragraph + DirectiveBlock > Paragraph
body
:::
```

**Obligation 2 is paid here rather than at 0a.11.** §4.2.3 requires 0a.11 to
convert the gate to an order-independent form, because moving `table` last makes
the fixture pass whether or not an extension declines correctly. Waiting costs
nothing and paying now costs one test, so
`extension_decline_yields_turn` in `packages/markdown-core/tests/api/main.c`
attaches `table` and then `directive` **itself** and asserts the directive block
still opens. It is §4.5's second suggested form, and it keeps failing under any
attach order. The fixture carries a paragraph saying it will go vacuous and
naming the test that will not, so whoever moves `table` reads it in the file
they are about to hollow out.

**0a.11's obligation therefore reduces to a re-run**: re-read both gates after
the reorder, confirm the api test still fails without the fix and the fixture
now passes with or without it, and say so.

**Mutant kill, narrowest form.** Reverting **one** of the six — the
`scan_table_start` decline — is enough: `extensions_conflicts` goes 2/2 → 0/2
and `api_engine` fails two named assertions. Both were measured.

**Zero golden rows moved**, as §2's table predicts, and the new fixture joins
the upstream-parity corpus: 802 → **804/804**. `correctness` 66/66, both
sanitizer presets 58/58.

---

#### 4.2.12 0a.6 landed: D3, D7, and a defect the un-gating made visible

**What is in the engine.** `adjust_subj_node_newlines` runs unconditionally —
the `MARKDOWN_CORE_OPT_SOURCEPOS` guard is gone and the `options` parameter with
it, at the function and its three call sites. Its end column adds
`subj->block_offset`, which is the amendment §2 specifies. `make_autolink`'s two
columns add `subj->column_offset + subj->block_offset`, which every other column
in `core/inlines.c` already did. **The option bit's `#define` stays** — it is
public surface — but it now has no live use, so Q14's deletion at Step 3 is a
deletion and not a behaviour change, which is what §4.2.3 asked for.

**13 golden rows in `spec.txt`, across 12 examples**, every one reviewed against
the source line by hand: eleven are a multi-line `Code` or `HTML` end moving off
the start line, and example 500 moves two, the `HTML` and the `Text` after it —
`)` goes from `1:17` to `2:5`, which is where the byte actually is. Nothing else
in the corpus moved: `regression_commonmark`, `extensions_gfm` and every
extension fixture were green before regeneration.

**The oracle readings, and §4.2.7's pre-measurement holds exactly.**

| Gate | Before | After | Predicted at 0a.1 |
|---|---|---|---|
| inline sourcepos | 12 | **0** | 12 → 0 |
| scope containment | 58 | **58** | unmoved |
| a position is a place | 132 | **122** | 13 clear, 3 appear |
| scope-sanity | 206 | **207** | +1, recorded exception |

**§4.2.3's "After D3: 1 (spec example 200)" is wrong, and it is wrong in both
directions.** The reading is 12 → 0, not 13 → 1, and spec example 200 — the
table cell with `` `\|` `` in it — agrees with upstream before the fix *and*
after it. Measured twice: once as a throwaway experiment at 0a.1 with the raw
un-gate, once here with the amendment included.

**The three rows that appear are the end-column-zero class**, named in
`specs/positions/places.json`'s `purpose` as 0a.6 was instructed to. A code span
that opens with a line ending now reports `1:3..3:0`: a start one past the end
of a two-byte line, an end naming a column that does not exist — and both halves
are exactly what cmark-gfm reports, which is why the upstream oracle reads zero
over them. Agreeing with an authority that is itself wrong is the intended
division of labour between these two gates, not an oversight.

**The scope-sanity growth is the composition §4.2.2 predicted, to the row.**
`regression.txt` 16 → 17, because D7's fix restores the *column offset* and that
term is zero on the first line of a paragraph — so the witness has to be a
continuation line, and a paragraph with a second line has a `SoftBreak` between
them, and a `SoftBreak` is a sentinel by construction. The file is back where
Stage 0a found it: 17 at the baseline, 16 after 0a.2, 17 here. The ledger's
`purpose` records it as the second exception of the same shape.

**Mutant kills, and one of them exposed a missing gate.**

- **D3's guard restored:** `correctness` 64/66 and the inline-sourcepos oracle
  reports all twelve rows appearing.
- **D7 reverted:** `regression_commonmark` fails on the block-quote example, and
  `audit-scope-containment.mjs` reports **five** rows appearing — the containment
  invariant catching a child that escapes its parent, which is what §4.5 says it
  is for.
- **The `block_offset` amendment dropped: NOTHING CAUGHT IT.** 66/66, both parity
  gates green, all three position oracles unmoved — because `> a `x⏎> y` b`
  ending at `2:1` instead of `2:3` is a *place*, its siblings do not overlap it,
  and upstream is not consulted for it. §2 says the amendment "moves zero
  additional rows"; the flip side, which §2 does not say, is that **it had no
  gate at all**. One `regression.txt` example now pins it, and the mutant kills
  that example.

**D31, found by doing this.** Un-gating makes raw HTML report a line-crossing
tag one column short of its own literal: `a <b`⏎`c> d` gives
`HTML scope=1:3..2:1` for a literal whose last byte is at `2:2`, while
`a <b c> d` gives `1:3..1:7`, which covers it. The cause is the span handed to
`adjust_subj_node_newlines` — raw HTML passes `matchlen` with an `extra` of 1,
which omits the tag's last byte from the newline count while making the
*following* node's column right. **None of the three position oracles can see
it**: the end is a place, the siblings leave a gap rather than overlapping, and
cmark-gfm is wrong the same way.

It is **pinned, not fixed**, and the reasoning is the one 0a.6 follows for the
end-column-zero class: fixing it is a deliberate divergence from upstream on six
spec rows, and §4.2.3's own rule for 0a.0 is that no defect commit smuggles a
divergence decision. It belongs to Step 8's position model. The pin is a
`regression.txt` example with prose above it naming the defect and its owner —
the same shape §4.5 credits for D10, where "the fixture pins the defect and
unpinning it is the gate" — so the six `spec.txt` rows regenerated here are not
the only record of it.

---

#### 4.2.13 0a.7 landed: one rule, three sites, sixty rows, and Q39 taken

**The rule is `null` means not written and `""` means written and empty**, and
the engine stated it three different ways. All three now agree, in one commit,
because landing them apart would have made the middle commit assert `""` for
something the next one calls absence:

- **D6** — `make_autolink` set `title = chunk_literal("")`. An autolink has no
  syntax for a title, so the field is now left as `make_simple` calloc'd it.
  One line deleted.
- **D16a** — `chunk_clone` always allocated, so a NULL-data source became a
  non-NULL `""`. A copy of "never written" is "never written". Four lines.
- **D16b** — `markdown_core_parse_reference_inline`'s no-title branch wrote
  `chunk_literal("")`. Behaviour-neutral today, because `clean_title` folds
  length-0 to `CHUNK_EMPTY` before the map sees it, and taken anyway so the rule
  is stated once at every site rather than compensated for downstream.
- **D5** — the title-rewind path un-read the title and left `title` holding the
  scanned chunk, which went into the reference map. The bytes were stated twice,
  once as paragraph prose and once as a title. It now records absence — and
  reads `title=null` from the first commit rather than `""` for one commit and
  `null` at Step 14, which is the whole reason the four land together.

**60 rows moved, and 59 of them are one substitution.** `title=""` →
`title=null`, in `spec.txt` (54), `extensions.txt` (4) and `regression.txt` (2),
plus one assertion string in `tests/api/main.c`. §4.2.6's table predicted 54 and
4; the two extra are 0a.6's own autolink examples, which carried `title=""` when
they were written a commit ago. The hand review is therefore complete in a way a
58-row diff usually is not: a script confirmed that **exactly one** row differs
by anything other than that substitution.

**That row is Q39, and it is taken here.** `spec.txt` example 169, `[foo]: <>`,
goes `destination="" title=""` → `destination=null title=null`. The destination
*was* written and was empty, so `""` is arguably right — but
`markdown_core_clean_url` folds a zero-length destination to `CHUNK_EMPTY`
before it ever reaches the map, so `<>` is already indistinguishable from *no
destination* by the time the reference path sees it, and the inline path already
answers `[a](<>)` with `destination=null`. **The decision is consistency, not
correctness**, and the limit is stated in the ledger: separating the two truly
requires the folds to stop, which is Step 14's, and this is the one corpus input
that will move again there.

**Two numbering faults found and fixed while taking it.** Q39 was written as
**Q32**, and §11.8's inventory already owns Q31–Q36 — its Q32 is snapshot
ownership, answered and cross-referenced from Q35. The pair is renumbered to
**Q38** (D13's `empty-text-node` divergence, still owed by 0a.14) and **Q39**,
and both are now in §4.1.6's ledger, which is where §4.2 said they were and
where they were not. §4.2's own table claims 0a.0 took them; **it did not**, and
the row now says so.

**Mutant kills, one per site, and D5's kills in two different ways.**

- D6 restored: **20 examples red** — 17 `spec.txt`, 1 `extensions.txt`, 2
  `regression.txt`.
- D16a restored: **37 examples red** — 35, 1, 1.
- D5 restored: `regression.txt` red **and** `check-upstream-parity.mjs` at
  807/809 with `registered divergences: 4/5` and the message *"registered
  divergence `refdef-title-rewind` no longer reproduces: the two now agree"*.
  A gate that notices a registered difference disappearing is worth more here
  than one that notices a new one, because upstream keeping the title is the
  thing this fix is deliberately unlike.

**The goldens are the only mechanism that can hold D6 and D16 down**, and that
is measured rather than assumed: `scripts/lib/upstream-cmark.mjs` maps
`title:""` to `"null"` before comparing, on all three parity oracles, so no
parity gate can ever see the difference. That is exactly why the 60 rows are
regenerated once, deliberately, in a commit whose subject says so.

**`refdef-title-rewind` is now a registered delta**, moved out of
`pendingDeltas` — 809/809 with 5/5 divergences reproducing. Its `pendingStep`
said Step 9; §4.2's defect schedule moved it to 0a.7 and the entry records that.
The mdast corpus note at `corpus.md` is amended in the same commit to say the
title half is fixed and only the node model remains, so the next reader does not
re-derive D5 from a row that still diverges for a different reason.

---

#### 4.2.14 0a.8 landed: D9 pinned, and the trade is now measured on both sides

**No engine change.** One comment at `core/map.c`, two gates, and one README.

**The trade, measured rather than argued.**

| | order-independent | output bounded |
|---|---|---|
| today, with the budget | **no** — 100 of 200 identical references resolve | yes — **0.999x** |
| the guard deleted | yes — D9's witness resolves | **no** — **204.678x** |
| Step 9a's model | yes | yes |

Both middle-row numbers were taken by deleting the three-line guard and
rebuilding: `[b]` starts resolving after an unrelated `[a]`, and 656 KB of input
starts producing **134 MB** of copied destinations. That is the whole argument
for why D9 is the one defect Stage 0a pins: the budget buys the bound *by*
breaking resolution, so neither gate can be satisfied by giving up the other,
and a reference that NAMES its definition instead of copying it is the only
thing that reaches both.

**Gate 1 —
`scripts/audit-reference-order-independence.mjs`, REGISTERED RED.** Two
properties, both failing, both named to Step 9a:

- **uniform:** 200 references to one label are identical, so all must resolve or
  none must. **100 resolve, 100 degrade to text.** §2 records this as *"200 refs
  → 99 resolve, 101 do not"*; measured at a 1000-byte destination it is 100 and
  100, because the guard admits a lookup while `ref_size + r->size <=
  max_ref_size` and 100 × 1000 is exactly the 100 KB floor. The split moves with
  the destination's length; **that it splits at all** is the defect, and the doc
  should not have pinned a number that depends on a parameter it did not state.
- **independent:** `[b]` resolves alone and does not after an unrelated `[a]`
  spends the budget. The contamination crosses labels, which is what makes this
  a resolution defect rather than a size limit.

**It fails when a row STOPS reproducing**, which is the point and is verified:
deleting the guard clears both rows and the gate reports them as `CLEARED` with
*"a row that moved is a behaviour change"*. A gate that only caught the defect
appearing would be satisfied for the wrong reason the day someone deletes the
budget — rows clear, every other suite stays green, and the engine quietly
produces 134 MB from 656 KB.

**Gate 2 — `reference_expansion_bound` in `complexity_runner.c`, GREEN.** The
runner measured only *time* before; this case measures *output size*, summing
every resolved destination and title in the tree and requiring the total to stay
under 8× the input. It reads **0.999x** today and **204.678x** with the guard
deleted, so it is a real bound and not a formality. `correctness` goes 66 → 67.

**And the statement lives beside the code.** A 25-line comment at the guard says
what it buys, what it costs, that deleting it is measured and is not the fix,
and which two gates hold the two halves — so a reader who arrives at three
suspicious lines without this document finds out why they are there before
removing them. §2's citation `map.c:307` still lands on the guard.

---

#### 4.2.15 0a.9 landed: recognition is a question about the source

**One condition.** The branch tested the DECODED first byte, so `[\^a]` and
`[&#94;a]` opened footnote calls — and neither could ever resolve, because the
label was reconstructed from a different coordinate space than the lookup key.
What they produced was a rebuilt `[^` prefix over decoded bytes. It now tests
`subj->input.data[opener->position]`, the byte after the `[` in the source.

**The 432-case matrix, re-measured composed with 0a.2 as §4.2.3 required, and
two of §2's numbers are now stale.** Six caret spellings × eight labels × three
tails × three definition contexts:

| | §2, on the untouched baseline | measured here, after 0a.2 |
|---|---|---|
| cases that move | 252 | **360** |
| rows emitting NUL bytes | 162 | **0** |
| rows emitting invalid UTF-8 | 90 | **0** |

The zeroes are 0a.2's doing, not 0a.9's: the label became a slice of the source
there, so the heap bytes were already gone before this commit. **What 0a.9 moves
is spelling, and the split is exactly the caret** — all 72 raw-caret cases are
untouched, and all 72 of each of the five other spellings move. The label and
tail dimensions never affect the decision, which is why the fixture drops them.
§2's *"252 move"* was a measurement of a tree that no longer exists.

**Zero pre-existing golden rows moved.** The two rows that did are 0a.2's own
fixtures, whose registered `pending` note said this commit would move them:
`[\^abc] x` goes `[^^abc] x` → **`[^abc] x`**, and `x [&Hat;abcdefghij] y` goes
`x [^Hat;abcdefghij] y` → **`x [^abcdefghij] y`**. Both are now simply the
decoded text, which is what an escape and an entity are for.

**The reduced matrix is one example and it reads at a glance**: six spellings of
one label with the definition present, as six paragraphs. Exactly one
`FootnoteReference` appears and the definition resolves to it; the other five
decode to the identical `a[^n] b` and open nothing. They are separate paragraphs
rather than one, deliberately — one paragraph would add five `SoftBreak`
sentinels to a ratchet whose whole purpose is to shrink.

**Upstream's answer for that input is worse than a spelling difference**, and
registering it is what turned 809/809 into 810/810 with a sixth divergence:
cmark-gfm emits `a[^n]] b` for the escaped one and then **a single empty text
node for the rest**, losing four paragraphs outright.

**Mutant kills, and one honest negative.** Reverting to the decoded byte fails
three regression examples. Putting the bounds test *after* the subscript kills
**nothing** — 58/58 under ASan — and the comment now carries the proof of why:
reaching this function means a `]` was consumed after the `[`, so
`opener->position <= initial_pos - 2 < subj->input.len`. The guard is redundant
and kept anyway, because a reader should not have to reconstruct that argument
before touching the line. Saying "no mutant kills it" beside it is better than
implying one does.

**Half of Step 9a's rule is discharged.** "Opens with a raw `^`" is settled here.
"And the document defines that label" stays 9a's, because it is a model question
about what a failed call becomes — and this commit measured the premise §2 gave
for deferring the whole thing: **no escaped or entity-spelled call resolved at
the baseline either**, across all 144 matrix cases with a matching definition.
The narrowing removed broken behaviour only, so §2's *"a policy move, not a
repair"* does not survive, exactly as §4.2.4 says.

---

#### 4.2.16 0a.10 landed: a closing fence closes the block, and three sites not two

**D21 — one constant, one extracted helper, one threaded out-param.**
`MARKDOWN_CORE_BLOCK_CLOSED` is public extension API; `S_set_end_to_current_line`
is lifted out of `finalize`'s middle branch byte-identically, so the document, a
closed fenced code block and a setext heading go on saying what they said and an
extension container can now say it too. `parse_extension_block` gained a
`should_continue` out-param exactly as `parse_code_block_prefix` already had one,
and on `CLOSED` it closes every block still open inside the container, then the
container, then positions it at the fence's line.

The whole defect was that `directive_block_matches` marked `closed`, consumed
the fence, and **returned 1**. The container stayed open, so the next non-blank
line arrived as a lazy paragraph continuation:

```
:::note      before:  DirectiveBlock 1:1..4:5 > Paragraph 2:1..4:5
body                    Text "body", SoftBreak, Text "after" at 3:1
:::          after:   DirectiveBlock 1:1..3:3 > Paragraph 2:1..2:4
after                 Paragraph 4:1..4:5 > Text "after"
```

**D22 — three sites, and §4.2.3 says two.** The primitive is right as described:
`markdown_core_inline_parser_set_offset` now counts the newlines it moves over
and updates `line` and `column_offset` in the same frame `handle_newline` and
`adjust_subj_node_newlines` use. Only forward moves count, because autolink
rewinds through the same call and a rewind is inside the current line by
construction.

What the doc undercounts is directive's side. `make_name_only_directive` and the
attributes branch are the two obvious sites, and fixing only those left
`Directive 1:1..1:29` unchanged. **The labelled form takes its end from a third**:
`make_delimiter_text`, which builds the `]{...}` closer — and a label closer
carries its attributes in that literal, so `]{title="one⏎two"}` is a *delimiter*
that spans a line ending. `insert_label_directive` then takes the whole
directive's end from that node. With all three reading their end back after the
consume, the result is exactly §2's oracle: `Directive 1:1..2:5`,
`Text 2:6..2:10`.

**Three golden rows moved and §4.2.3 named two of them.** Both predicted rows are
`extensions-directive.txt` example 16, where the inner `:::spoiler` ended at the
**outer** `::::` fence's line — the golden was wrong. The third is
`extensions-conflicts.txt` example 2, which did not exist when §4.2.3 was
written: it is 0a.5's own fixture, and its inner paragraph ran to the fence line
for the same reason. The `specs/canonical-ast/structure.ast` row §4.2.3 predicted
also moved, `Paragraph 8:1..9:3` → `8:1..8:4`, and only the `conformance` preset
catches it.

**D22 was completely unpinned**, which §4.1's verdict calls the finding: zero
existing golden rows move for it. Three new examples now pin the pair — the
fence case, the fence case inside a block quote, and the line-crossing attribute
— and the mutant proves they bite.

**Mutant kills.** Returning 1 instead of `CLOSED` reddens **four** examples
across two fixtures and **both** conformance tests. Making `set_offset` stop
counting newlines reddens exactly the new D22 pin — one example, which is what
"completely unpinned" means once a pin exists.

**Backlog 23 → 24, authorised in advance and named.** `extensions-directive.txt`
is mdast corpus, so D22's pin is compared against remark, and it diverges for
the **pre-existing** attributes-JSON and label-shape gap: this engine emits
attributes as a JSON string and has no visible `DirectiveLabel` node. D22 is what
makes the input's *positions* right, and positions are not what that gate
compares. The entry says so and names Step 7.

**One claim in §4.2.3 is stale and is corrected here.** It says the lifetime
invariant — that `container` survives its own `finalize` because an
extension-minted type is never `PARAGRAPH` — cannot be policed by ASan *"the CLI
is the arena path"*. **0a.3 already retired that**: the fixture runner allocates
through malloc, so `correctness-asan` runs the whole directive corpus over this
path. The invariant is asserted in the code as well, which is belt and braces
rather than the only mechanism.

---

#### 4.2.17 0a.11 landed: one attach path, and the order is what was worth having

**What is in the engine.** `markdown_core_core_extensions_attach(parser, mask)`
in `extensions/core-extensions.c` walks one ordered table and is the **only**
call to `markdown_core_parser_attach_syntax_extension` in the shipped library.
Both product entry points now pass a **set** and cannot express a sequence:
`extensions/ast.c` turns its eleven `options->` booleans into bits, and
`core/main.c` turns its profile, its `--directive` flag and every `-e NAME` into
the same bits. The order is `strikethrough → autolink → tasklist → formula →
directive → table`, which is Q9.

**D24 is one read.** `open_tasklist_item` took `checked` from `strstr` over the
whole line and now takes it from `input[first_nonspace + 1]`, the second byte of
the marker `scan_tasklist` just matched. The read is in range by the scanner's
own rule (`("[ ]"|"[x]"|"[X]")spacechar+` ⇒ `matched >= 4`), asserted under
`#ifndef NDEBUG` the way D4's is.

**The `-e NAME` by-name path is DELETED, not routed.** §4.2.3 asks for `-e` to
be *routed through the same bit table*; the honest reading turned out to be
stronger. `attach_syntax_extension` and `parser_has_syntax_extension` in
`core/main.c` existed to attach a name the bit table does not know — and no such
name can exist in this product: the registry is populated by exactly one plugin,
`core_extensions_registration`, so `markdown_core_find_syntax_extension` can
only ever answer with one of the six. The fallback was unreachable code whose
only effect was to keep a second attach site alive. `-e bogus` still prints
`Unknown extension bogus` and `-e` with no argument still prints `No argument
provided for -e`; both now happen in the argument pass, before the parser
exists.

##### What the reorder is worth, measured on this tree rather than quoted

Over **2,744 ordered triples of 14 significant lines** (prose, `:::note`, `:::`,
`::name`, `:inline{a=1}`, `$$`, `x`, `\[`, a table header, a delimiter row, a
task item, `~~gone~~`, a bare URL, and a line claiming `:` twice), each parsed
through a probe that attaches by name so the three orders can be compared
directly:

| | differ | of 2,744 |
|---|---|---|
| CLI's old order vs facade's old order | **4** | 0.1% |
| facade's old order vs the shipped order | **6** | 0.2% |
| CLI's old order vs the shipped order | **2** | 0.1% |
| **CLI binary vs facade, after the fix** | **0** | 0.0% |

**§4.2.1's `414 (15.1%)` was NOT re-measured, and the reason is stated rather
than hidden**: reproducing it requires reverting 0a.5, and the 4 above is the
same experiment on the tree as it stands. What the pair of numbers says is that
**D8's fix already took about 99% of the CLI/facade gap** — which is exactly
0a.5's warning read from the other side.

**All six inputs the reorder moves are one shape**, and it is the shape Q9
names: *a line inside an OPEN table that a narrower extension also claims.*
`| a | b |` / `| - | - |` / `:::note` under the old facade order becomes a table
row holding `Text "::"`, an inline `Directive`, and an autocompleted empty cell;
`$$ / x / $$` after an open table becomes three one-cell rows. **D8 does not
touch this class.** D8 answers the case where table's opener *declines*; nothing
but the order answers the case where its row matcher *succeeds*, because inside
an open table every line is a candidate row.

**Q15 gets a negative result it should have before Step 3 spends effort on it.**
§4.1.6 recommends `autolink` before `directive` on the grounds that both claim
`':'`. That order is preserved here, but it has **no witness**: over 12 hand-built
collision candidates and 4,000 random documents drawn from `:`/URL/attribute
fragments, moving `directive` to first or to the middle changes **0** outputs.
Only `table`'s position is observable. Step 3 should keep the recommendation as
a tie-break and stop describing it as a fix.

##### The three obligations, all discharged

1. **`tasklist-checked-marker` needed a corpus example, and it did.** The
   registered input `- [ ] call me [x] later` was in no fixture, so the JSON edit
   alone would have failed `check-upstream-parity.mjs`'s "no longer in the
   corpus" check — verified, because that is precisely how the D24 mutant below
   fails. The example is appended to `extensions.txt`'s task-list section, which
   is the END of the file, so **no existing line number moved** and the three
   position ledgers (keyed by `file:line`) were untouched. The entry moved from
   `pendingDeltas`/`pendingExpectedDivergences` to `deltas`/`expectedDivergences`;
   its `pendingStep` said *"Step 4c"*, a step number that stopped existing at
   §4.0's re-ordering, and it is dropped rather than corrected.
2. **0a.5 already paid obligation 2**, and this commit confirms rather than
   repeats it.
3. **Both of 0a.5's gates were re-read after the reorder, and both answered as
   0a.5 predicted.** With the `scan_table_start` decline reverted to
   `return parent_container;` — the narrowest D8 mutant, confirmed live by
   watching the directive block fail to open through the facade —
   `extensions_conflicts` reads **4 passed / 0 failed** where at 0a.5 it read
   0/2, i.e. **the fixture went vacuous exactly as its own paragraph warned**;
   and `api_engine` **fails**, `712 tests passed, 2 failed`, naming
   `an extension attached after table still gets its turn` and
   `a declining table does not swallow the directive block`. The fixture's
   warning paragraph is rewritten in place to say it *has* gone vacuous and that
   the api test is now the only holder of that property.

##### The gates, and one mutant nothing else catches

The vacated ground is re-occupied rather than abandoned. `extensions-conflicts.txt`
gains a second section, **"An open table must not swallow another extension's
block opener"**, holding the two witnesses above. They are the order's gate:

| Mutant | `extensions_conflicts` | `api_engine` | `extensions_gfm` | upstream parity | attach-order audit |
|---|---|---|---|---|---|
| **A** — revert one of D8's six declines | 4/4, **vacuous** | **FAIL**, 2 named | pass | pass | pass |
| **B** — put `table` first in the table | **2 passed / 2 failed** | pass | pass | pass | **FAIL** |
| **C** — restore D24's `strstr` | pass | pass | **FAIL** | **FAIL**, the divergence stops reproducing | pass |
| **D** — a second attach site in `markdown_core_document_parse` | pass | pass | pass | pass | **FAIL, alone** |

**Mutant D is the finding.** Re-introducing a second attach site — which is D15
verbatim — leaves `correctness` at 67/67 and `conformance` at 2/2, and is caught
by nothing but the new audit. The reason it is invisible is structural and
cannot be fixed with a corpus row: **every fixture in this repository runs
through the facade**, so a fixture can only ever observe one of the two orders.
`conformance` looked like it should catch it — `facade_native` and
`facade_dump_cli` compare the *facade* and the *CLI* against the same six
canonical goldens — but the six canonical inputs contain nothing order-sensitive,
and on the 2,744-triple set the particular order mutant D installs is itself
output-neutral. That is D2's situation again: a real invariant with no output
signature, gated by reading the source.

So **`scripts/audit-extension-attach-order.mjs`** (≈100 lines, no new
dependency, registered in `package.json` and in CI beside
`audit:extension-special-chars`) asserts three things:

1. `markdown_core_parser_attach_syntax_extension` is called from exactly one
   function in `core/` and `extensions/`, and that function is
   `markdown_core_core_extensions_attach`. Tests are exempt on purpose —
   `extension_decline_yields_turn` attaches by hand precisely so that it keeps
   failing under any order.
2. The table names every extension `core_extensions_registration` registers,
   exactly once, so a seventh extension cannot become attachable without being
   given a position.
3. It ends with `table` (Q9).

##### What moved, and what did not

**Three golden examples added, zero golden rows changed.** One in
`extensions.txt` (D24's divergence pin) and two in `extensions-conflicts.txt`
(the order witnesses), all appended, all reviewed against the source columns by
hand. Nothing pre-existing moved, which is what §4.2.1's *"zero golden rows"*
predicted for both defects — and it is also the reason the reorder needed a new
fixture at all.

Gates after: `correctness` **67/67**, `correctness-asan` **58/58**,
`correctness-ubsan` **58/58**, `conformance` **2/2**, upstream parity
**816/816** with **7/7** divergences reproduced (813 → 816 is the three new
examples), mdast **54/54** with the backlog unmoved at **24/24**, fuzz-parity
**300/300**, scope-sanity **207** and the three position oracles **0 / 58 /
122** — every one of them unchanged, which is the right answer for a commit that
adds no position and removes no node. `audit-source-lists.mjs` throws on a
missing `packages/swift-markdown-core/Package.release.swift`; it throws
identically at `f98fefe`, so it is one of §0's untriaged-by-era items and not
this commit's.

---

### 4.3 The ordering argument

**The old plan said: "Step 3 must come before every later step."** That sentence
was an assumption with no experiment behind it, and it is now falsified where it
mattered most.

The experiment: clone the baseline, write all eleven fixes minimally at exactly
the sites §2 names — **74 changed lines across six files** by
`git diff --numstat`, which counts an amended line as one added plus one removed
and includes D9's seventeen-line budget deletion, i.e. the same thirty-nine
edits the table above counts once each — then replay all 53 per-file patches of
`e95aa17`'s `core` + `extensions` diff with `git apply --reject`.

- **Control** (pristine baseline, no fixes): 52/53 files clean, **1 reject** —
  `core/main.c`, from Step 1's already-landed `--profile` flag. That is the
  noise floor.
- **Treatment** (baseline + 11 fixes): **8 rejects in 7 files — 7 new, out of
  358 hunks = 2.0%.** Not one is semantic. Every one is "the patch's context
  lines moved."

The reason the claim looked true is that `e95aa17` was read as one commit. It is
three programs:

| category | hunks | ± lines | what it is |
|---|---|---|---|
| **Step 2 — the formatter** | **208 (58%)** | 1,296 | pure brace insertion; `.clang-format` gains exactly one line, `InsertBraces: true` |
| the session program (§7 DROP-1, dropped whole) | 8 | 940 | new files only |
| **Step 3 proper** | 142 | 1,927 | ~135 hunks / ~1,792 lines after subtracting `ast.c`'s session rewiring |

**Five of eleven defects — D3, D4, D5, D10, D11 — are entirely invisible to Step
3**: not one Step-3 hunk touches their regions. Three more — D6, D7, D9 —
collide only with the *formatter*, and the formatter is not a patch. §7 DROP-3
already says "take the config, not the patch", and `clang-format` run over a tree
is a fixpoint operation that cannot conflict with anything. **Three of the seven
new rejects disappear the moment Step 2 is run instead of replayed.** The
remaining four are `formula.c`, `directive.c` and `table.c` ×2, and all four
resolve to *writing `.emphasis = false` instead of `.emphasis = true`* and
*omitting `'}'` from an array literal*. Twenty minutes.

**Wasted work if defects go first: exactly one line.** D8's
`return` at `table.c:365` sits inside `if (markdown_core_arena_pop())`, which
Step 3 deletes along with the arena.

**And the counterweight runs the other way, twice.**

1. **Regression by transcription.** Step 3 does not modify the mechanism D1
   names — `ext->emphasis` is read in exactly one place,
   `markdown_core_manage_extensions_special_characters` (`blocks.c:504-518`),
   feeding `markdown_core_inlines_add_special_character`, whose
   `if (emphasis) parser->skip_chars[c] = 1;` Step 3 leaves alone. What Step 3
   does is *retype the decision*: `set_emphasis(ext, 1)` becomes
   `.emphasis = true` in a static initializer. Whoever writes those descriptors
   mechanically from the 1.0 baseline will faithfully restore the defect, and
   nothing in the suite will notice, because 795/795 stays green over it. This is
   not hypothetical: the historical fix `7c5025d` got the shape right and still
   carried half of D1 forward on a false premise. **Defects first means the
   descriptor author transcribes an already-correct source.**
2. **D8's statement decays.** `try_opening_table_header` has eleven
   `return parent_container;` at the baseline and **ten** after Step 3 —
   measured on the patched tree. The path Step 3 deletes is *live*:
   `markdown_core_arena_push()` returns early when the parser has no arena and
   `markdown_core_arena_pop()` then returns 0, so the retry block executes **only
   when the parser uses the arena allocator**, which `core/main.c:238` does in
   every non-`DEBUG` build — and `CMakePresets.json`'s `default` preset is
   Release. **The retry always runs in the binary the oracles drive and never
   runs in the library the bindings use.** Fixing D8 after Step 3 means fixing a
   defect whose statement no longer matches the code, written by someone who
   will never see that fork. Defects-first documents the path before it is
   deleted.

**Where the claim is defensible: Steps 6, 7 and 8.** Step 3's `inlines.c` hunks
land in `process_emphasis`, `get_extension_for_special_char`,
`extension_has_special_char`, `try_extensions` and
`find_extension_opener_for_special_char` — precisely the delimiter machinery
Step 8 rewrites — and Steps 6 and 7 rewrite the `create_*_extension` functions
Step 3 replaces. **"Step 3 before 6–8" is real. "Step 3 before 4" was never
tested and is false.** Amend the note accordingly.

**Step 5 does not depend on Step 3 either.** All four `core/iterator.c` hunks in
`e95aa17` are brace-only.

**One thing this re-ordering forces into the open, and it is a gain.** Step 3 as
shipped is **not behaviour-neutral**, and two live changes ride inside it with no
oracle pinned to either: the arena removal (which changes the Release CLI's
allocator and deletes the table retry path above), and deleting
`enable_safety_checks`, which makes `node.c`'s O(depth) ancestor check
**unconditional** where it was off by default. Neither is a rename. Both are now
Step 3a, per R1: a behaviour change lands in a step that names it.

### 4.4 Golden-regeneration accounting

**The eleven defects move 32 golden rows in total**, and under the revised order
all 32 are regenerated once, against the baseline engine, with §2's statement of
the defect in hand:

| Defect | File | Rows | Also |
|---|---|---|---|
| D3 | `tests/fixtures/spec.txt` | 13, across 12 examples (91, 345–347, 500, 634–635, 644, 659–662) | example 500 moves 2 rows because the following `Text` moves with the span |
| D6 | `tests/fixtures/spec.txt` | 17 | + 1 row in `extensions.txt`, + 1 assertion string in `tests/api/main.c:1075` |
| D10 | `tests/fixtures/regression.txt` | 1 (example 24, line 474) | + 1 `specs/scope-sanity/ledger.json` row |
| D1, D2, D4, D5, D7, D8, D11 | — | **0** | — |

Seven of the eleven move **nothing**, which is the finding, not the relief: they
move nothing because nothing in the corpus can see them. `specs/canonical-ast/*.ast`
is untouched by all eleven, and `ctest --preset conformance` stays 2/2.

**What the old order cost, stated three ways.**

1. **Step 14 was going to move rows Stage 0a moves anyway.** In the old order D6
   sits in Step 14, the last engine step, behind Steps 3, 5, 6, 7, 8, 9, 10, 11,
   12 and 13. Those 18 rows are pure duplicate: regenerated at Step 14 for a
   defect that could have been closed at the baseline for one deleted line. Under
   the revised order Step 14 keeps only its enforcement job — the rule made structural,
   plus D16's two remaining sites — and its engine diff drops from *fix and
   regenerate eighteen rows* to *fix two sites and enforce*.
2. **The reviewer's reference is a golden known to be wrong and not yet
   scheduled.** The standing gate says every moved row is reviewed by hand and
   named in the commit message. Reviewing a moved row means deciding whether the
   new value is *right*, and deciding that requires knowing what right is. For
   these 32 rows the only available answer, at every regeneration between now
   and the defect's assigned step, is "unchanged from before, therefore fine".
   That is exactly how `Code scope=1:9..1:17` — a column past the end of a
   twelve-character line — survives ten steps of hand review. `spec.txt` is
   regenerated by Steps 6, 7 and 8 at minimum (R1 originally named Steps 4, 6 and
   7 as output-moving; §4.1 has Step 8 carrying four syntax fixes) and again if the
   dump vocabulary moves at Steps 11–12 (§9 Q5 renames `id=` → `label=`).
3. **One row is not merely stale — it is pinned backwards.**
   `tests/fixtures/regression.txt:474` currently asserts
   `Text scope=0:0..0:0 literal="[^~~is~~1]"` as the **expected** output. The
   corpus asserts an impossible position as correct, and the suite is green
   because of it. Under the old order that pin stands until Step 9. Under the
   revised order it is unpinned at 0a.2 — and the fixture flips from defending
   the defect to killing it, which is the difference between a test and a
   ratchet.

**Corpus growth.** Stage 0a adds ~13 fixture examples. Ten of them land in files
that are also the upstream-parity corpus (`specs/upstream-parity/deltas.json`
`corpus`), so `check-upstream-parity.mjs` goes from 795 inputs to roughly 805,
with one registered divergence activated (`refdef-title-rewind`, measured
796/796 with 1/1 reproduced), one new `expectedDivergence` for D10's byte
retention, and one **model** delta extension for D11 — prefer extending
`applyUpstreamFootnoteModel` in `scripts/lib/upstream-cmark.mjs` over an
input-keyed entry, because *"upstream keeps the winner, this engine keeps both"*
is a rule, not a point difference.

### 4.9 The history is closed, except for six test oracles

**Owner ruling, 2026-08-20:** *"If your plan is to borrow code or cherry pick
something from current main, you are BS me. You should ignore any existing
commit except the formula and directive syntax fix."* Tightened the same day:
*"forbid any main commit read except the whitelisted commit for formula/directive
syntax fix. The whitelist should also be specified to test oracles only, that
tells us what the issue need to fixed is."*

**The rule, in full.**

> No commit in this repository's history after `580d10c` may be read — not its
> diff, not its code, not its message — with one exception, whitelisted below by
> **path and commit**. The whitelist admits **test oracles only**: fixture files,
> whose expected output states what correct behaviour is. Implementation is not
> whitelisted anywhere, at any commit, for any reason.

**Why oracles and not code.** A fixture says *what the engine must do*; an
implementation says *one way somebody did it*. Taking the first is taking a
requirement, which is the thing we lack. Taking the second is inheriting a
design — including its defects, which is exactly how eleven of them survived
into 1.0.3 and how the streaming program accumulated the rest. An oracle also
stays honest in a way a patch cannot: it was written to describe behaviour, not
to make a particular implementation pass.

**The whitelist.** These six paths, at these five commits, and nothing else:

| Path | Whitelisted at | Carries |
|---|---|---|
| `tests/fixtures/extensions-directive.txt` | `8926594`, `752768a`, `3d8d329`, `26045be` | the directive grammar: names, attribute forms, the `#`/`.` shorthand, class accumulation, malformed-attribute degradation |
| `tests/fixtures/extensions-directive-option-gates.txt` | — *(identical at baseline; nothing to read)* | — |
| `tests/fixtures/extensions-formula-github.txt` | `8926594`, `a22f04f`, `3d8d329` | the dollar forms, and the inline-math padding rule |
| `tests/fixtures/extensions-formula-latex.txt` | `a22f04f` | the `\(` and `\[` forms |
| `tests/fixtures/extensions-formula-option-gates.txt` | `a22f04f` | which delimiter sets each option admits |
| `tests/fixtures/extensions-formula-conflicts.txt` | — *(identical at baseline; nothing to read)* | — |

All paths are under `packages/markdown-core/`.

**What the whitelist does NOT admit**, stated because each is a tempting
half-step: the engine hunks in those same five commits; their commit messages;
their review discussion; any other fixture; the goldens of any non-whitelisted
fixture; and `specs/canonical-ast/*.ast`, which is a projection of an engine we
are not rebuilding from.

**Provenance citations confer no permission.** §2 cites commits as evidence that
a defect is real — *"still present in upstream 0.29.0.gfm.13"*, *"fixed in
7c5025d"*. Those are historical annotations recording how a defect was found.
They are not an invitation to open the commit, and a step may not be justified by
"this is what commit X did". A step is justified by what the engine must be true
of, and nothing else.

**What is already in the tree stands.** Steps 0 and 1 landed under the earlier
ruling and are closed: the parity harness, the specs and the operational layer
are here and are not re-litigated. The prohibition governs everything from here
forward.

**Consequences for the plan.** The `[CP]` / `[CX]` / `[HW]` grading is void — it
answered "how hard is this hunk to move", and hunks are no longer moved. Every
step is designed and written fresh against a stated requirement, and §4.1 lists
requirements rather than sources. Step 8 in particular is no longer "port the
delimiter engine or defer it"; it is the open question of what the inline phase
must guarantee and whether meeting those guarantees needs an engine at all.

### 4.11 There are no options

**Owner ruling, 2026-08-20 (Q14):** *"delete all options, it is inherited from
cmark, no meaning to preserve it."*

The whole surface goes — the twelve `MARKDOWN_CORE_OPT_*` bits in the core
header and all eleven fields of the public `markdown_core_parse_options`. The
engine parses Markdown one way.

**Which behaviour survives is not a question for ten of the eleven**, and that
is why most of this ruling costs nothing to execute:
`markdown_core_parse_options_init` sets **every field to `true`**
(`extensions/ast.c`), so the shipped product already has one behaviour and
deleting the switches makes the code say what it already does. Smart punctuation
on, HTML comments classified, both formula delimiter sets live, footnotes and
directives on.

**UTF-8 is the exception, and it is decided the other way.**

> **Owner ruling, 2026-08-20:** *"For UTF-8 we follow cmark's practice, assume
> input is UTF-8 but do not validate it."*

The facade sets `VALIDATE_UTF8` unconditionally today, so this is a real
behaviour change and not a deletion of a switch nobody moved — an earlier draft
of this section claimed the whole ruling was free, and for this option that was
wrong. `markdown_core_utf8proc_check` (`core/utf8.c`), reached from
`S_process_line` (`core/blocks.c:1591`), **rewrites the input**: every invalid
sequence becomes a three-byte U+FFFD before the parser ever sees the line.
Deleting it means invalid bytes reach the tree's literals unchanged.

**The measured consequence is positional, and it is an argument for the ruling
rather than a cost of it.** On `caf<0xE9> x`: validating gives
`Text scope=1:1..1:8`, not validating gives `1:1..1:6`. The six is the truth —
the user's line is six bytes. Validation lengthens it to eight and every column
after the invalid byte then names a place in a buffer the author never wrote.
**Not validating is what makes a position honest about the actual input**, which
is the same principle §5 applies to labels and §11 to spans.

Two things do **not** change, and both matter:

- **NUL still becomes U+FFFD.** That substitution lives in `S_parser_feed`
  (`core/blocks.c`), not in the validator, and it is required by CommonMark.
  Verified: `a\0b` yields `a<U+FFFD>b` with validation off.
- **No fixture in the repository contains invalid UTF-8**, so no golden moves.
  Checked with a strict decoder over all 24 fixture and oracle files, not with
  `iconv`, which fails on macOS for an unrelated ioctl reason and reports two
  false positives. The change is observable only on input this corpus does not
  contain — which is exactly why it needs a fixture of its own, in the step that
  lands it.

Downstream, each binding already repairs invalid bytes on decode — Swift's
`String(decoding:as:UTF8.self)`, JavaScript's `TextDecoder`, and Kotlin's
`String(ByteArray)` all substitute U+FFFD. The engine stops doing a job its
consumers do anyway, and stops corrupting positions to do it.

**Extension attachment is the only remaining lever, and it is not an option** —
it is which grammar the parser was built with, fixed at compile time by the
descriptor table (Step 3), ordered by Q9 with `table` last.

Three consequences:

- **R5 and R6 dissolve.** They asked whether removing `VALIDATE_UTF8` and
  `strip_html_comments` was a cleanup or a product change. Neither is a
  decision now; both behaviours are unconditional.
- **The `--profile` flag added at Step 0 loses its reason.** It exists to select
  an option set for the parity harness. With one behaviour, `gfm` and
  `gfm-extended` differ only by which extensions are compiled in, and the flag
  becomes a build question rather than a runtime one. Step 3 must say what
  replaces it before the harness breaks.
- **The option-gate fixtures stop testing anything** —
  `extensions-formula-option-gates.txt` and `extensions-directive-option-gates.txt`
  describe gates that will not exist. They are whitelisted oracles (§4.9), so
  they are read for the *grammar* they pin and then retired, not carried.

### 4.12 Every defect is fixed before any other task

**Owner ruling, 2026-08-20 (Q25):** *"fix all defects before start any tasks."*

This generalises the question that was asked. It is not only D16's two sites:
**every one of the twenty-five defects that can be fixed on the untouched
baseline moves into Stage 0a**, including those §2 currently assigns to Steps 3,
5, 7, 8, 9a, 10 and 14.

Ten were already proved fixable there (D1–D8, D10, D11), and D17 is fixed. The
remaining fourteen — D12, D13, D14, D15, D16, D18, D19, D20, D21, D22, D23, D24,
D25 — must each be put to the same test that settled the first ten: **applied to
the untouched baseline with no other step landed, built, run against every gate,
and reverted.** A defect that passes that test belongs in Stage 0a. A defect
that fails it is a defect with a *real* architectural dependency, and it must be
named with the dependency and pinned by a known-red gate meanwhile, exactly as
D9 is.

**D9 remains the one known exception**, and its exemption is measured rather
than assumed: its budget is the only thing between a resolved reference and
68.7 GB of output from 1 MiB of input, because resolving a reference copies the
destination into the node. It is fixed by deleting the copy, which is 9a's model
change, and by nothing smaller.

### 4.13 Atomic append — the mechanism, and what it costs

Q34 is settled: `append(chunk:) throws`, and under value semantics that makes the line's work a transaction. §11.8 states the property and then hands this section the bill — *"every allocation-failure point inside a line must either be moved before the first mutation, or be undoable."* Four subsystems were swept independently against the working tree at `c1a7201` with a `markdown_core_mem` that refuses the *k*-th allocation of one line and then serialises what survived: the block phase, the inline phase, the six extensions, and the allocation layer with the late-resolved maps. **Citations below are `file:line` relative to `packages/markdown-core/`, pinned to `c1a7201`, and re-verified against the tree while this section was written** — three citations carried in the sweeps were off (`add_child` is at `core/blocks.c:471-489`, not 1477; `last_block_matches` at `core/blocks.c:1170`, not 1200) and are corrected here.

The headline from the sweeps: of 107 allocation-failure points reachable inside one line of a mixed corpus, **zero leave the parser standing where it stood.** 53 diverge in tree, buffer or map state; the other 54 differ only in sticky poison bits, which nothing in the engine ever clears. The engine's OOM strategy today is the exact inverse of the contract — poison the parser (`core/blocks.c:1586` short-circuits every later line) and destroy the document (`core/blocks.c:1697-1704` frees the root and returns NULL).

---


> **Verified independently before this section was accepted**, against the tree
> at `c1a7201`: `check_open_blocks` does call `finalize` on a closing fence
> (`core/blocks.c:1127`); `__OPEN` is cleared at `core/blocks.c:366` while the
> fallible info-line decode sits at `core/blocks.c:410`, well after it;
> `parser->line_number++` at `core/blocks.c:1624` is the first durable write on
> the line path; and `finish` on a terminal loss frees the root **and** resets
> the parser (`core/blocks.c:1699-1701`), so today a lost allocation destroys
> both the document and the parser that could have retried.

#### 4.13.1 The verdict

**Yes. Append can be made atomic, and the mechanism costs the line — not the document.** Three qualifications, each named rather than assumed.

1. **Atomicity is bought by ordering, not by bookkeeping.** A line's failure becomes free if every fallible operation happens before the first irreversible one. That reordering is possible everywhere in the block phase, and in the one place where it is not — the inline parse of a closing block, whose allocation count is not a function of its length — the work is discardable instead, at a cost proportional to what it built.

2. **Atomic per *line* is free; atomic per *call* needs one added mechanism.** A chunk may contain many lines, and ordering alone leaves a failed multi-line call standing at an interior line boundary. Rolling those lines back needs a copy-on-first-touch record of the nodes the *call* has touched — Θ(open depth + nodes the call created), freed at commit, never a function of the document. It is armed only when the chunk contains more than one line ending. Recorded as **Q37**.

3. **Three classes of operation cannot be undone at any price below O(block), and every one of them is already scheduled for deletion by a step on this list.** They are exactly the operations that destroy source bytes: `strbuf_drop` (`core/blocks.c:353`, `:422`), `chunk_buf_detach` of a block's content (`core/blocks.c:425`, `:430`), and the node frees and retypes at `core/blocks.c:393`, `extensions/table.c:369`, `extensions/formula.c:535`. Steps 8, 9a, 9b, 11a and 11c delete all of them, because a retained normalized source with literals expressed as byte ranges leaves nothing to destroy. **The exception list is non-empty before Stage 0 and empty after it**, which is the strongest thing this analysis found: atomicity is not a new subsystem, it is a property that falls out of work already assigned, provided the steps are told to preserve it.

**What a caller sees when the throw fires.** `document` is unchanged and readable — it is an owned value (Q32), not a view into the parser. The parser behind it stands at the byte offset where the call began; a retry with the same chunk is exact. Nothing is poisoned: the failure is a fact about one call, not a property acquired by a buffer, so a retry after memory is freed elsewhere succeeds. The error names allocation failure and carries no partial tree. **The one failure the contract cannot cover is the one that is not a throw**: `abort()` and stack exhaustion. The engine's only `abort()` is the arena's (§4.13.10), and it is deleted at 3a.

---

#### 4.13.2 The boundary, stated exactly

The whole design is the position of one line in the code, so durable state is defined first. **Durable** is state that survives the call and is observable by the next append or in the document:

| # | Durable state | Where |
|---|---|---|
| 1 | Parser scalars carried across lines: `line_number`, `last_line_length`, `total_size`, `linebuf` and its size | `core/blocks.c:869`, `:896-909`, `:1624`, `:1645-1650` |
| 2 | The tree: links, `__OPEN` / `__LAST_LINE_BLANK` / `__LAST_LINE_CHECKED`, `end_line` / `end_column`, the `as` union, each open block's `content` bytes | `core/blocks.c:366-380`, `:1481-1499`, `:284-297` |
| 3 | The late-resolved maps: `refmap->refs`, `map->size`, `entry.age`, `map->ref_size` | `core/references.c:52-57`, `core/map.c:307-309` |
| 4 | The sticky failure bits: `parser->oom`, every `strbuf.oom`, `map->oom` | 92 write sites engine-wide |

Everything else in the parser is per-line scratch, reset unconditionally at `core/blocks.c:1607-1614` — `offset`, `column`, `first_nonspace`, `first_nonspace_column`, `indent`, `blank`, `partially_consumed_tab`, `thematic_break_kill_pos`. It is not part of the transaction.

**Today's first durable write, per path:**

- **On the chunk path, before any line begins:** `parser->total_size += len` (`core/blocks.c:869`) and `markdown_core_strbuf_put(&parser->linebuf, …)` (`core/blocks.c:896`, `:905`, `:907`, `:909`).
- **On the line path:** `parser->line_number++` (`core/blocks.c:1624`).

Everything above 1624 is already the shape this section argues for: fill the scratch line buffer, test it, and **return before anything durable moves** (`core/blocks.c:1592-1605`). That is the one place in the engine that already gets it right, and it is the template.

There is one violation of the ordering inside the matching phase, and it is the only one: a closing code fence finalizes its block from inside `check_open_blocks` (`core/blocks.c:1125-1127`), which clears `__OPEN`, writes the end position, detaches the content buffer into `as.code.literal` and moves `parser->current` — all before `houdini_unescape_html_f` at `:411` can fail. Measured: refusing that single allocation leaves `CODE l1:1-3:3 closed info=<null> lit="body body body\n"`, and undoing it means re-allocating a buffer for the whole block and re-prepending the info line — O(block). It is fixed by moving `core/blocks.c:405-421` above the `__OPEN` clear at `:366`. **A pure code move makes the entire fenced close infallible.**

**The rule the redesign must establish, and it is checkable rather than aspirational:**

> **No durable write occurs outside `commit`, and `commit` performs no allocation.**

---

#### 4.13.3 The mechanism

Not one of the four candidates, and not a free choice — the shape of each piece of durable state picks its own, and the assignment is forced.

| Durable state | Mechanism | What is recorded | Cost |
|---|---|---|---|
| New block nodes the line opens | **stage** — build unlinked, link in `commit` | the staged list | O(containers the line opens) ≤ O(line) |
| Bytes appended to an open block's content | **hoist** — one `grow` for the exact size | nothing | O(1) |
| A closing block's inline children | **build-then-attach**, discard on failure | nothing (after 9b) | O(nodes built) = O(block) |
| Append-only lists: map entries, diagnostics, concrete records | **mark and truncate** | one length per list | O(1) to record |
| Parser scalars | **commit-phase writes** | ≤ 6 words if a call spans lines | O(1) |
| Spine flags, end positions, retypes | **commit-phase writes only** | nothing per line; one node copy per call (Q37) | Θ(depth) per call |
| Node destruction | **defer** — record on a kill list, free at commit | one pointer per victim | O(1) |

**Stage, for the block structure.** `open_new_blocks` (`core/blocks.c:1266-1470`) opens containers one at a time, and each iteration's decision depends on the node the previous iteration created — so a failure on the third of three leaves the first two in the tree. Measured: `> - > y` performs 7 allocations, and refusing the sixth leaves a QUOTE inside a LIST\_ITEM with no PARAGRAPH and the byte `y` nowhere. The fix is to split the loop into **decide** and **build**. Decide is pure: every core opener reads only `input` and the *type and payload* of the containers on the stack, so the loop can run against a small simulated stack of `(type, payload)` entries for the containers it is about to create — at most one entry per byte of the line. Build then allocates every node the plan named, unlinked, and `commit` links them.

Two things block that today and both are on the list. `finalize` lives *inside* `add_child` (`core/blocks.c:477-479`), so opening a block closes an unbounded number of ancestors before it allocates — lifting the close out of the open is a precondition, not an optimisation. And the one allocating decider, `parse_list_marker`'s `markdown_core_list` scratch (`core/blocks.c:718`, `:763`), is `memcpy`'d into the node at `:1418`/`:1427` and freed at `:1428`; it becomes a stack object and the allocation disappears. The footnote-definition arm is already written in the target shape — the label copy is taken at `core/blocks.c:1355`, before `S_advance_offset` and before `add_child`, and the failure path at `:1356-1359` returns with nothing mutated. **That arm is the model; the other seven are the work.**

**Hoist, for the buffers.** Every fallible buffer write in a line has a size known before the first byte moves. `add_line`'s tab expansion is a run of independent `putc` calls (`core/blocks.c:288-293`) followed by the body (`:295`), and a refusal mid-run is a genuinely split write — measured, one space of a two-space expansion committed to a durable content buffer. One `markdown_core_strbuf_grow(&node->content, node->content.size + chars_to_tab + (ch->len - parser->offset))` before the loop makes both infallible; both terms are in hand at `:287`. The same is true of `parser->linebuf`: the required size is `linebuf.size + len`, known at `core/blocks.c:862`.

**Discard, for the inline parse — the one thing that cannot be hoisted.** The inline pass's allocation count is not a function of the block's length. Measured on 40 000 bytes in a single paragraph:

```
40000 bytes of prose                inline-phase allocations:     10
40000 bytes of  a*a*a*…                                       90008
40000 bytes of  [][][]…                                       60029
40000 bytes of prose wrapped at 80 cols                        1507
```

Four orders of magnitude for the same byte count. There is no advance bound worth reserving against, and none is needed, because the parse is **discardable in full**: the subject is a stack object (`core/inlines.c:1680`), the block's content is borrowed and never written (`core/inlines.c:1681-1684`), both stacks drain unconditionally before return (`core/inlines.c:1691-1696`), and only four node types ever reach the pass, all of them block leaves with an empty child list beforehand. Measured over every single-failure and sticky-failure point, extensions off and on: **discard-then-reparse differs from the clean tree in 0 cases, the content buffer changes in 0 bytes, the block's own fields change in 0 cases, and 52 of 52 failure sites leak nothing.** The undo is "free the children", O(nodes built), which is the same order as the forward work of the line that closes the block — a quantity §11.5 already exempts and already sums to Θ(bytes) over the document.

Two residues make the discard incomplete today, and both are already being deleted. `markdown_core_map_lookup` charges `map->ref_size += r->size` (`core/map.c:307-309`) on every resolution, so a discard-and-retry double-charges and produces a *different* document — measured, with `max_ref_size = 9` and `[ref] [ref] [ref]`, the clean parse yields three links and the discard-and-retry yields one. Step 9b deletes `ref_size`, `max_ref_size` and `entry.size` outright (D9/H2), and with them the inline phase's undo record becomes empty. The second is `parser->oom`/`refmap->oom`, two bits (§4.13.7, A9).

**Mark and truncate, for the append-only lists.** A reference map is head-prepended (`core/references.c:56-57`), so "pop the *k* entries this line added" is O(1) per entry and needs one integer. The same shape covers Step 13's diagnostics and Step 11a's region records, and it must be stated for both now: **a line that is rolled back rolls back its diagnostics and its concrete records**, or the L1/L3 laws hold over a document whose tree disagrees with them.

---

#### 4.13.4 Why the other three lose on their own

- **Hoist alone loses** on the inline parse, for the measurement above; and on extension hooks, which today return a *node*, not a plan, so the core cannot reserve on their behalf (`core/blocks.c:1448`, `:1736`).
- **Undo alone loses** because several of today's mutations are not invertible below O(block) at all: `strbuf_drop` memmoves bytes off the front of a buffer (`core/blocks.c:353`, `:422`), `chunk_buf_detach` moves ownership and resets the buffer (`:425`, `:430`), and `markdown_core_node_free(b)` destroys a node outright (`:393`). Undo becomes affordable only *after* those are deleted — so it can be the residue's mechanism, never the primary one.
- **Stage alone loses** because a line's contribution is not one subtree. It closes ancestors, sets `__LAST_LINE_BLANK` on the whole spine (`core/blocks.c:1497-1501`), appends bytes into an existing buffer, and adds map entries. There is no single pointer write for that. Staging works in exactly the two places where the contribution *is* a subtree: a closing block's inline children, and an extension's opened block.

The combination is therefore forced, and it has one name: **reserve → build aside → commit.** Phase 1 and 2 are fallible and touch nothing durable. Phase 3 is infallible.

---

#### 4.13.5 The chunk, and Q37

`append(chunk:)` takes a string, not a line. With ordering alone, a chunk of *n* lines that fails on line *k* has committed *k−1* lines, and that is not "the parser stands exactly where it stood before the call". Three honest answers, and the owner must pick:

1. **Journal the call** (recommended). Copy a node the first time a call touches it — one generation counter on the node, one compare per touch after the first. The record is Θ(open depth + nodes the call created + open blocks the call appended to), freed at commit, and **not a function of the document**. Because the ordering discipline already makes each line's own failure free, the journal only ever has to reverse *committed* lines, which after §4.13.1's deletions are nothing but link writes, scalar writes, flag writes and deferred frees — all O(1) to invert from a node copy. Arm it only when the chunk contains a second line ending; single-line appends pay nothing.
2. **Report the boundary.** The throw carries `bytesConsumed` and the caller retries with the tail. Cheapest, but it makes `document` and its parser disagree, so it is not value semantics — it is resumption wearing the word *atomic*.
3. **Frame at the line in the facade.** Rejected: Q31 settled the surface as `chunk:`.

> **Q37 (proposed).** Is the transaction the line or the call? **Recommendation: the call, by the journal in (1).** The engine provides line-atomicity by ordering; the journal is a thin layer above it, is exercised only by multi-line chunks, and is bounded by the call.

---

#### 4.13.6 What it costs

Measured on this tree, line-at-a-time, all six extensions attached, default allocator:

| Quantity | spec.txt | extensions.txt | regression.txt |
|---|---|---|---|
| lines | 11 880 | 1 184 | 502 |
| block-phase allocations per line — **mean / max** | 0.65 / **8** | 0.20 / **3** | 0.51 / **3** |
| inline-phase allocations for one block — **max** | **331** | **1 717** | **455** |
| blocks whose inline demand is ≤ 16 | 2 097 of 2 463 | 122 of 143 | 69 of 80 |
| one full pass (feed line-by-line + finish + free) | 1.876 ms | 0.078 ms | 0.039 ms |

`sizeof(markdown_core_node)` is **176 bytes**; `sizeof(markdown_core_parser)` is 688.

**Bytes.** The staged plan is bounded by the containers one line opens — at most one per byte, in practice ≤ 8 nodes over 11 880 lines of `spec.txt`, so under 1.7 KB in the worst case observed and zero on 63% of lines. The truncate marks are one integer per append-only list. The journal, if Q37 takes the recommendation, is 176 bytes per node the call touched, and for a depth-3 document that is roughly half a kilobyte per multi-line append, released at commit. **No term is a function of the document, and nothing survives the call** — which is what Q36(a) requires and what its own slope gate will see.

One measured caution: the *byte* volume a line allocates is not bounded by the line — the largest single line in `extensions.txt` moves 45 760 bytes, all of it one geometric growth of an already-large open block's content buffer. That is amortized O(1) per input byte and it is a cost the engine already pays; the reservation must ask for it in one call rather than in a run of `putc`s, which is precisely the hoist above.

**Per-line time.** Two additions, both constant-factor. The decide pass re-walks the line's container prefix that the matcher already walks, so the block phase's per-line constant rises by at most its own matcher share against a baseline of 210-252 ns/line. The commit pass performs exactly the writes the engine performs today. Nothing is re-derived and nothing is re-scanned that was not already scanned, so **the fitted slope in *i* that Stage 1's gate measures does not move** — which is the only statement about time this section is entitled to make before the code exists, and the gate that will check it already exists.

---

#### 4.13.7 What must change in the engine, by step

No new steps. Every requirement below lands on a step that already owns the file it touches.

| id | Requirement | Step |
|---|---|---|
| **A1** | The engine has one **failure model** as well as one allocator model: an allocation failure is a fact about a transaction, not a property a buffer acquires. `markdown_core_strbuf.oom` either ceases to exist (a `reserve` becomes the only fallible buffer operation and every write after it is infallible) or gains an explicit clear. Today `markdown_core_strbuf_clear` (`core/buffer.c:78-83`) does not lift it and nothing else does — measured, one refused grow silently swallows every later line into that block **with the allocator working again**. | **3a** |
| **A2** | No allocation path aborts. Delete `core/arena.c` and the two `markdown_core_arena_push`/`_pop` pairs at `extensions/table.c:342` and `:550`. §4.13.10. | **3a** |
| **A3** | `parser->oom` stops being one sticky bit meaning four things. A failure is a **returned status**; a terminal "parse lost" state becomes unreachable, because after A1–A2 and the ordering discipline nothing can fail after commit begins. `markdown_core_parser_finish` stops reporting loss by freeing the root (`core/blocks.c:1697-1704`). | **3a**, surfaced by **13** |
| **A4** | `S_strbuf_grow_by` (`core/buffer.c:34-36`) checks `add` against `INT32_MAX/2 - buf->size` **before** the sum. Today a negative target satisfies `target_size < buf->asize` at `:41` and returns without growing *and without poisoning*, after which `_put` memmoves past the end. Verified by direct call; needs a single put above ~1.07 GiB, which `append(chunk:)` makes reachable at `core/blocks.c:909`. | **3a** |
| **A5** | Hooks separate **decide** from **mutate**, and a hook reports *declined / opened / failed* as three distinct answers. §4.13.8. | **3** |
| **A6** | Hook cadence is declared, and a hook that runs at finish is inadmissible under "append returns the document". `autolink`'s `postprocess_text` (`extensions/autolink.c:386`) is Θ(document), destructive and prefix-dependent (H8); mid-loop failure at `:529` leaves the email in the tree **twice**, because the sibling was linked at `:527` before the prefix was shrunk at `:539`. | **3** |
| **A7** | Node lifetimes serve the transaction: `unlink` is the exact inverse of the link that made it; a staged subtree is freeable while it is in no tree and without an iterator; and **inside a transaction a node destruction is recorded, not performed** — the kill list drains at commit. `markdown_core_node_free(b)` at `core/blocks.c:393` and `markdown_core_node_replace`+`free` at `extensions/formula.c:534-535` are the two sites this rule exists for. | **5** |
| **A8** | The inline pass is the transaction's discardable phase: it builds into a child list, `commit` attaches it, and the "inlines parsed" marker (H4) is set **in commit, never during**. Literal ownership at emission (already Step 8's) is what makes the discard leave the content buffer untouched. | **8** |
| **A9** | `markdown_core_parse_inlines` returns a status instead of writing `parser->oom` (`core/inlines.c:1698-1699`), and `try_extensions` (`core/inlines.c:1529-1547`) owns `subj->pos`: snapshot before each `match_inline`, restore on decline, stop the chain on failure. | **8** (contract from **3**) |
| **A10** | `markdown_core_reference_create` builds the entry complete and links it with one pointer write. Today three allocations run *after* the point of no return and the entry is linked unconditionally (`core/references.c:29-57`): measured, refusing the url leaves a live definition pointing at `""`, refusing the title leaves one with no title, and a dropped definition renumbers `entry.age` for every later one, which is the first-wins tiebreaker at `core/map.c:189`. | **9b** |
| **A11** | `markdown_core_parse_reference_inline` gains a failure return. It returns `subj.pos` whether or not the definition stored (`core/inlines.c:1772-1776`), and its caller then destroys those source bytes (`core/blocks.c:353`) — measured, all four failure modes leave the paragraph stripped and the map wrong. 9a/11c delete the drop; the status is still owed, because a definition that was lost must not be reported as consumed. | **9b**, bytes by **9a/11c** |
| **A12** | The line's contribution to the append-only records is marked and truncated: a rolled-back line leaves no diagnostic and no concrete region behind. | **13**, **11a** |

**Two things Stage 0 must not do**, added to §11.7's list. Do not regenerate a golden over a tree produced by a poisoned parser — after A1 the poison is gone, but before it, one refused allocation silently truncates every later line into the same block. And do not let any new code path read `parser->linebuf` without testing its `oom`: it is written at six sites and its `oom` is read at **zero** in the entire engine, which is finding §4.13.11-D27.

---

#### 4.13.8 The extension interface — a requirement for Step 3, now

A hook that both decides and mutates makes atomicity impossible, and Step 3 must be told before the descriptor is written rather than after. Six shape requirements, each with the measurement that produced it.

1. **Three answers, not two.** `markdown_core_open_block_func` returns `markdown_core_node *`, and NULL means both "I declined" and "I ran out of memory" — so every extension smuggles the difference through `parser->oom`, a sticky field that also kills every subsequent line. The descriptor's opener must answer **declined / opened / failed**. The same overload has a live correctness consequence: `try_opening_table_header` returns `parent_container` on all eleven paths including "no table here" (`extensions/table.c:326-457`), and `core/blocks.c:1450` treats any non-NULL as "opened" and breaks the loop — so **directive and formula blocks cannot interrupt a paragraph whenever tables are enabled**, reproduced through the public API with the shipped attach order.

2. **`match` is pure and non-allocating; `build` cannot fail.** `match` receives a **read-only view** of the container — its type, its payload and its content bytes, not a `markdown_core_node *` — and returns a *claim*: node type, start column, bytes consumed, payload size, and any payload the decide pass already computed. `build` receives storage sized by the claim and writes it. This is what lets the core hoist on the extension's behalf, which is impossible today because the hook creates its own node through `markdown_core_parser_add_child` (`core/blocks.c:1736` → `:471`), and that function finalizes an unbounded number of ancestors before it allocates. Measured: `[lab]: /u` then `:::note{a=1}`, refusing allocation 7 — the reference definition is in the map, the paragraph node is **destroyed**, `parser->current` has moved to the document, and **no directive block exists**. No extension can fix that, because it is never told which ancestors will close.

3. **Deciding does not allocate.** `table`'s `matches` builds a whole row speculatively on every line while a table is open and throws it away (`extensions/table.c:545-560`); `directive`'s `scan_parsed_attributes` builds a complete attribute list purely to validate a `]{…}` closer and frees it (`extensions/directive.c:985-994`). Both write `parser->oom` when work whose result was going to be discarded fails — a *discarded trial poisons the document*.

4. **No hook mutates outside its claim.** `autolink` advances the subject and truncates the **previous sibling** before it allocates (`extensions/autolink.c:312-313`, then `:315`; `www_match` at `:241`/`:243`). Measured on `see www.example.com/p end`, refusing allocation 2: the output is `text "see "` + `text " end"` — **17 bytes of user text silently deleted**. With directives also on, the mutated subject is handed to the next extension and a directive node is manufactured from the wreckage. `markdown_core_node_unput` (`core/inlines.c:1925-1934`) is O(1) reversible if `(node, n)` were recorded; nothing records it. Under Step 8's byte-range literals the backward edit should not exist at all.

5. **Two silent-failure primitives are fixed or banned in extension code.** `markdown_core_node_set_string_content` returns `true` unconditionally (`core/node.c:405-408`) and `table` depends on it at `:305`, `:445`, `:506`. `markdown_core_chunk_to_cstr` leaves a **borrowed** pointer on failure (`core/chunk.h:58-76`); `extensions/formula.c:114-125` discards its result and returns 1, which is a live heap-use-after-free (§4.13.11-D28). `extensions/directive.c:183-188` guards it correctly and is the model.

6. **`opaque` payloads join the transaction.** `opaque_alloc`/`opaque_free` must be inverses (H16), and a staged node's payload must be freeable while the node is in no tree. `extensions/formula.c:219-224` sets `oom` and returns NULL **without unlinking the node `:213` already added**; `extensions/directive.c:1137` does the same thing correctly.

Two hooks in the tree already have the target shape and both get it the same way — **allocate everything, then mutate**: `directive`'s `open_directive_block` (`extensions/directive.c:1099-1146`, twelve of thirteen failure points leave the pre-line tree bit-identical and the thirteenth is a genuine benign fallback) and `formula`'s `replace_with_formula_block` (`extensions/formula.c:527-535`). Step 3 should transcribe those two and repair the other four to match.

---

#### 4.13.9 The gate

Three parts. Only the second is expensive, and it is affordable.

**G1 — the commit is infallible, checked structurally.** In the debug build, `commit` runs with the allocator swapped for one that fails the test on any call, and every durable write goes through a primitive that asserts it is inside a commit. One pass over the corpus, no measurable cost, and it kills the entire class rather than sampling it.

**G2 — the Nth-allocation sweep, resume style.** For each line and each *k* in that line's allocation demand: fail the *k*-th allocation of that line, catch the throw, compare the parser's durable state against a digest taken immediately before the line, then clear the injected failure and re-run the line normally and continue. **Because the transaction is the line, a failure at line *i* needs no replay of lines 1…*i−1*** — the same run continues — so the number of passes is the *maximum per-line allocation demand*, not the total allocation count.

The digest is the open spine (O(depth)) plus a fixed set of monotone counters that a rolled-back line must leave untouched: nodes allocated, nodes freed, `refmap` size, each open block's `content.size`, `line_number`, `linebuf.size`, `total_size`, diagnostics length, region-record length. The run ends with a **byte-identical comparison of the finished tree against a clean parse**, which is what catches anything the spine digest cannot see. The resume trick assumes the property it tests, so the digest must be checked *before* resuming — if it ever fails, the gate fails and the resumption never happens.

Measured cost, from this tree's own numbers:

| | passes needed | pass cost | sweep |
|---|---|---|---|
| spec.txt | 8 today; ~340 once inlines are charged to the closing line | 1.876 ms | 0.64 s |
| extensions.txt | 3 today; ~1 720 after | 0.078 ms | 0.13 s |
| regression.txt | 3 today; ~460 after | 0.039 ms | 0.02 s |

**Under 0.8 s per extension configuration**, and roughly 2.5 s allowing 3× for the digest — a per-commit gate, not a nightly one. For comparison, the naive form that restarts the whole parse for every allocation index in the document costs Σ allocations × pass, which for `spec.txt` alone is 22 469 × 1.876 ms ≈ **42 s**; keep it as the quarterly cross-check that validates the resume form, run on `regression.txt` (0.015 s) every time.

**G3 — the discard oracle.** For every block in the corpus and every *k* in its inline demand, run the inline pass with the *k*-th allocation refused, discard, re-parse cleanly, and assert the tree equals the clean tree and the block's own fields and content bytes are byte-identical. This is the sweep that already produced 0 differences across four modes; it becomes a gate. Where a block's demand exceeds a stated cap (the corpora's maximum today is 1 717, so a cap of 2 048 is exhaustive over the fixtures), the sweep samples with a seed derived from the commit so the union over runs is exhaustive.

**What the gate must also assert, because it is the actual contract:** after a caught throw, the *next* append succeeds and produces the tree a clean parse of the same bytes produces. That is what distinguishes atomicity from a tidy failure, and it is the assertion today's engine fails at every one of the 107 points.

---

#### 4.13.10 The arena

**It cannot survive the contract, at any amount of work, and the reason is not performance.**

`alloc_arena_chunk` calls `abort()` on both of its allocation failures (`core/arena.c:17`, `:21`) — demonstrated, `Abort trap: 6`, status 134. `arena_calloc` (`:58-81`) and `arena_realloc` (`:83-91`) have no NULL return path at all, so under the arena `buf->oom` is never set, `map->oom` is never set, `parser->oom` is never set from an allocation failure, and **there is nothing for `throws` to throw**. An abort is not a throw; it is the one outcome the contract exists to make impossible, and it destroys in the caller's address space the very object the contract promises will still be in scope.

Three independent reasons beyond the abort, each already recorded elsewhere and each pointing the same way. It is **on the per-line path** — `markdown_core_arena_push()` allocates 10 KiB plus a header on every line while a table is open (`extensions/table.c:342`, `:550`), so every one of those lines is an abort site. It is **process-global and unlocked** (`core/arena.c:7-12`), so one parser's pop releases another parser's chunks and it cannot be per-transaction state even in principle. And `arena_free` is a no-op while `arena_realloc` always allocates fresh and copies (`:83-96`), so **a parser held open across appends grows monotonically in the number of lines fed** — a direct violation of Q36(a), invisible to any timing gate.

Deletion is already **Step 3a**'s stated requirement (`0 new · −140`). Q34 changes its status from a simplification to a **correctness precondition**, and it is the fifth independent reason on the record for Q12. One related item travels with it: `assert(!map->prepared)` (`core/references.c:38`, `core/footnotes.c:35`) compiles out under the `default` preset's `-DNDEBUG`, so the second abort-shaped path in the engine does not abort — it silently loses the definition. Neither is a throw. Step 9's map rewrite deletes both asserts by making the interleaving legal.

---

#### 4.13.11 Four defects the sweep found, and what to do with them

Numbered as §2 additions from the next free id. **These four hold D27–D30, and 0a.6's raw-HTML column defect is therefore D31, not D27** — it was written as D27 first, because §2's index table stops at D25 and these four live only in this subsection. The index now carries all of them. **Two are live outside allocation failure and must go to Stage 0a; two exist only under allocation failure and should be pinned by the §4.13.9 gate rather than separately fixed, because the mechanism deletes them.**

> **D27 (proposed, measured).** `parser->linebuf.oom` is written at six sites (`core/blocks.c:853, 858, 896, 905, 907, 909`) and **read at none**. Measured: feeding a four-line document in 32-byte chunks and refusing the first allocation of chunk 1 turns 244 input bytes into 102, with `parser->oom == 0` and `finish` returning a document — the accumulated prefix is handed to `S_process_line` at `:897` and committed **as if it were a whole line**, and because the poison is sticky the truncation continues for the rest of the stream. The only allocation-failure gate in the tree, `case_oom_sweep` (`tests/runners/fallback_runner.c:624`), feeds its corpus in one call, so `linebuf` is never written during the entire sweep and the gate is structurally blind to the exact buffer `append(chunk:)` makes the normal path. Severity: silent truncation. Fix: test the flag, and hoist the growth to `linebuf.size + len` at `core/blocks.c:862`. **Owner: 3a, with A1.**

> **D28 (proposed, measured).** `extensions/formula.c:114-125` ignores `markdown_core_chunk_to_cstr`'s failure and returns 1, leaving the chunk holding a **borrowed** pointer into a buffer freed on the next line (`:423`). ASan: `heap-use-after-free`, READ of size 5 in `markdown_core_extensions_get_formula_literal` at `formula.c:61`, with `parser->oom == 0`. Same shape at `:550` and `:557`. Severity: memory safety. **Owner: 0a, ahead of Step 6.**

> **D29 (proposed, measured).** `extensions/table.c:297` does not check `markdown_core_node_new_with_mem`, and `:305` then calls `markdown_core_node_set_string_content(NULL, …)`. Reproduced: SIGSEGV on `lead text\nx | y` / `--|--`. Two neighbours travel with it — `:300-303` frees the lead paragraph and returns **without setting `oom`**, and `:309-311` frees a failed insert with `mem->free` instead of `markdown_core_node_free`, leaking its content buffer. Severity: crash. **Owner: 0a, ahead of Step 3.**

> **D30 (proposed, measured).** `markdown_core_reference_create` commits an entry whose url or title was lost (`core/references.c:48-57`), and `resolve_reference_link_definitions` destroys the source bytes either way (`core/blocks.c:353`). Measured on `[gone]: /destination-value "the title"`: refusing allocation 3 yields a live definition with an empty destination; refusing 4 yields one with no title; refusing 1 or 2 drops the definition **and renumbers `entry.age` for every later one**; in all four the paragraph is stripped. `parser->oom` stays 0 in every case — the loss lands on `refmap->oom` and is translated only at `core/blocks.c:1697`, i.e. at finish. **This is the only site in the engine that silently produces a wrong document with the failure bit clear.** Severity: wrong-document under allocation loss. **Do not schedule at 0a**: A10 and A11 delete it, 9a/11c delete the byte-drop, and the §4.13.9 gate pins it in the meantime. Recorded so that §4.12's "all defects before any other task" is not read as requiring a fix the redesign removes.

The §4.12 consequence is worth stating plainly, because it is a scheduling decision and not a technical one: **D28 and D29 are ordinary defects and belong in Stage 0a; D27 and D30 are allocation-failure-only and belong to the steps that delete their mechanisms.** Fixing D30 at the baseline means writing a careful two-phase insert into `core/references.c` that Step 9b then deletes entire.

---

#### 4.13.12 Ledger

| id | Question | Recommendation |
|---|---|---|
| **Q37** | Is the append transaction the **line** or the **call**? | **The call.** Line-atomicity comes free from the ordering discipline; call-atomicity adds a copy-on-first-touch node journal, armed only for chunks containing more than one line ending, bounded by Θ(open depth + nodes the call created) and freed at commit. Answer (2) — a throw carrying `bytesConsumed` — is cheaper and is not value semantics. |

And one amendment to **Q34**'s recorded recommendation. §11.8 recommends *"split `oom` into a terminal 'parse lost' bit and a per-call 'snapshot failed' result."* Under this mechanism the terminal bit has nothing left to record: an allocation can only fail before commit, and a failure before commit is exactly the per-call result. **The recommendation collapses to one half — a returned status and no sticky state anywhere** (A1, A3). That is a stronger contract than Q34 asked for, and it is reachable because the operations that made it unreachable are the same ones Steps 8, 9a, 9b, 11a and 11c already delete.

### 4.10 The release from this base is 3.0

**Owner ruling, 2026-08-20.** There is no 1.0.4 release.

**Owner ruling, 2026-08-21, which supersedes the marker: `VERSION` is `3.0.0`
now**, taken at 0a.4's close and before 0a.5. This is Q27's second option,
adopted early rather than at the end of the stage, and the reason to prefer it
over the 1.0.4 marker is measured rather than aesthetic: **the marker was
unreachable.** `check-release-version.mjs` requires every existing tag to be
strictly below `VERSION` whenever `v$VERSION` is absent, and `v2.0.0` exists, so
1.0.4 makes that assertion permanently unsatisfiable. 3.0.0 satisfies it.

**What the bump did, and what it did not.** Nine files carry the number and all
nine moved together — `VERSION`, the tracked `markdown-core-version.h`, the npm
manifest, and the seven README and consumer coordinates the gate pins. The
CMake-generated header and the Kotlin publications derive from `VERSION` and
needed nothing. `docs/deprecated/releases/3.0.0.md` and a `## 3.0.0 -
unreleased` CHANGELOG section exist from this commit, which is exactly what Q27
said adopting 3.0.0 early would cost; the release note says plainly that it
accumulates and that `v3.0.0` does not exist. **`check-release-version.mjs` now
fails on one thing only — two legacy tags that are not versions** — where before
the bump it also failed on the missing release note. The bump made that gate
*more* reachable, not less.

**It carries no release obligation.** Nothing here schedules a 3.0.0 tag. The
number states what the tree is; the tag states that it shipped, and that is
§4.8's business.

Two consequences worth stating, because the plan was written assuming otherwise:

- **The ABI break window is not a constraint.** R4 and Step 12 were built around
  batching six public breaks into one release so consumers broke once. Shipping
  3.0 from this base means the surface is free to change as the design requires,
  and the discipline that remains is only that it changes *deliberately* and
  the bindings follow. Step 12 keeps the "write the target header first" method
  and loses the "one window" urgency.
- **The release gates are off the critical path** until 3.0 — but less of them
  than this bullet assumed. The release notes and the README examples were paid
  at the bump and are green; what remains a 3.0 obligation is
  `check-release-version`'s **legacy-tag condition** alone.

### 4.8 Stage 0 acceptance

Stage 0 is **not** accepted by the mdast backlog reaching zero — that happens at
Step 10 and says nothing about Steps 11–15. It is accepted by all of the
following, together:

**Deliverables**
- [ ] Directive grammar conformance (Step 7) — deliverable #1
- [ ] The formula fix (Step 6) — deliverable #2
- [ ] CST concrete records (11a, 11b, 11c) and diagnostics (13) — deliverable #3
- [ ] The reference model (9a, 9b) and the positions that depend on it (10)
- [ ] The facade and its single ABI break window (12), the null/empty rule (14)
- [ ] Bindings, specs and docs regenerated (15)

**Defects** — **all thirty-one of §2** closed, or explicitly carried with a named
owner step and a registered known-red gate. ~~seventeen~~ was stale from the
revision before D18–D25 and §4.13's four were added; §2's own heading says
thirty-one, its index table carries thirty rows (D1–D25, D27–D31) and **D26 is
the thirty-first, proposed in §4.2.5 and not yet measured** — so closing the
stage means D26 has been put to the same test as the others, or is carried with
an owner like D9 is.

**Gates**, all green and none of them vacuous:
- [ ] `correctness`, `correctness-asan`, `correctness-ubsan` — each having
      actually run its tests, not merely exited 0 (§0's warning)
- [ ] `conformance`
- [ ] upstream parity, and **both** fuzz oracles
- [ ] mdast parity with an EMPTY backlog
- [ ] scope-sanity, having only shrunk
- [ ] `check-canonical-ast-fixtures`, `audit-public-surface`,
      `audit-ast-projections`, `check-generated-scanners` — the last two are
      known-red today and must be green or re-owned by close
- [ ] `pnpm check:contracts`, formatters, linters, repository audits
- [ ] `check-release-version` — including the legacy-tag condition

**Decisions** — Q8, Q9 and Q10 settled and recorded in §9.

### 4.5 Per-defect gates

**Every defect fix lands with a test that fails before and passes after.** Where
the gate had to be invented, it was written and its mutant kill was verified by
reverting the fix and watching the gate go red.

| Defect | Gate | New? | Mutant kill proved | Which oracle can see it |
|---|---|---|---|---|
| D1 | 3 rows in `specs/mdast-parity/corpus.md` (`foo:_bar_`, `foo$_bar_`, `a}*.foo.*`) + 3 engine examples in `extensions-formula-option-gates.txt` / `extensions-directive.txt` | rows only | yes — 0/3 → 3/3 vs remark | **mdast only, and only after the rows exist.** Upstream parity is structurally blind: it runs `--profile gfm`, which detaches both extensions |
| D2 | structural invariant: every registered `special_inline_chars` byte is dispatched by `match_inline` or is a sentinel `< 0x20` | **new, ~20 lines** | by construction | **none.** With D1 fixed, D2 has no output signature at all (exhaustive 37,448-case differential: 0 diffs) |
| D3 | regenerated `spec.txt` (13 rows) **+ the new inline-sourcepos oracle** | oracle new | **LANDED 0a.6, measured**: restoring the guard makes `correctness` read 64/66 and the inline-sourcepos oracle report all twelve rows appearing | **none today.** Both parity gates compare rendered output; `audit-scope-sanity.mjs` reads the same before *and* after, because it classifies only sentinel, negative and line-zero rows. **The `block_offset` amendment had NO gate of its own** — dropping it kept every suite green until 0a.6 added a `regression.txt` example for it |
| D4 | `assert(after_char_pos < subj->input.len)` under `#ifndef NDEBUG`, tripped by the existing ASan/UBSan presets on `a *~~` | **new** | yes — kills the operand-order revert | **none, and no sanitizer either**: 0 ASan reports over 14,783 executions of the read |
| D5 | 1 example in `regression.txt` + activating `refdef-title-rewind` in `specs/upstream-parity/deltas.json`; `check-upstream-parity.mjs` then requires the divergence to still reproduce | rows only | yes — 796/796, `registered divergences: 1/1` | **upstream parity**, and only once registered: `regression.txt` is in the parity corpus, so adding the example without registering the delta fails the gate |
| D6 | the 18 moved golden rows, strongest at `extensions.txt:667` (both spellings of one construct, three columns apart on one line) | existing | the goldens are the gate | **none.** `scripts/lib/upstream-cmark.mjs:174` folds `title:""` to `"null"` before comparing, for all three parity oracles |
| D7 | 2 examples in `regression.txt` (blockquote pins `block_offset`, continuation line pins `column_offset`) **+ the new scope-containment invariant** | **both new** | **LANDED 0a.6, measured**: reverting the two lines makes `regression_commonmark` FAIL *and* `audit-scope-containment.mjs` report five rows appearing | **none, and upstream cannot be the oracle** — cmark-gfm reports the same wrong columns |
| D8 | new `tests/fixtures/extensions-conflicts.txt`, 2 examples, framed as *enabling `table` must not change another extension's block opener* | **new** | yes — 0/2 at baseline, 2/2 with the fix | **none.** The corpus tests one extension at a time: 761 of 798 examples enable nothing, and no example ever co-enables `table` with `formula` or `directive` |
| D9 | order-independence oracle (**registered red**, names Step 9a) + output-size bound in `complexity_runner.c` (green) | **both new** | n/a — the fix is Step 9a | **none.** With the budget deleted, every existing gate stays green while 1 MiB of input produces 68.7 GB of output |
| D10 | position half: `regression.txt` example 24 **already exists and pins the defect** — unpinning it is the gate. Byte half: new example `x[^a\nb] tail` + an `expectedDivergence` | half new | yes, both halves | **half.** No corpus input loses bytes here, and upstream loses the same bytes |
| D11 | new `regression.txt` example (the nested-duplicate reproducer) + an upstream **model** delta; sanitizers and `leaks --atExit` gate the ownership half | **new** | the minimal fix moves zero goldens, so the example is mandatory | **none.** Nothing in the corpus has a nested duplicate label |
| D15 | 2 order witnesses in `extensions-conflicts.txt` (a `:::note` and a `$$` block after an OPEN table) **+ the new `scripts/audit-extension-attach-order.mjs`** | **both new** | **LANDED 0a.11, measured**: putting `table` first takes the fixture 4/4 → 2 passed / 2 failed; a second attach site in `markdown_core_document_parse` is caught by **the audit alone**, with `correctness` 67/67 and `conformance` 2/2 | **none, and no corpus can be**: every fixture runs through the facade, so no fixture can compare the two attach orders. `conformance` runs the CLI and the facade against the same six canonical goldens but none of the six inputs is order-sensitive |
| D24 | 1 example in `extensions.txt` + activating `tasklist-checked-marker` in `specs/upstream-parity/deltas.json` | rows only | **LANDED 0a.11, measured**: restoring the `strstr` fails `extensions_gfm` **and** fails `check-upstream-parity.mjs`, which reports the divergence no longer reproducing | **upstream parity**, and only once the corpus reaches it: the registered input was in no fixture, so the JSON edit alone fails the gate's own reachability check |

**Four defects — D2, D4, D7, D8 — are invisible to every oracle in this
repository, and their gates are assertions and structural properties rather than
output comparisons.** **D15 is a fifth, and it is the strongest case of the
class**: not merely unseen by the corpus but *unseeable* by one, because a
fixture can only ever run through one of the two entry points that disagreed. That has a consequence worth stating in §8: a later
refactor can delete the assertion and pass. Those four gates must be re-run and
re-read *by name* at Steps 3, 8 and 11.

**Two more are invisible for a reason no corpus row can fix.** D6 is normalized
away by the parity harness itself. D3 is invisible because positions are
invisible: R7 already says so and it is empirically true — `audit-scope-sanity.mjs`
reads 207 rows before and after both position fixes. **A well-formed but wrong
position sails through the ratchet.** The two oracles in 0a.1 close most of that
hole and are the reason 0a.1 exists as its own step.

### 4.6 What this does to the mdast reconstruction backlog

**Nothing closes in Stage 0a. Zero of twenty-three.** That is the honest answer
and it is worth saying plainly, because the opposite is the natural guess. The
backlog measures distance to *mdast's model*; the eleven defects are almost
entirely wrongness relative to **this engine's own stated intent**. They are
different axes.

Two entries touch defect territory and neither closes on a defect fix:

- **`specs/mdast-parity/corpus.md:243`** — `[foo]: /url\n"title" ok\n\n[foo]\n` —
  is D5's input. D5's fix lands in 0a.7 and the entry **still diverges**,
  correctly, because remark's expectation needs `ReferenceDefinition` and
  `LinkReference` nodes. Measured: backlog stayed 23/23 with the fix applied.
  **Its note must be amended in the 0a.7 commit** to say the title defect is
  fixed and only the node model remains — otherwise the next reader reads the
  entry as wholly outstanding and re-derives D5.
- **`specs/mdast-parity/corpus.md:69`** — `no references here\n\n[^orphan]: still
  a definition\n` — closes on *definition retention*, which is a model decision
  (§9 Q1), not a defect fix. It stays in Step 9 — specifically **9a**, which is
  the re-attribution below.

The re-attribution is therefore about **which half of Step 9**, and it moves one
entry a long way earlier in wall-clock terms:

```
    14  Step 7   — directive grammar conformance          unchanged
     5  Step 9b  — the reference node model               was Step 9
     1  Step 9a  — definition retention                   was Step 9  (corpus.md:69)
     2  Step 6   — formula                                unchanged
     1  Step 10  — the split-off table lead               unchanged
    --
    23  remaining
```

That single move is the substantive one: **Step 9a has no dependency on the CST at all**,
which the old table asserted as "9 depends on 11 (hard)". Measured — definition
retention plus the one-line anchor fix at `blocks.c:1363`
(`add_child(..., parser->first_nonspace + matched + 1)` →
`parser->first_nonspace + 1`, which *is* §5.1's anchor rule and gate G2) was
built at the untouched baseline with nothing else landed: 65/65, upstream
795/795, scope-sanity 207 at budget with only-shrink holding, 39 golden lines
moved across `extensions.txt` and `regression.txt`, and the mdast gate then
**demands** `corpus.md:69` be deleted because the engine agrees with remark.
Retention also surfaces one pre-existing negative scope (`[^a]:[^b]:` yields
`FootnoteDefinition scope=4:11..4:10`) which the anchor line closes — the two go
together or not at all.

**Do not pull retention into Stage 0a.** It is a model change, and a defect stage
that smuggles model changes in stops being falsifiable as a defect stage. Land it
at 9a, early, with the measurement above already in hand.

The upstream policy file moves in Stage 0a even though the mdast backlog does
not: `pendingDeltas` goes 3 → 2 (`refdef-title-rewind` activates at 0a.7;
`tasklist-checked-marker` and `table-split-lead-spelling` stay pending for their
steps), `deltas` goes 4 → 5, and `applyUpstreamFootnoteModel` gains D11's rule.

### 4.7 Notes that change the order or the risk

- **Steps 0 and 1 have landed.** What Step 1 actually cost, recorded because the
  next person will hit the same thing: main's policies could not be copied. Two
  corpus fixtures had left with the cross-link/embed feature; three upstream
  divergences and two mdast divergences describe fixes not yet re-applied and
  moved to `pendingDeltas` / `pendingExpectedDivergences`, each naming the step
  that restores it; the mdast self-test canary asserted the padding-stripped
  literal, which the baseline does not produce, and now asserts `" mid "` until
  Step 6 flips it back. The CLI also gained `--profile`, which the harness
  invokes and the baseline lacked — a named option set only, with the extension
  attach *order* deliberately untouched.
- **Step 1 is non-negotiable and must not be deferred.** The parity oracles
  arrive at `3d8d329` (#79); at the baseline there are **zero** of them. Every
  "the parity gates pass" claim in this repository's history is a statement about
  a harness the reset removed. `deltas.json` must be **re-pinned against the
  untouched baseline binary**.
- **Stage 0a is now what Step 1 was for.** R1 said the first behaviour change
  must not land without an oracle. Stage 0a is that first behaviour change, and
  0a.1 exists so the sentence stays true for positions too.
- **~~Step 3 must come before every later step.~~ Step 3 must come before Steps
  6, 7 and 8.** It removes the process-global registry and the `once`, which also
  makes a paused parser a plain struct in Stage 1 — but that is an argument for
  it preceding Stage 1, not for it preceding a seventy-four-line defect stage.
  See §4.3 for the experiment.
- **Step 3 must decide D15, not inherit it.** Fixing the extension order into a
  static table makes the CLI/facade divergence permanent one way or the other.
  Decide which order is the language, and say so in the commit.
- **Step 8 is the one genuine branch point.** It carries four syntax fixes and
  unblocks the CST's inline one-funnel property. Recommendation: take it, but
  only after 6 and 7 have landed and their goldens have been regenerated, so the
  largest risk is not entangled with a regeneration it did not cause.
- **Step 11a has no prerequisites at all** (see §6) and is the fastest proof that
  the CST verdict is right.
- **Measure in a private worktree, and wipe `build/` first.** Two independent
  hazards, both of which fired during this analysis. (a) The shared working tree
  is a lost-update race: one agent's fix was silently overwritten between a read
  and a write, and a second agent's golden-movement measurement reported a row
  that belonged to a third. (b) `build/` in the checkout can be **stale** —
  make-3.81's same-second mtime trap means `cmake --build` will not rebuild
  objects whose source carries the checkout timestamp. With a stale `build/` the
  baseline reads 64/65, not 65/65, and D10's documented impossible position does
  not reproduce. **`rm -rf build/` and reconfigure before measuring anything, or
  you will measure a different engine.**
- **Two fixture traps.** `tests/fixtures/regression.txt` line 7 holds a
  deliberate CR+CR+LF fixture; a text-mode read-modify-write silently rewrites
  it. And `spec_runner --rewrite` must be run with the **same `--option` flags
  the ctest entry passes** — running it without them once rewrote
  `extensions.txt` down by 855 lines.
- **A pre-existing failure, unrelated to any of this:**
  `node scripts/check-canonical-ast-fixtures.mjs` already fails on the pristine
  tree at `8e76a94` ("structure does not demonstrate declared order:
  directive.label-before-content"; "state coverage is missing: htmlComment.false,
  htmlComment.true"). It fails identically before and after every defect fix.
  Fix it or register it, but do not let a defect commit inherit the blame.

---

## 5. Steps 9a and 9b — one reference model

*"Step 9" below is the umbrella for both halves. Where a statement belongs to
one half only, it says 9a or 9b.*

**Step 9 now lands in two parts, and the split is not the old one.** The old
draft split it by *construct* — a footnote contract and a reference-definition
contract — and that was wrong for the reason below. This split is by
*dependency*, and it was found by measurement:

- **9a — the anchor, the order, the retention, the budget.** Registration in
  document order (D11's `EXIT` → `ENTER`), definition retention, the definition
  anchor rule of §5.1, deleting the reference expansion budget together with the
  destination copy that makes it necessary (D9), and the failed-call prefix
  reconstruction (D14). **None of this needs a concrete record.** Retention plus
  the one-line anchor fix at `blocks.c:1363` was built at the untouched baseline
  with nothing else landed: 65/65, upstream 795/795, scope-sanity 207 at budget
  with only-shrink holding, 39 golden lines moved.
- **9b — the node model.** `markdown_core_association`, `identifier`,
  `LinkReference` / `ImageReference` / `ReferenceDefinition`, and the ~260 lines
  of §5.3 deletions. This is the half that needs Step 11a.

The old table's "**9 depends on 11 (hard)**" is therefore false for every
data-loss part of Step 9 and for the anchor rule. Do not hold the byte-keeping
behind the CST.

This step replaces what an earlier draft split into two "independently decidable
halves" (a footnote contract and a reference-definition contract). That
decomposition was wrong: **a link reference definition and a footnote definition
are the same construct with different bodies.** Deciding them separately decides
the same question twice and lets the answers drift, which is exactly how the
engine reached a state with three accessors, two node payloads and two frozen
dump vocabularies for one field.

### 5.1 The rule

> **A reference definition is a block node at the byte where its opening bracket
> was written, in the container it was written in, and it stays there.** There
> are two kinds and they differ in exactly one thing: what the definition's body
> is. A link reference definition's body is a resource — a destination and an
> optional title — so it is a leaf. A footnote definition's body is flow
> content, so it is a container with block children. Everything else is one rule
> for both: each carries the label exactly as the source spells it, delimiters
> excluded, character escapes and character references unresolved, whitespace
> uncollapsed, case unfolded; each exists whether or not anything refers to it;
> each keeps the outcome of matching off the node, in the reference map; and
> neither is ever moved, reordered, renumbered, dropped, or given a
> back-reference by anything that runs after the parse. A reference —
> `LinkReference`, `ImageReference`, `FootnoteReference` — is the same rule from
> the other end: it carries the label it was written with and no destination,
> because the destination is stated once, at the definition. A label that no
> definition defines does not produce a reference node at all; the brackets are
> prose. Numbering, first-use order, resolution state and back-reference
> ordinals are not node content — they are a renderer's output, derived by one
> preorder walk in which first-use order is exactly the document order of the
> reference nodes. This is mdast's model, adopted deliberately in preference to
> cmark-gfm's, which erases a link reference definition into a parser-private
> map and hoists footnote definitions to the document tail in first-reference
> order.

That paragraph goes above the struct family in `core/node.h` and opens the
reference section of the canonical-AST spec — and nowhere else in a second
wording.

### 5.2 The shape

The apparent obstacle is that a link definition's payload is `{url, title}` and
a footnote definition's is block children, and no honest struct holds both. But
**a container's children live on the node, never in the payload union.** So the
footnote definition's payload is exactly `{label}`, the link definition's is
`{label, url, title}`, and the shared part is real, total, and is mdast's
Association. The relation is extension, not alternation.

```c
/* The association every reference and definition carries. TWO values, and
 * neither derives the other in either direction.
 *
 * `label` is the bytes between the delimiters exactly as written: escapes and
 * character references unresolved, whitespace uncollapsed, case unfolded.
 * `identifier` is the match key: full Unicode case fold, trim, collapse
 * internal whitespace — and for a footnote it KEEPS its leading `^`, so that a
 * link definition and a footnote definition of the same name cannot collide in
 * a consumer's single map. That caret is a correction to mdast, which
 * separates the two namespaces only by node type and so cannot survive being
 * flattened onto a wire.
 *
 * NORMATIVE: `identifier` is compared with memcmp over its bytes. It is never
 * case mapped, never NFC/NFD normalized, never re-encoded, and never used as a
 * key in a language map whose `==` has an opinion about Unicode — Swift's
 * `String ==` is canonical equivalence, which would collapse the NFC and NFD
 * spellings of `[café]` that this parser deliberately keeps apart. Bindings
 * expose it as an opaque byte-hashable, not as a string. */
typedef struct {
    markdown_core_chunk label;
    markdown_core_chunk identifier;
} markdown_core_association;

/* A link reference definition: Association + mdast's Resource. A leaf; its
 * body is the destination, not children. */
typedef struct {
    markdown_core_association association;   /* must stay first */
    markdown_core_chunk url;
    markdown_core_chunk title;
} markdown_core_definition;

/* A link or image reference: Association + mdast's referenceType. It holds no
 * destination. A footnote reference is deliberately NOT this type: there is
 * one footnote call syntax, so there is no form to record. */
typedef struct {
    markdown_core_association association;   /* must stay first */
    markdown_core_reference_type form;       /* FULL | COLLAPSED | SHORTCUT */
} markdown_core_reference_link;
```

**This is NOT a common-initial-sequence trick, and an earlier draft of this
section claimed it was.** That claim was wrong twice over, and both errors are
worth recording so neither returns.

*Wrong in law.* C11 6.5.2.3p6 licenses inspecting the common initial part of two
union members only where corresponding members have **compatible types** for a
sequence of one or more initial members. `markdown_core_association` begins with
a `markdown_core_chunk`; `markdown_core_definition` begins with a nested
`markdown_core_association`. Those are distinct struct types and therefore not
compatible, so the common initial sequence between those two arms has length
**zero**. The only licensed pair is `definition` ↔ `reference_link`, which both
begin with an `association` — and that is not the read the draft wanted.

*Wrong in fact, which settles it.* Measured on this machine: `chunk` 16,
`association` 32, `definition` **64**, `reference` 40, and the widest arm in
`node.as` today (`code`) is **40**. So a definition stored INLINE grows the union
40 → 64, contradicting §5.8's cost argument; and a definition **boxed** — which
is what §5.8 requires — means `as.association.label` on a definition node would
read a *pointer* as `chunk.data`. The uniform read is impossible whatever the
standard says, because the cost decision already forbade it.

**So the accessor is type-dispatched**, and the union arms are free to differ:

```c
/* Answers for all five reference kinds and refuses every other node type. */
bool markdown_core_node_association(const markdown_core_node *,
                                    markdown_core_string_view *label,
                                    markdown_core_string_view *identifier);
```

One function, one switch on `node->type`, and no reliance on layout at all. It
costs a branch that the union trick would have saved, and buys back a guarantee
that the union trick never actually had.

### 5.3 What must be deleted to get the anchor rule

Roughly **260 lines**, all at `580d10c`, none of which has a replacement:

| What | Where |
|---|---|
| `core/footnotes.c` / `.h` and both CMake entries | whole files |
| the sort, `sort_footnote_by_ix` | `blocks.c:548-552` |
| `process_footnotes`, all three passes | `blocks.c:554-684` |
| the numbering, `if (!footnote->ix) footnote->ix = ++ix;` | `blocks.c:599-601` |
| back-reference bookkeeping, `cur->parent_footnote_def = ...` | `blocks.c:605` |
| the aliased ordinal pair, `ref_ix` / `def_count` | `blocks.c:610` |
| **the label destruction**, `snprintf(n, sizeof(n), "%d", footnote->ix)` | `blocks.c:612-618` |
| the hoist, `markdown_core_node_append_child(parser->root, ...)` | `blocks.c:672` |
| the unreferenced drop | `blocks.c:668-671` |
| **the map's claim on a node still in the tree** (D11's real free site) | `blocks.c:683-684` |
| `union { int ref_ix; int def_count; } footnote;` | `node.h:81-84` |
| `markdown_core_node *parent_footnote_def;` | `node.h:86` |
| `resolve_reference_link_definitions` — the byte destruction | `blocks.c:343-355` |

One thing must be **added** to core, which both earlier halves silently assumed
and neither owned: **`parser->footnote_defs`**, registered when the container
opens and probed by the inline pass. At `580d10c` the footnote map is
function-local to `process_footnotes`, so definedness cannot be answered at
inline time at all.

### 5.4 Numbering is not lost

An earlier draft filed "deleting footnote v1 removes numbering with no
replacement" as a risk. **That was backwards.** mdast has no ordinal field
anywhere; the numbering, the `<ol>` and the `↩` back-references are specified as
HTML generation, downstream of the tree. cmark's tail order was first-reference
order, and first-reference order is exactly the preorder order of the
`FootnoteReference` nodes — a renderer derives it in one walk with a set of seen
labels.

What the baseline had was not numbering as content but numbering **destroying**
content: `blocks.c:612-618` frees the reference's label and writes `"3"` in its
place, after which the label is recoverable only through the
`parent_footnote_def` back-pointer. **The deletion adds recoverable information
and removes none.** Gate G6 proves it.

### 5.5 Where cmark-gfm and mdast disagree

| Construct | cmark-gfm at 580d10c | mdast / remark | This engine |
|---|---|---|---|
| `[a]: /u "t"` | no node; bytes dropped, a definitions-only paragraph is freed | `definition`, flow content, at its source position | node, scope starts at its `[` |
| `[a]` resolved | `Link` with the destination copied in | `linkReference` — no url, no title | `LinkReference`, label + form |
| `[nope]` undefined | `Text`, brackets intact | `text`, brackets intact | `Text`, brackets intact |
| `[^a]: body` | **hoisted to the document root** in first-reference order; container nesting discarded | stays where written, inside its container | stays where written |
| `[^a]:` unreferenced | unlinked and freed | kept unconditionally | kept |
| `[^a]` payload | **label destroyed**, overwritten with the decimal index | `{type, identifier, label}` | label only, as written |
| duplicate label | first wins; the losing footnote is **freed** | first wins for matching; **both nodes remain** | both remain |

### 5.7 Q2 is settled: the interior of a failed footnote call is reparsed

Not as a preference for remark, and not by analogy. The construct that fails
here is **an unmatched `[`**, which CommonMark specifies normatively: all three
failure branches of *look for link or image* remove the delimiter-stack entry
and return a literal `]`, and **none of them touches the interior**. Failure is
defined as *not re-parenting*. The interior nodes exist because core inline
parsing already built them, under CommonMark's rules, before any footnote code
ran.

So the question is not "what may an extension do when its construct fails" but
"may an extension reach backwards and free nodes core already built, for a
construct that turned out not to exist." Nothing authorizes that: **GitHub's GFM
spec never uses the word footnote**, and `micromark-extension-gfm-footnote`'s
own README says it matches github.com "except for its bugs". There is no
specification of any kind for this case — both behaviours are implementations.

cmark-gfm is alone here, and alone against *itself*: remove `-e footnotes` and
`x[^*y*] tail` gives `x[^<em>y</em>] tail`. Same bytes, same core parser, two
answers depending on whether an extension is loaded.

| | undefined `[*y*]` | undefined `[^*y*]` |
|---|---|---|
| CommonMark | `[` + parsed interior + `]` | out of scope |
| cmark-gfm | `[` + parsed interior + `]` | **one flat literal, children freed** |
| remark | `[` + parsed interior + `]` | `x[^` + parsed interior + `] tail` |
| this engine, 1.0 | `[` + parsed interior + `]` | one flat literal |

**And the flattening is not merely less structured — it is lossy.** See defect
10. That is what decides it: a mechanism that drops source bytes and writes
impossible positions is not a behaviour to preserve.

Recognition moves to the **opening** bracket, gated on the document's definition
set, which is why this makes the engine smaller: the failure path then costs
nothing and the success path destroys nothing. The current flattening needs a
defending mechanism — record tombstoning — whose only purpose is to keep the
flattening consistent.

The definition side is untouched: a label is never inline content, so
`[^*y*]: b` still has the label `*y*`.

### 5.8 Q4 is settled: both kinds carry `identifier` beside `label`

Derived, not inherited. The ecosystem argument that first suggested this was
circular — `identifier` is mdast's only required field, so of course every
consumer reads it; that is a consequence of the schema, not evidence for it.
And mdast's own asymmetry is a 2016/2018 back-compat artifact. Both were barred
from the reasoning.

**Around one reference there are three strings, not one.** The authored bytes;
the match key (fold, trim, collapse); and the display form (escapes and
character references resolved), which is the reference's children. `[a\_b]`
matches `[a\_b]` and not `[a_b]`, while its children read `a_b`.

**The derivability lattice decides it.** `raw → key` needs the 1,401-case fold
table, 104 arms multi-codepoint. `key → raw` is impossible — the fold is
many-to-one, and `[ß]` and `[ss]` are two definitions with one key. `display →
raw` is impossible; escape resolution is lossy. The producer computes the key
at **zero marginal cost** — it already builds one per occurrence for its own
map, then throws the reference's away at lookup time.

**The engine already ships a pairing token, and it is the worst of the three.**
`markdown_core_node_footnote_id` returns, for a *reference*, the winning
DEFINITION's raw literal — so `[^FOO]` with `[^foo]:` reports `id="foo"` and the
author's spelling is unrecoverable. Verified. The question was never whether a
reference carries an identifier; it is which one. `label` + `identifier` replaces both
that and the 8-byte-per-node `parent_footnote_def` back-pointer.

**The relation could be an edge, and must not be.** A pointer does not survive
the copy into value types, and there are no node ids. At the copy the binding
must mint *some* value, and its whole menu is the key, an ordinal, or the
denormalized payload — so the edge does not decide, it re-asks the question in
three languages. Two of the three answers are silently wrong: an ordinal shifts
under `filter` and goes out of range under `slice`; a position **collides**
under `merge`, because both documents have a definition at 1:1. The key
retargets to the merged document's first-wins winner, which is exactly what
re-parsing the concatenation produces. **The key makes resolution late-bound and
re-parse-equivalent**, which no locator can be.

**Cost: the node struct gets smaller.** A reference `{label, identifier, form}` is 40
bytes — exactly the width of the widest existing payload arm (`code`), so the
union does not grow. The definition measures 64 and is therefore **boxed**,
which is the fact that forces §5.2's accessor to be type-dispatched rather than
a single union read. Deleting `parent_footnote_def`
removes 8 bytes from *every* node: −800 KB on a 100,000-node document. The key
bytes are an ownership move, not a new allocation — the parser already
allocates exactly one per occurrence, and today frees them with the refmap at
teardown while the document keeps only `root`.

**Against mdast, checked afterwards:** convergent on `identifier`, on keeping the
label, on the form discriminator, and on the reference holding no destination.
Divergent in three places, and mdast is wrong in one of them: **`label` must not
be optional.** A consumer written against an optional label writes
`label ?? identifier`, and re-emitting the folded key writes `[straße]` where
the author wrote `[Straße]` — a silent authorial rewrite on every round-trip. A
field whose absence forces a lossy substitute should not be declarable absent.
mdast is also merely *lucky* on the comparison domain: JS string equality is
code-unit equality, so byte-faithful; Swift's `String ==` is canonical
equivalence, which collapses the NFC and NFD spellings of `[café]` that this
parser deliberately keeps apart. Hence the normative memcmp rule above.

**On the name.** The field is `identifier`, matching mdast's vocabulary,
because this tree's stated target is mdast's model and a shared concept should
carry a shared name. The semantics here are *stricter* than mdast's in two ways
that the shared name must not be allowed to hide, so both are normative in the
declaration above: comparison is `memcmp` over bytes rather than string
equality, and a footnote's identifier keeps its `^`.

**The falsifier, and it has already fired once — and has now been answered.**
The case against an edge rests on the winner being derivable as "group by
identifier, first in document order". At the baseline it is not: registration is
on the iterator's `EXIT` event, so close order beats document order (D11). **Step
9a must move registration to document order**, and the gate is: for every
reference in the corpus, the definition selected by that derivation is the
definition the engine matched. If that proved unfixable, the key would be
insufficient to identify a node and the definition would need a `winner` bit.

It does not prove unfixable. Changing one word at `blocks.c:578` makes preorder
`ENTER` the registration order, `age` (`footnotes.c:45`) then measures document
order of definition *starts*, and the `refcmp` tie-break (`map.c:189`) and
`index_map`'s oldest-wins (`map.c:255`) select the first-**written** definition —
by construction, which is what "derivable" means. **It is necessary and not
sufficient**: alone it moves the data loss from the outer definition to the inner
one, because the loser is destroyed by the map teardown at `blocks.c:683-684`
while still spliced into the tree. The second half is an eight-line sweep that
clears `node` on every map entry whose node still has a parent. Both halves land
in Stage 0a (§4.2, step 0a.2), ahead of the rest of Step 9, because the loss is
data loss and the fix needs nothing else. **The key survives the falsifier.**

### 5.6 What the oracles cannot see

Even fully restored, neither parity gate can police this step.

- **`liftFootnoteDefinitions`** strips every footnote definition recursively,
  re-attaches them to the root and sorts them. The *upstream* gate must do this,
  because cmark hoists. **The mdast gate does it too, even though remark does
  not hoist** — so the delta file that calls placement "the reason this oracle
  exists" normalizes away the one property it exists to check. Removing that
  call from the mdast gate is part of this step.
- **The comparison table has no key for `FootnoteDefinition` or
  `FootnoteReference`.** A footnote label can live under `id=`, `label=`,
  `literal=`, or be absent, and the rendered strings stay byte-identical. On the
  upstream side cmark's XML writer emits `<unknown>`. **Footnote label bytes are
  compared by nobody, on either side.**
- **Label folding has one example and no footnote example at all**, and the
  mdast fuzzer excludes every fragment containing `[^`, so recombination will
  never make one.

Seven hand gates cover it: `footnote_label_identity` (G1),
`definition_anchor_position` (G2 — Step 9a), `no_tree_rewrite_after_parse` (G3),
`definition_retention` (G4), `label_fold_equivalence` (G5),
`numbering_derivable` (G6), `polymorphic_label_accessor` (G7).

G3 is worth naming on its own: *at every level of the tree, children are in
non-decreasing scope order.* One assertion, and it catches every hoist mechanism
including ones not yet written.

---

## 6. The CST needs no substrate

> Can concrete records and the ownership-region model be built against a plain
> immutable input buffer on a one-shot parser, without the persistent source
> substrate, rope and extents?

**Yes, unambiguously — and nothing replaces the substrate, because the CST never
used it.**

1. **The record encoding is structurally incapable of naming a substrate.** A
   block record is `(line − node->start_line, column-within-normalized-line,
   length, kind, flags)`. An inline record is `(start, length, head, tail, kind,
   flags)` in the owning node's own `content` buffer. **No record holds a
   document offset.** `concrete_records.c` includes three headers and touches
   one external type.
2. **`core/extents.c` has never existed on `main`.** The unit sequence and
   private order labels are an unmerged branch.
3. **The rope was already retreated.** `extensions/source.c` on `main` is 102
   lines of flat append-only buffer whose own header records the walk-back from
   "an AVL-balanced rope of windows into refcounted immutable buffers" to "a
   buffer filled once at construction."
4. **`document.concrete` is a pointer return** — the semantic root and the
   concrete owner are the same pointer.
5. **The coordinate frame already exists at the baseline, byte-identically.**
   The normalized line — each NUL replaced by the three-byte U+FFFD, EOL
   excluded — is produced in `S_parser_feed` at `blocks.c:864`, before
   `S_process_line`, exactly as on `main`.

What *is* entangled, and is in scope: reference-definition records need Step 9b;
table records need line marks; the inline one-funnel property needs Step 8.
Everything else — the whole block half, all ten block sites, the node slots, and
the region partition — needs nothing beyond the baseline.

---

## 7. Drop list

**Whole programs, not partial drops.** Named exhaustively so nothing creeps back
one hunk at a time. 23 commits are dropped whole.

### DROP-1 · The session / incremental / delta / streaming / append program

**Files:** `session.c`, `session_internal.h`, `incremental.c`, `changeset.c`,
`delta.c`, `adopt.c`, `lookups.c`, `footnote.c`, `reference.c`, `diff.c`,
`document.c`, `document_internal.h`, `source.c`/`.h`, `arena.c`/`.h`,
`append_replay.{c,h}`, `equivalence_runner.c` — and `core/text.c`/`.h`, which
**do not exist at 580d10c**; they were added for the session program and must
never reappear.

**Mechanisms:** the whole warm publish/retract/settle machine; the inline seam
family; subtree hashes and node stamping; `opaque_size`/`restore_opaque`; the
frontier; `markdown_core_document_edit` and `_append`; the tightness memo;
`markdown_core_chain`.

**Facade:** every `markdown_core_session_*` and `markdown_core_delta_*`,
`markdown_core_scope_entry`, `markdown_core_reference_info`,
`markdown_core_footnote_info`, node ids and revisions, and `MarkupSession` /
`MarkupID` in all three bindings.

**Docs:** the incremental spec, the sessions-and-deltas spec, the
streaming-and-documents contract, and every milestone planning doc. All are in
`docs/deprecated/` or were never on this branch.

### DROP-2 · Out of stated scope

CrossLink `[[ref]]` and Embed `![[ref]]` (`cross_reference.{c,h}`). Self-contained
and low-risk *if scope ever widens*, but its two `inlines.c` hunks are deleted
again by the delimiter engine, so porting it and then taking Step 8 means
writing and unwriting the same code.

### DROP-3 · Behaviour-neutral churn with no consumer

The reference-map v2 rewrite (a one-shot harvests every definition before any
lookup, so the observable result is identical); the self-referential allocator
ABI; the 2.0.0 version bump; the formatter commit as a *diff* (take the config,
not the patch).

**One exception worth remembering:** the per-block `postprocess_block` pipeline
is dropped as a session artifact, **but it is the correct shape for a resumable
parser.** Port it deliberately when Stage 1 begins — not now.

### DROP-4 · Streaming-only fixes that look like engine fixes

List-tightness flag copying in the formula shell is inert in a one-shot, because
tightness is settled in `finalize` before postprocess runs.

---

## 8. Risks

| ID | Risk | Cheapest experiment | Cost |
|---|---|---|---|
| R1 | **The first behaviour change lands with no oracle.** Now Stage 0a, then Steps 6 and 7. | **Discharged for output** by Step 1, and for positions by step 0a.1. Every behaviour change from here names its own oracle or does not land. | done + ½ day |
| R2 | The delimiter-engine fork is the largest unknown — ~1,100 lines gating the CST inline funnel. | Land 11a alone, then attempt inline capture against baseline emphasis code for one fixture. If only the funnel test fails, the price of skipping is known exactly. | 2–3 days |
| R3 | ~~Footnote v1 deletion removes numbering~~ | **Retired.** See §5.4. Replaced by gate G6. | — |
| R4 | Six independent ABI breaks, unbatched. | Write the target public header first, as one diff against the baseline's 232 lines. | 1 afternoon |
| R5 | Removing `VALIDATE_UTF8` is a live product change; the facade sets it unconditionally. | Build twice, run the fuzz corpus and spec fixtures through the dump CLI, diff. | 1 hour |
| R6 | `strip_html_comments` removal has the same shape. | Same corpus diff, plus grep the bindings. | 1 hour |
| R7 | **Positions are invisible to every oracle**, and the ported ratchet does **not** close it: `audit-scope-sanity.mjs` classifies only sentinel, negative and line-zero rows, so a well-formed but *wrong* position sails through. It reads 207 before and after both position fixes. | The two oracles of step 0a.1: inline sourcepos vs the pinned `cmark-gfm --to xml --sourcepos` (13 → 1 mismatch over 671 examples), and a parent/child scope-containment invariant, which upstream cannot supply because it has D7 too. | ½ day |
| R8 | Unclear whether the iterative dump stack or the canonical walk is needed. | Dump a 50,000-deep blockquote; time the binding scope walk. | 2 hours |
| R9 | 20,459 lines of checked-in re2c output, and **no re2c invocation or version pin** in the build. | Regenerate from the untouched `.re` and diff. | 2 hours |
| R10 | The CST test debt is the bulk of Steps 11a–11c — a 7,067-line runner, half of it streaming. | Extract the 14 non-streaming cases into a standalone runner *at HEAD first*. | 1 day |
| R11 | Option-struct layout across three bindings. | Fold into R4; the bridge asserts fail loudly at build time. | — |
| R12 | **Four defect fixes have no output signature, so a later refactor can revert them silently.** D2, D4, D7 and D8 are held by assertions and structural properties, not goldens — and a refactor that deletes the assertion passes. | List those four gates by name in the commit that lands them, and re-run and re-read them explicitly at Steps 3, 8 and 11. Cheap, and it is the only thing standing between the fix and its own erasure. | ½ day, thrice |
| R13 | **§2's `file:line` citations go stale the instant the first defect lands.** D3's four-line deletion alone moves D4 from `inlines.c:492` to `488` — and that exact shift already produced one confident, wrong "the doc is off by four" correction during this analysis. | Cite `function` (`file:line`), and re-pin the remaining citations in each defect commit. The function name is the half that survives. | minutes per commit |
| R14 | **Step 3 must now be re-derived against a changed source, and it silently deletes a defect fix.** D8's `return` at `table.c:365` goes with the arena retry; the statement "eleven non-opening paths" becomes ten. | Step 3's commit message names the already-fixed line it removes, and 0a.5's fixture — which survives Step 3 — re-proves the property afterwards. The fixture is the durable artifact; the line is not. | 20 minutes |
| R15 | **Step 3 as shipped is not behaviour-neutral and no oracle is pinned to either change.** The arena removal changes the Release CLI's allocator and deletes a path that runs *only* under the arena; deleting `enable_safety_checks` makes `node.c`'s O(depth) ancestor check unconditional. | Unbundle both into Step 3a and land them as named behaviour changes with their own oracle run, per R1. Do not let a rename step carry them. | 1 day |
| R16 | **Stage 0a moves parse output before Step 12's ABI window.** `1.0.4` would be a patch release whose parse output differs from `1.0.3` in eleven ways. | Decide before 0a.2 whether the defect stage ships at all. If it does, the release note is the eleven defects and their measured footprints; if it does not, `VERSION` still moves so the branch does not lie about what it is. | 1 hour |
| R17 | **A shared working tree loses updates, and a stale `build/` measures a different engine.** Both fired during this analysis: one fix was overwritten between a read and a write, one golden-movement number belonged to another agent, and one baseline read 64/65 from stale objects. | One private worktree per defect, never the shared tree; `rm -rf build/` and reconfigure before any measurement. Both are stated in §4.7 so they are not rediscovered. | — |
| R18 | **The extension corpus tests one extension at a time**, so cross-extension interference is invisible as a class. 761 of 798 examples enable nothing, and no example ever co-enables `table` with `formula` or `directive`. D8 is one member; there is no reason to believe it is the only one. | Generalize 0a.5's fixture into a pairwise-independence property: for every pair of extensions, co-enabling must not change a parse that uses only one of them. | 1 day |

---

## 9. Decision ledger

**Renamed `Q`, not `D`.** These are open *decisions*; `D1`…`D16` in §2 are
the baseline *defects*, and one document cannot spell two things the same way.

Every decision this plan depends on, with a status. **Q1–Q7 are SETTLED** — Q2
and Q4 were the two genuinely contestable ones and were settled on 2026-08-20
(§5.7, §5.8); this intro used to say they still awaited the owner, twelve lines
above rows already marked settled, which is how a stale sentence outlives the
table it introduces.

Three decisions were being carried in prose and risk tables rather than here,
which is why they kept getting re-argued:

| id | Question | Status | Decided in | Blocks |
|---|---|---|---|---|
| **Q8** | May the reconstruction take code from existing commits? | **SETTLED 2026-08-20 — NO.** See §4.9. Ignore every existing commit except the formula fix and the directive syntax fix. Everything else is designed and written fresh. | owner | the entire port list |
| **Q9** | What is the extension attach order? (D15) | **SETTLED 2026-08-20 — table LAST, with a test. IMPLEMENTED 0a.11**, and the ruling is load-bearing: all six inputs whose parse the reorder moves are a line inside an OPEN table that a narrower extension also claims, which D8's fix does not touch. Every other extension's position is measured to be free — moving `directive` changes 0 of 4,000 random `:`/URL documents (§4.2.17). A decided order, not an inheritance: a table's row opener matches any line inside an open table, so every narrower claim attaches first. D15's CLI/facade disagreement is fixed in the same step. | owner | Step 3, 0a.5, **0a.11** |
| **Q11–Q29** | Nineteen decisions the requirement restatement exposed | **PROPOSED** unless listed below, each with a recommendation in §4.1.6 | §4.1.6 | their owning steps |
| **Q14** | The option surface | **SETTLED 2026-08-20 — DELETE ALL OF IT.** See §4.11. | owner | 3, 6, 7, 12, 15 |
| **Q24** | Is the concrete view opt-in? | **SETTLED 2026-08-20 — NO. It is not optional; it is part of the model.** Diagnostics on directive attributes have nowhere to point without it. | owner | 12, 13 |
| **Q25** | When are defects fixed? | **SETTLED 2026-08-20 — ALL of them, before any other task.** Not just D16's two sites: every defect that *can* be fixed at the baseline moves into Stage 0a. See §4.12. | owner | Stage 0a |
| **Q26** | Do `Link.destination`, `Image.source`, `ReferenceDefinition.destination` stay optional? | **SETTLED 2026-08-20 — NO, all three are required.** Q7's argument generalises: a value reachable only through allocation loss is not optionality, it is a node that lies. | owner | 9b, 14 |
| **Q10** | Does 1.0.4 ship? | **SETTLED 2026-08-20 — NO.** The release from this base is **3.0**. 1.0.4 is an internal alignment marker only, carrying no release obligation. | owner | see §4.10 |

| ID | Question | Recommendation |
|---|---|---|
| Q1 | Is an unreferenced definition kept? | **Keep both kinds.** Dropping requires the post-parse rewrite Step 9 exists to delete. |
| ~~Q2~~ | Is the interior of a failed footnote call reparsed? | **SETTLED — yes.** Not by analogy: the construct that fails is an unmatched `[`, which CommonMark specifies normatively, and nothing authorizes an extension to free children core already built. The flattening also loses source bytes (defect D10). See §5.7. |
| Q3 | Do footnote references carry a form? | **No.** mdast gives `footnoteReference` Association and not Reference; one call syntax means a field with one value. |
| ~~Q4~~ | Does the node carry a normalized `identifier`? | **SETTLED — yes, on both kinds, plus the exported fold.** Derived from the lattice, not from mdast. The "export the normalizer instead" answer was wrong: the consumer has no engine to call, because every binding frees the handle and copies into value types. See §5.8. |
| Q5 | Does the dump keep `id=` for footnotes? | **Rename to `label=`.** Two names for one field after unifying the field is the failure mode that produced three accessors. |
| Q6 | Where does a footnote definition's scope start? | **At `[^`.** It is the only block in the engine opened past its own marker, and the binding docs already promise otherwise. |
| Q7 | Is a definition's destination optional? | **Required.** The null is reachable only through allocation loss, where C and Swift currently disagree. Set the failure bit and emit no node rather than a node that lies. |

---

## 10. Deprecated documents

Everything under `docs/deprecated/` was moved there wholesale by the reset. It
is archive. Some of it is still *accurate* for the baseline engine — the
migration phase records, the toolchain and environment notes, the release notes
— and some of it is actively false, including `specs/canonical-ast.md`, which
still claims link reference definitions "are not a difference: both parsers
consume them into the reference map and neither leaves a node behind." That
sentence is contradicted by §5 of this document.

The rule is deliberately blunt, because a half-true document is worse than an
archived one: **nothing under `docs/deprecated/` is normative.** A document
returns to `docs/` by a commit that says which step made it true again.

---

## 11. The Stage 1 state inventory

This is the deliverable §3 names under *"What Stage 1 owes before it starts"*. It is placed here rather than inside the roadmap because it is longer than the roadmap it serves. Six subsystems were inventoried independently against the working tree at `b71c8a9` — the parser struct, the block phase, the inline phase, the extensions, buffers and memory, and the late-resolved reference and footnote maps — and merged here with the duplicates collapsed.

**Citations are `file:line` relative to `packages/markdown-core/`, pinned to `b71c8a9`.** As in §2, the enclosing function name is the durable half; a landed fix moves every line below it. Every classification below is either a citation or a measurement; where a measurement decided the class, the measurement is given.


> **Both headline measurements re-verified independently before this was
> accepted**, on the same tree, with a purpose-built probe rather than the
> agent's own:
>
> - **Line-at-a-time equals one-call on every line-boundary prefix.**
>   `regression.txt` 502 prefixes, `spec.txt` 11,880, `extensions.txt` 1,184 —
>   **13,566 prefixes, 0 differing.** Criterion 1 is already satisfied by the
>   line loop at HEAD.
> - **Per-line feed cost is flat; the close is linear.** Feed cost by decile
>   over 20,000 lines: 252 220 224 228 208 209 211 208 208 210 ns — no growth
>   in *i*. `markdown_core_parser_finish` at 5k/10k/20k/40k/80k/160k lines:
>   2.38 / 4.38 / 7.89 / 15.48 / 28.24 / 54.64 ms — dead linear, doubling with
>   the document.
>
> So calling the close once per line is Σᵢ O(i) = O(l²), which is exactly the
> cheat criterion 2 forbids — and the line loop itself already meets both
> criteria. **Stage 1's problem is not making the parser resumable. It is that
> there is no way to read the tree without ending the parse.**

### 11.0 What a pause is, in the engine's own terms

A line boundary is the return from `S_process_line` (`core/blocks.c:1579-1652`). Measured at exactly that point:

- **`curline.size == 0`, always** — cleared at `core/blocks.c:1651`. The *allocation* survives; the *bytes* do not.
- **`linebuf.size == 0` iff the feed ended on a line terminator.** A feed ending mid-line leaves the tail in `linebuf` and does not increment `line_number`. This is why §3 is right that Stage 1 has zero partial-line complexity: at a boundary the held-line problem does not exist.
- **The eight per-line cursors hold the residue of the line just processed**, and are re-zeroed at `core/blocks.c:1607-1614` before the next line reads any of them. They are simultaneously readable through public accessors (`core/blocks.c:1716-1734`) at the one moment they are meaningless.
- **`root` is the still-`__OPEN` DOCUMENT, and `current` is on the open spine but is not necessarily its bottom** (H9).

Two measured facts frame everything that follows.

**Criterion 1 as written is already satisfied at HEAD.** For every line-boundary partition of `docs/RECONSTRUCTION.md` (110 sampled prefixes) and `tests/fixtures/spec.txt` (698 sampled), feeding line-at-a-time and feeding the whole prefix in one call produce byte-identical trees — type, `start_line:start_column..end_line:end_column`, literal, and enter/exit order. `S_parser_feed` (`core/blocks.c:862-930`) is a pure line splitter, and the block phase has no lookahead past the current line. **What is missing is not correctness of the line loop; it is that there is no way to read the tree without ending the parse.** This is the single most consequential result in the inventory, and §11.7 draws the conclusion from it.

**Criterion 2 is met by the line loop and broken by the close.** Per-line feed cost is flat in *i* across every workload measured except two named exceptions (H23, H7): one huge paragraph 170→119 ns by decile over 20 000 lines; tight list items 214→186; fenced code 26→25; table rows 518→553. `markdown_core_parser_finish`, by contrast, costs 0.35 / 0.60 / 1.08 / 1.87 / 3.04 / 5.43 ms at 5k…160k lines — dead linear. Running that per line is Σᵢ O(i), which is precisely the cheat §3 forbids.

---

### 11.1 CARRIED — must survive the pause and be restored identically

| # | State | Where | Why CARRIED / what breaks if lost or stale |
|---|---|---|---|
| C1 | `parser->mem` | `core/parser.h:17`; restored across reset `core/blocks.c:189` | Embedded in every node (`core/node.h:120-122`) and in both strbufs; cannot be swapped at a pause. See H12. |
| C2 | `parser->root` | `core/parser.h:21`; `core/blocks.c:197,1686,1701,1707` | The living tree. Not derivable at any price short of reparsing. |
| C3 | `parser->current` | `core/parser.h:23`; w `core/blocks.c:198,488,794,1127,1515,1574` | Drives lazy continuation (`core/blocks.c:1509-1511`) and indented-code suppression (`core/blocks.c:1256,1429`). **Not derivable** — H9. Clobbering it to `root` at each boundary aborts at `add_text_to_container` (`core/blocks.c:1516`). |
| C4 | The open spine below `current` | tree, via `__OPEN` (`core/node.h:48`), walked by `S_last_child_is_open` (`core/blocks.c:1051-1053`) from `check_open_blocks` (`core/blocks.c:1193`) | Read on every subsequent line, and it extends *below* `current` under table. Carried state that no parser field names. |
| C5 | `parser->line_number` | `core/parser.h:25`; `core/blocks.c:1624` | Every `start_line`/`end_line`; the empty-list-item blank rule (`core/blocks.c:1493`); BOM detection (`core/blocks.c:1621`). Clobbering it changes the tree. |
| C6 | `parser->last_line_length` | `core/parser.h:46`; w `core/blocks.c:1645-1649`, r `core/blocks.c:371,383` | **The cross-line carry.** It is written at the end of line *i* and read during line *i+1* as the `end_column` of any block that closes then. The field most easily mistaken for per-line; clobbering it changes the tree. |
| C7 | `parser->linebuf` | `core/parser.h:49`; `core/blocks.c:896-909,1662` | The un-terminated byte tail. Empty at a true line boundary, so Stage 1 never has to serialize it; Stage 2 does. |
| C8 | `parser->last_buffer_ended_with_cr` | `core/parser.h:56`; `core/blocks.c:871,875,923` | One bit for a CRLF split across feed calls. Clobbering it produces a spurious blank line. |
| C9 | `parser->total_size` | `core/parser.h:57`; `core/blocks.c:866-869` | Sole consumer is the reference budget (`core/blocks.c:802-803` → `core/map.c:307-309`). Measured: clobbering it turns 40 resolved links into 24. Carried state that exists only to serve a hazard (H2). |
| C10 | `parser->oom` | `core/parser.h:55`; ~66 writers, 41 of them in extensions; r `core/blocks.c:1586,1637,1699` | Sticky and terminal. A poisoned parser must stay poisoned or a truncated document is reported as success. See H13. |
| C11 | `parser->options` | `core/parser.h:51` | Immutable configuration, read per line by formula, directive, strikethrough. |
| C12 | `parser->syntax_extensions`, `parser->inline_syntax_extensions` | `core/parser.h:58-59`; `core/blocks.c:162-167` | **Order is semantics**, not tidiness: the same list drives block-open dispatch (`core/blocks.c:1446-1451`), inline match order (`core/inlines.c:1534`) and postprocess order (`core/blocks.c:1683`). D8, D15, Q9, Q15. |
| C13 | `parser->backslash_ispunct` | `core/parser.h:60`; w `core/blocks.c:1748`, r `core/inlines.c:902` | Embedder configuration — and **silently dropped by every `finish`** today (H1). |
| C14 | `parser->refmap`, and its entire internal state | `core/parser.h:19`; `core/map.h:37-51` | The only record that a definition existed. **Not derivable at any price**: the source bytes are dropped at `core/blocks.c:353` and the paragraph node freed at `core/blocks.c:393`. Includes `prepared`, `indexed`, `sorted`, `index`, `ref_size`, `size`, `oom`. |
| C15 | `entry.label`, `ref->url`, `ref->title`, `entry.age`, `entry.size` | `core/references.c:46,48,49,52,54` | All **owned heap copies** — the refmap holds no pointer into `curline`, `linebuf` or any block's `content`. It is the one subsystem that is already boundary-safe. `age` decides first-definition-wins (`core/map.c:189,251-259`). |
| C16 | Emptiness of `parser->curline` (1 bit) | `core/parser.h:44`; read as a branch at `core/blocks.c:368` | An undeclared signal meaning *"no line is in flight"*, which makes `finalize` take the end-of-input branch. It is why snapshot end positions are correct for free today, and why Stage 2 must not park a partial line there. See H14. |
| C17 | Every open block's `content` strbuf | `core/node.h:60`; created `core/blocks.c:125` | The block's accumulated raw text, plus the buffer that every inline literal borrows into. Includes its `oom` bit. |
| C18 | `__LAST_LINE_BLANK`, `__LAST_LINE_CHECKED` | `core/node.h:49-50`; w `core/blocks.c:1481,1495,1499`, r `core/blocks.c:331,338,441,449` | Where the cross-line effect of `parser->blank` actually lives. `__LAST_LINE_CHECKED` is a memo that is never cleared — H10. |
| C19 | `as.code.{fenced,fence_char,fence_length,fence_offset,fence_closed}` | `core/node.h`; r `core/blocks.c:1106-1137` | Read on every subsequent line to decide continuation and close. |
| C20 | `as.html_block_type` | `core/node.h`; r `core/blocks.c:1145-1161,1525-1549` | The HTML end condition. |
| C21 | `as.list.{marker_offset,padding,list_type,delimiter,bullet_char}` | r `core/blocks.c:786-790,1089-1090`; `extensions/tasklist.c:42` | Continuation indent and list matching. |
| C22 | `as.heading.setext` | r `core/blocks.c:374,1560` | Selects the end-position branch and whether trailing hashes are chopped. |
| C23 | `container->first_child == NULL`, `container->start_line` | r `core/blocks.c:1092,1492-1493` | The "empty list item opened on this very line" rule. |
| C24 | `node->extension` (raw registry pointer) | `core/node.h`; r `core/node.c:47-49,178-179`; `core/blocks.c:261-262,276-277,1170-1172` | Behaviour-bearing, not decoration: it routes containment, continuation, `accepts_lines`, `contains_inlines` and payload freeing. A pointer-forgetting snapshot changes the parse *and* leaks every payload. |
| C25 | `node_table {n_columns, alignments, n_rows, n_nonempty_cells}` | `extensions/table.c:31-36,385` | `n_columns` decides cell count and autocompletion for every later row. `n_rows`/`n_nonempty_cells` feed the DoS cap at `extensions/table.c:104-111,468` and are **invisible to a tree-equality oracle**. |
| C26 | `MARKDOWN_CORE_NODE__TABLE_VISITED` | `extensions/table.c:16,353` | The memo that stops a failed header probe re-running on every later line. Lose it and H7's one-line spike becomes a genuine quadratic. Invisible to every oracle. |
| C27 | `node_table_row.is_header`, `as.cell_index` | `extensions/table.c:38-40,121-127`; `core/node.h:95` | Row/cell identity. `as.cell_index` shares a union slot with an extension payload pointer — H16. |
| C28 | `node_formula {block_delim, closed, mode, literal}` | `extensions/formula.c:26-31,228,242` | `block_delim` is how the block knows which closer to look for; `closed` carries a deliberate one-line lag (`extensions/formula.c:247`). Both invisible to every oracle; lose either and the block never terminates. |
| C29 | `node_directive {fence_length, closed, consume_line, name, attributes, has_label, has_attributes}` | `extensions/directive.c:33-42,1152-1154,1174-1180` | `fence_length` makes `::::` refuse to close on `:::`. `consume_line` is a per-line flag stored in persistent state — H17. Bytes are copied, never borrowed (`extensions/directive.c:177-204`). |
| C30 | `as.list.checked` plus `node->extension` on a tasklist LIST\_ITEM | `extensions/tasklist.c:84,88`; read via `extensions/ast.c:354-360` | "Is this a task item" is stored nowhere; it is a `strcmp` against a process-global type string. The extension pointer also replaces core's list-item continuation logic. |

**Carried scalar footprint of the struct itself: 176 bytes** (the two 256-byte tables are derivable). Everything genuinely expensive is reachable from `root`, `current` and `refmap` — none of it is in the struct.

---

### 11.2 DERIVED — recomputable, with the cost stated

| # | State | How it is derived | Cost, and whether that is acceptable |
|---|---|---|---|
| D-1 | The open-container chain | `root`, then `last_child` while `__OPEN` (`core/blocks.c:1051-1053`) | O(open depth) per line — already paid every line by `check_open_blocks`. Acceptable, **except** that it does not reproduce `parser->current` (H9). |
| D-2 | `cont_type`, `parent` links | tree reads | O(1). |
| D-3 | `parser->special_chars[256]`, `skip_chars[256]` | `markdown_core_inlines_reset_special_chars` + one pass over the inline extension list (`core/inlines.c:1507-1526`) | O(512 + Σ extension chars) = O(1) in document size. **At every line boundary they already equal the base tables** — the pair is added at `core/blocks.c:534` and removed at `core/blocks.c:543`, strictly inside `process_inlines`. See H8. |
| D-4 | `curline` contents at a boundary | It is already empty (`core/blocks.c:1651`) | O(1), and load-bearing — C16. |
| D-5 | `refmap->index` | `index_map` (`core/map.c:247-264`) | O(D) in definitions. Fine **once**; rebuilding per line is an O(D·lines) term and a flat criterion-2 failure. Under Stage 1 it is really CARRIED and must be **incrementally maintained**, not rebuilt. |
| D-6 | `refmap->sorted` | `sort_map` (`core/map.c:197-220`) | O(D log D), same conclusion. Reached only when `index_map` fails to allocate (`core/map.c:287`) — H15. |
| D-7 | `max_ref_size` | `max(100000, total_size)` (`core/blocks.c:801-806`) | O(1) — but derived from a quantity that **does not exist at line *i***. This is D9; it is not derivable, it is unknowable. H2. |
| D-8 | Footnote ordinals and first-reference order | one preorder walk with a seen-set | O(tree). Deleted by Step 9a (§5.4); until then it is a whole-document function evaluated once. |
| D-9 | `node->parent_footnote_def` | look the reference's own label up in the footnote map | O(1) *if the label survives* — and it does not: `core/blocks.c:612-618` overwrites it with the ordinal. Derivability destroyed by the pass that sets it. |
| D-10 | An owned copy of any chunk | `markdown_core_chunk_to_cstr` (`core/chunk.h:58-76`) | O(len) per chunk; **tree-wide it is O(document) per snapshot** — H5/H-C. |
| D-11 | `node_directive.attributes_json` | `render_attributes_json` (`extensions/directive.c:393-416`) | O(attributes) — but it is computed lazily *at read time and cached into the node*. H18. |

**Nothing in the late-resolved subsystem is derivable from the tree.** The tree retains neither link definitions (erased at `core/blocks.c:353`) nor a resolved footnote reference's label (overwritten at `core/blocks.c:612-618`). That is the sharpest classification result in the inventory.

---

### 11.3 PER-LINE — created and consumed inside one line

| # | State | Where | Proof it is not read across a boundary |
|---|---|---|---|
| P1 | `offset`, `column`, `first_nonspace`, `first_nonspace_column`, `thematic_break_kill_pos`, `indent`, `blank`, `partially_consumed_tab` | `core/parser.h:27-42`; reset `core/blocks.c:1607-1614` | Unconditionally zeroed before any reader runs. Clobbering each one individually at every boundary produces an identical tree; clobbering `line_number`, `last_line_length`, `curline` or `last_buffer_ended_with_cr` does not. |
| P2 | `curline`'s **bytes** | `core/blocks.c:1589-1600,1651` | Cleared at both ends of the line. (Its emptiness is C16; its allocation is carried for free.) |
| P3 | The `markdown_core_chunk input` frame | `core/blocks.c:1616-1618` | Stack-local, borrows `curline.ptr`, last read at `core/blocks.c:1645-1649`, buffer cleared two lines later. |
| P4 | Tab accounting | `S_find_first_nonspace` `core/blocks.c:985-1012`, `S_advance_offset` `core/blocks.c:1023-1049`, consumed by `add_line` `core/blocks.c:287-294` | `chars_to_tab` is recomputed from `column` on entry each line; no tab state crosses a boundary. |
| P5 | `table_row` / `node_cell` scratch, `row->cells`, `cell->buf`, `row->paragraph_offset` | `extensions/table.c:20-29,179-290,270` | Freed at every one of eleven exits; cell bytes reach the tree only through `markdown_core_node_set_string_content`, which copies. |
| P6 | `parsed_directive` scratch | `extensions/directive.c:44-57,786-812` | Freed at all five exits (`extensions/directive.c:1122,1129,1138,1147,1158`). |
| P7 | The cursor chunk in `resolve_reference_link_definitions` | `core/blocks.c:346-352` | Local; its final value is consumed at `core/blocks.c:353` in the same call. |
| P8 | `lab`, `url`, `title` and the `subject` in `markdown_core_parse_reference_inline` | `core/inlines.c:1719-1751,1726` | Borrows into the paragraph's own buffer, copied by `markdown_core_reference_create` before return. **`subject_from_buf(NULL, …)` — harvest is deliberately parser-blind and therefore a pure function of the paragraph bytes.** Preserve that. |
| P9 | `norm` in `markdown_core_map_lookup`; `sample` in `map_index_expected_size` | `core/map.c:279-301,228-243` | One `calloc`/`free` per lookup; function-local. |
| P10 | `raw_label` in `handle_close_bracket` | `core/inlines.c:1283-1299` | Borrow into `subj->input`, freed at `core/inlines.c:1299`. |
| P11 | The whole `subject` (424 B), the delimiter chain, the bracket chain, `backticks[81]` | `core/inlines.c:52-73,36-45`; drained `core/inlines.c:1690-1696` | Per **inline pass**, not per line — and the inline pass runs once, at finish. This is "per-line" only in the vacuous sense that no inline work happens per line today. See §11.6. |
| P12 | The footnote map | `core/blocks.c:560-684` | Function-local to `process_footnotes` — i.e. per **finish**. Step 9 must promote it to `parser->footnote_defs`. |
| P13 | The info-line `tmp` strbuf in `finalize` | `core/blocks.c:411-414` | Detached into the node in the same call. |

---

### 11.4 HAZARD — the finding

Ranked by how badly each blocks Stage 1. The first six are blockers: Stage 1 cannot be built while they stand. The next six corrupt a snapshot silently. The rest are latent — they become live the moment a Stage 1 substrate changes how bytes are addressed.

| Rank | Hazard | Blocks |
|---|---|---|
| H1 | `finish` is the only exit, and it is a reset | the whole stage |
| H2 | The reference budget makes the answer a function of the future (D9) | prefix equality |
| H3 | The refmap freezes at the first lookup, and the guard is an erased assert | per-line resolution |
| H4 | `process_inlines` is not idempotent and there is no "already parsed" marker | any snapshot at all |
| H5 | Every inline literal borrows a buffer that five mechanisms move | snapshot validity |
| H6 | `process_footnotes` is a destructive whole-document rewrite | snapshot-by-finish |
| H7 | Table's header probe is Θ(document so far) on one line | criterion 2 |
| H8 | Postprocess is Θ(document), destructive and prefix-dependent | criterion 2 |
| H9 | `current` is not the bottom of the open spine, and `__OPEN` is not its index | reconstruction |
| H10 | `finalize` is destructive per type and non-idempotent; `finalize(LIST)` is O(items) | snapshot-by-close |
| H11 | The extension model is process-global | *"a paused parser is a plain struct"* |
| H12 | The arena is ambient process state, and it hides the rest | measurement itself |
| H13 | `oom` is one sticky bit shared by the live parse and the snapshot | error reporting |
| H14 | `curline.size == 0` is an undeclared "no line in flight" signal | Stage 2 |
| H15 | NUL-sentinel scanning throughout the block phase | any slice substrate |
| H16 | `opaque_alloc`/`opaque_free` are not inverses, and a payload aliases a scalar | Step 3 |
| H17 | Per-line facts stored in persistent structures, and stale public accessors | correctness of the format |
| H18 | Reading the tree mutates it | snapshot as a value |
| H19 | `feed_reentrant` is a live use-after-free on the exported surface | nothing, once deleted |
| H20 | Invisible carried state — the oracle cannot see most of §11.1 | the gate |

**H1 · `markdown_core_parser_finish` is the only path to a finished tree, and it destroys the parser.** `core/blocks.c:1654-1710`: it frees `curline` and `linebuf`, runs `finalize_document`, consolidation, every `postprocess_func` and the HTML-comment strip, hands `root` to the caller, and then calls `markdown_core_parser_reset` (`core/blocks.c:180-210`), which `memset`s the struct and installs a **fresh empty DOCUMENT**. Measured: pre-finish `root` has 2 children; post-finish `root` is a different node with 0 children, `line_number=0`, `total_size=0`, and a second `finish` returns that empty document rather than NULL — so the "already finished" guard at `core/blocks.c:1659` is unreachable. *Unclassifiable because:* it is not a read, it is the only write that converts open state into finished state, and it does so by deleting the open state. *At a pause:* there is no non-destructive exit; a snapshot cannot be a call to `finish`. *Smallest change:* a separate non-destructive entry point, and `reset` must stop dropping `backslash_ispunct` (C13) — measured `0x1` before finish, `0x0` after.

**H2 · The reference-expansion budget makes the parse of lines 1…*i* a function of lines *i+1*…** `max_ref_size = max(100000, total_size)` is set at `core/blocks.c:801-806` from a number only the last line knows, and spent monotonically at `core/map.c:307-309`. Measured twice, independently: a 4 248-byte document with 60 references to one definition resolves **24** of them; the same 62 lines followed by 303 001 bytes of unrelated filler resolves **60**. Separately, appending 368 890 bytes *after* a paragraph flips that paragraph from `Text "[b]"` to `Link "/short"`. *Unclassifiable because:* `total_size` is carried, but the value it feeds does not exist until the stream ends. *At a pause:* prefix equality is not merely expensive, it is **unattainable** — no carried state can supply a number that has not been fed. *Smallest change:* delete `ref_size`, `max_ref_size` and `entry.size` (Step 9a). **This upgrades D9 from "wrong output, pinned by two gates" to a Stage 1 prerequisite.**

**H3 · The refmap is build-once-then-freeze, and the freeze is one-way.** First `markdown_core_map_lookup` prepares the map (`core/map.c:287`, setting `prepared` at `:218,:261`); `markdown_core_reference_create` then hits `assert(!map->prepared)` (`core/references.c:38`, and `core/footnotes.c:35`). The `default` preset is Release with `NDEBUG`, so **the assert is gone in every shipping build** and the definition is appended to `refs` but never indexed — silently invisible. Measured both ways: Release loses `[b]: /bbb`, ASan aborts. Three further faults ride along: `index_map` re-`memset`s the index without freeing `slots` (`core/map.c:69`), so re-preparing leaks the whole table; `map->size` means *entries inserted* before preparation and *unique labels* after (`core/references.c:57` vs `core/map.c:217,260`), and `entry.age` is seeded from it, so a post-preparation insert collides with an existing age and corrupts first-wins; and `markdown_core_map_lookup` mutates the map, so it is not a query. *Smallest change:* delete `prepared`/`indexed`/`sorted`, the lazy-prepare branch and both asserts; maintain the index incrementally on insert; separate the two counters.

**H4 · `process_inlines` is not idempotent, and nothing marks a block as parsed.** `contains_inlines` (`core/blocks.c:275-281`) tests only `node->type`; `markdown_core_parse_inlines` (`core/inlines.c:1678-1700`) reads `parent->content` and *appends* children without clearing it. Run it twice and every inline child is duplicated. *Unclassifiable because:* "has this block's inlines been parsed" is a real piece of parser state with no home. *At a pause:* "snapshot = do the finish work, then keep going" is impossible without an undo — which is exactly what forced the previous program's two-tree clone (§1). *Smallest change:* one flag bit per block, tested by `process_inlines`.

**H5 · Every inline literal borrows a buffer, and five mechanisms move it.** `markdown_core_chunk_dup` returns `alloc = 0` — a borrowed pointer into `parent->content.ptr` (`core/chunk.h:109-113`), used at nine sites in `core/inlines.c` and by two extensions. The buffer moves under: (a) `add_line` → `strbuf_put` → `grow` → `realloc` (`core/blocks.c:295`, `core/buffer.c:57`) — measured, `content.ptr` moved four times in twelve lines, and a literal parsed before the move then points at freed memory; (b) `strbuf_drop`'s `memmove` when leading reference definitions are consumed (`core/blocks.c:353`, `core/buffer.c:216-226`), which fires on an ordinary continuation line via the setext path (`core/blocks.c:1330`), not only at close; (c) `strbuf_drop` again for a fenced block's info line (`core/blocks.c:422`); (d) `markdown_core_chunk_buf_detach`, which steals the buffer outright (`core/blocks.c:424,430`); (e) `markdown_core_node_set_type` on the paragraph→table retype (`extensions/table.c:447`), whose `free_node_as` frees the chunks in the union. *Unclassifiable because:* `alloc` is not an ownership discriminator — it conflates "borrows a strbuf that may move", "borrows another node's chunk" and "points at `.rodata`", and nothing can tell them apart. *At a pause:* a snapshot's bytes are valid only until the next line is fed. *Smallest change:* Q17 already settles it — store a byte **range**, not a pointer, on the inline node; positions become projections and the borrow disappears. Until then, `markdown_core_node_own` (`core/iterator.c:135-176`) is the only tool and it is **incomplete and has zero callers** (it misses IMAGE, CODE\_BLOCK, both footnote kinds, every extension payload, and the node's own `content`).

**H6 · `process_footnotes` is a destructive, non-idempotent, whole-document rewrite.** `core/blocks.c:554-685`: it registers definitions on iterator **EXIT** (close order, not open order — D11), numbers them by first *reference*, **overwrites each reference's label with its decimal ordinal and frees the label** (`core/blocks.c:610-618`), replaces unresolved references with Text and frees the node, drops unreferenced definitions (`core/footnotes.c:12-13` frees the tree node), and **hoists every definition to the end of `root`** in ordinal order (`core/blocks.c:665-672`), discarding container nesting. *At a pause:* run twice, the second pass looks an ordinal up as a label. The hoist means the document tail's sibling order is a function of the whole document — a reference on line *i+5* reorders nodes emitted at line 3. *Smallest change:* Step 9a's deletion list (§5.3) is not only a model correction, it is a **Stage 1 prerequisite**.

**H7 · Table's header probe costs Θ(document so far) on one line.** `try_opening_table_header` reads the entire open paragraph's accumulated content (`extensions/table.c:347`) and runs `row_from_string` over all of it, possibly twice (`extensions/table.c:357-367`). Measured Δ attributable to that one line: 1.39 ms at 48 KB of preceding paragraph, 5.19 ms at 192 KB, 16.4 ms at 768 KB, 69.6 ms at 3 MB — ~22 ns per preceding byte, against 26–39 ns for an average line. *Unclassifiable because:* it is amortised to once per paragraph **only** by `TABLE_VISITED` (C26), a memo the tree-equality oracle cannot see. *At a pause:* the memo must be carried or the spike becomes a genuine quadratic. *Smallest change:* none available in Stage 1 — the probe is inherent to GFM's delimiter row. State it as a named exemption in the slope gate, with the memo pinned by a structural invariant.

**H8 · Postprocess is Θ(document), destructive and prefix-dependent.** Autolink's runs the whole tree at finish (`extensions/autolink.c:564-601`), re-running consolidation itself at `:571`; measured 1.59 / 2.72 / 4.68 / 9.92 ms at 0.27 / 0.55 / 1.1 / 2.2 MB — **~2.7× the entire block feed**, 72–75 % of finish. It is not idempotent (it always inserts a `post` node, leaving empty Text nodes with zeroed positions), and it edits backwards via `markdown_core_node_unput` (`extensions/autolink.c:313` → `core/inlines.c:1925-1933`), shortening an already-emitted sibling. Formula's is recursive over the whole tree (`extensions/formula.c:542-585`) and **frees the paragraph node it promotes** (`extensions/formula.c:534-536`); measured, `$$x$$` alone is a `FormulaBlock` and `$$x$$\ny` is a `Paragraph` — so the promotion must be undoable and today the node it would restore is gone. *Smallest change:* consolidation and autolink's postprocess are strictly per-parent and can move to block close; formula's promotion must be decided at close, not re-decided per snapshot.

**H9 · `parser->current` is not the bottom of the open spine, and `__OPEN` is not a reliable index of it.** Instrumented over three corpora: `spec.txt` 0/11 880 mismatches, `extensions-directive.txt` 0/657, **`docs/RECONSTRUCTION.md` 158/1870** — every one a table, `current = TABLE_ROW` while the chain tail is `TABLE_CELL`. Cells are added by `markdown_core_parser_add_child` (`extensions/table.c:441,500,516`) and never finalized; `finalize_document` closes only `current`…`root` (`core/blocks.c:792-797`), so **689 of 6 785 nodes still carry `__OPEN` in the finished tree**. *Smallest change:* carry `current` explicitly, and stop shipping `__OPEN` (and `__LAST_LINE_BLANK`, 593 survivors, and `__LAST_LINE_CHECKED`, 45) to the caller.

**H10 · `finalize` is destructive per node type, non-idempotent, and O(items) for lists.** `assert(b->flags & __OPEN)` (`core/blocks.c:365`) is compiled out in Release; a second call re-detaches an already-empty buffer and, for a fenced block, reads past the logical end with `assert(pos < node_content->size)` also gone (`core/blocks.c:408-418`). A definitions-only paragraph is **freed** (`core/blocks.c:391-393`) — the node `parser->current` may name. `S_ends_with_blank_line` is a query with a permanent side effect (`core/blocks.c:329-340`), setting a flag nothing ever clears while `__LAST_LINE_BLANK` keeps being rewritten. And `finalize(LIST)` walks every item and every item's children (`core/blocks.c:435-461`): measured 12 / 40 / 123 / 209 µs at 1k / 4k / 16k / 32k items, against ≈0 for every other close. *At a pause:* "snapshot = finalize the open chain, keep going" aborts on the second snapshot, permanently poisons the tightness of every list it touched, and carries an O(items) term. *Smallest change:* separate *answering* the close questions from *performing* the close.

**H11 · The extension model is process-global, so a paused parser is not a self-contained value.** Node types are registration ordinals assigned into non-static globals (`extensions/table.c:18`, `strikethrough.c:4`, `formula.c:14-15`, `directive.c:29-31`) with `MARKDOWN_CORE_NODE_LAST_BLOCK`/`_LAST_INLINE` mutated at `core/syntax_extension.c:36-45`; the flag bit comes from a function-static cursor (`core/node.c:21`); the registry has no unregister path by design (`core/registry.h:11-15`); and a second registration **aborts the process** (`core/node.c:25-28`, measured exit 134). Measured values: `MARKDOWN_CORE_NODE_TABLE = 32779`, `_STRIKETHROUGH = 49163`. *At a pause:* the struct is achievable, but only after Step 3; until then a snapshot is a struct **plus an implicit dependency on this process's registration history**. *Smallest change:* Q16 — a fixed enum decoupled from attach order, and static descriptors.

**H12 · The arena is ambient process state, and it hides the rest of this list.** `static struct arena_chunk *A` (`core/arena.c:12`) is one chain for the whole process; `arena_free` is a no-op (`core/arena.c:93-96`); `arena_push` is a silent no-op while `A` is cold (`core/arena.c:27`); an unbalanced `pop` frees the whole chain; `markdown_core_arena_reset` is public and frees documents their owners still hold; and OOM policy is `abort()` (`core/arena.c:16,21`). Table calls `push`/`pop` unconditionally mid-line (`extensions/table.c:342,352,357,550,556`) regardless of whether `parser->mem` is the arena, so its control flow is a function of *another parser's* allocation history — measured with `leaks --atExit`: 0 leaks with a malloc parser alone, **12 leaks / 480 bytes** for the same document if any arena parse happened earlier in the process (16 / 608 at three columns). And because the CLI uses the arena (`core/main.c:238`), **every use-after-free in this list is invisible through the `markdown-core` binary** — H5's dangling literal silently serves stale bytes instead of crashing. **Not under the ASan preset, though**, and 0a.2 proved it: the fixture runner allocates through malloc, so an ordinary corpus example makes `correctness-asan` a real memory-safety gate. The blind spot is the CLI and `dump_cli_runner`, not the suite. *Smallest change:* Q12 already rules delete. This inventory makes it a Stage 1 prerequisite rather than a cleanup: a parser held open across snapshots grows monotonically under the arena, which alone breaks the allocation bound (Q36).

**H13 · `oom` is a single sticky bit shared by the live parse, the inline phase, the extensions and any snapshot pass.** Set at ~66 sites, 41 of them inside extensions; read at `core/blocks.c:1586` (feed becomes a no-op), `:1637`, `:1699` (finish frees the tree and returns NULL, then resets — so `oom` is silently back to 0 with a fresh document). *At a pause:* an allocation failure inside a *snapshot's* inline or postprocess pass poisons the *live* parse for a reason unrelated to the bytes fed; and a parser with `oom` set has no correct snapshot at all, because the only way the baseline reports the failure destroys the parse. *Smallest change:* split the bit — see Q34.

**H14 · `curline.size == 0` is an undeclared signal, and the branch it selects is already wrong.** `finalize`'s three-way end-position branch (`core/blocks.c:368-383`) reads emptiness as *"end of input"*. Branch 2 — DOCUMENT, fenced CODE\_BLOCK, setext HEADING — reports the *current* line, which is right for a fence closed on its own fence and wrong for a setext heading closed later: `foo\n===\n` gives `Heading 1:1..2:3`, but `foo\n===\nnext paragraph is quite long\n` gives `Heading 1:1..3:33`. An unclosed fence inside a container produces a child that outreaches its parent. This is inherited character-for-character from `.tools/cmark-gfm/0.29.0.gfm.13/src/blocks.c:296-311`, is not in the sixteen, and no gate sees it (`specs/scope-sanity/ledger.json` tracks sentinels and negative ranges only). *At a pause:* it matters twice — it is a place where "the document ended here" gives a different and better answer, and it means **Stage 2 parking a partial line in `curline` silently flips every block to branch 2**. *Smallest change:* replace the emptiness test with an explicit "is a line in flight" field.

**H15 · The block phase scans by NUL sentinel, not by length.** `#define peek_at(i, n) (i)->data[n]` (`core/blocks.c:44`) is unguarded, and `S_find_first_nonspace:992`, `S_advance_offset:1027`, `S_scan_thematic_break:967`, `parse_list_marker:741`, `chop_trailing_hashtags:939` and `parse_footnote_definition_block_prefix:1078` all terminate on the terminator. D4's read at `core/inlines.c:492` is the inline instance. Safe today **only** because `input` is always the whole of `curline` and strbuf keeps it NUL-terminated. *At a pause:* nothing breaks. *At the substrate change Stage 1 will want* — handing the block phase a slice of a larger buffer to avoid copying each line — all of them break silently, with no gate. *Smallest change:* land D4 at 0a.3 as planned, and forbid slice-shaped `subj.input` until then.

**H16 · `opaque_alloc`/`opaque_free` are not inverses, and one payload aliases a scalar.** `extensions/table.c:593-603` allocates for TABLE\_CELL; `:605-611` frees only TABLE and TABLE\_ROW — measured 32-byte leak on a constructed cell. Worse, `as.opaque` shares the union slot with `as.cell_index`, written as an `int` at `extensions/table.c:125` (`core/node.h:95-96`) — a pointer and an int in one field, with liveness decided by construction path. `markdown_core_node_set_type` calls `free_node_as` but never the extension's alloc/free pair, which is why table hand-allocates at `extensions/table.c:385` and why a retyped node keeps its old paragraph text in `content` forever. **This is the union-aliasing class §1 records as having killed the previous attempt.**

**H17 · Per-line facts stored in persistent structures, and public accessors that lie.** `node_directive.consume_line` (`extensions/directive.c:1154,1174,1180`) is written on line *i*, cleared on line *i+1* before anything reads it — but it lives on the node, and the early return at `:1171-1172` skips the clear once `closed`, so a closed node keeps it set forever. Symmetrically, `markdown_core_parser_get_offset`/`_column`/`_first_nonspace`/`_first_nonspace_column`/`_indent`/`_is_blank`/`_has_partially_consumed_tab` (`core/blocks.c:1716-1732`) are documented (`core/markdown-core-extension-api.h:336-460`) as describing "the line currently being processed" and, between lines, return the previous line's residue. *Smallest change:* Step 3 declares each payload field's cadence; the accessors either report "no line in flight" or are not exported.

**H18 · Reading the tree mutates it.** `markdown_core_chunk_to_cstr` takes ownership on first call (`core/chunk.h:58-76`), so `markdown_core_node_get_literal`/`_url`/`_title`/`_fence_info` (`core/node.c:367,371,582,651,683`), `markdown_core_extensions_get_formula_literal` (`extensions/formula.c:56-62`), `_get_directive_name` (`extensions/directive.c:592-598`) and `render_attributes_json` all **write into the node on read**. Whether a snapshot is self-owning therefore depends on what the consumer happened to read. *At a pause:* every dump of a snapshot is a write into live parser-owned memory. *Smallest change:* Q17's stored range removes the reason these exist.

**H19 · `markdown_core_parser_feed_reentrant` is a live use-after-free on the exported surface.** `core/blocks.c:849-860` re-enters `S_process_line`, which clears and refills `curline` while the outer frame still holds `input.data` into it. Confirmed under ASan: `heap-use-after-free … READ of size 1 … add_text_to_container blocks.c:1478`, freed by `strbuf_put` ← `S_process_line:1594` ← `feed_reentrant:856`. It saves and restores only `linebuf`, and does so through `strbuf_cstr`/`_sets` (NUL-truncating); `line_number`, `offset`, `column`, `first_nonspace`, `indent`, `blank`, `partially_consumed_tab` and `last_line_length` are all clobbered and the outer frame keeps using them. The only mitigation is the `current == parser->current` guard at `core/blocks.c:1641`. Zero in-tree callers. **Q28 already rules delete; this is the witness.**

**H20 · Most of §11.1 is invisible to a tree-equality oracle.** The dump exposes table alignments/columns/isHeader, list-item checked, formula mode/literal, directive mode/name/attributes/label. It does **not** expose `TABLE_VISITED`, `n_rows`, `n_nonempty_cells`, `block_delim`, formula `closed`, `fence_length`, directive `closed`, `consume_line`, or any of `__OPEN`/`__LAST_LINE_BLANK`/`__LAST_LINE_CHECKED`. *At a pause:* criterion 1 as written **cannot detect the loss of any of them**, because equality is taken after finish, where they have already done their work. *Smallest change:* Stage 1's gate must include structural invariants over the carried set, not only tree equality — and for `TABLE_VISITED` the invariant is not correctness but the criterion-2 bound.

Two further hazards are recorded because they are cheap to fix and expensive to rediscover. `markdown_core_strbuf_drop` writes `buf->ptr[0] = '\0'` unconditionally (`core/buffer.c:216-226`); on a never-grown buffer `ptr` is the process-global `markdown_core_strbuf__initbuf` (`core/buffer.c:17`), so it is a write to a byte shared by every parser and thread — benign today, a TSan race tomorrow, and every other mutator guards correctly. And `MAX_LINK_LABEL_LENGTH` (1000, `core/parser.h:14`) is enforced inside `markdown_core_map_lookup` (`core/map.c:271`), which the **footnote** map also goes through: measured, a 900-character footnote label resolves, a 1200-character one silently fails to resolve and the definition is then dropped from the tree as unreferenced. Data loss with no diagnostic. Link labels can never reach it (`core/inlines.c:1126` caps them); footnote labels are capped nowhere.

#### A new defect: D25

**A `FootnoteReference`'s label can be a dangling pointer, and it is read on every footnote lookup.** `handle_close_bracket` builds the label as a **borrow of a sibling node's literal** with a length computed from **columns** (`core/inlines.c:1338-1339,1353`), and the loop at `core/inlines.c:1381-1386` then frees that node. Normally the `^`-bearing node's literal is itself a borrow, so the dangle is harmless by accident — but when the `^` arrives as a character reference (`&Hat;`, `&#94;`), the node's literal is the entity-decode buffer: **owned, one byte long**, and the column-derived length reads far past it. ASan witness on `x [&Hat;abcdefghij] y\n\n[^z]: note\n`, default allocator: `heap-use-after-free … READ of size 1` in `markdown_core_utf8proc_iterate` ← `case_fold` ← `markdown_core_map_lookup (map.c:279)` ← `process_footnotes`, freed by `markdown_core_node_free` ← `handle_close_bracket (inlines.c:1384)`, allocated by `houdini_unescape_ent`. Under the CLI's arena it does not crash; it prints `literal="x [^\u0000…<U+FFFD>…] y"`. No gate saw it: `grep -rn '&Hat;\|&#94;\|&#x5[eE];' tests/ samples/` returned nothing. **The claim that H12 also made it invisible under `--preset asan` is wrong** — the fixture runner uses the default allocator, so the corpus example added at 0a.2 is a complete gate; see §4.2.9. It is the same root-cause family as **D10** and 0a.2's ~10-line fix may well cover it, **but D10 is written up as the undefined-call reconstruction path while this fires inside `markdown_core_map_lookup` on any document with at least one footnote definition.** Name it in 0a.2's statement with this witness, or it will be fixed by accident and left unpinned.

---

### 11.5 The verdict on criterion 2

**T(document) = Σᵢ T(line i) is achievable, and the inventory says so with one qualification and one deletion.**

The qualification: it is achievable in the **summed** form as §3 states it, and *not* in the "flat per-line" form §3 offers as the testable restatement, unless the gate names its exemptions. Three per-line costs scale with something other than the current line, and only one of them is a Stage 1 defect. Table's header probe (H7) is Θ(paragraph so far) once per paragraph, and it is inherent to GFM. Uncapped block-quote depth (`MAX_LIST_DEPTH` at `core/blocks.c:38` guards lists and footnotes only) makes `check_open_blocks` and the ancestor loop at `core/blocks.c:1497-1501` walk a spine bounded only by line length — measured 948 ns/line at depth 1 to 12 668 ns at depth 4 000, a slope in *i* that a one-shot parse pays identically. And closing a block is O(block): `strbuf_drop`'s memmove (`core/buffer.c:222`) from `core/blocks.c:353` and `:422` costs 26 / 24 / 71 / 124 µs at 160 KB / 640 KB / 2.5 MB / 5.1 MB, and `finalize(LIST)` is O(items). **The gate must be a slope over a corpus of bounded blocks and bounded depth, with these three named as separate series carrying stated per-line spike bounds.** A gate that does not do this fails Stage 1 for reasons Stage 1 did not cause.

The deletion: **the single mechanism most likely to violate criterion 2 is `process_inlines` re-parsing `parent->content` for every block on every snapshot** (`core/blocks.c:522-546,808` → `core/inlines.c:1678-1700`). It is Θ(tree) per call, it is the entire content of the "clone and finish" cheat, and it re-derives work proportional to the document so far for one reason: **`contains_inlines` is type-only — there is no per-block record that a block's inlines have already been parsed** (H4). That is the specific thing. Give a block one flag bit and move the call into `finalize`, and the sum becomes Θ(bytes) by construction: every block is parsed exactly once, in the line that closes it.

**Between the two named candidates — the inline phase and the late-resolved reference map — the reference map is worse.** Three reasons, in order of weight.

1. **The inline phase's violation is an implementation accident; the map's reaches backwards by construction.** Moving `markdown_core_parse_inlines` to `finalize` requires no new carried state at all — the subject is *already* per-block (`subject_from_buf` resets all 21 fields, `core/inlines.c:229-250`), and Fact 2 of the inline inventory holds: `contains_inlines` is true only for PARAGRAPH, HEADING, DIRECTIVE\_LABEL and TABLE\_CELL, of which only the paragraph survives a line boundary. So exactly **one** subject is ever live across a pause, not one per open block. The map, by contrast, must edit output that was already emitted and already correct.

2. **The map's cost is proportional to the document already parsed, and it is measured.** 20 000 `[a]` references with no definition parse in 13.44 ms; the same document with one trailing `[a]: /url` takes 17.72 ms. **4.28 ms — 0.21 µs × 20 000 — is attributable to one line.** The footnote equivalent is 16.4 → 24.2 ms, 7.8 ms on one line. And today the only implementation available for that flip is a tree rescan, because `markdown_core_reference_create` (`core/references.c:18-58`) has **no back-index to the sites waiting on the label it just defined**. Without one, every definition line costs O(document) — a flat criterion-2 failure on *every* definition, not just pathological ones.

3. **The map is the only subsystem whose *answer*, not merely its cost, depends on bytes not yet fed.** H2. No carried state fixes that; only deletion does.

The inline phase's residual problem is **distribution, not asymptotics**: with the call moved to `finalize`, the max over lines becomes Θ(largest block) — measured 4.6 / 7.4 / 13.1 / 25.3 ms for a single paragraph of 2k / 4k / 8k / 16k lines, ≈1.5 µs per paragraph line, all of it landing on the closing line. That is a burst on a corpus of unbounded blocks and it is flat on a corpus of bounded ones. Flattening it further — a resumable subject inside the open block — is a strict addition, and it should be scoped separately with its reach measured first, because its seven hazards (H5 and the three end-of-buffer memos at `core/inlines.c:387,1034,1046,1056,1067`, which become false negatives on resume) are all ways to be silently wrong rather than slow.

---

### 11.6 The late-resolution question, answered

**Yes. "A snapshot of *i* lines is a parse of *i* lines" resolves the semantics of late resolution completely, and it is the right frame. It does not resolve the cost, and it does not make snapshots monotone.**

Stated plainly, as the rule Stage 1 adopts: **a definition arriving at line *i+5* changes the tree at *i+5*, not retroactively.** The snapshot at line *i* was never wrong and never provisional. At line *i* the label was undefined, and §5.1 is already settled on what that means — *a label that no definition defines does not produce a reference node at all; the brackets are prose*. So the snapshot at line *i* shows `Paragraph → Text "[foo]"`, the snapshot at line *i+5* shows `Paragraph → Link → Text "foo"`, and both are exactly what CommonMark says about their own bytes. There is no pending state, no provisional node kind, no invented semantics, and — decisively — **nothing to retract**, which is the mechanism that killed the previous program.

What it costs, precisely:

- **Snapshots are not monotone.** `one_shot(1…i+5)` is not an extension of `one_shot(1…i)`. A node emitted on line 1 changes kind, and today a footnote definition also changes parent and sibling position. Calling each snapshot its own parse makes the rewrite *correct*; it does not make it *free*.
- **The rewrite must be O(sites waiting on the label just defined), and each site must flip at most once.** That is the whole of the criterion-2 argument for this subsystem. A site waits on exactly one label; when that label is defined the site flips and leaves the pending set; so the total flip work over a document is O(number of reference sites) = O(document), and Σᵢ T(line i) stays linear. **If instead the flip re-runs the block's inline pass, a block with *m* distinct undefined labels is re-parsed *m* times and the sum becomes Θ(block × m) — that is the quadratic.** The distinction between "re-resolve the site" and "re-parse the block" is the single design decision that decides whether criterion 2 survives late resolution.
- **The one thing that no framing fixes is D9.** While `max_ref_size` is a function of the final document size, the tree over lines 1…62 depends on bytes at line 63. Deleting the budget is a prerequisite, not an optimization (H2).

What it therefore requires, as state: `parser->refmap` carried (already is, and already boundary-safe — C15); **a label → pending-sites reverse index**, which §5.3 does not currently name and criterion 2 requires; `parser->footnote_defs` registered at container **open** rather than iterator EXIT, so definedness is answerable during inline parsing and D11's close-order data loss goes away with it; a per-block "inlines parsed" marker (H4); a map that permits interleaved insert and lookup (H3); and the deletions of §5.3 plus the budget.

**One further consequence, and it is the reason this section matters more than it looks.** §3 states criterion 1 twice in non-equivalent forms: the block quote is partition-invariance of the *final* tree, the surrounding prose demands that a snapshot after *k* lines equal a one-shot of those *k* lines. **Under the written form, Stage 1 is already finished** — measured, 808 line-boundary prefixes across two corpora already produce byte-identical trees, because `S_parser_feed` splits on line ends regardless of how the caller chunks the input. A criterion that HEAD already satisfies cannot be the acceptance criterion for a major refactor. **The prose reading is therefore the operative one**, and §11.9's Q33 records the ruling. Keep the written form as a regression gate; it is free and it is real.

---

### 11.7 What Stage 0 must not break

The inventory's most immediately useful output. Each item is a constraint on work that starts now.

#### Step 3 — the extension model

**Must establish.** Fixed node-type values and fixed node-flag bits in an enum decoupled from attach order (Q16) — while `node->type` is a registration ordinal (`core/syntax_extension.c:36-45`) and the flag bit comes from a function-static cursor (`core/node.c:21`), a snapshot is only interpretable inside a process whose registration history matched, and *"a paused parser is a plain struct"* is unreachable (H11). Static descriptors with no registry mutation and no `abort()` on re-registration. **Deletion of the arena** (Q12) — not as a performance call but because table's opener branches on process-global allocator history (H12, measured 480-byte leak in a parser that never asked for it) and because the arena hides every lifetime defect in §11.4 from the binary the parity oracles drive. A declared cadence for every hook: which run per line, which at finish, and — new — which are safe to run more than once. And a stated, gated rule that no extension retains a pointer into `parser->curline` across a line boundary; this is true today at every site (`extensions/directive.c:177-204`, `extensions/table.c:219`, `extensions/formula.c:114-125`) and it is true by inspection only.

**Must preserve.** `parser->syntax_extensions` as a **single ordered list with one owner** — it is carried state, and Q9's "table last" must apply to the CLI and the facade alike (D15), because the same list decides block opening, inline matching and postprocess order. Every per-node opaque payload named in C25–C29 must survive the descriptor rewrite **by name**: `block_delim`, `closed`, `fence_length`, `consume_line`, `n_rows`, `n_nonempty_cells` and `TABLE_VISITED` are all carried, all load-bearing, and **all invisible to every existing oracle** (H20) — a Step 3 that regenerates goldens without a structural gate over them will bless their loss silently.

**Must fix while it is in there.** The `opaque_alloc`/`opaque_free` asymmetry and the `as.opaque`/`as.cell_index` union aliasing (H16). D24's whole-line `strstr` for `checked` is already assigned here; note that it makes `checked` a function of the line rather than of the construct, which a snapshot format should not enshrine.

#### Step 8 — the inline phase

**Must preserve, above everything else.** `markdown_core_parse_inlines` stays a **pure per-block function** of `(parent->content, parent->start_line, parent->start_column, parent->internal_offset, refmap, options, the two character tables, the inline extension list, backslash_ispunct)`. There is no mutable file-scope state in `core/inlines.c` today — every `static` is const data or a pure helper — and the subject is fully reset per block. **Nothing may be hoisted into a per-document accumulator.** That purity is the entire reason a per-block scheme is possible; it is also what makes Fact 2 hold, so that exactly one subject is live across a pause.

**Must establish.** A per-block "inlines already parsed" marker (H4) — one bit, tested by `process_inlines`. **Literal ownership at emission**, which Q17 already gives for free: store a byte range on the inline node instead of a pointer, and H5's five invalidation mechanisms stop mattering, positions become projections, D12 becomes unexpressible, and — the connection Q36 depends on — a closed block can release its `content` buffer, which is what makes the resident-memory bound achievable at all. The `markdown_core_manage_extensions_special_characters` pair must move from inside `process_inlines` (`core/blocks.c:534,543`) to parser lifetime, refcounted so two extensions claiming one character do not un-register each other (`core/inlines.c:1520-1528`); this also fixes D1 and D2.

**Must not do.** Do **not** turn `subj.input` into a slice of a larger buffer before D4 lands (H15) — that is the obvious way to avoid copying each line, and it converts six sentinel-terminated loops and one inline read into live overreads with no gate. Do not let consolidation move ahead of stack draining: `markdown_core_consolidate_text_nodes` frees nodes that `delimiter->inl_text` and `bracket->inl_text` pin (`core/iterator.c:117`), and today that is safe only by strict ordering.

**Must delete.** `bracket->active` (`core/inlines.c:41`, written at `:603`, **read nowhere** in core or extensions — the engine replaced upstream's per-bracket deactivation with `subj->no_link_openers`). A dead field must not enter a snapshot format. Also record that `subj->flags`' four HTML-skip bits, `scanned_for_backticks` and `backticks[81]` are memos **sound only over a complete buffer**: they are set precisely when a scan runs off the end. They are per-pass, never per-parser, and any future resumable subject must clear them.

#### Step 9 — the reference model

**Must establish.** A map that permits **interleaved insert and lookup**: delete `prepared`, `indexed`, `sorted`, the lazy-prepare branch and both asserts; maintain the index incrementally on insert; and while in there, fix `index_map`'s leak-on-reindex (`core/map.c:69`) and split `map->size`'s two meanings so `entry.age` cannot collide (H3). **A label → pending-sites reverse index** — §5.3 does not name it and criterion 2 requires it (§11.6). **`parser->footnote_defs`, registered at container open**, so definedness is answerable during inline parsing rather than reconstructed at finish; this is the one addition §5.3 already owes, and D11's data loss is an independent reason to take the ordering change.

**Must delete.** `ref_size`, `max_ref_size`, `entry.size` and the destination clone (D9 / H2) — nothing else makes prefix equality reachable. The ordinal, the back-pointer, the label overwrite, the qsort, the hoist and the unreferenced-drop (§5.3) — every one is a whole-document function evaluated once (H6). `MAX_LINK_LABEL_LENGTH`'s application to footnote labels (`core/map.c:271`), which silently deletes a long-labelled definition.

**Must preserve.** Harvest stays **per-line and parser-blind**: `markdown_core_parse_reference_inline` runs `subject_from_buf(NULL, …)` against the immutable base tables (`core/inlines.c:1726`), so a definition's meaning does not depend on which extensions are attached — that purity is why harvest is already boundary-safe and why the refmap needs no work. Label, url and title stay **owned copies** (`core/references.c:46-49`); do not optimize the refmap into borrows to save allocations, because it is the one subsystem in the engine that already survives a pause unchanged. And `resolve_reference_link_definitions` has **two** call sites — `finalize` (`core/blocks.c:390`) and the setext path (`core/blocks.c:1330`) — so a hook keyed on "definitions resolved" fires twice while one keyed on `finalize` fires once; key on `finalize`.

**Must fix at 0a.2.** D25, with the `&Hat;` witness, named explicitly rather than folded into D10.

#### Cross-cutting, for all three

`markdown_core_parser_reset` must stop dropping `backslash_ispunct` (C13, H1) — or `reset` must stop existing. Every golden regenerated during Stage 0 is regenerated over a tree that carries `__OPEN` on 689 nodes of one corpus, `__LAST_LINE_BLANK` on 593, and `__LAST_LINE_CHECKED` on 45; decide before regenerating whether those are part of the value. And no Stage 0 step may add a second consumer of `curline.size == 0` as a proxy for "no line in flight" (H14).

---

### 11.8 The six API decisions Stage 1 must settle

Recorded as ledger entries **Q31–Q36**, continuing §9's numbering. Recommendations are this inventory's, not the owner's.

| id | Question | Recommendation |
|---|---|---|
| **Q31** | What is the public append surface? | **SETTLED by the owner, 2026-08-20:** `Document(markdown:)` and `document.append(chunk:) -> Document`. There is no separate snapshot call — **append returns the readable document.** The C surface serves that shape; it does not define it. |
| **Q32** | Who owns a snapshot, and how long does it stay valid once more lines are fed? | **The caller owns it; it is a fully independent tree that aliases no parser memory and stays valid forever.** |
| **Q33** | Is equality required after every prefix, or only at the end? | **After every prefix.** Keep partition-invariance as a regression gate. |
| **Q34** | What is failure and OOM behaviour mid-stream? | **Split `oom` into a terminal "parse lost" bit and a per-call "snapshot failed" result, and expose a query for the former.** |
| **Q35** | Do the bindings participate in Stage 1? | **No — C only** — with one shape constraint that applies now. |
| **Q36** | What allocation bound accompanies the time bound? | **Two bounds, and the resident one gets its own slope gate.** |

**Q31 — the surface, settled.** The owner's shape is
`let document = Document(markdown: String)` and
`let updated = document.append(chunk: String)`. Append *is* the read; there is
no second call. What follows is the inventory's reasoning about the C surface
beneath it, which stands except where it proposed a separate `snapshot()` —
that proposal is superseded.

**A consequence that must be stated, because it is where this stage goes wrong
if it is not.** If every append returns a document, and materialising a document
costs O(document), then a caller appending *l* lines pays Θ(l²) — and it is no
longer "the caller's choice", because the API gives them no other option. **The
per-append cost must be O(line).** That is not a constraint the API imposes on
the engine; it is the flow's own property, restated at the surface: continuing
the flow costs the line, and nothing else. Whatever the C surface does, it may
not make reading the document a function of the document's size.

**Q31 (inventory's original reasoning on the C surface).** `markdown_core_parser_feed` already splits on line ends internally (`core/blocks.c:862-930`) and already satisfies partition-invariance; making the public call line-oriented would buy nothing and would hand callers a framing problem the engine already solves. The line is Stage 1's *internal* unit. Add one call, returning an owned tree; a caller that has fed half a line gets a snapshot of the lines completed so far, and Stage 2 is what lifts that restriction. Do **not** overload `finish`: it must stay the one-way terminator, because everything downstream of `finalize_document` is one-way (H6, H8, H10) and because a caller needs to be able to say "this stream is over" distinctly from "show me what you have". Finish should also stop being a reset (H1) — a finished parser reports finished, and reuse is `parser_free` + `parser_new`.

**Q32 — ownership and validity. Superseded in part.** The inventory's answer
below — an independent fully-owned tree per snapshot — is **correct about the
hazards and wrong as a per-append default**, because under Q31's settled shape
every append would pay it. Its own note concedes the arithmetic: *"a caller that
snapshots every line pays Θ(l²)"*. Under the settled API that is not a caller's
choice, so it is a violation of the flow.

What survives, and it is the important half: **no node pointer and no node
identity is stable across a line boundary today**, for five named reasons. That
is a statement about the *engine*, not about the API, and it is a defect list
for Stage 1 rather than a reason to copy. Making a closed block's nodes stable
once closed is the same work as doing each block's work in the line that closes
it — a block that is finished does not move again.

The original reasoning follows.

**Q32 (inventory's original reasoning).** A snapshot must be an **independent, fully-owned tree**, freed by the caller with `markdown_core_node_free`. The alternative — a borrowed view over live parser memory — is not merely risky, it is unimplementable: every inline literal borrows a block's `content` buffer that five mechanisms move (H5), table retypes and re-parents an open paragraph mid-line (`extensions/table.c:369-378,447`), formula's promotion frees the paragraph node it replaces (`extensions/formula.c:534-536`), and autolink edits a previously emitted sibling backwards (`extensions/autolink.c:313`). **No node pointer and no node identity is stable across a line boundary.** State the cost honestly in Q36: a snapshot is O(size of the snapshot), and a caller that snapshots every line pays Θ(l²) in *its own* allocation — which is fine, because it is the caller's choice and it is not the parser re-deriving anything.

**Q33 — prefix or end.** Prefix, for the reason §11.6 gives: **partition-invariance is already true at HEAD**, measured over 808 prefixes across two corpora, so adopting the written form alone makes Stage 1 vacuous. Adopt the prose reading as criterion 1b — *the tree after k lines equals a one-shot parse of those k lines* — and keep 1a as a cheap regression gate. This is also the ruling that makes the late-resolution question well-posed at all: without 1b there is nothing for a definition to change, because nobody looks until the end.

**Q34 — failure mid-stream. SETTLED by the owner, 2026-08-20: `throws`.**

`func append(chunk: String) throws -> Document`. And the shape carries a
requirement that must not be assumed away: under value semantics, when the call
throws, `updated` is never bound and **`document` is still in scope and must
still be readable.** So a failed append may not leave the parser part-way
through a line.

> **Append is atomic.** Either the line's work is applied in full, or none of it
> is and the parser stands exactly where it stood before the call.

This is the opposite of what the engine does today, in three named ways:
`finish` reports a terminal loss by **destroying the tree**
(`core/blocks.c:1697-1704`); `parser->oom` is one sticky bit meaning four
different things, written from 66 sites, 41 of them in extensions (C10); and
under the arena there is no allocation-failure path at all — `alloc_arena_chunk`
calls `abort()` (`core/arena.c:16,21`), which is a fifth independent reason for
Q12's deletion.

Atomicity is also the natural shape for the flow rather than an imposition on
it: the line is already the unit of work, so "apply the line or don't" is the
transaction the parser is already structured around. What it costs is that every
allocation-failure point inside a line must either be moved before the first
mutation, or be undoable. That is §4.13's question.

**Q34 (inventory's original reasoning).** Today `parser->oom` is one bit meaning four things: the block phase lost an allocation, the inline phase did, an extension did, and "this parse is over". Under Stage 1 a fifth appears — a snapshot failed to allocate — and it must not be the same bit (H13): a snapshot's failure must leave the live parse alive and untouched, and the live parse's failure must not be reported by destroying the tree, which is what `finish` does today (`core/blocks.c:1697-1704`). Recommend: `snapshot()` returns NULL on its own allocation failure and sets nothing; a terminal parse loss sets a sticky bit that makes further `feed` a no-op (as now) and makes `snapshot()` and `finish()` both return NULL; and add a query so a caller can distinguish truncation from success without calling `finish`. Note that under the arena there is no OOM path at all — `alloc_arena_chunk` calls `abort()` (`core/arena.c:16,21`) — which is a fourth independent reason for Q12's deletion.

**Q35 — bindings.** No. Three reasons: all three bindings copy into value types and free the handle, so a snapshot API costs a full deep copy per snapshot in each language and none of them can express a borrowed view even if Q32 allowed one; the ABI window is Step 12, after Stage 1; and Stage 1's gate is a timing slope on the C library, which no binding participates in. **One constraint applies now regardless:** Stage 1 must not adopt a C shape the bindings cannot express later — no borrowed views, no callback-driven feed, no snapshot whose validity is scoped to a parser generation. The surface added at 3.0 must be the same shape as the C one.

**Q36 — the allocation bound.** State two, because they answer different questions. **(a) Resident parser state is O(open depth + Σ open blocks' content + definitions so far), with no term in the number of lines already fed.** **(b) A snapshot costs O(snapshot) allocations, once, charged to the caller, with nothing retained by the parser.** Bound (a) is the one that matters and the one no timing gate can see: a "keep a copy of every line" cheat is invisible to a flat-slope timing series and obvious in a peak-RSS series over the same corpus. Gate it the same way — a fitted slope in *i* indistinguishable from zero on a bounded-block corpus. Two facts make (a) work to earn rather than to assume: **under the arena it is false today by design** — `arena_free` is a no-op and `arena_realloc` always allocates fresh and copies (`core/arena.c:83-96`), so a parser held open across snapshots grows monotonically including every superseded buffer copy — and **every block node keeps its `content` strbuf forever** (`core/blocks.c:125`; measured, a finished document's root still holds `asize=56`, and every paragraph holds its full source text). Releasing a closed block's content is exactly what Step 8's own-on-emission unlocks (§11.7), which is why Q17 and Q36 are one decision seen twice.
