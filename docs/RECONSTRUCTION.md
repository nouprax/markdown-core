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
| Landed | Steps **0, 1, 0a** (0a.0–0a.15), **2** (§4.14.2), **3a** (3a.1–3a.3, §4.14.3a), **3** (3.1–3.5, §4.14.3), **3b** (§4.14.3b), **5** (§4.14.5), **D35** (§4.14.5a), **15A.1 – 15A.4** (§4.14.15A), **6** (§4.14.6), **7.1 – 7.2 – 7c – 7d – 7e** (§4.14.7a–e), **10** (§4.14.10), **9a.1 – 9a.2** (§4.14.9a1–9a2), **11a** (§4.14.11a, §4.14.11a2), **8.1 – 8.2 – 8.3 – 8.4** (§4.14.8a–8d), **9b** (9b.1 – 9b.2, §4.14.9b1–9b2), **11b** (§4.14.11b), **11c** (§4.14.11c), **12.1** (§4.14.12a), **12.2's locator** (§4.14.12b), **`end-at-line-ending` CLOSED** (§4.14.11c2) |
| Engine | **no longer the baseline's, and this row was stale** — it described the tree before Stage 0a. Measured `580d10c`..Step 2 over `core/` + `extensions/` + `include/`: **27 files, +1,868 / −712**, of which Stage 0a's twenty-eight defect fixes and `--profile` are +771 / −165 and Step 2's braces are the rest. Step 3 then deleted seven files. |
| `VERSION` | **`3.0.0`**, as of the owner ruling of 2026-08-21. There is no 1.0.4; see §4.10 and Q27 |
| Next action | **Step 13**, then `14 15C`. **STEP 12 IS LANDED WHOLE** (§4.14.12a–12c): the C facade has both views and the law is gated by `facade_test` — which `ctest --preset correctness` does NOT run, so M30 and M33 both read 69/69 there and fail `conformance` — a region names its owner by a path that survives being copied, `markdown_core_document_region_owner_paths` answers for every region in **1.13 ms against the 96.8 ms the singular call costs in a loop**, and all three bindings return `Document` = `{semantic, concrete}` copied into value types. **The owner ruled twice here**: reading 1 over my recommendation (§4.14.12b), and then that the PAIR takes the name `Document` while the markup root becomes `DocumentRoot` — which is what C has always done. **`specs/positions/places.json` IS EMPTY** (§4.14.11c2). **Landed since**: Step 10 (§4.14.10), Step 9a (§4.14.9a1–9a2), Step 11a (§4.14.11a) with **Q44 answered** (§4.14.11a2), Step 8 (§4.14.8a–8d) with **Q45 answered** (§4.14.8d), **Step 9b** whole (§4.14.9b1–9b2) — the definition and both references are nodes, **D9 and D30 closed**, the **mdast backlog EMPTY** — **Step 11b** (§4.14.11b), which added L5 and L6 because L1–L4 are all true of the day before it, and **Step 11c** (§4.14.11c). Acceptance is **§4.8's checklist**, not the mdast backlog |

`--profile` is a named option set for the CLI, added because the restored parity
harness invokes it and the baseline had no such flag: `gfm` turns this
repository's own two extensions off so a parity run compares one language,
`gfm-extended` turns them on. No existing invocation parses differently, and
the extension attach ORDER is deliberately untouched — reordering `table` is a
behaviour change that belongs to Step 3.

### Every gate, and how to run it

The sanitizer presets have their OWN configure and build; building `default`
does not prepare them.

**`sh scripts/dev/gates.sh` runs every one of the following and prints the
number each produces**, which is how the stale rows below were found. It builds
nothing -- the three configures come first, or the sanitizer rows report GREEN
having run nothing. The binding suites are not in it; they are listed in its
header.

```
cmake --preset default && cmake --build --preset default --parallel
cmake --preset asan    && cmake --build --preset asan    --parallel
cmake --preset ubsan   && cmake --build --preset ubsan   --parallel

ctest --preset correctness -j 8            # 69/69
ctest --preset correctness-asan -j 8       # 60/60 — SEE THE WARNING BELOW
ctest --preset correctness-ubsan -j 8      # 60/60 — SEE THE WARNING BELOW
node scripts/check-canonical-ast-fixtures.mjs   # 32 kinds, 62 fields, 6 cases
node scripts/audit-ast-projections.mjs           # 32 kinds over 12 surfaces
node scripts/audit-source-lists.mjs              # 22 sources, 4 of 5 lists, 1 registered absent
bash scripts/audit-public-surface.sh
node scripts/audit-extension-special-chars.mjs   # 6 descriptors read, every byte dispatched
node scripts/audit-extension-attach-order.mjs    # one attach site, table last (D15, added 0a.11)
node scripts/check-plan-graph.mjs                # 22 steps, 45 edges, acyclic
node scripts/audit-source-lists.mjs              # 22 sources, 4 of 5 lists, 1 registered absent
node scripts/fuzz-parity.mjs --iterations 300                   # upstream, 300/300
node scripts/fuzz-parity.mjs --oracle mdast --iterations 300    # 300/300 SINCE 9b.2
node scripts/check-upstream-parity.mjs     # 888/888 vs cmark-gfm 0.29.0.gfm.13, 10/10
                                          # divergences, 4/4 projections acted
node scripts/check-mdast-parity.mjs        # 110/110, backlog EMPTY since 9b.2
node scripts/audit-scope-sanity.mjs        # 1 unresolved row, 5453 scanned, only-shrink holds

# The three position oracles, landed at 0a.1 (§4.2.7). Each fails on a row
# APPEARING and on a row CLEARING, so a fix that moves one without recording it
# fails here rather than in review.
node scripts/audit-inline-sourcepos.mjs    # 40 rows registered, 68 scanned — and
                                          # for the first time they are rows where THIS
                                          # side is right and upstream is not (§4.14.8a)
node scripts/audit-scope-containment.mjs   # 8 rows registered, 4225 scanned
node scripts/audit-position-places.mjs     # 0 rows registered, 4451 scanned -- EMPTY since §4.14.11c2

# Requirement 11a's four laws over the concrete record set, landed at 11a
# (§4.14.11a). L1 and L3 have no rows and hold by construction; L4 is checked
# by re-parsing every line-boundary prefix.
node scripts/audit-concrete-records.mjs   # 277 rows registered, 5860 regions

# D9's pin. It was REGISTERED RED from 0a.8 to 9b.2 and is now GREEN with an
# EMPTY ledger -- still fail-closed, so a row appearing fails the run. Deleting
# the budget alone cost 204.678x output growth; deleting the COPY costs nothing.
node scripts/audit-reference-order-independence.mjs  # 0 rows, green

# The formatters. `format-c.sh` became load-bearing at Step 2: with
# `InsertBraces: true` in `.clang-format` it is the only thing that says a
# conditional body has no braces, and its scope now covers the ES and Kotlin
# bridge sources as well as the engine (§4.14.2).
sh scripts/format-c.sh --check
sh scripts/format-cmake.sh --check
bash scripts/audit-test-topology.sh

# The -Werror build. NOT one of the presets above: `lint:c` configures its own
# Debug tree with MARKDOWN_CORE_WARNINGS_AS_ERRORS=ON, and it catches what a
# Release build does not. It was missing from this list until Step 6, where it
# was red on a Step 3b leftover nothing else could see (§4.14.6).
scripts/lint-c.sh
pnpm -w run lint                # lint:c + lint:swift + lint:kotlin + lint:es
```

**`22 steps, 42 edges` was stale**, and it is the second number in this table to
have drifted from what the command prints. `check-plan-graph.mjs` reads its edge
list out of *this file*, so the count moves whenever a section adds an arrow;
§4.13 added three. Anyone reconciling the gates should print the number rather
than trust the row — that is how D17 was found.

**A sanitizer preset with no build reports GREEN having run nothing.** With
`build/asan` absent, `ctest --preset correctness-asan` prints
`No tests were found!!!` and **exits 0**. Run the configure and build lines above
first, and treat a sanitizer run that does not report `60/60` as a failure
however it exited. This is a gate that cannot fail, which is worse than a gate
that is missing.

**`fuzz-parity` takes `--iterations`, not `--cases`.** An unknown flag is
silently ignored, so `--cases 5` runs the default 300 and prints `300/300`. Any
pin recorded with `--cases` measured the default and means nothing.

The upstream oracle needs a built cmark-gfm:
`scripts/init-environment.sh --install upstream-cmark`.

**Six checks are KNOWN-RED and owned, not forgotten** — the same pattern the
mdast backlog and D9's oracle use:

| Check | Why red | Owner |
|---|---|---|
| ~~`scripts/audit-ast-projections.mjs`~~ | **GREEN at 15A.2.** It was never era skew — §4.1.2 measured it as one binding a full era behind the other two, and Q30's typed child edges closed all sixteen Swift-only failures. | — |
| `scripts/format-swift.sh --check` | **NEWLY REGISTERED at 15A.2, and it was in no list.** `swift format lint --strict` exits 1 at `46e20f2` with **184** findings, all `[AllPublicDeclarationsHaveDocumentation]`; the pinned 6.3.0 matches, and `.github/workflows/ci.yml:182` runs it as a required health check. 15A.2 takes it to 170, Step 6's option deletion to **163**, Step 7.2's two new public types back to **164**, and 9b.2's rewrite of the two footnote types down to **155**. | **Q41** |
| `scripts/check-generated-scanners.sh` | Added at `8926594`; the baseline build has no re2c invocation or version pin (R9). | R9's experiment, then Step 3 |
| `node scripts/check-release-version.mjs --skip-swift` | **D17 is fixed and the 3.0.0 bump closed the rest**; what remains is **two** unexpected legacy tags — `codex-doc-pass-backup` and `pre-format-baseline` — which is repo hygiene, not engine state. Every version-drift, release-note and CHANGELOG assertion now passes. | release |
| ~~`node scripts/fuzz-parity.mjs --oracle mdast`~~ | **GREEN at 9b.2, 300/300.** It was red on every generated input for the same reason the backlog existed, and it turned green in the commit that emptied the backlog — measured on both sides, not assumed (§4.14.9b2). | — |
| `pnpm audit:ci` | **TRIAGED at Step 6, and it is era skew of the purest kind.** The script was restored from `main`, where every workflow action reference is pinned to a full commit SHA; the workflows are the baseline's, where they are tag refs. It names **`benchmark.yml`, `pr-metrics.yml` and others** — `actions/checkout@v7`, `setup-java@v5`, `setup-emsdk@v16`. No engine state is involved. Pinning them is infrastructure work and the SHAs are a license-adjacent record, so it is not something a step should invent. | release / 15C |
| `pnpm format:es:check` | **TRIAGED at Step 6: `prettier --check .` reports 100 files**, including `scripts/check-plan-graph.mjs` and `specs/upstream-parity/deltas.json`. Same skew — prettier's config came from `main`, the files did not. It is a required CI step (`ci.yml:97`). Reformatting 100 files in one commit would bury every real diff in Stage 0, and doing it per-step means each step's diff carries unrelated churn. **Q42.** | **Q42** |
| ~~`pnpm audit:source-lists`~~ | **TRIAGED AND GREEN**, ahead of Step 3a, whose row requires it to RUN. It did not fail, it **threw** — `ENOENT` on `packages/swift-markdown-core/Package.release.swift`, a release manifest that postdates `580d10c` and arrived with Step 0's `scripts/` restore. The absence is now registered in the script with an owner and printed on every run, and the pass line says **`4 of 5 lists in agreement, 1 registered absent`** so it can never read as though all five were compared. | the absence: 15C |

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

**A MUTANT HARNESS THAT RESTORES FROM A SNAPSHOT SILENTLY REVERTS EVERYTHING
YOU WROTE AFTER TAKING IT.** The loop used through Steps 10, 9a, 11a and 8 kept
a copy of each source it might mutate and copied them all back afterwards. The
snapshot for `core/inlines.c` was taken at 8.3's mutant; Q45 then edited the
same file; Q44's mutant ran, restored the whole snapshot set, and **took Q45 out
of the working tree.** Every suite was green when the commit was made and the
tree it committed was inconsistent — the goldens said a code span covers its
backticks and the engine said it does not — because the check ran against a
binary that still had the change. The repair is `9af96ce`. **Re-take the
snapshot immediately before each mutant, restore only the file the mutant
touched, and re-run the mechanised claim of the step you are standing on — not
just the suite — after a restore.**

**`rm -rf build/<preset>` before any measurement that spans a `git stash`.**
Two stash cycles at 7d left objects newer than the restored sources, so a build
that reported success ran the OLD code: the timings taken from it were
meaningless and the correctness probe silently showed the old tree. Read the
BEHAVIOUR after a rebuild, not just the exit code. This is §0's mtime trap
wearing different clothes, and it is the second time it has cost a wrong
reading.

**A workflow whose agents may build must work in a copy.** The assessment
launched at 7e let its agents edit the working tree -- an `int pending_enter`
experiment in `core/iterator.c`, a partial rewrite of `directive.c` -- and the
owner saw them before I did. Give such a run `isolation: "worktree"`, or tell
the agents to read and reason and never write.

**A preset that builds clean is not the preset CI runs.** `default`, `asan` and
`ubsan` are Release or sanitizer builds; `scripts/lint-c.sh` configures its own
Debug tree with `-Werror`, and it is the only one that fails on a discarded
qualifier or an unused result. Step 6 found it red on a Step 3b leftover, having
been green in all three presets for four steps (§4.14.6). Run `pnpm -w run lint`
before believing a C change is finished — it covers Swift, Kotlin and ES too.

**`eslint` walks git-ignored directories.** If any agent worktrees are left
under `.claude/worktrees/`, they are checkouts of the closed history and lint as
hundreds of errors nobody owns. `eslint.config.js` ignores that path as of Step
6; if a new scratch directory appears, add it rather than reading the errors.

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
2. **The mdast backlog only shrinks, and only on purpose.** Its entries each
   name the step that closes them; the gate requires each to *still* diverge, so
   a step that lands without deleting its own entries fails as loudly as a new
   divergence. Zero close in Stage 0a, by design — the backlog measures distance
   to mdast's *model*, while the defects measure wrongness against the engine's
   own intent. **24 at the baseline, 22 after Step 6, 20 after 7.1, 7 after 7.2, 6 after
   Step 10, 5 after 9a.2, and ZERO after 9b.2** — 9b.1 closed none of the five
   deliberately, because every one of them was a `Link`-versus-`LinkReference`
   disagreement rather than a definition one, and 9b.2 closed all five at once.
   **An empty backlog means Steps 6, 7, 9 and 10 have landed and NOTHING MORE**;
   Steps 11 through 15 close no entry here and Stage 0 is accepted by §4.8. Step 6's two closed by *leaving the corpus*, not
   by agreeing, so the gate now distinguishes a settled entry from an unreachable
   one and the two are recorded in `retiredBacklog` with the reason (§4.14.6).
   **An entry that stops being exercised is not an entry that closed.**
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
    --
     0  remaining
```

**EMPTY as of Step 9b.2.** 9b.1 moved the number not at all, which was the right
answer and not a stall — the definition half agreed with mdast exactly and all
five entries were the OTHER half, a `Link` carrying a copied destination where
remark has a `linkReference` carrying a label and a form. 9b.2 closed all five
in one commit, and the gate said so before the JSON was touched.

**It was 24 at the baseline and the whole of the difference is recorded.** Step
6 retired two by leaving the corpus, 7.1 closed two, 7.2 closed thirteen, Step
10 closed `pre \| lead` above a table — where the lead kept a spelling the
author did not write (§4.14.10) — and **9a.2 closed `[^orphan]`, which the JSON
had filed under Step 9b and §4.6 had said was 9a's.** §4.6 was right, and the
entry proved it by closing there.

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

### Thirty-five defects live in the baseline

The first eleven were found by reading. **All eleven have since been built,
gated and reverted** on isolated worktrees at `8e76a94` — every claim below
about a line count, a moved golden row or a green suite is a measurement, not
an estimate. Doing that found five more (D12–D16). D17 was found reconciling
the gates and is fixed at 0a.0. D18–D24 were found restating the port list as
requirements, D25 while inventorying parser state for Stage 1, D26 while executing the
Q25 ruling — see §4.2.5 — and **D31 at 0a.6**, by un-gating D3 and reading what the
newly live code then reported. It is the only one of the thirty-three that this
programme created a witness for rather than inherited, and it is inherited too:
cmark-gfm reports the same wrong column. **D32 was found at 0a.12**, while
measuring D26's cost: the backslash hard break is the third mechanism in
`core/inlines.c` that consumes a line ending, and the only one nothing had
looked at. Every one of the fourteen that Q25 put to the test was
found **fixable on the untouched baseline**; none produced an architectural
dependency, and D9 remains the plan's only exception.

**All thirty-six are recorded here**, because a defect the plan does not name
is a defect the plan will re-derive later at full price — and because a list
split across three sections is a list nobody reads.

#### The index — every defect, its owner, and how it was confirmed

D18–D25 were each reproduced independently before being scheduled, on the tree
at HEAD, with the witness shown. The three marked **[verified here]** are the
ones whose witness is stated in this section rather than in the row.

| # | What is wrong | Severity | Owner | Confirmed |
|---|---|---|---|---|
| D1 | extensions fold `$ : }` and bytes `0x01`–`0x08` into `skip_chars`, killing CommonMark flanking | wrong-output | **fixed at 0a.4** | built & reverted |
| D2 | `'}'` registered special, never consumed | wrong-output | **fixed at 0a.4** | built & reverted |
| D3 | `adjust_subj_node_newlines` behind an option nothing sets | wrong-position | **fixed at 0a.6** | built & reverted |
| D4 | `skip_chars[peek_at(...)]` read before the bounds test | latent | **fixed at 0a.3** | built & reverted |
| D5 | title-rewind path writes the scanned chunk into the refmap | wrong-output | **fixed at 0a.7** | built & reverted |
| D6 | `make_autolink` writes `title = ""` where nothing was written | wrong-output | **fixed at 0a.7** | built & reverted |
| D7 | `make_autolink` omits `column_offset + block_offset` | wrong-position | **fixed at 0a.6** | built & reverted |
| D8 | `try_opening_table_header` returns the parent on ~~eleven~~ **ten** non-opening paths | wrong-output | **fixed at 0a.5**; one of the ten went with the arena at 3a.1 (R14) | built & reverted |
| D9 | reference resolution is order-dependent | wrong-output | **CLOSED at 9b.2** (§4.14.9b2) — the budget is deleted because a reference copies nothing; `audit-reference-order-independence.mjs` is green and empty and `reference_expansion_bound` measures 0.399x | 200 refs → 99 resolve, 101 do not |
| D10 | an undefined footnote call **loses source bytes** | data-loss | **fixed at 0a.2** | `x[^a⏎b] tail` → `"x[^] tail"` |
| D11 | a nested duplicate definition **deletes a paragraph** | data-loss | **fixed at 0a.2** | `"OUTER opens first"` in no node |
| D12 | `consolidate_text_nodes` drops `end_line` | wrong-position | **fixed at 0a.14** | built & reverted |
| D13 | autolink's `len==0` sentinel leaves a zero-length `Text` | wrong-output | **fixed at 0a.14** | built & reverted |
| D14 | the `"[^"` prefix rebuilt over decoded bytes | wrong-output | **fixed at 0a.9** | built & reverted |
| D15 | the CLI and the facade attach extensions in different orders | wrong-output | **fixed at 0a.11** | built & reverted |
| D16 | two more null/empty sites | wrong-output | **fixed at 0a.7** | built & reverted |
| D17 | shipped v1.0.3 declares `MARKDOWN_CORE_VERSION` = **1.0.0** | wrong-output | **fixed at 0a.0** | header vs `VERSION` |
| D18 | a paragraph whose leading definitions were consumed keeps the **definition's** line | wrong-position | **fixed at 0a.12** | `[a]: /1⏎text here` → `Text 1:1..1:9`, a column that does not exist on line 1 **[verified here]** |
| D19 | a link takes `start_line` from the **closing** bracket | wrong-position | **fixed at 0a.12** | `[a](/u "t⏎t2") tail` → `Link 1:1..1:14`, `Text 1:15..1:19` — both on a 9-character line **[verified here]** |
| D20 | strikethrough never sets `end_column` | wrong-position | **fixed at 0a.12** | `a~~` → `Text scope=1:1..1:0` |
| D21 | **a container directive's closing fence does not close it** | **content-attribution loss** | **fixed at 0a.10** | `:::note⏎body⏎:::⏎after` → `after` is pulled *inside* the block **and** reported at line 3 while it is on line 4 **[verified here]** |
| D22 | an extension consuming a span with a line ending cannot report it | wrong-position | **primitive fixed at 0a.10; the MODEL is still Step 8's** | `Directive 1:1..1:29` on a 28-character line; blocks Step 7's oracle |
| D23 | `S_insert_emph` takes the **whole** run's start column | wrong-position + overlap | **fixed at 0a.13** | `***a**` → `Text "*"` claims columns 1–3 and `Strong` also starts at 1: two nodes, one byte |
| D24 | `tasklist` decides `checked` by `strstr` over the whole line | wrong-output | **fixed at 0a.11** | `- [ ] see [x] below` → `checked=true` |
| D25 | a `FootnoteReference` label can be a **dangling pointer**, read on every lookup | **use-after-free** | **fixed at 0a.2** | ASan: `heap-use-after-free`, READ of size 1 in `markdown_core_map_lookup (map.c:279)`, freed by `handle_close_bracket (inlines.c:1384)` |
| D26 | `handle_newline` and `handle_backslash` give `SoftBreak` and `LineBreak` no position at all | wrong-position | **fixed at 0a.12b** | proposed in §4.2.5 with every quantity wrong; measured at 0a.12 (153 rows, two sites, +22/−4), refused there because both available spellings trade one not-a-place class for another, and landed at 0a.12b once **Q40** decided that a line ending is a place for a node that IS one |
| D27 | `parser->linebuf.oom` written at six sites and read at none | silent truncation (allocation failure only) | **fixed at 3a.3**, with A1 | §4.13.11, measured: 244 input bytes become 102 with `parser->oom == 0`; re-measured at 3a.3 on a 279-byte document in 32-byte chunks — refusing allocation 6 of 25 leaves 55 of 275 text bytes |
| D28 | `extensions/formula.c` ignores `markdown_core_chunk_to_cstr`'s failure and keeps a **borrowed** pointer | **use-after-free** | **fixed at 0a.15** | §4.13.11, ASan: `heap-use-after-free`, READ of size 5 in `markdown_core_extensions_get_formula_literal` |
| D29 | `extensions/table.c:297` does not check `markdown_core_node_new_with_mem`, and `:305` dereferences NULL | **crash** | **fixed at 0a.15** | §4.13.11, SIGSEGV on `lead text⏎x | y` / `--|--` |
| D30 | `markdown_core_reference_create` commits an entry whose url or title was lost | wrong-document (allocation failure only) | **CLOSED** — the node refuses to commit a lost destination or title at 9b.1 (M19), and 9b.2 deletes the map's url and title outright, so there is no entry left to commit (§4.14.9b1–9b2) | §4.13.11, measured on four refused allocations |
| D31 | a raw HTML tag that crosses a line ending ends **one column short of its own literal** | wrong-position | 8 | found at 0a.6 and pinned as a golden row: `a <b`⏎`c> d` gives `HTML scope=1:3..2:1` for a literal whose last byte is at `2:2`, while `a <b c> d` gives `1:3..1:7`, which covers it. cmark-gfm is wrong the same way |
| D35 | `finalize` ends a block at `parser->line_number - 1`, which assumes a **later** line closed it — false for an HTML block of type 2 to 5, whose terminator can be on its own first line | wrong-position (reversed range) | **fixed at D35, after Step 5** | found by reading `specs/scope-sanity/ledger.json`'s eleven negative rows for an owner: TEN of them were this. `printf 'para\n\n<!-- c -->\n'` gives `HTMLBlock scope=3:1..2:0` for a literal whose last byte is at `3:10`, and `last_line_length` there is the length of the BLANK line before it |
| D34 | `markdown_core_node_insert_before` / `_insert_after` accept `sibling == node` — `S_can_contain(node->parent, sibling)` starts its ancestor walk at the PARENT and never meets the child, so it answers yes | **unbounded sibling list** | **fixed at 3b** | found at 3b while writing its gate: `insert_before(b, b)` returns 1 and leaves `b->next == b` and `b->prev == b`, with `a->first_child` and `a->last_child` disagreeing; walking `a`'s children never terminates. Reachable with `markdown_core_enable_safety_checks` in EITHER position, so the flag never covered it |
| D33 | `process_emphasis` chooses its arm by the delimiter's **byte**, and the chain has **no final `else`** — a delimiter no arm claims leaves the cursor where it is, is freed by the removal below, and is read again on the next turn | **use-after-free**, or a **non-terminating loop** | **fixed at 3.3** | found at 3.3 by a probe extension: ASan `heap-use-after-free`, READ of size 8 in `process_emphasis`; with `can_open` set the loop never ends. Unreachable in-tree because every extension pushes a tag it declares — and the push is PUBLIC and takes the byte from the caller. §4.1.3 predicted a NULL dispatch here and did not notice the fall-through |
| D32 | a **backslash hard break** consumes a line ending without telling the subject, so every later node in the paragraph keeps the break's own line | wrong-position | **fixed at 0a.12** | found at 0a.12 while measuring D26: `foo\`⏎`bar` gives `Text 1:6..1:8` — three columns that do not exist on a four-character line 1. `handle_backslash`'s hard-break branch calls `skip_line_end` and then `make_simple_subj` without `handle_newline`'s `++subj->line; column_offset = -pos`. cmark-gfm reports the same numbers. **5 registered `multi-line-span` findings, all of them attributed by the ledger to a defect that could not close them** |

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
| D9 | the reference budget makes resolution order-dependent | wrong-output | **NO — genuinely blocked** | — | **no** | **CLOSED at 9b.2** (§4.14.9b2); pinned by two gates from 0a.8 until then |
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

   **Correction: "eleven non-opening paths" over-counts, and 3a.1 made it ten.**
   There were eleven
   `return parent_container;` statements, but six were wrong declines with the
   node still a `PARAGRAPH` (`325, 329, 337, 354, 365, 372`) — **`365` was the
   arena re-parse retry's, and Step 3a deleted the retry, so five remain
   (R14)** — four run *after*
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
the refmap. **D9 is fixed by deleting the copy, which is Step 9b's model
change, and by nothing smaller.** ~~Step 9a's~~ — this said 9a while Step 9 was
one step, and the 9a/9b split left it stale; §4.14.9a2 re-derives it from the
sentence above. The refmap dies with the parser and the document holds only
`root`, so a `Link` that borrows a map entry's destination dangles; giving the
map a new owner is work 9b then deletes.

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
| ✅ **2** | Every C source the build compiles is a `clang-format` fixpoint, and the config makes **braces mandatory on every `if`/`else`/`for`/`while`/`do` body**. `scripts/format-c.sh --check` is the gate. | 0 new · 1 config line · **2,472** diff lines over **38** sources (~~2,393 over 36~~, stale: Stage 0a added code), plus 35 lines over the **three bridge sources the gate could not see** | 0a has landed and each defect commit re-pinned its own `file:line` citations (R13). No other work is in flight in `packages/markdown-core/` (R17). |
| ✅ **3a** | The engine has **one allocator model**: `markdown_core_mem`, supplied per parser, defaulting to `calloc`. There is no process-global scratch allocator, no `core/arena.c`, and no re-parse retry in `table.c`. The Release CLI allocates and frees exactly as the library does, so `#if DEBUG` in `core/main.c` collapses to one path. | 0 new · **−140** | `extensions-conflicts.txt` exists (0a.5) and re-proves D8 after the retry path it patched is deleted (R14). `node scripts/audit-source-lists.mjs` **runs** (it throws at HEAD). |
| ✅ **3** | An extension is a `static const` descriptor in a fixed compile-time table. It is not registered, not looked up by name, and carries no mutable state. A parser records *which* extensions are on as a bitmask and **cannot express an order** — the order is the table's, and `table` is last. A descriptor declares **three** byte sets (terminates-text, dispatch, flanking-transparent), not one list. A delimiter names its **rule**, not a byte. Node types and node-flag bits are compile-time constants. There is no process-global mutable state anywhere in the extension path. | **+500 / −535** | D1, D2 fixed (0a.4) so the descriptor author transcribes a correct source. D8 fixed (0a.5). The tree is a format fixpoint (2). One allocator (3a). The source-list audit runs. |
| ✅ **3b** | `markdown_core_node_append_child` / `_prepend_child` / `_insert_before` / `_insert_after` / `_set_type` refuse any link that would make a node its own ancestor — **always**. `markdown_core_enable_safety_checks` does not exist. | ~25 | None beyond "the tree builds". See **Q13**: this may be a §2 defect, not a refactor by-product. |
| **15A** | **One** machine-readable AST contract lives in `docs/` (normative) and **one** audit checks all six projection surfaces against it — C header, C dump, Kotlin bridge + decoder + model, ES bridge + export list + decoder + model, Swift model + dumper, and the canonical-AST manifest — and it is **green**. | 0 C · ~500 JSON+script | Nothing under `docs/deprecated/` is normative *and* no executable policy file still points there. |
| ✅ **5** | The iterator's event contract is **total** (every node gets `ENTER` and `EXIT`; `S_is_leaf` is gone). Its mutation rule names *nodes*, not events: only the node whose `EXIT` is current may be freed. A subtree operation stays inside its subtree. **No zero-length `Text` node exists in a finished tree, and no node carries `0:0..0:0` as a stand-in for "no bytes".** A merged run's scope is the union of what it merged, line **and** column. One function computes a position from a byte range. | ~200 | D3 and D7 fixed (0a.6) so merged positions are merged from correct operands. D10's replacement node carries a start line (0a.2). |
| **6** | **Deliverable #2.** Attaching `formula` is the *only* gate — the two delimiter options do not exist. Five inline forms, four block forms, and one padding rule: one leading and one trailing space-or-line-ending is stripped from an inline formula's body when the body is not all whitespace. | ~60 · deletions across 18 files | 3. D1 fixed (0a.4), else one oracle row stays red and must be named as 0a.4's. |
| **7** | **Deliverable #1.** The directive grammar of micromark-extension-directive 4.0.0 and mdast-util-directive 3.1.0, applied to **code points**: name rules, one/two/three-colon forms, `#`/`.` shorthand, `class` accumulation, last-value-wins elsewhere, and **degradation** — a malformed label or attribute block leaves the directive standing and the punctuation as prose. `DirectiveLabel` is a visible node whose scope spans its brackets. A container's closing fence **closes it and every block open inside it** (D21). A directive that consumes a span containing a line ending leaves the subject's position honest (D22). Attributes are an ordered key/value sequence; the JSON round-trip is deleted. | ~530 written · **+150 net** | 3. 15A (this is the first step that changes the node inventory). 0a.6's newline-adjust mechanism is live, or Step 7 lands it (D22). |
| ✅ **10** | For any block node with a content buffer and any byte offset within it, the engine can name the **source line and column** of that byte. Every node synthesized from a content offset carries a position that is a place: the split-off table lead, its inline children, the recovered header row and cells, and any paragraph whose front was consumed (D18). The lead keeps its authored spelling. | ~110 | **Nothing.** Every mechanism exists at the baseline; both consumers run while the marks would be live. |
| ✅ **9a** | A footnote definition is a block node at the byte where its `[` was written, in the container it was written in, and it **stays there**. No pass runs after the parse that moves, reorders, drops or re-parents any node. Every definition the author wrote is in the tree. The reference map never owns a node. A reference carries **the label the author wrote**; numbering is derived, not stored. A `[…]` is a footnote call only if it opens with a **raw** `^` and the document defines that label; otherwise the brackets take the ordinary unmatched-`[` path and nothing frees children core already built. | **+90 / −290** | 0a.2's D10 fix, so a reference's label is sliced from the parser's own buffer. |
| ✅ **11a** | A parse produces, beside the tree, a **concrete record set** in which every block-level byte of the normalized source is owned by exactly one node, in exactly one of three roles (`MARKER`, `CONTENT`, `DISCARDED`). Three laws hold over every corpus: **L1** the regions on a line tile it exactly; **L2** every region lies inside its owner's scope and descendants lie inside their ancestor's `CONTENT`; **L3** concatenating the regions in order reproduces the normalized source byte for byte. **L4 — added by the owner before the step was written, and the reason the other three are not enough: the records are complete for lines 1…N once line N has been fed.** The first three constrain the RESULT and not when it is built, so a close-time construction satisfies all of them and recreates §11.5's quadratic cheat one level down. The document **retains** that normalized source and its line index. A region may be *refined* — split, never moved, never deleted — which is how extensions capture without breaking L1. | ~600 + ~350 gate | 0a (an L3 gate written over the unfixed engine would encode D10/D11's loss as expected). 5 (no node without source bytes). 10 (the content-to-source marks 11a retains — **Q22**). |
| ✅ **8** | **The inline position model.** An inline node's position is a *projection* of the byte range it covers, not a counter each handler maintains: one `seek` primitive, one newline index, offsets stored on the node, and one constructor for a delimiter run. `adjust_subj_node_newlines`, `count_newlines`, `subj->column_offset`, `subj->block_offset` and the three hand-written `make_delimiter_text` copies cease to exist. Subsumes D3, D7, D12 and D19/D20/D23 by construction. | **+330 / −245** | 3 (rules exist). 6, 7 (the grammars are settled, so the extensions are rewritten once). 11a (the retained `CONTENT` records are what make the projection exact on continuation lines). |
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

**Considered for deletion and surviving conditionally: Step 2.** Measured at HEAD: `sh scripts/format-c.sh --check` **exits 0**, and `.clang-format` contains no `InsertBraces` line. The tree is already a fixpoint of the current config, so the step as written — *"run `clang-format`"* — is a **no-op**, and the 1,296 lines §4.3 attributes to it are an observation about a historical commit, not a requirement — **measured on this tree the invariant costs 2,472 diff lines over 38 sources, 835 brace pairs** (§4.14.2). Its only possible content is adopting one invariant: **braces on every conditional body**. That invariant is worth having here for one reason that survives the closed history — Stage 0a and Steps 3–14 consist very largely of adding a statement to, or removing one from, an existing conditional body (§2's own defect list: *"adds one line inside the successful rewind"*, *"plus four lines in `blocks.c:625`"*, *"an 8-line sweep before `blocks.c:675`"*), and in a braceless body "add one line" and "change the control flow" are the same edit and look identical in review. **If the owner declines the rule (Q11), Step 2 is deleted outright, because there is nothing else in it.**

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

**The release gates are off the critical path until 3.0, and there are seven of them, not three.** Two are era skew from Step 0's wholesale `scripts/` restore (`audit:ci` wants 40-hex Action SHA pins that `.github/` predates; `audit:source-lists` ~~throws~~ **is triaged and green**; the missing `packages/swift-markdown-core/Package.release.swift` is a registered absence owned by 15C), one is two minutes of formatting on restored files (`format:es:check`, three real files), one is a second unexpected legacy tag (`pre-format-baseline`, which §0 does not name beside `codex-doc-pass-backup`), one is the ordering assertion of Q27, one is the release-notes path — hard-coded to `docs/deprecated/releases/$(cat VERSION).md` in five places, so publishing 3.0 would publish from the archive — and one is `audit:ast-projections`, which **is not era skew at all** but the live Swift drift of §4.1.2. That closes §0's fifth known-red row.

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
| **Q11** | Does the repository adopt `InsertBraces: true`? | 2 | **TAKEN at Step 2, and every number in this row was wrong.** Footprint ~~2,393 diff lines across 36 files, 561 of them in `core/` + `extensions/`~~ → **2,472 across 38, of which 1,700 are in `core/` + `extensions/` and 772 in `tests/`**; the old split understated the engine's share threefold. Neutrality ~~29/29 objects identical~~ → **83/83 Release objects BYTE-identical**, no normalization needed; a debug build type moves only `assert`'s `__LINE__` immediate (82 substituted instructions under `asan`, 58 under `ubsan`, **zero** added or removed). The tool is `scripts/audit-format-neutrality.sh <rev>`, and it is a measurement tool, not a standing gate — §4.14.2 says why. |
| **Q12** | Is the arena deleted, or made parser-owned? | 3a | **Delete.** Measured: ~7% CLI-only parse win, **+10–16% peak RSS**, `abort()` on allocation failure inside a library with a careful sticky-OOM discipline, total sanitizer blindness on the binary the parity oracles drive, and a demonstrated **480-byte leak in a parser that never asked for it** (a global `A != NULL` makes an unrelated default-allocator parse take `table.c`'s retry branch). Parser-owned is impossible without a document-owned lifetime model this engine does not have. Output-neutral: 7,251 comparisons, 0 differences. |
| **Q13** | Is the cycle check unconditional — and is it a defect or a refactor by-product? | 3b | **TAKEN at 3b: unconditional, and it was a defect** — the witness is in §4.14.3b and writing its gate found a second one, D34. The measured cost on this tree is +0.9% and +3.5% on two deep-nesting inputs and noise on two ordinary ones; the row's 10.7% is not reproduced and nothing here takes 36 seconds. The row's own reading — *"the shipped library makes `b->parent == b` on request while the test that denies it flips a flag nothing else flips"* reads exactly like D1–D16 — was exactly right. |
| **Q14** | One knob per extension, or two? | 3, 6, 7 | **TAKEN — the formula half at Step 6.** Both delimiter options are gone from the C header, the facade, the CLI, the C tests, all three binding models and the shared manifest; `formulas` turns on `$`, `$$`, `` $`...`$ ``, `\(...\)` and `\[...\]` together, and no sub-grammar selection was stated so none was kept. The gate is live: attaching formula regardless of the option fails `spec_commonmark` and `extensions_formula_option_gates` (§4.14.6). `MARKDOWN_CORE_OPT_DIRECTIVE` went the same way at 7c, with `directive_enabled`; attaching the extension regardless of the option fails `spec_commonmark`, `extensions_gfm` and `extensions_directive_option_gates`. **Q14 is closed.** |
| **Q15** | What is the **inline** dispatch precedence? | 3 | Q9 settles the *block* order (`table` last) and says nothing about inlines — `table` has no inline hooks at all. `autolink` and `directive` both claim `':'`, and first-non-NULL wins today. **Recommend: table order is also inline order, `autolink` before `directive`** (a bare `:` far more often begins a URL), stated in the commit and pinned by a fixture. **MEASURED AT 0a.11 AND THE COLLISION HAS NO WITNESS**: moving `directive` to first or to the middle changes 0 of 12 hand-built candidates and 0 of 4,000 random `:`/URL/attribute documents (§4.2.17). The recommendation stands as a tie-break; it must not be shipped as a fix, and **a fixture cannot pin it until an input exists that distinguishes the two** — finding one, or recording that none does, is Step 3's. |
| **Q16** | Are extension node types and node-flag bits re-assigned as fixed constants? | 3 | **TAKEN at 3.1.** A fixed enum decoupled from the table order, at exactly the values the runtime allocator produced (measured both sides). The export map is 32 facade symbols and `local: *`, so renumbering was available and was declined to keep the commit structural. §4.14.3. |
| **Q17** | Is an inline node's position a projection of a stored byte range? | 8 | **Yes**, and store the pair — two `bufsize_t` on the inline node. This is what makes D12 *unexpressible* rather than fixed, and it is the concession that makes 11b cheap. |
| **Q18** | Which inline-math padding rule? | 6 | **TAKEN at Step 6, and this row's phrasing is what misled.** "Strip one leading and one trailing space-or-line-ending" reads as two independent strips and is not: the oracle pins `text $$ mid$$ text` as `literal=" mid"`, so it is **both or neither**. The `\(…\)` / `\[…\]` forms are covered, by two new pins. Two further corrections came out of the implementation: a **tab is not whitespace** for the all-whitespace test — `$$ \t $$` strips to `"\t"`, exactly as `` ` \t ` `` does — and the **CRLF clause is unreachable**, because the line reader hands inline content LF-only. Both measured; §4.14.6. |
| **Q19** | Are directive attributes sorted in the model, or only in the dump? | 7 | **TAKEN at 7.2: sorted in the model**, by a linked-list merge sort that cannot fail to allocate -- the duplicate normalizer above it already degrades to a no-index path, and a sort that could fail would hand back an unsorted list with nothing to say so. remark's own projection is sorted, so the mdast oracle checks it. Originally: **Sorted in the model.** After class-accumulation and last-value-wins the list *is* a map; source order is meaningful only inside `class`'s accumulated value, which is already a string. Two orders is how a third order appears in a binding. |
| **Q20** | Are character references decoded in directive attribute values? | 7 | **TAKEN at 7c, and the oracle answered it.** `mdast-util-directive` decodes in three places -- an attribute's value and the `#id` and `.class` shorthand values -- and nowhere else; a name is taken as written. This engine now decodes at the same three through `houdini_unescape_html_f`, its sixth call site. The narrower question, whether the semicolon is required, was measured exhaustively: **2125 of 2125 named references agree with the semicolon** and the whole difference is 162 semicolon-less forms, where micromark uses an HTML *attribute* rule that appears nowhere else in its own parser. This engine keeps CommonMark's rule and the difference is registered, which is what this row's own last sentence asked for. §4.14.7c. |
| **Q21** | Does a reference definition box itself, or only its resource? | 9b | **Only its resource.** Measured on this machine: `chunk` 16, `association` 32, `definition` 64, `reference` 40, widest existing union arm (`markdown_core_code`) **40**. `{association; resource *}` is 32+8 = **40** — the union does not grow, the association stays inline and uniformly readable for all five kinds, and the label can never be lost to a failed box allocation. |
| **Q22** | Does the content-to-source map have **one** owner? | 8, 10, 11a | **ANSWERED AT STEP 10, and the recommendation held.** `parser->line_marks` is produced by `add_line`, read by the table extension, by the reference harvest and by the inline phase through the single `S_line_start_column`, and 11a retains it. Nothing built a second one. §4.14.10 has the measurement; what follows is the original reasoning. **Yes, and this is the sharpest thing the restatement found.** Three steps independently proposed a mechanism for one fact: Step 10's per-line parse-time marks, Step 8's newline index, and 11a's `CONTENT` regions. **Recommend: 10 produces it, 11a retains it, 8 projects through it, 11b tiles it.** Three implementations of one fact is the disease this plan names in five other places. |
| **Q23** | Does the document retain the normalized source? | 11a | **Yes** — one append-once buffer, 1× the input, plus 4 bytes per line of index. §6's verdict ("nothing replaces the substrate") is true of the *rope* and silently assumed the bytes survive; they do not (`parser->curline` is cleared per line, `linebuf` freed at finish, `source` borrowed). The alternative is re-implementing the normalizer — including `markdown_core_utf8proc_check`'s replacement policy — byte-identically in Swift, Kotlin and JS. **This is a §6 amendment, not just a Step 11a decision.** |
| **Q24** | Is the concrete view opt-in? | 12 | **A parse option defaulting to `true`.** Cost is ~2.5–3× input resident. The gate that makes it safe: the semantic dump must be **byte-identical** with the option on and off, over every corpus. An option that changes the parse is a second engine. |
| **Q25** | Do D16's two site fixes move into 0a.7? | 14 | **Owner call, because Stage 0a is otherwise closed.** Measured: 58 golden rows carry `title=""`; 18 are D6's; the remaining **~40 are D16's** `chunk_clone` path, and under the current schedule they are regenerated by nine steps with the reviewer's only available answer being "unchanged, therefore fine". Moving them is ~6 lines and resolves D5's stated tension in the commit that already has the defect statement in hand. If it does not move, Step 14 moves 40 rows; if it does, Step 14 moves **zero**, which is the right shape for a step whose deliverable is an invariant. |
| **Q26** | Do `Link.destination`, `Image.source`, `ReferenceDefinition.destination` stay optional? | 14 | **No — required.** Q7 already rules a definition's destination required; §5.1 rules that a reference carries none. Once 9b splits `LinkReference` out of `Link`, an inline link's destination has no reachable null except allocation loss, which Q7 answers with the failure bit. |
| **Q27** | Does `VERSION` move to 1.0.4 at all? | 15C | **SETTLED 2026-08-21: no — it went to `3.0.0` early**, the second of the two options this row offered. Measured: the ordering assertion in `check-release-version.mjs` passes at `3.0.0` and **fails at `1.0.4`**, because `v2.0.0` exists and the script requires every tag to be strictly less than `VERSION`. `3.0.0-dev` fails the `stableSemver` assert. The stated cost — a release-notes file from that commit — was paid; see §4.10. |
| **Q28** | Is `markdown_core_parser_feed_reentrant` deleted? | 11a | **Yes.** Zero in-tree callers, and it re-enters line processing with bytes that are in no source line — unrepresentable under L1. Keeping an entry point whose only purpose is to inject bytes no position can name, in the step that establishes that every byte has a position, is carrying a contradiction forward for no consumer. |
| **Q29** | Does `mode` survive on `Code`, `CodeBlock`, `Directive`, `DirectiveBlock`? | 15A | **TAKEN at 15A.4, and it is FIVE kinds, not four.** `FormulaBlock` is not "genuinely variable" either: the corpus has 12 `standalone` and zero `embedded`, and `markdown_core_extensions_set_formula_mode` REFUSES any other value for that kind (`extensions/formula.c:100`). `Formula` is the only kind whose mode is a fact about the source. 195 golden rows, twelve surfaces, and a seventh hand-written copy of the contract found and deleted. ~~**No** — delete it from those four, keep it on `Formula`/`FormulaBlock` where it is genuinely variable. Both decoders prove the point: Kotlin and ES hard-code the constant and one of them then *asserts* the constant it just synthesized, and the Kotlin wire format does not transmit it. A field whose value is implied by its type is ceremony four surfaces must keep in step.~~ **Every one of those claims was verified before acting on it, and every one was true.** |
| **Q30** | Do the bindings spell child edges typed (`content`, `items`, `label`, `header`, `rows`, `cells`) or flat (`children`)? | 15A | **TAKEN at 15A.2: typed.** The Swift dump is byte-identical afterwards and `audit-ast-projections.mjs` is green. ~~**Typed.** Kotlin and ES already do; Swift's flat `children` is what forces `labelCount: Int?`, forces `Table.init` to filter rows by `isHeader` and `preconditionFailure` if the count is not one, and forces `children: [any Markup] = []` onto eleven leaf kinds. Two of three bindings and the contract already assume it.~~ **Every one of those was measured true at 15A.2 and every one of them is gone.** |
| **Q42** | When does `prettier --check .` get satisfied, and by reformatting or by scoping? | 15C | **OPEN.** `ci.yml:97` runs it as a required step and it reports **100 files** at Step 6, none of them engine sources — `scripts/*.mjs`, `specs/**/*.json`, docs. Same era skew as `audit:ci`: the config came from `main` with Step 0's `scripts/` restore, the files did not. **Recommend: one deliberate `prettier --write` commit at 15C that touches nothing else**, rather than letting each step carry unrelated churn or leaving a required check red through Stage 0. Scoping prettier away from `specs/` is the alternative and is worse — those are the files a reader diffs most. |
| **Q43** | Is a directive's label found LEXICALLY, or by the inline delimiter machinery? | 7 | **ANSWERED AT 7e: LEXICALLY**, and at Step 7 rather than Step 8 -- the redesign it looked like turned out to be a deletion. `match_colon_directive` scans the label at the colon and both branches continue, so a label that closes is a label and one that does not is prose; the bytes are consumed there, so no other extension is offered them. Eleven functions, a delimiter rule, a dispatch byte, an extension hook and the whole of 7d went with it: **8 files, +214 / −409.** The two dead-end proposals and the off-by-one in the 32-deep cap are recorded in §4.14.7e. |
| ~~**Q45**~~ | Does a code span's position cover its own backticks? | 8 | **ANSWERED BY THE OWNER 2026-08-23 — YES**, and taken at 8.4 (§4.14.8d). Every other inline construct covers its own delimiters — emphasis its asterisks, a link its brackets and parens, strikethrough both tilde pairs — and a code span reported the extent of its CONTENT, which is what cmark-gfm reports and is a defect inherited from it. A scope exists so a consumer can map a node back to the source it came from, and the source a code span came from includes the ticks that make it one. **55 golden rows moved and every one is a `Code` node; `places` 69 → 66 and down to two families; `inline-sourcepos` 9 → 40**, every row of it now a place where this side is right and upstream is not. |
| ~~**Q44**~~ | What does a node with NO SOURCE BYTES report as its position? | 11a | **ANSWERED BY THE OWNER 2026-08-23**, and the answer came with the criterion the question was missing: *a scope is what a consumer follows to map an element back to source*, so a node completion invented points AT the place it was completed — the end of its row. Both spellings 11a had measured were asking which coordinate pair is least wrong about the EXTENT of something with no extent, and that was the wrong question. Taken at §4.14.11a2: `places` 66 → 57 and down to one family, `containment` 21 → 9, and the six sibling-overlap rows it costs are registered with the reasoning that accepts them. |
| **Q41** | Does the repository keep swift-format's `AllPublicDeclarationsHaveDocumentation`? | 15A / 15C | **OPEN, and it is the owner's.** It is a required CI health check that has been failing: 184 findings at `46e20f2`, 170 after 15A.2, 163 after Step 6, **164** after Step 7.2. Satisfying it means writing a doc comment on every public declaration in the Swift binding, and for a projection layer most of those can only restate the signature — the pass this repository rejected once already. **Recommend: scope the rule to types and functions, or turn it off**, and say so in `.swift-format` rather than leaving a required check red. Whichever way it goes, it is an owner decision and §4.8 needs an answer before Stage 0 closes. |
| **Q38** | Does the empty `Text` node D13 removes become a registered divergence from cmark-gfm? | 0a.14 | **OPEN.** Upstream emits the node too, so removing it costs one normalizer projection, one `NORMALIZED_DELTAS` name and one `deltas.json` entry. Measured at §4.2.3. Owed by the commit that lands D13. |
| **Q39** | `[foo]: <>` resolves to `destination=null`, not `destination=""`. Is that right, when the destination WAS written and was empty? | 0a.7 | **TAKEN 2026-08-21, at 0a.7: yes, on consistency grounds, and the limit is stated.** `markdown_core_clean_url` folds a zero-length destination to `CHUNK_EMPTY` before it ever reaches the map — the same fold `clean_title` does — so `<>` is indistinguishable from *no destination* by the time the reference path sees it, and the inline path already answers `[a](<>)` with `destination=null`. Making `chunk_clone` preserve absence made the two paths agree. **This is consistency, not correctness:** a rule that truly separates "written and empty" from "not written" requires the folds to stop, which is Step 14's structural job, and this row is the one input in the corpus that will move again there. It is one row, `spec.txt` example 169. |

---

### 4.1.7 Seven defects the restatement found — §2 additions, D18–D24

Recorded here because §2's own rule is that a defect the plan does not name is a defect the plan re-derives later at full price. D17 is taken (the version macro, fixed at 0a.0). None of these changes Stage 0a; each goes to the step that was already going to touch that code.

| # | Defect | Severity | Witness | Owner |
|---|---|---|---|---|
| **D18** | A paragraph whose leading reference definitions were consumed keeps the **definition's** start position, so every inline child reports the definition's line. | wrong-position | `[a]: /1\ntext here` → `Text scope=1:1..1:9`; truth `2:1..2:9`. **The PARAGRAPH carries it too, and through the setext path so does a `Heading`** (§4.2.4). Upstream has it identically (`cmark-gfm --sourcepos` agrees), so upstream cannot be the oracle. ~~Invisible to every gate~~ — it is invisible to every *parity* gate, and pinned, wrongly, by two golden files. | **fixed at 0a.12** |
| **D19** | `handle_close_bracket` takes a link's `start_line` from the **closing** bracket and never adjusts for newlines, so a link with a multi-line label or title has a wrong line *and* contains a child that starts before it. | wrong-position | `[a](/u "t⏎t2") tail` → `Link 1:1..1:14`, `Text 1:15..1:19`; truth **`1:1..2:4`, `2:5..2:9`** — ~~`1:1..2:6`, `2:7..2:11`~~ was arithmetically wrong and §4.2.4 ordered it corrected; it stayed wrong until 0a.12 and is corrected here from the landed engine. The site is `core/inlines.c`'s `match:` label; ~~`:1411`~~ was 70 lines stale and pointed into D14's territory, which is why §2's rule is that the FUNCTION name is the durable half. | **fixed at 0a.12** |
| **D20** | `strikethrough`'s `match` sets `start_column` and never `end_column`, so the calloc'd `0` survives consolidation whenever the run ends the paragraph. **Only the UNPAIRED run: `insert` derives a paired `Strikethrough`'s end from the closer's START column plus its literal length and never reads the closer's `end_column`, so no `Strikethrough` node is ever wrong.** | wrong-position | `a~~` under `--profile gfm` → `Text scope=1:1..1:0`. Three bytes, the default GFM profile, and every parity oracle blind because none compares positions. Un-consolidated witness: `` `x`~~ `` → `Text scope=1:4..1:0`. | **fixed at 0a.12** |
| **D21** | **A container directive's closing fence does not close it.** `directive_block_matches` marks `closed` and consumes the fence but returns 1, so the container and every block open inside it stay open; the next non-blank line is taken as a lazy paragraph continuation, pulled into the container, and recorded on the wrong line. | **content-attribution loss** | `:::note⏎body⏎:::⏎after` → one `Paragraph 2:1..4:5` whose third child is `Text scope=3:1..3:5 literal="after"`. Inside a block quote it moves `after` into the quote. A blank line after the fence hides it. The formula block is unaffected (it is a leaf with no open children). | **7** |
| **D22** | An extension that consumes an inline span containing a line ending cannot report it: `markdown_core_inline_parser_set_offset` does not advance the subject's line counter, so **every later node in the paragraph is displaced**. | wrong-position | The oracle case `:note[label]{title="one⏎two"} tail` requires `Directive 1:1..2:5`; HEAD says `1:1..1:29` and `Text 1:30..1:34` — columns that do not exist on line 1. **Blocks Step 7 outright.** | **7** lands the primitive; **8** owns the model |
| ~~**D36**~~ | A directive label's closing `]` was found by the inline delimiter machinery, so any construct that consumed that `]` first took the whole directive with it -- and a label that never closed lost its directive outright. | wrong-tree | **CLOSED at 7e (§4.14.7e).** The root cause was not the closer but the OPENER: `:name[` pushed a delimiter and bet on a later `]` instead of scanning the label at the colon, which is what micromark's `effects.attempt(label, afterLabel, afterLabel)` does. Nine witnesses, nine matching remark. Two cheaper repairs were measured dead first and a third, 7d's structural-closer primitive, was a side path and is deleted. | — |
| **D23** | `S_insert_emph` gives an emphasis node the start column of the **whole** delimiter run: it shortens `opener_inl->as.literal.len` from the end (`inlines.c:843`) and then assigns `emph->start_column = opener_inl->start_column` (`inlines.c:875`), while `handle_delim` had spanned the entire run. | wrong-position + overlap | On `***a**` the leftover `Text` and the `Strong` both claim the run's first byte — two nodes, one byte. Correct value: `opener_inl->start_column + opener_num_chars`. **11a's L1 gate detects it mechanically.** | **8**, gated by **11b** |
| **D24** | `tasklist` decides `checked` by searching the **whole line**: `strstr((char *)input, "[x]") \|\| strstr((char *)input, "[X]")` (`extensions/tasklist.c:88`), while `scan_tasklist` matched only at `parser->first_nonspace`. | wrong-output | `- [ ] see [x] below` reports `checked=true`. May be the same thing as the pending upstream delta `tasklist-checked-marker` — check before re-deriving. | **3** (the descriptor rewrite touches it) |

Two further findings that are not new defects but change what an existing item means. **The content→source column map is wrong whenever a continuation line's stripped prefix differs from the block's first line** — `make_literal` uses a per-node constant `block_offset`, so `"> foo\n*bar*\n"` reports the emphasis at column 3 (truth 1) and `"> foo\n>bar *baz*\n"` at column 7 (truth 6). That is the **general case of which D7 is one instance**, and Q22's single map is what closes it. And **there are three producers of zero-length `Text` nodes, not one**: D13 names `autolink`'s `postprocess_text`, but `markdown_core_node_unput` (core, `inlines.c:1925`) empties a literal and leaves the node spliced in, and consolidation merges runs of empties into an empty. Corpus footprint, measured: `Text scope=0:0..0:0 literal=""` on **36 golden rows**, and **16 rows carry a negative range**, 12 of them ending at column 0 — including `tests/fixtures/extensions.txt:804`, which pins `Text scope=59:1..59:0` as *expected*. Step 5 owns all three.

---

### 4.1.8 Where the whitelisted oracles are stale

`specs/oracles/README.md` states the rule: *where an example's expected output disagrees with what this engine should produce, this engine is right and the example is stale — say so in the commit.* These are the places, collected once so each is a deliberate divergence rather than a surprise mid-step. Measured by running all 96 examples through the HEAD engine: **43 of 43 formula examples and 2 of 53 directive examples already reproduce exactly**; of the 49 directive examples that move, 18 are spelling-only, 29 are grammar or position, and 2 are D1's.

| Where | What it says | What this engine will do | Why |
|---|---|---|---|
| ~~`extensions-directive-option-gates.txt` — prose~~ | *"These examples attach the directive extension without enabling its parser option."* | **DONE at 7c**, with `MARKDOWN_CORE_OPT_DIRECTIVE` itself. Both inputs and both expected blocks stand; the prose says what the file shows. | **Vacuous as wired**: the ctest entry passes no `--option`, the examples carry no tags, and `spec_runner` starts from `ts_ast_options_none()` — so the extension is **not attached at all**. The oracle cannot distinguish "attached, option off" from "not attached", which is exactly the gap D1 lived in for eleven releases. Under Q14 there is only one knob; do not build a second to satisfy a comment. |
| `extensions-directive.txt` — prose above `:shortcut{#identifier}` | *"HTML-style `#id` and `.class` shortcuts are outside this extension's generic key-value grammar and remain ordinary Markdown text."* | Implement the shorthand; **delete the sentence**. | It contradicts its own expected output, which shows both recognized. The expected block is authoritative; the sentence is a leftover from the fixture the oracle was extracted from. |
| `extensions-directive.txt` — prose above `:ordinary[label]{…}` | *"`id` and `class` are ordinary keys. Like every repeated key, their last value wins while their first source position is retained."* | Implement `class` accumulation and last-value-wins for everything else; **delete both clauses**. | Stale on both halves. The expected output shows `class="red green blue"` — `class` is the one key that does *not* take the last value — and "first source position is retained" describes a per-attribute position that no dump field and no proposed accessor exposes. Do not build an API to justify a sentence. |
| `extensions-formula-github.txt` — `foo$_bar_` · `extensions-directive.txt` — `foo:_bar_`, `a}_b_` | (expected output correct) | Green before Steps 6 and 7 start. | **Not stale — misattributed.** These are **D1's and D2's** rows, closed at 0a.4 by deleting three lines. Steps 6 and 7 must not claim them; if either lands before 0a.4, the row is listed as known-red naming 0a.4. |
| ~~`extensions-formula-github.txt` — prose~~ | frames the dollar and fenced forms as *"a surface recognized by the `formula` extension"* | **DONE at Step 6.** The fixture is at its oracle content plus one new pin (§4.14.6). | Cosmetic, but under Q14 the two delimiter options cease to exist and the prose currently implied otherwise. |
| ~~`extensions-formula-option-gates.txt` — title and framing~~ | written against two option knobs | **DONE at Step 6**, at the oracle's content: the file now holds the two LaTeX rows the baseline had filed under the github fixture, and it runs with no `--option` at all. | Same one-knob correction. `extensions-formula-conflicts.txt` was not affected and `extensions-formula-github.txt`'s attached-but-inert case stayed distinct. |
| All six files — positions | positions reflect fixes scheduled separately | Derive positions from this engine at each step; the oracle's positions are a **cross-check**, not a golden. | The README says so, and two specific classes prove it: the directive's `Directive 1:1..2:5` needs **D22**, and `DirectiveLabel scope` spanning brackets inclusive needs the label node to be visible — the baseline's hidden label spans the content only and its empty form is a *negative* range. |
| `extensions-directive.txt` — the 18 spelling-only rows (`attributes=[…]`, `DirectiveLabel`) | new dump vocabulary | **Adopt them verbatim.** | **Not stale.** They are the surface change, and `scripts/lib/mdast-oracle.mjs` already sorts remark's attributes and compares the rendered bracket form — the gate was written against that exact spelling before this branch existed. |
| `extensions-directive.txt` — the `:a-[]` / `:-a[]` / `:_a[]` prose | records that an earlier version of the example was wrong and how it was found | **Keep verbatim.** | It is the only place where the leading-`-`/`_` rule's provenance is written down, and the rule reverses baseline behaviour (which produces directives named `-a` and `_a`). |

**~~One oracle-adjacent gate must move with Step 6 and is easy to miss~~ — DONE (§4.14.6):** `scripts/check-mdast-parity.mjs`'s self-test canary asserted `literal=" mid "` — the *unpadded* answer — with a comment naming Step 6 as the flip. An oracle whose canary asserts the defect is an oracle that has been told to expect it. Step 6 flipped the assertion, moved `github-backtick-math-padding` and `inline-display-math-across-lines` from `pendingExpectedDivergences` into `expectedDivergences` (both reproduce), and retired the two `baselineBacklog` entries that close by leaving the mdast corpus. **This paragraph was right about the mechanism and the gate was not** — it reported both as having come to agree with remark, so the gate now distinguishes a settled entry from an unreachable one and the retirement is recorded in `retiredBacklog` rather than deleted.

---

Scratch artifacts (outside the repository): `/private/tmp/claude-501/-Users-donz-Repos-GitHub-markdown-core/19b6648c-2779-4f7c-bddf-acfaf7c2be6b/scratchpad/dag.mjs` and `dag2.mjs` — the acyclicity check and the linear-order verifier of §4.1.4, runnable with `node`. Repository unmodified; `git status` clean.

# Replacement for §4.2, and the edits the ruling forces

*Drop-in prose. §4.2 replaces `docs/RECONSTRUCTION.md` lines 1046–1193 in full; the consequent edits follow, each keyed to its section. The edge list has been run through `scripts/check-plan-graph.mjs` and the linear order through a verifier — both results are stated below.*

---

### 4.2 Stage 0a — the defect stage

**Owner ruling, 2026-08-20 (Q25):** *"Fix all defects before start any tasks."*

The ruling was executed, not paraphrased. The fourteen defects §2 had assigned to Steps 3, 5, 7, 8, 9a, 10 and 14 — D12, D13, D14, D15, D16, D18, D19, D20, D21, D22, D23, D24, D25 — were each put to the test that settled the first ten: **applied to the untouched baseline with no other step landed, built, run against every gate in the repository, and reverted.**

**Fourteen tested. Fourteen fixable. Zero produced an architectural dependency.** D9 remains the only exception in the plan, and its exemption is still the measured one: its budget is the only thing between a resolved reference and 68.7 GB of output from 1 MiB of input, because resolving a reference copies the destination into the node; it is fixed by deleting the copy, which is **Step 9b's** model change (§4.14.9a2), and by nothing smaller. It is pinned, not fixed, at 0a.8.

Two of the fourteen *looked* like dependencies in §2 and were not. **D12 "blocked by D13"** is a sequencing constraint between two defects that are now both inside this stage — not a step dependency, and the two land in one commit. **D22 "7 lands the primitive, 8 owns the model"** was an ownership label, not a blocker: the primitive is twenty lines in `core/inlines.c` and needs nothing Step 7 provides. Two more — **D14 "that is a policy move, not a repair"** and **D16 "the rule has to become structural"** — were arguments about *desirability*, and the ruling is precisely a decision about desirability. Both are now measured to be repairs: see the verdict rows.

#### 4.2.1 The fourteen, tested

| # | Verdict | Evidence, measured on the untouched baseline | Lands |
|---|---|---|---|
| **D12** | **LANDED 0a.14**, in the same commit as D13 and no other | The one-line fix *alone* turns `extensions.txt:804`/`:809` from `59:1..59:0` into `59:1..0:0` — a strictly worse row that **every gate in the repository passed** at the time this was written: 65/65, 795/795, 46/46, canonical green, ledger 207 unchanged, because `endLine < startLine` keeps it in the same `negative` bucket it left. **That clause expired at 0a.1**: `audit-position-places.mjs` reads a live parse and reports the three rows moving to line zero, re-measured at 0a.2 (§4.2.8). With D13 and D10 landed it has **no witness at all**: 4 hits over the 860-example corpus, every one through an operand with no position; 0 hits over 40,000 random inputs filtered to merges where both operands are positioned. It is a real defect (the assignment is plainly missing) that is unobservable on this engine, and it must not be sold as fixing anything measurable | **0a.14** |
| **D13** | **LANDED 0a.14**, by removing the node, not by respelling the position | Option A (§2's wording — give the empty fragment an honest empty range) was built in two cuts and **rejected on measurement**: every sentinel row it removes returns as a negative row, because a closed `(line, column)` interval cannot express an empty range; `extensions.txt` negative goes 10 → 36 (narrow) or 38 (wide) and `specs/scope-sanity/ledger.json` forbids growth in either class, so A cannot land without raising the ratchet, which defeats the ratchet. A-narrow also does not clear its own class — producer (2) still emits `0:0..0:0`. Option B is 24 lines across `core/iterator.c` and `extensions/autolink.c`: 106 rows changed, net **−46**, ledger **207 → 169**, and every gate green **after** one registered upstream divergence (Q38) | **0a.14** |
| **D14** | **LANDED 0a.9.** Fixable at the baseline | **§2 is wrong twice, and two of this row's own numbers went stale.** Re-measured composed with 0a.2 at §4.2.15: **360** of the 432 move, not 252, and the NUL and invalid-UTF-8 rows are **0** and **0**, not 162 and 90 — 0a.2 removed the heap bytes before this commit ran. The original reading: It reproduces on the untouched tree with no D10 fix: `x[\^a] tail` → `literal="x[^a]] tail"` (backslash lost, `^` invented, `]` doubled) and `x[&#94;a] tail` → `literal="x[^\0\0\0\0\0] tail"`. And the "policy move, not a repair" objection does not survive measurement: at the baseline **no** escaped or entity-spelled call ever resolves, because the column arithmetic makes the lookup key `n]` or `\0\0\0\0\0`, never `n` — verified with `a[\^n]` + `[^n]: note`, which drops the definition before *and* after. The narrowing removes broken behaviour only. 432-case matrix (6 caret spellings × 8 labels × 3 tails × 3 definition contexts): 252 move; the baseline emits **invalid UTF-8 on 90 of them and NUL bytes on 162** — heap bytes materialised into a document. One condition, bounds-tested before the subscript. **Zero golden rows** | **0a.9** |
| **D15** | **LANDED 0a.11.** Fixable at the baseline | Over all 2,744 ordered triples of 14 significant lines, **414 (15.1%) parse differently through the CLI than through the facade**; after one shared attach path, 0. The 809-input fixture corpus shows 0 CLI-vs-facade differences, which is why no oracle sees it — **and no corpus ever could**, because every fixture runs through the facade and so can see only one of the two orders (§4.2.17, mutant D). **Re-measured at 0a.11 with D8 fixed: the two old orders disagree on 4, not 414; the 414 is a baseline-era reading and was not reproduced, because reproducing it means reverting 0a.5.** `markdown_core_core_extensions_attach(parser, mask)` walking one ordered table with `table` last (Q9), declared **without** `MARKDOWN_CORE_EXPORT` so the export map and `audit-public-surface.sh` are untouched. The CLI's `-e NAME` lever must route through the same bit table or the hole is still open — **at 0a.11 it was deleted instead: no name outside the table can reach the registry, so the by-name path was unreachable code holding a second attach site open.** **Zero golden rows moved; three examples added, and a new structural audit, because the by-construction claim had no gate** | **0a.11** |
| **D16** | **FIXABLE-AT-BASELINE** | 40 rows — 37 `spec.txt`, 3 `extensions.txt` — cross-checked independently here: the corpus carries **58** `title=""` rows (54 spec + 4 extensions), 18 of them D6's, and 58 − 18 = 40 exactly. **Mechanism correction:** `markdown_core_clean_title` already folds a zero-length title to `CHUNK_EMPTY`, so `inlines.c:1755` is **behaviour-neutral today**; the entire visible defect is `chunk_clone`, which `calloc`s `len+1` unconditionally and turns the refmap's NULL back into `""`. Take both anyway — `chunk_clone` alone leaves 1755 asserting "written and empty" for something never written, which is the exact tension 0a.7 was told not to resolve | **0a.7** |
| **D18** | **LANDED 0a.12.** Fixable at the baseline | 10 rows, one file (`spec.txt` examples 177, 179, 184, 185), every one of them the **golden being wrong**: example 185 pinned `Text "===" scope=1:1..1:3` for text on line 2 and `Link scope=2:1..2:5` for a link on line 3. The fix counts `\n` in the prefix `resolve_reference_link_definitions` drops, which is sound because `markdown_core_parse_reference_inline` only returns after `skip_line_end` succeeds — so the dropped prefix always ends on a line boundary. Putting it in the helper covers **both** consumers (`finalize` and the setext path); the setext one is why example 184 moves. Verified under block quotes, list items, stacked and multi-line definitions, CRLF, and the all-consumed case | **0a.12** |
| **D19** | **LANDED 0a.12.** Fixable at the baseline | 1 row (`spec.txt` example 518, which pinned `Link scope=1:1..1:25` on a 14-character line 1). +14/−1 at the `match:` label, reusing the file's own `count_newlines` and reproducing `handle_newline`'s exact `column_offset` convention. Two further witnesses separate the defect's halves: `[a\nb](/u) tail` gives a **link that begins after its own child** with no newline counting involved, and `*[a](/u "t\nt2")* tail` displaces every later node in the paragraph. Do **not** guard it on `MARKDOWN_CORE_OPT_SOURCEPOS` — that is D3, nothing sets it, and the guard would make the fix a no-op | **0a.12** |
| **D20** | **LANDED 0a.12.** Fixable at the baseline | One line. 3 rows in `extensions.txt` (568, 582, 584), each a negative range becoming a real one; ledger 207 → 204 via `--update`, the gate's sanctioned path. The `extensions_gfm` red was the **golden** being wrong | **0a.12** |
| **D21** | **FIXABLE-AT-BASELINE** | +54/−14 across three files, including one new extension-API constant. **Two smaller candidates were built and discarded**: returning 0 on the fence line does not fix it (`check_open_blocks` does not close unmatched blocks — the lazy branch still fires, because `parser->blank` is false), and advancing past the line end to make it read as blank silently changes list tightness. Full-corpus differential — every example in all ten fixture files × two profiles, 11,180 dump lines — moved **exactly two lines**, both in the golden that pinned the defect, plus one row in `specs/canonical-ast/structure.ast` that the task's gate list does not cover. **External confirmation:** mdast parity goes 46/46 → **48/48 with the backlog unchanged at 23** — remark, driven by the normative grammar Step 7 will port, produces the same tree as the fixed engine. 4,000 directed random documents through the ASan build, 0 failures | **0a.10** |
| **D22** | **FIXABLE-AT-BASELINE**, and it does **not** need Step 7 | Make the primitive honest (`markdown_core_inline_parser_set_offset` advances the subject's line counter over a consumed span) and let directive's two sites read the end back from the subject instead of computing `start_column + len - 1`. Result is exactly the oracle: `Directive 1:1..2:5`, `Text 2:6..2:10`, agreeing byte-for-byte with a softbreak control at the same indent under two newlines, CRLF, block quote and list item. `set_offset`'s other three callers are unaffected. **Zero existing golden rows — the defect is completely unpinned**, which is the finding; the pin was added and proved to bite | **0a.10** |
| **D23** | **LANDED 0a.13.** Fixable at the baseline, and the complete cut | §2's named one-liner (`emph->start_column += opener_num_chars`) gives the right `Strong` and **leaves the defect's other half open**: the leftover `Text "*"` still claims columns 1–3, so the two nodes still overlap, and the mirror case `**a***` is untouched. 31 rows. The complete cut is 4 more lines in the same function — the opener keeps its leading bytes, the closer its trailing ones — and every case becomes byte-exact and non-overlapping: `*****a*****` → `Emphasis 1:1..1:11 > Strong 1:2..1:10 > Strong 1:4..1:8`. **57 rows** (spec 45, regression 11, extensions 1). Taking the one-liner buys a second golden churn later | **0a.13** |
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
- **(c) A position is a place** — new. For every dumped scope, the line exists in the input and the column is within that line's length. This is the oracle that watches D18, D19, D20 and D22, none of which trips (a), (b), the sentinel/negative/line-zero ratchet, or any parity gate. **Its baseline reading is already measured and recorded in §4.1.3: 78 of 1,928 inline nodes over the three fixture files at `--profile gfm-extended` carry a position that is not a place.** Stage 0a must drive that number down and name what is left; the residue is the continuation-line column class, which is Step 10's. **It was, and it went: Step 10 cleared 21 of the 22 rows and the twenty-second turned out to be a different defect** (§4.14.10).

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

**0a.12 — D18, D19, D20 and D32: positions that are not places. LANDED; §4.2.18 records it, and it took a FOURTH defect nobody had named.** ~~14 rows in two files~~ — **21 rows in three files** (spec 16, extensions 3, regression 2), every one the golden being wrong. The three are independent — different files, disjoint rows, verified from a scratch reconfigure — and land together because they are one class and one oracle reading. Two honest limits belong in the commit message, not discovered later:

- D18 corrects the **line**. `start_column` is deliberately untouched and is right wherever the remaining first line has the same stripped prefix as the definition's line, which is every corpus case plus block quotes and list items. Where the prefixes differ, the residue is the content→source column class that exists with no reference definition in sight (`a\n  *b* tail` → `Emphasis 2:1..2:3`, truth `2:3..2:5`). That is Q22/Step 10's — **taken there, and the column half needed a witness this repository did not have** (§4.14.10).
- D19's example 518 ends at `2:10` where the true source column is `2:12`; the two-column shortfall is the continuation line's stripped leading spaces — the same class, visible with no link present.

**0a.12b — D26, the break-node class. LANDED; §4.2.19 records it, and Q40 is taken.** Measured at 0a.12 and given its own sub-step there. §4.2.5 said it "belongs at 0a.12"; putting it to the test says otherwise, and the reason is not its size. See §4.2.5 for the corrected numbers and §4.2.18 for the measurement. In short: assigning a break its honest position moves **153** rows out of `scope-sanity`'s sentinel class and **138** of them into `audit-position-places.mjs`'s `off-column` class, because a line ending is at column *len+1* and `lineLengths()` excludes it — so by that oracle's definition **a soft break has no position that is a place.** The alternative spelling (the last byte of its own line) was measured too: **3** places rows and **138** *containment* rows, because the break then overlaps the text it follows. Both readings are this programme's own, taken independently and agreeing to the row. That makes D26 a **ruling about what a position is**, not a repair — and §4.2.3's own 0a.0 item 5 says a defect commit must not smuggle a divergence decision. It also forces deleting the declared `scope.zero` coverage state from `specs/canonical-ast/manifest.json` and its validator from `check-canonical-ast-fixtures.mjs`, because the only two witnesses of that state in the canonical corpus are the two rows the fix clears — a public-contract edit. **0a.12b owes: the ruling (call it Q40 — is a line ending a place?), the two-site fix (`handle_newline` AND `handle_backslash`; a one-site fix leaves four LineBreak sentinels standing), the contract edit, and 151 fixture rows plus 2 `.ast` rows plus 3 hand-written C assertions in `tests/api/main.c`.** It lands after 0a.12 rather than before it, which costs exactly one row — spec example 185's `SoftBreak` — regenerated twice.

**0a.13 — D23, the overlap class. LANDED; §4.2.20 records it.** The complete cut, four lines beyond §2's one-liner. 57 rows, hand-checked against the source columns before regenerating; CommonMark 426 (`foo******bar*********baz`) becomes `Strong 4..18 > 6..16 > 8..14` with the tail `Text` at `1:19`, against the golden's `4..21 / 6..21 / 8..21 / Text 1:13`. It lands after 0a.12 and separately from it because 57 rows is the largest single regeneration in the stage and it deserves its own review. It does **not** interact with 0a.14: `S_insert_emph` already frees a delimiter node whose literal is spent (`if (opener_num_chars == 0) markdown_core_node_free(opener_inl)`), so the emphasis path creates no empty `Text` for D13 to clean up.

**0a.14 — D12 and D13, the empty-`Text` class. LANDED; §4.2.21 records it, and Q38 is taken.** It is last because it *removes* rows, and how many it removes depends on which empties exist — which 0a.12 and 0a.13 both affect. Option B, 24 lines: consolidation drops a `TEXT` node that owns no bytes and takes a merged run's end only from an operand that owns bytes (the `len > 0` guard is what makes the D12 line safe, since an empty operand may still be absorbed by a merge); autolink's `postprocess_text` stops leaving an empty prefix or an empty tail. Producer (3) — consolidation merging a run of empties into an empty — becomes unreachable by construction.

**106 rows change, 60 are replacements, 46 disappear.** The 46 are cross-checked here against the corpus: exactly 46 `Text … literal="" children=0` rows exist, of which **36 are the `0:0..0:0` sentinel** (30 `extensions.txt`, 6 `spec.txt`) and 10 carry honest positions — the base-language empty a hard break's stripped spaces leave. The other 60 are `children=` counts on `Paragraph`/`Link`/`TableCell`/`Strikethrough`. One hand-written assertion, `tests/api/main.c:1166`, **pins the defect** as expected output, the same shape as D10's; it is updated in this commit. The ledger goes **207 → 169** by `--update`, all shrink.

**Two things this commit must state.**

1. **The upstream red is the oracle, not the fix, and clearing it is a decision.** `check-upstream-parity.mjs` reads 784/795 without it: **cmark-gfm emits the empty text node too**, verified directly, and 11 corpus inputs diverge (8 autolink, 3 hard-break/shortcut-reference). This is the history already recorded in `specs/mdast-parity/deltas.json` under `empty-text-node` — *"suppressing it here failed `scripts/check-upstream-parity.mjs`, which is how this was classified as a shape delta rather than a defect"* — reproduced. Registering it costs one normalizer projection (drop an empty-literal `Text` from **both** sides in `scripts/lib/upstream-cmark.mjs`), one `NORMALIZED_DELTAS` name and one `deltas.json` entry of kind `deliberate-difference`: **795/795, green.** That is **Q38**, and §4.1's Step 5 row should have said so from the start rather than leaving it to be discovered at Step 5. The `specs/mdast-parity/deltas.json` `empty-text-node` entry goes half-stale in the same moment and is updated here.
2. **The free happens at `ENTER`, and that is legal today for a reason Step 5 removes.** `TEXT` is a leaf, so `S_is_leaf` suppresses its `EXIT` and `markdown_core_iter_next` has already computed `iter->next` past it. When Step 5 makes the event contract **total**, every node gets an `EXIT` and the rule "only the node whose `EXIT` is current may be freed" makes this free illegal. Step 5 must move it, or state that a leaf's `ENTER` *is* its `EXIT` for mutation purposes. Write that into the commit and into Step 5's row.

**0a.15 — D28 and D29, the two memory-unsafety defects §4.13 added after this list was written. LANDED; §4.2.22 records it, and there were FIVE unchecked allocations, not four.** Added 2026-08-21, before 0a.11, per §2's standing instruction; the reproductions and the argument for landing it *last* are in §2 beside the defect index. Neither fix moves a golden row and neither is reachable without an injected allocation failure, so it does not interact with 0a.12–0a.14's regenerations.

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

Counting the sentinel rows to size 0a.14 turned up a class nothing in §2 names. **Every number in the paragraph that follows was wrong, and each is corrected in place** — re-counted twice at 0a.12, once by hand over the fixtures and once by an independent measurement, agreeing row for row. The ORIGINAL text said 188 rows, `133 SoftBreak / 37 Text (36 empty-literal + 1 non-empty) / 17 LineBreak / 1 Paragraph`, and that `handle_newline` (`core/inlines.c:1443-1460`) uses `make_simple`. What is true:

```
    135  SoftBreak
     36  Text          all 36 empty-literal — D13's. D10's non-empty row is GONE; 0a.2 closed it
     18  LineBreak     14 from handle_newline, 4 from handle_backslash -- TWO sites, not one
      1  Paragraph     the SPLIT-OFF TABLE LEAD (extensions.txt), which is Step 10's, not Step 9a's
    ---
    190  and the ledger agrees: its budget sums to 190 sentinel + 16 negative + 1 partial = 207
```

The 190 includes `specs/canonical-ast/inlines.ast`, which the ledger counts and the old table omitted. `handle_newline` is at **`core/inlines.c:1512-1530`** and calls **`make_simple_subj`** (`:131`), the wrapper; `make_simple` (`:120`) is the `calloc` one level down. **Step 9a closes 0 of the 190, not 1.** So Stage 0a's D13 closes 36 and **D26 owns 153**, and Step 5's requirement — *"no node carries `0:0..0:0` as a stand-in for 'no bytes'"* — is unreachable without them.

> **D26 (proposed, MEASURED at 0a.12, and NOT taken there).** `handle_newline` and `handle_backslash` give `SoftBreak` and `LineBreak` no position at all. Severity: wrong-position. **153 golden rows, not 150; +22/−4 of C across two functions, not "a two-line fix"; and it is not free.** Put to the same test as the other fourteen, twice and independently:
>
> | spelling | scope-sanity | places | containment |
> |---|---|---|---|
> | the line ending's own column (`len+1`) | **−152** | **+138**, all `off-column` | +1 |
> | the last byte of its own line | −152 | +3 | **+138**, all sibling-overlap |
>
> **Both spellings trade one not-a-place class for another**, which is exactly why D13's Option A was rejected (§4.2.1). The cause is structural and is already written down in `specs/scope-sanity/ledger.json`'s own `purpose`: *the dump has no spelling for "no position" that does not borrow a coordinate.* `lineLengths()` (`scripts/lib/source-positions.mjs`) excludes the line ending, so by `audit-position-places.mjs`'s definition **a soft break has no position that is a place at all.**
>
> That makes D26 **a ruling about what a position is** before it is a repair, and it forces a public-contract edit besides: the only two witnesses of the declared `scope.zero` coverage state in the canonical corpus are the two rows the fix clears, so `specs/canonical-ast/manifest.json` and `check-canonical-ast-fixtures.mjs` must both drop it. **It therefore gets its own sub-step, 0a.12b**, carrying the ruling as **Q40 — is a line ending a place?** The honest reading of the transfer is that `L:len+1` strictly dominates `0:0..0:0` (it names the line, and 147 of 148 newly-positioned breaks sit strictly between their siblings), so the likely answer to Q40 is yes and the one-line change is to `fault()`; but that is a decision for the owner, not a side effect of a defect commit.

That is the ruling working as intended: the discipline of proving each defect fixable at the baseline is also the discipline that finds the ones nobody had counted — **and, at 0a.12, the discipline that stopped one being taken on an estimate that was wrong in every quantity.**

#### 4.2.6 What Stage 0a now moves, and why §4.4's argument gets stronger

**THE ESTIMATE WAS 252 GOLDEN ROWS, 2 C ASSERTIONS AND ONE `.ast` ROW. THE MEASURED TOTAL IS 419 ROWS, 5 ASSERTIONS AND 4 `.ast` ROWS**, and the whole of the difference is two sub-steps that did not exist when the estimate was written: 0a.12b (D26, 152 rows) and D32 riding inside 0a.12 (5 rows). Every figure below is what the commit measured, not what it predicted:

| Sub-step | Defects | `spec.txt` | `regression.txt` | `extensions.txt` | other |
|---|---|---|---|---|---|
| 0a.2 | D10, D11, D25 | — | 1 | — | + 1 ledger row; 4 new examples |
| 0a.5 | D8 | — | — | — | new fixture `extensions-conflicts.txt`, 2 examples |
| 0a.6 | D3, D7 | 13 | — | — | 2 new examples |
| 0a.7 | D5, D6, D16 | 54 | — | 4 | + 1 assertion; Q39 |
| 0a.9 | D14 | — | — | — | 3 new examples, **0 rows** |
| 0a.10 | D21, D22 | — | — | — | 2 `extensions-directive.txt`, 1 `structure.ast` |
| 0a.11 | D15, D24 | — | — | — | **0 rows**, 3 new examples |
| 0a.12 | D18, D19, D20, **D32** | 16 | 2 | 3 | + 1 new example |
| 0a.12b | **D26** | 104 | 18 | 5 | 17 `smart_punct.txt`, 6 `extensions-directive.txt`, 2 `extensions-formula-option-gates.txt`, 2 `inlines.ast`, 3 assertions |
| 0a.13 | D23 | 45 | 11 | 1 | |
| 0a.14 | D12, D13 | 28 | 18 | 122 | 1 `inlines.ast`, 1 assertion; **107 removed, 61 rewritten** |
| 0a.15 | D28, D29 | — | — | — | **0 rows**; 1 new fallback case |
| | **total** | **260** | **50** | **135** | **419 rows, 4 `.ast` rows, 5 assertions, net row count −46** |

`specs/scope-sanity/ledger.json` goes **207 → 14**, not the estimated ~166 — an order of magnitude further, because D26 was not in the estimate. Every movement is a shrink except two recorded exceptions of the shapes the ledger already tracks (0a.12's third SoftBreak-in-a-second-line row, 0a.12b's one sentinel→partial), and 0a.12b **closed the hole that made the second one invisible**: the `partial` class was measured, stored, counted and never compared against the budget. `specs/positions/places.json` goes **122 → 109** with two whole families emptied; `specs/positions/containment.json` goes **58 → 45**.

**The three caveats below were written while the 252 was a sum of independent measurements. All three are now discharged, and the answers are recorded here rather than deleted.**

1. **The four pairs were re-measured composed, and none interacted.** D13 × D20: different files and different mechanisms — D20 is `strikethrough.c`'s unset end column, D13 is autolink's zero-length split, and 0a.12's three `extensions.txt` rows are disjoint from 0a.14's. D13 × D23: 0a.13 moved 57 rows and 0a.14 moved none of them back. D6 × D16: additive as predicted, 18 + 40 = 58. D14 × D10/D25: composed at 0a.9 and the composition **changed the numbers**, which §4.2.15 records — 360 of the 432 matrix cases move, not 252, and the NUL and invalid-UTF-8 rows are 0 and 0, not 162 and 90, because 0a.2 had already removed the heap bytes.
2. **The composed number was expected to be ≤ 252 and is 419.** The prediction was sound for the defects it covered; what broke it is two defects that did not exist when it was written.
3. **Corpus growth, measured at the close:** upstream parity goes **795 → 817 inputs** with **7** active `expectedDivergence`s and `deltas` **4 → 9**. mdast holds at 54/54 with the backlog at 24, which is the design (§2: Stage 0a closes none of them).

**The original three caveats, for the record:**

1. **Four pairs must be re-measured composed, once:** D13 × D20 (both concern consolidation carrying a zero end position, and the agent's `--update` runs overlap in `extensions.txt`'s negative bucket), D13 × D23 (both touch emphasis examples in `spec.txt`), D6 × D16 (expected additive — 18 + 40 = 58, cross-checked against the corpus's 58 `title=""` rows, but prove it), and D14 × D10/D25 (same branch, same function). D18 × D19 is already verified additive: 10 + 1 = 11 rows and nothing else.
2. **The composed number is expected to be ≤ 252**, never more, because a row fixed twice is counted twice here.
3. **Corpus growth:** ~20 new examples, of which most land in files that are also the upstream-parity corpus, taking that gate from 795 inputs to roughly **813** with **four** registered `expectedDivergence`s active (D5, D10's byte retention, D25's entity caret, D24's tasklist marker) and `deltas` going 4 → 6 (D11's footnote-model rule, D13's `empty-text-node`). Measure it once at the close of the stage rather than per commit.

**Does 252 change §4.4's argument that regenerating once beats regenerating repeatedly? It strengthens it, on both halves.**

*The duplicate-work half.* Under the old schedule these same 252 rows were regenerated by Steps 5, 7, 8, 9a, 10 and 14 — and regenerated *again* whenever an earlier step touched the same file first. Step 14 alone was going to move 40 rows for `title=""`, ten steps after the defect could have been closed for eight lines. Step 8 was going to move 71 position rows behind six other steps. Step 5 was going to move 106. Under this schedule **Step 5, Step 8 and Step 14 each move zero**, and 5 and 8 gain something better than a smaller diff: an acceptance test that says *the rewrite moves no golden row*, which is the only falsifiable form of "subsumes it by construction".

*The blessed-golden half — and this is where 252 is a much stronger number than 32.* §4.4's corollary is that a golden regenerated while a defect is live **blesses** the defect, because the reviewer's only available answer is "unchanged from before, therefore fine". The stage now unpins **five** goldens that currently assert a defect as expected output — `regression.txt:474` (`Text scope=0:0..0:0 literal="[^~~is~~1]"`), `extensions.txt:804`/`:809` (`59:1..59:0`), `extensions-directive.txt` example 16 (an inner fence ending at the outer fence's line), `tests/api/main.c:1076` and `:1166` — plus a sixth, `specs/canonical-ast/structure.ast`, which no defect statement predicted and which only the `conformance` preset catches. Each of those flips from defending the defect to killing it. **A corpus that asserts six wrong answers is not a corpus that got 32 rows more expensive to regenerate; it is a corpus that cannot be used to review anything until it is corrected.**

*The counterweight, stated honestly.* 419 rows is a lot of hand review, and the standing gate requires every moved row to be reviewed and named. That is why the stage is seventeen commits and not one: the largest single regeneration is 152 rows (0a.12b), then 168 line-changes over 107 rows (0a.14), then 58 (0a.7) and 57 (0a.13), each in a commit whose subject is the defect and whose reviewer has §2's statement in hand. **And "reviewed by hand" was made mechanical wherever the claim allowed it**, which is what made 419 rows reviewable at all: 0a.12b checked that every deleted line is a break carrying `0:0..0:0` and every added line is a break carrying a real position (0 exceptions), and that 145 of 152 end exactly at their own line's ending counted in bytes; 0a.13 checked that all 57 new spans are strictly inside their old ones (57 of 57); 0a.14 classified all 107 removals and 61 rewrites by shape. A script cannot say a row is *right*, but it can say every row moved in the one direction the fix claims — and then the eye only has to look at the residue. The alternative is not "fewer rows"; it is the same rows, spread across eight steps, with no statement in hand at any of them.

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

#### 4.2.18 0a.12 landed: three defects were scheduled, four were fixed, and the fifth was refused on measurement

**What is in the engine, four hunks.**

- **D18** — `resolve_reference_link_definitions` (`core/blocks.c`) counts the `\n` in the prefix it drops and advances `b->start_line` by that many. It is in the HELPER, so both consumers get it: `finalize` and the setext path — which is why a `Heading` moves. Sound because `markdown_core_parse_reference_inline` returns only after `skip_line_end` succeeds, so the dropped prefix always ends on a line boundary, and because `S_process_line` normalises every terminator to one `'\n'`, so the count is right under CRLF and lone CR alike.
- **D19** — at `handle_close_bracket`'s `match:` label, the start comes from `opener->inl_text` and the newlines the bracket handler consumed for itself are counted by `adjust_subj_node_newlines(subj, inl, subj->pos - initial_pos, 0)`.
- **D20** — one line in `strikethrough.c`'s `match`: `res->end_column = res->start_column + delims - 1`.
- **D32** — `handle_backslash`'s hard-break branch does what `handle_newline` does: `++subj->line; subj->column_offset = -subj->pos;`.

##### The span in D19 is [`]`, `)`], not [`[`, `)`], and getting that wrong is silent

The obvious reading — count from the opening bracket, since that is the link's extent — **double-counts**, because the link TEXT was walked by the inline parser and every newline in it already went through `handle_newline`. Built that way first, `[a⏎b](/u) tail` reports **line 3 of a two-line document**. The correct span is `[initial_pos, subj->pos)`: what the bracket handler consumed without the inline parser. Nothing in the corpus would have caught the wrong version — `spec.txt` example 518's newline is inside the destination, not the label — so it is written down here.

##### D32, which no section named

Found while measuring D26's cost, by reading `core/inlines.c` for every path that consumes a line ending. There are three. `handle_newline` advances the subject. `adjust_subj_node_newlines` advances it (that is 0a.6's un-gating, and it covers inline code and raw HTML). **`handle_backslash`'s hard break advances nothing** — it calls `skip_line_end` and returns. So `foo\`⏎`bar` reported `Text 1:6..1:8`, three columns that do not exist on a four-character line 1, and every later node in the paragraph inherited it. cmark-gfm reports the same numbers, so upstream is no oracle here either.

**It was already registered, and mis-attributed.** Five of `specs/positions/places.json`'s seven `multi-line-span` findings are this defect, and all seven carried `closedBy: "0a.6 — D3, un-gating adjust_subj_node_newlines"`. 0a.6 could never have closed them: neither `handle_close_bracket` nor `handle_backslash` calls that helper. **A landed sub-step was on record as owing five rows it had no mechanism to move.** The sixth was D19's; the seventh is D12's and 0a.14 owns it.

##### What moved

**21 golden rows in three files** — `spec.txt` 16, `extensions.txt` 3, `regression.txt` 2 — plus one new `regression.txt` example. Every row read by hand against its own input line:

| defect | file | rows | classification |
|---|---|---|---|
| D18 | `spec.txt` 177, 179, 184, 185 | 10 | 9 correct; **1 still wrong but better** — example 184's `Heading` start moves to `2:1`, its end stays `4:5`, which is **H14** (§11.4) and visible with no definition in sight: `bar⏎===⏎x` gives `Heading 1:1..3:1` overlapping `Paragraph 3:1..3:1` |
| D18 | `regression.txt` (0a.7's `refdef-title-rewind` example) | 2 | correct — **and §4.2.1's D18 row said "10 rows, one file"**; 0a.7's own note had already said these two would move here, and the row-count was never updated |
| D19 | `spec.txt` 518 | 1 | **still wrong but better**: `1:1..1:25` (a linear offset printed as a column) → `2:10`, a real place; the true source column is `2:12` and the two-column shortfall is the continuation line's stripped indent, Q22/Step 10's. **CLOSED at Step 10, at the column this row predicted**: the example now reads `Link scope=1:1..2:12` (§4.14.10) |
| D20 | `extensions.txt` 568, 582, 584 | 3 | all three correct |
| D32 | `spec.txt` (4 backslash-hard-break examples) | 5 | all five correct |

**Ledgers.** `scope-sanity` **207 → 205**; `places` **122 → 110**; `containment` **58** and `inline-sourcepos` **0**, both unmoved. The 12 places rows that cleared are 3 `consumed-definition-line` (D18 — the family is now empty), 1 `multi-line-span` (D19), 3 `end-column-never-set` (D20), 5 `multi-line-span` (D32).

**One ledger class GREW, deliberately, and it is the third time in the same shape.** `regression.txt`'s sentinel budget goes **17 → 18**, because the new example's second line brings a `SoftBreak` and a `SoftBreak` is a sentinel by construction. `specs/scope-sanity/ledger.json`'s `purpose` already records two exceptions of exactly this shape (0a.6's and the 2026-08-10 autolink one) and now records a third with its reason. All 18 disappear if 0a.12b lands.

**`--update` also normalised churn that is not this commit's**, and it is named so nobody reads it as movement: four `source:` line numbers for `regression.txt` entries were stale by one, and three entries had `class`/`closedBy` in the opposite key order. The reconciler keys on input + findings, so it had drifted invisibly. Both ledgers' `purpose` prose was hand-corrected in the same commit — `--update` rewrites rows only, and the prose was wrong about the `end-column-never-set` split (it claimed the corpus carried "the autolink spelling of it and not yet the strikethrough one"; exactly three of the six were strikethrough) and about the total (`207 -> 206 -> 207`).

##### Mutant kills, and the gate that cannot kill anything

Each mutant `touch`ed, `sleep 2`, rebuilt, and confirmed live by running its witness before any suite was believed.

| mutant | `correctness` | `places` | `containment` | `scope-sanity` | parity |
|---|---|---|---|---|---|
| revert D18 | `spec_commonmark` **and** `regression_commonmark` red | **3 APPEARED** | — | blind | blind |
| revert D19 (both halves) | `spec_commonmark` red | **1 APPEARED** | — | blind | blind |
| revert D19's **start-line half only** | **GREEN 67/67** | — | — | blind | blind |
| revert D20 | `extensions_gfm` red | **3 APPEARED** | — | blind | blind |
| revert D32 | `spec_commonmark` red | **5 APPEARED** | — | blind | blind |

**D19's start-line half had NO gate**, and that is the finding. Reverting only `inl->start_line = opener->inl_text->start_line;` leaves `correctness` at 67/67 and all three position oracles green — a link that begins after its own first child is not a *place* violation (both coordinates exist) and the corpus contains no input with a multi-line link label. The commit closes it: `regression.txt` gains `[a⏎b](/u) tail`, and with it `audit-scope-containment.mjs`'s parent/child rule fires, because `child.start = [1,2]` sorts before `parent.start = [2,1]`. That example is the sentinel exception above; it is the price of the gate and it is worth it.

**`audit-scope-sanity.mjs` kills nothing, and §4.2.3 lists it as though it watched the engine.** It reads the fixture files' expected dumps — it never runs the binary. Once a commit regenerates its goldens the ratchet cannot see the engine revert at all: every mutant above leaves it green. It is a **corpus ratchet**, and the three oracles 0a.1 added are the **engine** oracles. Its 207 → 205 movement is a consequence of the regeneration, not an independent detection. Write that down wherever the four are listed together.

##### D26 was measured here and refused here

§4.2.5 carries the numbers and §4.2.3 carries the new sub-step. The short form: it is real, its fix is +22/−4 across **two** sites rather than two lines at one, and both available spellings move ~138 rows from one not-a-place class to another. It is a ruling about what a position is, and a defect commit does not get to take one.

##### Gates after

`correctness` **67/67** · `correctness-asan` **58/58** · `correctness-ubsan` **58/58** · `conformance` **2/2** · upstream parity **817/817** with **7/7** divergences (816 → 817 is the new regression example) · mdast **54/54**, backlog **24/24** · fuzz-parity **300/300** · scope-sanity **205** · position oracles **0 / 58 / 110** · reference-order 2 rows, still red · canonical-ast 28/47/6 · public surface · special chars · attach order · plan graph 22/45 · topology · format-c · format-cmake.

---

#### 4.2.19 0a.12b landed: Q40 taken narrowly, and 153 sentinels become places

**Q40 is answered, and the narrow form is the one that survives measurement.** A
line of *L* bytes has *L+1* boundaries and the last of them is where the line
ending lives, so column *L+1* is a place — **for a node that IS a line ending,
and for nothing else.** The general form was built and measured first and
rejected: admitting *L+1* for every kind would have excused **twelve** rows
already in `specs/positions/places.json` (eleven `Text`, one `Emphasis`) that
are wrong for other reasons. The narrow form excuses none of them, because no
break node was ever registered there — before this commit they all carried
`0:0..0:0` and were deferred to `audit-scope-sanity.mjs`. The rule lives in
`fault()` in `scripts/audit-position-places.mjs` and the reasoning is in the
file, not only here.

**Two sites, and §4.2.5 named one.** `handle_newline` writes the break's line
and column read in the frame of the line being *left*. `handle_backslash`'s
hard-break arm does the same — a one-site fix would have left four `LineBreak`
sentinels standing.

**The two arms are symmetric BY CONSTRUCTION, and the first cut was not.** An
adversarial re-read caught it: the backslash arm read `subj->line` and
`subj->column_offset` *after* `skip_line_end` had consumed the newline, and its
answer was right only because `skip_line_end` happens not to advance the line
frame. That is an accident, not a contract — and it is an accident that would
turn into `LineBreak 2:-1..2:-1`, a **negative column on the wrong line**, the
day anything fixes `skip_line_end`. The capture is hoisted to the top of
`handle_backslash`, beside the `start` offset that was already captured there.
The output is identical in both worlds; **the change is robustness, and it moves
zero rows** — `correctness` reads 67/67 across it.

**What each break covers.** The bytes that spell it. A soft break and a
two-space hard break own the line ending alone — the two spaces stay with the
`Text` they follow, as upstream also has them. A backslash hard break owns the
backslash *and* the line ending, because the backslash belonged to no node at
all before this, so the break takes nothing from anyone.

##### What moved

**152 fixture rows across six files, 2 `.ast` rows, and 5 lines carrying 3
hand-written C assertions** — `spec.txt` 104, `regression.txt` 18,
`smart_punct.txt` 17, `extensions-directive.txt` 6, `extensions.txt` 5,
`extensions-formula-option-gates.txt` 2, `specs/canonical-ast/inlines.ast` 2,
`tests/api/main.c` 3 assertions.

Mechanically classified rather than eyeballed, twice and independently:

- **every** deleted line is a break carrying `0:0..0:0` and **every** added line
  is a break carrying a real position — 0 exceptions in either direction,
  checked by grep over the whole diff;
- **145 of the 152** end *exactly* at their own line's ending, counted in
  **bytes** (the first count said 144 and was wrong: it measured `String.length`,
  and one spec example's line contains a two-byte U+00A0);
- the other **7 are in classes already registered to someone else**, and none is
  new wrongness: 3 block-quote lazy continuations and 1 stripped-indent
  continuation whose siblings are already `continuation-line-content-offset`
  (Q22/Step 10), 2 more of the same class inside a `<span>` fixture, and 1 whose
  whole paragraph is on line zero because it is the split-off table lead
  (Step 10's).

##### The ledgers, and a hole this commit had to close first

`scope-sanity` **205 → 52**, and five of its seven files leave the ledger
outright. What remains is 37 sentinel — 36 of them D13's empty `Text`, which is
0a.14's — plus 13 negative and 2 partial. `places` **110 → 113**, `containment`
**58 → 59**; the four new rows are annotated to Q22/Step 10 by hand, because
`--update` writes rows and not owners.

**`partial` was measured, stored, counted in the total, and never compared
against the budget.** `audit-scope-sanity.mjs` iterated `["sentinel",
"negative"]` only, so a fix that moved a row from one not-a-place class into the
third could grow it without limit and the gate stayed green — which is the one
thing this ledger exists to prevent, and exactly how D13's Option A was
rejected. This commit moves one row that way (the split-off table lead's break
inherits its paragraph's line zero), so the hole is closed **first** and then
the growth is recorded as the fourth exception in the ledger's own `purpose`.
Proved: with the budget set back to `partial: 1` the gate now throws
`partial rows grew 1 -> 2`; before the fix it printed `at budget, only-shrink
holds`.

##### The contract edit, and why there is no honest alternative

`specs/canonical-ast/manifest.json` declared **`scope.zero`** as a required
coverage state, and `check-canonical-ast-fixtures.mjs` carried its validator.
The only two witnesses of that state in the whole canonical corpus are
`inlines.ast`'s `LineBreak` and `SoftBreak`, and this fix gives both a real
position — so the gate goes red with
`manifest state vocabulary must exactly match the fail-closed audit vocabulary`,
and it goes red the same way whether you edit the manifest or the validator
alone. Both must go, and **re-witnessing is not available**: the only remaining
producers of `0:0..0:0` are D13's empty `Text`, which 0a.14 deletes, and the
split-off table lead, which is Step 10's — so adding a witness would pin a
defect the stage is closing, which is §4.4's corollary exactly. This is a
**coverage obligation**, not a grammar or schema change: the dump still permits
`0:0..0:0`, no golden format moves, and no binding is affected.

##### Mutant kill

Reverting `core/inlines.c` alone, with goldens and ledgers left fixed,
`touch`ed, `sleep 2`, rebuilt, and the mutant confirmed live in the binary
first:

| gate | green | mutant |
|---|---|---|
| `ctest --preset correctness` | 67/67 | **60/67** — `spec_commonmark`, `spec_smart_punctuation`, `regression_commonmark`, `extensions_gfm`, `extensions_directive`, `extensions_formula_option_gates`, `api_engine` |
| `ctest --preset conformance` | 2/2 | **0/2** — both `facade_native` and `facade_dump_cli` |
| `audit-position-places.mjs` | 113 | **red, 3 CLEARED** |
| `audit-scope-containment.mjs` | 59 | **red, 1 CLEARED** |
| `audit-scope-sanity.mjs` | 52 | **green** — it reads goldens, not the binary |
| `check-canonical-ast-fixtures.mjs` | 28/47/6 | **green** — the validator this fix deleted was the only thing that watched the class |

The last row is worth stating plainly: **the gate that blocked the fix does not
defend it.** That is not an argument for keeping `scope.zero`; it is an argument
for knowing which gate holds which fact.

##### Gates after

`correctness` **67/67** · `correctness-asan` **58/58** · `correctness-ubsan`
**58/58** · `conformance` **2/2** · upstream parity **817/817** with **7/7** ·
mdast **54/54**, backlog **24/24** · fuzz-parity **300/300** · scope-sanity
**52** · position oracles **0 / 59 / 113** · reference-order 2 rows, still red ·
canonical-ast 28/47/6 · public surface · attach order · plan graph 22/45 ·
topology · format-c · format-cmake.

---

#### 4.2.20 0a.13 landed: 57 rows, all of them a strict shrink, and the one-liner measured

**What is in the engine.** Four assignments in `S_insert_emph`, and the rule
behind them is one sentence: **the emphasis takes the delimiters adjacent to its
content, so the leftovers are the opener's LEADING bytes and the closer's
TRAILING ones.**

```
emph->start_column   = opener_inl->start_column + opener_num_chars
emph->end_column     = closer_inl->end_column   - closer_num_chars
opener_inl->end_column   = opener_inl->start_column + opener_num_chars - 1   (if it survives)
closer_inl->start_column = closer_inl->end_column   - closer_num_chars + 1   (if it survives)
```

**The two `if it survives` guards are not decoration.** A leftover with zero
bytes is freed four statements later, and writing its end unconditionally would
put a reversed range in the tree for the length of those four statements — true
only by reading ahead, which is not a property worth relying on. `correctness`
reads 67/67 across adding the guards, so they cost nothing and remove a fact a
reader would otherwise have to reconstruct.

##### 57 rows, and the classification is mechanical

`spec.txt` **45**, `regression.txt` **11**, `extensions.txt` **1** — exactly what
§4.2.1 predicted, file for file. Every moved row was checked by a script rather
than by eye, and the property it checks is the one the fix claims: **the new span
is strictly inside the old one.** 57 of 57. No row grows, none changes kind, none
moves line, and no row is merely re-spelled.

Two hand-checked against the source columns, both byte-exact and
non-overlapping:

- CommonMark 426, `foo******bar*********baz` → `Strong 1:4..1:18 > 1:6..1:16 >
  1:8..1:14` with the tail `Text` at `1:19..1:24`, against the golden's
  `4..21 / 6..21 / 8..21` and `Text 1:13`. That is §4.2.3's prediction to the
  column.
- `regression.txt`'s issue #177, `a***b* c*` → `Text "a*" 1:1..1:2`,
  `Emphasis 1:3..1:9 > Emphasis 1:4..1:6`, `Text "b" 1:5`, `Text " c" 1:7..1:8`.
  Nine characters, five nodes, no byte claimed twice.

##### The one-liner, measured rather than argued

§2 names a one-line fix — `emph->start_column += opener_num_chars` — and §4.2.1
says taking it "buys a second golden churn later". That is now a number.
**Reverting the complete cut down to the one-liner leaves SEVEN of the fourteen
sibling overlaps standing**, because the leftover `Text` still spans the whole
run: `***a**` reports `Text 1:1..1:3` beside `Strong 1:2..1:6`, and the two
still claim two of the same bytes. `correctness` is red either way, so the
goldens alone cannot tell the two cuts apart — only the containment oracle can.

##### Mutant kills

| mutant | `correctness` | `containment` | others |
|---|---|---|---|
| full revert | red — `spec_commonmark`, `regression_commonmark`, `extensions_gfm` | **14 APPEARED** | places, sanity, inline-sourcepos, both parity gates all blind |
| §2's one-liner only | red — the same three | **7 APPEARED** | same |

The containment ledger goes **59 → 45**, and the fourteen it loses are the cause
the sibling half of that oracle was built for — §4.2.7's *"the only statement
that catches it is that two nodes claim one byte"*, doing exactly that.

##### Gates after

`correctness` **67/67** · `correctness-asan` **58/58** · `correctness-ubsan`
**58/58** · `conformance` **2/2** · upstream parity **817/817** with **7/7** ·
mdast **54/54**, backlog **24/24** · fuzz-parity **300/300** · scope-sanity
**52** · position oracles **0 / 45 / 113** · reference-order 2 rows, still red ·
canonical-ast 28/47/6 · public surface · attach order · plan graph 22/45 ·
topology · format-c · format-cmake. Neither `places` nor `scope-sanity` moved,
which is right: an overlap is not a not-a-place, and that is the whole reason
0a.1 built three oracles instead of one.

---

#### 4.2.21 0a.14 landed: a Text that owns no bytes is not a node, and Q38 is a projection

**Option B, both halves, and the halves are not interchangeable.**

- `markdown_core_consolidate_text_nodes` (`core/iterator.c`) takes a merged run's
  end — **line and column together, which is D12** — only from an operand that
  owns bytes, and then **drops any `TEXT` node whose literal is empty.** The drop
  makes the third producer unreachable by construction: a run of empties can no
  longer merge into an empty, because the operands are gone before the merge.
- `postprocess_text` (`extensions/autolink.c`) stops creating an empty prefix and
  an empty tail. **This half is not optional and consolidation cannot cover it**:
  `markdown_core_consolidate_text_nodes` runs at `core/blocks.c:1751`, *before*
  every extension postprocess, and autolink's own call to it is the first thing
  its postprocess does — so every empty the split makes is made after the last
  consolidation that could have seen it.

##### One line the allocation-failure sweep caught, and it is worth naming

The first cut skipped the buffer detach when the merged buffer came out empty,
which looks like a tidy way to avoid reporting an honest empty as a loss. It is
not: a **poisoned** buffer also comes out empty, and skipping the detach then
dropped a node whose bytes an allocation failure had eaten — silently. The
`regression_fallback_oom_sweep` gate caught it on the first run, which is the
gate doing exactly what §4.13.9 built it for. The shipped form detaches
unconditionally, reports `ok = 0` when the detach returns NULL, and **`continue`s
past the drop**, so the drop can only ever remove a node that is honestly empty.

##### What moved

**107 rows removed, 61 added, net −46** — against §4.2.3's prediction of "106
rows change, 60 are replacements, 46 disappear". The 46 is exact; the other two
are each one higher, and the extra pair is one `Text` position row and its
container's `children=` count. Mechanically classified:

| | |
|---|---|
| removed rows that are `Text … literal="" children=0` | **46** — exactly the population §4.2.3 counted |
| replacement rows that are a `children=` count | **45** (13 `Link`, 29 `Paragraph`, 1 `Strikethrough`, 2 `TableCell`) |
| replacement rows that are a `Text` position | **16** |

Plus `specs/canonical-ast/inlines.ast` (one row and its parent's count) and one
hand-written C assertion. **`tests/api/main.c` pinned the defect** — it asserted
`Text scope=0:0..0:0 literal=""` as *expected* output for an autolink at column
one, and a `Paragraph children=2` holding one thing. Unpinning it is the fix,
the same shape as D10's `regression.txt` example 24 at 0a.2.

**Ten of the 46 carried honest positions, and dropping them is a stated
consequence rather than an oversight.** They are the base-language empty a hard
break's stripped spaces leave, and after the drop those two bytes reach no node
— which is already true of every other markup byte in this model (a code span's
backticks, a strikethrough's tildes) and is the CST's business, not this
defect's.

##### The canonical corpus lost a state it can no longer honestly demonstrate

`inlines` declared **`escaping.empty-string`**, and its only witness in that
case was the `literal=""` this commit removes. The state stays in the global
requirement — `completeness.ast` still carries `Link title=""`, which is a title
that *was* written and is empty, honest since 0a.7 — so only the `inlines`
case's claim is dropped. Adding a witness instead would have meant pinning one
of the two remaining producers, and both are defects the stage is closing.

##### Q38, and the shape of the answer

Without it the gate reads **806/817 with eleven inputs diverging** — eight
autolink, three hard-break and shortcut-reference — which is §4.2.3's number
reproduced. **The answer is a PROJECTION, not eleven `expectedDivergences`
rows**, and the reason is the same test every other delta is held to: the
difference appears wherever the construct does, so it is a *model* difference,
and a list of inputs would go stale the moment the corpus grew. `normalize` in
`scripts/lib/upstream-cmark.mjs` drops an empty-literal `Text` from **both**
sides, after the adjacent-run join so that a merged run which came out empty
goes with it; `empty-text-node` joins `NORMALIZED_DELTAS`; and
`specs/upstream-parity/deltas.json` gains the entry.

`specs/mdast-parity/deltas.json` carried the same id and its evidence line said
*"suppressing it here failed `scripts/check-upstream-parity.mjs`, which is how
this was classified as a shape delta rather than a defect."* That sentence is now
**history**: the failure was reproduced exactly and then answered. The entry is
amended to say the delta is upstream-only.

**And the projection costs something, which the commit says out loud.** With
`empty-text-node` projected away, re-introducing this side's empty node is
**invisible to upstream parity** — measured: mutant C below leaves it at
817/817. The goldens are the only gate on it. That is the price of calling it a
model difference, and it is the same price `own-extensions` and
`reference-definition-node` already pay.

##### Mutant kills

| mutant | `correctness` | `places` | `scope-sanity` | upstream |
|---|---|---|---|---|
| take the end column but not the end LINE (D12 alone) | `regression_commonmark` red | **1 APPEARED** | blind | blind |
| take the end from **any** operand (drop the `len > 0` guard) | `extensions_gfm` red | 0 | blind | blind |
| keep the empty `TEXT` (drop the consolidation half) | `spec_commonmark` + `regression_commonmark` red | 0 | blind | **blind, 817/817** |
| keep autolink's empty prefix and tail | `spec_commonmark` + `extensions_gfm` red | 0 | blind | blind |

Every one of the four is caught, and **three of the four only by the goldens.**
`audit-scope-sanity.mjs` is blind to all of them for the reason 0a.12 recorded —
it reads the regenerated fixtures, not the binary.

##### Ledgers

`scope-sanity` **52 → 14**: **one** sentinel, 11 negative, 2 partial. The single
remaining sentinel is the split-off table lead's paragraph, which
`try_inserting_table_header_paragraph` creates with all four coordinates zero —
**Step 10's**, and the row §4.2.5 originally mis-attributed to Step 9a. Over the
whole stage that ledger has gone **207 → 14**.

`places` **113 → 109**, and **two whole families are now empty**:
`multi-line-span` (nineteen → seven → one → none) and `end-column-never-set`
(six → three → none). Both are recorded in the file rather than deleted from it,
because each was cleared by a **different defect than the one the ledger named**
— which is the ledger's own `closedBy` discipline reporting on itself.

##### Gates after

`correctness` **67/67** · `correctness-asan` **58/58** · `correctness-ubsan`
**58/58** · `conformance` **2/2** · upstream parity **817/817** with **7/7** and
a ninth registered delta · mdast **54/54**, backlog **24/24** · fuzz-parity
**300/300** · scope-sanity **14** · position oracles **0 / 45 / 109** ·
reference-order 2 rows, still red · canonical-ast 28/47/6 · public surface ·
attach order · plan graph 22/45 · topology · format-c · format-cmake.

---

#### 4.2.22 0a.15 landed: five unchecked allocations, not four, and a gate that reads what no other gate reads

**D29 — `try_inserting_table_header_paragraph` (`extensions/table.c`).** §4.13.11
names three problems and there are **five**, because two allocations it did not
name are unchecked in the same twelve lines:

| # | what | consequence |
|---|---|---|
| 1 | `markdown_core_node_new_with_mem` unchecked | **SIGSEGV** — the NULL reaches `markdown_core_node_set_string_content`, which dereferences it |
| 2 | `unescape_pipes` returning NULL | the lead paragraph is dropped and `parser->oom` stays clear |
| 3 | the failed insert frees with `mem->free` | the node's content buffer leaks |
| 4 | **`unescape_pipes` returning a POISONED buffer** | *not named anywhere.* It hands back `res` with `res->oom` set rather than NULL, and the caller copies whatever fitted |
| 5 | **`markdown_core_node_set_string_content` cannot fail** | it returns `true` unconditionally, so a poisoned `node->content` is invisible unless the caller reads the flag itself |

**(4) and (5) were found by the gate, on its first run**, and they are the
interesting ones: with (1)–(3) fixed the sweep still reported *"allocation
139 / 429: lossy document reported as success"* with the lead paragraph's text
missing entirely. That is precisely the failure mode §4.13.9 built the sweep
for — a wrong document with the failure bit clear — and it is why "fix the three
things the doc names" would have shipped a hole.

**D28 — `set_formula_literal_bytes` (`extensions/formula.c`).** The chunk is
pointed at a **borrowed** buffer and `markdown_core_chunk_to_cstr` is then asked
to copy it; the failure was ignored, so the borrow survived and the owner died
on the next statement — `make_backslash_delimited_formula` frees its strbuf,
`replace_with_formula_block` frees the whole old code block, `postprocess_node`
clears the node's own content. The fix drops the borrow and returns 0, and all
**three** call sites now propagate it (`make_formula_node`,
`new_formula_block_from_literal`, `postprocess_node`).

##### The sweep could not see D28, and the reason is structural

`case_oom_sweep` compares trees with a deliberately **allocation-free**
comparator — the sweep allocator is still armed during the comparison, so the
comparator must not allocate, and the public literal accessors do. It therefore
never touches an **extension payload**, and a formula's literal is exactly that.
Measured: with D28 reverted and the corpus additions in place, `correctness`
reads **68/68** and the sweep **passes**. Nothing crashes, because nothing reads
the dangling pointer.

So 0a.15 adds a seventh fallback case, **`formula_literal_borrow`**, which reads
the literal through the public accessor with the sweep allocator **disarmed for
the read** — arming it during the comparison would inject a second failure into
the measurement. Reverting D28 against it gives, under `default`,
*"allocation 18 / 23: formula literal lost or changed in a successful parse"*,
and under ASan **§4.13.11's witness character for character**:
`heap-use-after-free`, `READ of size 5`, in
`markdown_core_extensions_get_formula_literal` at `formula.c:61`.

##### The corpus additions, and why the sweep was blind before

`FB_SWEEP_CORPUS` carried a table and an info-string fence, and neither of the
two shapes that matter: **a paragraph a table splits its header row out of**
(no blank line between), and a ```` ```formula ```` fence. Both are added with
the reason written beside them. The sweep's own contract — *each injected failure
must either surface as a failed parse or leave the output byte-identical to the
control* — was already the right assertion; it simply had nothing to assert it
over.

##### Mutant kills

| mutant | result |
|---|---|
| D29's unchecked `markdown_core_node_new_with_mem` | `regression_fallback_oom_sweep` **SEGFAULT** |
| D29's poisoned-buffer check | `oom_sweep` fails: *lossy document reported as success*, lead text missing |
| D28's ignored copy failure | `formula_literal_borrow` fails at allocation 18/23; under ASan, `heap-use-after-free` READ of size 5 |
| D28, with `formula_literal_borrow` absent | **nothing** — 68/68 and the sweep green. That is the case for the new gate |

##### Gates after

`correctness` **68/68** · `correctness-asan` **59/59** · `correctness-ubsan`
**59/59** (the counts grew by one: the new fallback case) · `conformance` 2/2 ·
upstream parity **817/817** with 7/7 · mdast **54/54**, backlog **24/24** ·
fuzz-parity 300/300 · scope-sanity 14 · position oracles 0 / 45 / 109 ·
reference-order 2 rows, still red · canonical-ast 28/47/6 · public surface ·
special chars · attach order · plan graph 22/45 · topology · format-c ·
format-cmake. **Zero golden rows moved**, which is what an allocation-failure
fix should move.

---

### 4.14 The step records — Steps 2 onward

Stage 0a's records are §4.2.7 through §4.2.22, one per sub-step. The steps of
§4.1's list keep the same shape and continue here: what the step moved, what it
corrected in this document, and what it left.

---

#### 4.14.2 Step 2 landed: 835 brace pairs, a gate the reformat broke, and three C sources no gate was reading

**The invariant.** `.clang-format` gains exactly one line, `InsertBraces: true`,
and the effective configuration differs from the baseline's in exactly that one
key — `clang-format --dump-config` before and after differ on line 188 and
nowhere else, and `RemoveBracesLLVM` stays `false`, which matters below.

**What it costs, measured rather than repeated.** §4.1 said *2,393 diff lines*
and Q11 said *36 files, 561 of them in `core/` + `extensions/`*. Both are stale
and the second is stale by a factor of three:

| area | files | diff lines |
|---|---|---|
| `core/` + `extensions/` | 24 | **1,700** |
| `tests/` | 14 | 772 |
| **the engine total** | **38** | **2,472** |
| the three bridge sources (below) | 3 | 35 |
| `.clang-format`, `format-c.sh`, `audit-extension-attach-order.mjs` | 3 | 39 |

**835 brace pairs inserted, and the claim is mechanised.** Standing rule 3 asks
that a moved row be more than eyeballed. The claim here is *the reformat added
and moved braces and layout, and nothing else*, and it is checkable exactly:
**delete every whitespace character and every `{` and `}` from both versions of
a file and the residues must be byte-identical.** They are, for all 41 changed C
sources — 38 that gained braces (835 `{` and 835 `}`, balanced per file, none
removed anywhere) and 3 that only reflowed. The eye then reads nothing, because
there is no residue to read.

##### The reformat broke a gate, and it broke it loudly

`scripts/audit-extension-attach-order.mjs` went **red**, with
*"no call to `markdown_core_parser_attach_syntax_extension` in the library at
all — this audit is reading the wrong tree."*

The audit distinguished a call from the function's own definition by a
heuristic: *a call is followed eventually by a `;` with no `{` in between*.
`InsertBraces` turned the one real call site,

```c
if (!extension || !markdown_core_parser_attach_syntax_extension(parser, extension))
    return 0;
```

into the braced form, and the `{` then landed between the call and the next `;`
— so the audit classified D15's single attach site as a definition and found
zero call sites. It is repaired by reading the **parentheses** instead: balance
from the `(` that opens the argument list, and if the next non-space character
after the closing `)` is `{`, it is a definition. That is immune to where the
braces sit.

**The interesting half is that it failed rather than passed.** 0a.11 wrote the
clause *"no call … at all — this audit is reading the wrong tree"* as a
belt-and-braces line; it is the only reason this did not land as a gate that
silently stopped watching. A source-scanning audit without a saw-nothing
assertion is one formatting change away from being vacuous, and nothing else
would have said so. The repaired audit was re-proved against three mutants, in
**both** brace spellings:

| mutant | result |
|---|---|
| a second attach site in `core/blocks.c`, **braced** call | FAILED — names the function, cites D15 |
| the same site, **braceless** call | FAILED — identically |
| `CORE_EXTENSIONS[]` reordered to put `table` first | FAILED — *"must end with `table` (Q9)"* |

`scripts/audit-extension-special-chars.mjs` is the other audit that parses C
bodies; it was re-proved too (undispatch `'$'` in `formula.c` → red) and is
unaffected, because it delimits a function body by `\n}\n` at column zero and
an inserted brace is always indented.

##### Three C sources the gate was not reading, and they were not fixpoints either

The requirement says *"every C source **the build compiles**"*. `format-c.sh`
searched `packages/markdown-core` only, and four tracked C sources live outside
it: `packages/es-markdown-core/src/bridge.c`, compiled by
`packages/es-markdown-core/scripts/build.mjs:76`, and the three files under
`packages/kotlin-markdown-core/src/native/`, compiled by the Kotlin cinterop and
by `android-runtime/src/main/cpp/CMakeLists.txt:41`. **Three of the four were
not fixpoints of even the OLD config** — 53 lines of column-limit drift, dating
from whenever they were last formatted by hand. No gate could see it, so the
requirement's own sentence was false at HEAD for a reason that has nothing to do
with braces.

The `find` now names the two extra roots explicitly rather than searching
`packages/`, which would walk `node_modules` and every build output. The scope
extension is load-bearing, and the mutant says so: a braceless body in
`markdown_core_kotlin_jni.c` is **red** with the extension and **green**
without it.

##### Neutrality: 83/83 objects byte-identical

Q11 asked for *normalized-disassembly equality, measured 29/29 objects
identical*. The measurement is both bigger and stronger than that. Under the
`default` (Release) preset, **all 83 objects are byte-identical** before and
after — not merely equal after normalization, equal as files. `assert` is
compiled out at Release and no formatted source contains `__LINE__` or
`__FILE__`, so there is nothing left for a moved line to change.

Under `asan` and `ubsan` the objects necessarily differ, and the difference is
exactly characterised: comparing normalized disassembly over all 74 objects of
each build gives **52 identical / 22 differing** (`asan`) and **50 / 24**
(`ubsan`), and every differing object differs only in one-line substitutions —
**82 under `asan`, 58 under `ubsan`, and zero added or removed instructions in
either.** Every substituted line is a `mov w2, #IMM`: the third argument of
`__assert_rtn(func, file, line, expr)`, which is the line number. An assert
moved down three lines reports three lines lower.

The tool is **`scripts/audit-format-neutrality.sh <rev>`**, which builds `<rev>`
in a detached worktree and the working tree side by side with identical flags
and compares every object. It is deliberately **not** a standing gate and is not
in §0's list: it needs a `<rev>` to compare against, and at any commit where the
tree is already a fixpoint there is nothing to compare — so it refuses (exit 2)
rather than reporting success, and it can never pass vacuously. The standing
gate is `scripts/format-c.sh --check`.

##### The mutant that kills nothing, and it is the config line itself

Reverting `InsertBraces: true` — deleting the line this step exists to add —
leaves **every gate green**, because `RemoveBracesLLVM` is `false` and
`clang-format` will not take braces away. The braced tree is a fixpoint of both
configurations.

So the config line buys nothing about the code that is here; it buys the code
that is *not* here yet. The mutant that does kill is the one the step is for:

| mutant | `format-c.sh --check` |
|---|---|
| a braceless `if` body in `core/arena.c`, config as landed | **exit 1**, naming the two lines |
| the same braceless body, `InsertBraces` line deleted | exit 0 |
| a braceless `if` body in `markdown_core_kotlin_jni.c`, scope as landed | **exit 1** |
| the same, `format-c.sh` scope reverted to `packages/markdown-core` | exit 0 |

Two of those four rows are green, and both are green because the *gate* was
weakened rather than the code. That is the whole argument for the step: from
here, "add one line inside a conditional" and "change the control flow" cannot
look like the same edit in review, and the tree cannot drift back without the
gate saying so.

##### Gates after

`correctness` **68/68** · `correctness-asan` **59/59** · `correctness-ubsan`
**59/59** · `conformance` 2/2 · upstream parity **817/817** with 7/7 · mdast
**54/54**, backlog **24/24** · fuzz-parity 300/300 · scope-sanity 14 · position
oracles 0 / 45 / 109 · reference-order 2 rows, still red · canonical-ast
28/47/6 · public surface · special chars · attach order · plan graph 22/45 ·
topology · format-c · format-cmake. **Zero golden rows moved, and no fixture,
spec or golden file is touched at all** — which is what a formatting step should
move, and the neutrality measurement is why it is a fact rather than a hope.

##### A fifteenth stale number, found the same way as the other fourteen

§0's state table said *"Engine | byte-identical to `580d10c` … **except**
`core/main.c`"*. It has not been true since 0a.2. `git diff --shortstat 580d10c`
over `core/` + `extensions/` + `include/` prints **27 files, +1,868 / −712**;
Stage 0a's share alone is 16 files and +771 / −165. The row described the tree
the plan was written against and nobody re-ran the command. It is corrected in
place, with both halves stated so the formatting share is separable from the
defect share.

##### What it left

`node scripts/audit-source-lists.mjs` still **throws** at HEAD on a missing
`packages/swift-markdown-core/Package.release.swift`. It is Step 3a's stated
prerequisite, not Step 2's, and it is triaged there.

---

#### 4.14.3a Step 3a: one allocator model

§4.1's row is the arena and the CLI; §4.13.7 adds **A1**, **A3** and **A4** to
the same step, and §4.13.11 adds **D27**. They land as sub-steps, one commit
each, in the order below.

##### 3a.1 — A2: the arena is gone, and its only live bug was a leak in a parser that never asked for it

**What was deleted.** `core/arena.c` (96 lines), the two declarations in
`core/markdown-core.h` (`markdown_core_get_arena_mem_allocator`,
`markdown_core_arena_reset`), the two in `core/markdown-core-extension-api.h`
(`markdown_core_arena_push`, `_pop`), both push/pop pairs and the re-parse retry
in `extensions/table.c`, and both `#if DEBUG` blocks in `core/main.c`. Four of
the five source lists lose `core/arena.c`; the fifth is the registered absence.
**27 sources where there were 28.**

**The witness, reproduced before anything was changed.** Q12 records *"a
demonstrated 480-byte leak in a parser that never asked for it"*. It reproduces
exactly, and the mechanism is worth stating because it is the whole argument
against a process-global allocator:

```
$ arena_probe                 # one default-allocator parse of a table
Process: 0 leaks for 0 total leaked bytes.
$ arena_probe --arena-first   # an unrelated arena parse first, then the same parse
Process: 12 leaks for 480 total leaked bytes.
```

Both leak roots are `row_from_string` under `try_opening_table_block`, **in the
default-allocator parse**. `markdown_core_arena_pop()` answers *"the arena
freed your rows, build them again"* whenever the global `A` is non-NULL — and
`A` is non-NULL because some *other* parser, possibly in another library, used
the arena once. The retry then overwrote `delimiter_row` and `header_row`
without freeing the pair it had just built: 240 bytes each. A parser that never
named the arena paid for it.

After the deletion the probe does not compile —
*"call to undeclared function `markdown_core_get_arena_mem_allocator`"* — which
is the strongest form the fix can take.

**R14, discharged here rather than at Step 3.** D8 turned **six** wrong declines
in `try_opening_table_header` from `return parent_container` into `return NULL`.
The retry held one of the six, so **five** remain and §2's *"eleven"* becomes
ten. The line is gone and the property is not: `extensions-conflicts.txt` is
4/4 and is what re-proves it. The comment above the function now says so, in
the file, where the next reader of that `return NULL` count will be.

**Output neutrality, measured on the binary that changed.** The Release CLI is
the only thing in the repository whose allocator moved, so the corpus was run
through **both CLIs**: 817 fixture examples × 3 profiles (`default`, `gfm`,
`gfm-extended`) = **2,451 comparisons, 0 differences**.

**What it costs, and Q12's number is the bottom of a range.** Q12 says
*"~7% CLI-only parse win"*. Measured here with the parse and the teardown timed
separately, against the same tree built both ways:

| workload | parse, arena | parse, calloc | delta | teardown the arena never did |
|---|---|---|---|---|
| 995 KB of plain paragraphs | 1.20 ms | 1.27 ms | **+5.8%** | +0.13 ms |
| 50 KB of cmark's `benchmarks.md`, ×40 | 0.44 ms | 0.54 ms | **+22.7%** | +0.07 ms |
| 690 KB of this repository's benchmark samples | 4.88 ms | 7.30 ms | **+49.6%** | +0.92 ms |

The spread is allocation density, and ~7% is the prose end of it. Whole-CLI
wall time on the 690 KB input moves 20.17 → 23.95 ms median, **+18.8%**. Peak
RSS moves the other way, as Q12 says it should: **−3.5%** at 92 KB and
**−8.6%** at 690 KB.

**None of that reopens Q12**, and the reason is §4.13.10's rather than
performance: `alloc_arena_chunk` calls `abort()` on both of its allocation
failures, `arena_calloc` and `arena_realloc` have no failure return at all, and
an abort is the one outcome the append contract exists to make impossible. A
19% CLI regression is a price; an `abort()` inside a library is a broken
contract. The cost is recorded because the doc's number was wrong by up to
seven times, not because the decision is in question.

**One thing found and deliberately not fixed.** `core/syntax_extension.c:10`
holds `static markdown_core_mem *_mem = &MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR;`
— a second allocator selection, hidden, so an extension's own struct never
comes from the parser's `mem`. **Nothing in the engine assigns it**, so it is
not mutable state in practice, and Step 3 deletes it outright by making an
extension a `static const` descriptor that allocates nothing. Recorded so it is
not re-found. `core/node.c`'s `enable_safety_checks` (3b),
`core/registry.c`'s `syntax_extensions` list and `extensions/table.c`'s
runtime-assigned `__TABLE_VISITED` flag bit (both Step 3) are the other three
file-scope mutables in the library, and all three already have owners.

**Gates after.** `correctness` **68/68** · `correctness-asan` **59/59** ·
`correctness-ubsan` **59/59** · `conformance` 2/2 · upstream parity **817/817**
with 7/7 · mdast **54/54**, backlog **24/24** · fuzz-parity 300/300 ·
scope-sanity 14 · position oracles 0 / 45 / 109 · reference-order 2 rows, still
red · canonical-ast 28/47/6 · public surface · special chars · attach order ·
plan graph 22/45 · **source lists 27 sources, 4 of 5** · topology · format-c ·
format-cmake. **Zero golden rows moved.** `leaks --atExit` on the CLI reads 0
before and 0 after: the arena CLI did not leak, it simply never freed anything
that a leak checker could still see a root for.

##### 3a.2 — A4: an int32 sum, and the guard the hardware answered about the wrong number

**The defect, reproduced by direct call before anything changed.** `bufsize_t`
is `int32_t`. Every append went through
`S_strbuf_grow_by(buf, add)` → `markdown_core_strbuf_grow(buf, buf->size + add)`,
and two things were wrong, either of which is enough:

- the sum is **undefined behaviour** once it passes `INT32_MAX`, and it wraps
  **negative**;
- `markdown_core_strbuf_grow` answered a negative target with *"already big
  enough"* — its `assert(target_size > 0)` compiles out under `NDEBUG`, and
  `target_size < buf->asize` is then true for every negative number.

`put` then `memmove`d `add` bytes into a buffer that had not grown. Measured:

```
grow(-1):     oom=0 asize=0 size=0          # accepted, silently, nothing poisoned
put(len = INT32_MAX/2 + 10) on size = INT32_MAX/2
                                            # SIGSEGV, status 139
```

The forged `size` is the largest a legitimate buffer may hold — the cap is
`INT32_MAX/2` — so this is the state one 1.07 GiB line reaches through
`markdown_core_parser_feed`'s `linebuf`, and the put is the next chunk. It is
forged in the gate rather than fed because feeding it costs 2 GiB.

**The fix is two guards, and both are needed.** `markdown_core_strbuf_grow`
poisons on a non-positive target instead of asserting, and `S_strbuf_grow_by`
tests `add` against the room the cap leaves **before** adding, so the sum never
happens. No legitimate caller is affected: the five `grow` call sites all pass a
strictly positive target, and for every non-overflowing sum the new test and the
old cap test agree exactly.

##### The half that surprised: the surviving guard did not save it, and the reason is in the machine code

The obvious prediction is that guard (a) alone suffices — the sum wraps
negative, `grow` sees `target_size <= 0`, poisons, and nothing is written.
**Measured, that is false: with only (a) in place the Release build still
SIGSEGVs.** The disassembly of `markdown_core_strbuf_put` says why, and it is
not "the optimizer deleted the check":

```
53c: adds  w8, w8, w2          ; w8 = size + len, flags set
540: b.le  <poison>            ; the `target_size <= 0` guard
...
560: cmp   w8, w9              ; the `target_size < buf->asize` test
564: b.ge  <grow>
```

`adds` sets **V** on signed overflow, and AArch64's `LE` is `Z==1 || N!=V`. With
`V=1` and `N=1` the condition is **false**, so the guard answers about the true
33-bit sum — 2,147,483,656, which is not ≤ 0 — and does not fire. Four
instructions later `cmp w8, w9` reads the same `w8` as an ordinary negative
integer and concludes the buffer is already big enough. **One expression, read
two different ways in one function**, which is what undefined behaviour buys.
A reduced 15-line demonstration does *not* reproduce it, because with `grow`
marked `noinline` the compiler must materialise the wrapped value and compare
it: the bug needs the inlining that the real code has.

##### A gate that printed the error and passed anyway

With only guard (b) reverted, `correctness` SIGSEGVs — but the interesting
reading is the sanitizer. `correctness-ubsan` reported

```
core/buffer.c:41:46: runtime error: signed integer overflow:
    1073741823 + 1073741833 cannot be represented in type 'bufsize_t'
```

**and read 59/59 green.** UBSan is recoverable by default: it prints and
continues, `ctest` shows output only on failure, so the message was never even
displayed. Every undefined-behaviour finding in this repository's history could
have been reported and discarded the same way.

The fix is one line in `CMakePresets.json` — `UBSAN_OPTIONS =
halt_on_error=1:print_stacktrace=1` on the `correctness-ubsan` test preset — and
it is **measured on both sides**: the whole suite is 59/59 clean with it, and
the same mutant now reads **58/59** with the overflow named in the failure
output. CI runs this preset (`.github/workflows/ci.yml:976`), so the change
reaches it. Nothing else in the repository was relying on a UBSan report being
survivable.

**Mutant kills**

| mutant | result |
|---|---|
| restore `assert(target_size > 0)` in place of grow's poison | `correctness` **67/68**; `api_engine` fails two assertions |
| delete `S_strbuf_grow_by`'s pre-sum test | `correctness` **SIGSEGV**; `correctness-ubsan` **58/59** naming the overflow |
| the same, before the preset carried `halt_on_error` | `correctness-ubsan` **59/59 green**, with the error printed and swallowed |

**Gates after.** `correctness` **68/68** (the api test gains four assertions,
not a ctest entry) · `correctness-asan` **59/59** · `correctness-ubsan`
**59/59**, now halting on error · `conformance` 2/2 · upstream parity 817/817
with 7/7 · mdast 54/54, backlog 24/24 · fuzz-parity 300/300 · scope-sanity 14 ·
position oracles 0 / 45 / 109 · reference-order 2 rows, still red ·
canonical-ast 28/47/6 · public surface · special chars · attach order · plan
graph 22/45 · source lists 27, 4 of 5 · topology · format-c · format-cmake.
**Zero golden rows moved.**

##### 3a.3 — A1 and D27: the failure model, and the buffer the allocation sweep could not reach

**D27, reproduced before anything changed.** `parser->linebuf.oom` was written
at six sites and read at **none**. A refused growth makes
`markdown_core_strbuf_put` a no-op, and `S_parser_feed` then hands the
accumulated **prefix** to `S_process_line` as though it were a whole line and
commits it — with `parser->oom` clear, so `finish` returns a document. Measured
on a 279-byte document fed in 32-byte chunks: refusing allocation **6 of 25**
leaves **55 of 275** text bytes and reports success; refusing 7 leaves 80.
§4.13.11's own numbers were 244 → 102 on a different input; the shape is
identical.

**The fix is a reservation and a test.** `S_linebuf_reserve` grows the held
partial line to its full new size in **one** call and then reads the flag: a
refusal sets `parser->oom` and `S_parser_feed` returns without processing
anything. `markdown_core_parser_finish` tests it too, before processing the
held final line — what is in the buffer after a refusal is a prefix, and
committing it would invent a line the author did not write. The arithmetic is
64-bit, because `linebuf.size + chunk_len` is exactly the int32 overflow 3a.2
closed one level down, and the NUL-replacement path is the one that writes
twice, so reserving first is what makes the refusal atomic rather than partial.

**A1: `markdown_core_strbuf_clear` lifts the failure bit.** An allocation
failure is a fact about the write that failed, not a property the buffer keeps.
`oom` says *"content was lost"*; after a clear there is no content, so there is
nothing left for it to say. Before this, `markdown_core_strbuf_detach` was the
**only** operation in the engine that lifted it — which is why
`consolidate_text_nodes` recovers per run and a cleared-and-reused buffer did
not.

##### The mutant that kills nothing, and the gate it earned

**Reverting A1's lift alone leaves `correctness` at 69/69 and both
allocation-failure sweeps green.** The two engine buffers that are cleared and
reused are `parser->curline` — whose `oom` was already tested at
`S_process_line`'s head — and `parser->linebuf`, which this same commit taught
to report. With both reporting at the transaction, the poison never survives to
be reused, so the lift removes the class **by construction** and no parse can
show it.

So it gets a property test rather than a parse test:
`strbuf_failure_is_a_transaction` in `tests/api/main.c` refuses one growth
through a test allocator, asserts the poison, clears, and asserts that the next
write lands intact with the allocator working again. Reverting the lift fails
**four** of its assertions.

##### The chunked sweep, and why the old one was structurally blind

§4.13.11 said it and it is exactly right: `case_oom_sweep` feeds its corpus in
**one call**, so `parser->linebuf.size` is 0 at every decision, the branch that
accumulates a partial line never runs, and the buffer is never written during
the entire sweep. A gate that injects a failure at **every single allocation**
could not see six unread flag writes.

`oom_sweep_chunked` is the same contract over the same corpus fed **7 bytes at a
time** — every line split, most of them more than once — and its control is
still the one-call parse, so it also asserts that chunking the same bytes builds
the same document. It costs **433** injected failures where the one-call sweep
costs 429.

| mutant | `oom_sweep` | `oom_sweep_chunked` |
|---|---|---|
| drop D27's flag test, keep the hoist | **PASSED** | **FAILED** — *allocation 19 / 433 (chunk 7): lossy document reported as success*, `Paragraph with ` → `Paragraph w` |
| revert A1's lift | PASSED | PASSED — and `correctness` 69/69; see above |

##### A3 is half already true, and its other half is not 3a-shaped

§4.13.7 assigns A3 to *"3a, surfaced by 13"*: *"`parser->oom` stops being one
sticky bit meaning four things"*. **Measured: it means one thing.** There are 70
write sites across `core/` and `extensions/` and exactly **three** reads
(`core/blocks.c:1762`, `:1818`, `:1894`), and all three mean the same thing —
*the document lost bytes, abandon*. The "four things" is a description of the
session/streaming era's parser, which no longer exists; §11.8's split was
written against that engine.

What is left of A3 is *"a failure is a **returned status**"*, and 3a cannot land
it: `markdown_core_parser_finish` reports loss by freeing the root and returning
NULL because NULL is the only vocabulary the surface has. Giving it another one
is `markdown_core_error` and the diagnostics contract — **Step 13**, with the
facade half at **12**. A3 is carried, and the owner column is corrected to say
so rather than to say 3a.

##### Gates after

`correctness` **69/69** · `correctness-asan` **60/60** · `correctness-ubsan`
**60/60** — each **+1**, the new `regression_fallback_oom_sweep_chunked` entry;
the two new api-test functions add assertions, not entries · `conformance` 2/2 ·
upstream parity 817/817 with 7/7 · mdast 54/54, backlog 24/24 · fuzz-parity
300/300 · scope-sanity 14 · position oracles 0 / 45 / 109 · reference-order 2
rows, still red · canonical-ast 28/47/6 · public surface · special chars ·
attach order · plan graph 22/45 · source lists 27, 4 of 5 · topology · format-c
· format-cmake. **Zero golden rows moved**, which is what an
allocation-failure fix should move.


---

#### 4.14.3 Step 3: the extension descriptor

§4.1's row is one requirement with six clauses, and they are independent
enough to land and gate separately. Each sub-step below is one commit.

##### 3.1 — Q16: node types and the one node flag are compile-time constants

**What they were.** Nine `markdown_core_node_type` globals, zero-initialised
and filled in at first use by `markdown_core_syntax_extension_add_node`, which
post-increments one of two process-global counters
(`MARKDOWN_CORE_NODE_LAST_BLOCK` / `_LAST_INLINE`) — so a node type's numeric
identity was a consequence of the order in which `core_extensions_registration`
happened to call the six `create_*` functions, in a different file, and
**nothing in the repository asserted a single one of the nine values.** The one
extension node flag was worse: `markdown_core_register_node_flag` hands out
bits in call order and `abort()`s if the same global is registered twice.

**What they are.** One fixed `enum` in
`extensions/markdown-core-extensions.h`, and one file-scope `enum` in
`extensions/table.c` for the flag. `markdown_core_syntax_extension_add_node`,
`markdown_core_register_node_flag`, `MARKDOWN_CORE_NODE_LAST_BLOCK`,
`MARKDOWN_CORE_NODE_LAST_INLINE` and the long-dead
`markdown_core_init_standard_node_flags` are deleted;
`MARKDOWN_CORE_NODE__REGISTER_FIRST` becomes `_EXTENSION_FIRST`, because
nothing registers any more.

**The values are exactly what the allocator produced**, measured on the tree
before the change and again after:

| | | | |
|---|---|---|---|
| `TABLE` `0x800b` | `TABLE_ROW` `0x800c` | `TABLE_CELL` `0x800d` | `FORMULA_BLOCK` `0x800e` |
| `DIRECTIVE_BLOCK` `0x800f` | `STRIKETHROUGH` `0xc00b` | `FORMULA` `0xc00c` | `DIRECTIVE` `0xc00d` |
| `DIRECTIVE_LABEL` `0xc00e` | | | |

Renumbering was available — nothing outside the library can see a value, since
the export map is 32 facade functions and `local: *` — and was declined, so
that this commit is a structural change and nothing else.

**The gate is the test that was already there, extended.**
`node_type_values` in `tests/api/main.c` asserted that the core block and
inline types are contiguous from 1. The nine extension types *continue those two
sequences*, so appending them to the same two arrays makes the existing
assertions pin every value and makes a collision or a gap impossible, with no
new logic.

| mutant | result |
|---|---|
| give `FORMULA` `DIRECTIVE`'s value | **4 failures** — `api_engine` and three fixture suites |
| move `TABLE_CELL` out of the contiguous run to `0x8020` | **1 failure — `api_engine` alone** |

The second row is the case for the gate: a value that collides shows up in the
goldens because `get_type_string` answers with the wrong name, but a value that
merely *moves* is invisible to every fixture in the repository, and the
contiguity assertion is the only thing that sees it.

**Gates after.** `correctness` **69/69** · `correctness-asan` **60/60** ·
`correctness-ubsan` **60/60** · `conformance` 2/2 · upstream parity 817/817 with
7/7 · mdast 54/54, backlog 24/24 · fuzz-parity 300/300 · scope-sanity 14 ·
position oracles 0 / 45 / 109 · reference-order 2 rows, still red ·
canonical-ast 28/47/6 · public surface · special chars · attach order · plan
graph 22/45 · source lists 27, 4 of 5 · topology · format-c · format-cmake.
**Zero golden rows moved.**


##### 3.2 — three byte sets, and the audit that stopped reading

**One list, five readers, five meanings.** `special_inline_chars` was a single
`markdown_core_llist` on the descriptor, and §4.1.3 counted the consumers:
`markdown_core_manage_extensions_special_characters` folded it into two byte
tables, `try_extensions` used it for cursor dispatch,
`get_extension_for_special_char` used it for delimiter-tag **ownership**,
`bracket_takes_close_bracket` used it for `]` arbitration, and
`handle_backslash` used it to decide whether a core fast path was safe. Whether
it also fed the *second* byte table was one `emphasis` bool covering **every**
byte the extension named — which is D1, and `'}'` sitting in the list
dispatching to nothing is D2.

**Three sets now, declared in one call.**

| set | question | folded into |
|---|---|---|
| `terminates_text` | does this byte end a text run? | `parser->special_chars` |
| `dispatch` | is this byte offered to `match_inline`? | asked directly; also answers ownership and `]` arbitration |
| `flanking_transparent` | does `scan_delims` look through it? | `parser->skip_chars` |

`markdown_core_syntax_extension_set_special_inline_chars` and
`_set_emphasis` are deleted, and so is the `emphasis` bool.
`markdown_core_inlines_add_special_character(parser, c, emphasis)` becomes four
functions, one per table and direction, because the two folds are now
independent facts rather than one call with a flag.

What each extension declares, printed by the audit:

```
autolink.c:      terminates=':' 'w'                dispatch=':' 'w'                    transparent=(none)
directive.c:     terminates=':' 0x08               dispatch=':' ']' 0x08               transparent=(none)
formula.c:       terminates='$' 0x01..0x04         dispatch='$' '\' 0x01..0x04         transparent=(none)
strikethrough.c: terminates='~'                    dispatch='~'                        transparent='~'
table.c, tasklist.c: block-only, all three empty
```

**Behaviour-neutral, and the two asymmetries are why.** `\` leaves formula's
terminator set and `]` leaves directive's, because
`is_core_special_character` refused both there already — the old single list
passed them to a function that dropped them on the floor. Both stay in
`dispatch`, where they are load-bearing: `handle_backslash` asks whether any
extension owns `\`, and `bracket_takes_close_bracket` asks whether any owns
`]`. Zero golden rows move.

##### The audit stopped reading, and said nothing

`scripts/audit-extension-special-chars.mjs` parsed the run of
`markdown_core_llist_append` calls. With the declaration replaced by one
`set_byte_sets` call its reader matched nothing, `registered.length === 0` hit a
`continue` for all four extensions, and it printed

```
Extension special characters: every registered byte is dispatched or is a sentinel.
```

**with an empty report and exit 0.** It is the same failure 3.1's sibling audit
avoided by asserting that it saw *something* — and this one had no such
assertion, so a refactor turned a gate into a sentence.

It now reads the new declaration, prints all three sets for all six extensions,
and asserts three things instead of one. The two new ones are laws relating the
sets, and each is one of the defects made **unexpressible**:

| law | the defect it forbids |
|---|---|
| `terminates_text ⊆ dispatch` | **D2** — a byte that ends a text run and is dispatched to nobody |
| `flanking_transparent ⊆ dispatch` | **D1** — a byte an extension makes invisible to `scan_delims` without owning it |
| every dispatch byte ≥ 0x20 is compared in `match_inline` | the original law |

And every `create_*_extension` in the directory must be found **and must
declare**, even the two block-only ones, so that a missing call always means the
reader is broken and never means "empty". That requirement caught `table` and
`tasklist` on the first run.

| mutant | result |
|---|---|
| `'}'` in `terminates_text` only | *`'}'` ends a text run and is in no dispatch set. That split is pure cost (D2).* |
| `'$'` in `flanking_transparent` only | *`'$'` is flanking-transparent and the extension does not own it. That is D1.* |
| `'}'` in `dispatch` | *declares `'}'` in its dispatch set, and its match_inline never dispatches on it* |
| delete an extension's `set_byte_sets` call | *defines a create_\*_extension and no set_byte_sets call. This audit cannot see it.* |

##### What it left

The sentinels are still in two sets. `FORMULA_DELIM_*` (0x01–0x04) and
`DIRECTIVE_LABEL_DELIM` (0x08) are **delimiter tags, not source**, and they are
declared only because `get_extension_for_special_char` derives a delimiter's
owner from its `delim_char`. A literal 0x01 in user text therefore still splits
a text run and still dispatches to `formula`. §4.1.3 is right that only removing
the concept closes it, and that is 3.3: a delimiter names its **rule**.

**Gates after.** `correctness` **69/69** · `correctness-asan` **60/60** ·
`correctness-ubsan` **60/60** · `conformance` 2/2 · upstream parity 817/817 with
7/7 · mdast 54/54, backlog 24/24 · fuzz-parity 300/300 · scope-sanity 14 ·
position oracles 0 / 45 / 109 · reference-order 2 rows, still red ·
canonical-ast 28/47/6 · public surface · special chars (now reporting six
extensions and three sets each) · attach order · plan graph 22/45 · source
lists 27, 4 of 5 · topology · format-c · format-cmake. **Zero golden rows
moved.**


##### 3.3 — a delimiter names its rule, and D33: the loop that did not advance

**The byte was answering three questions.** A `delimiter` carried an
`unsigned char delim_char`, and `process_emphasis` derived three different
things from it:

| from the byte | what it decided | what was wrong |
|---|---|---|
| `get_extension_for_special_char(parser, delim_char)` | **who owns it** | the first attached extension whose dispatch set contains the byte — i.e. attach order when two claim one (`autolink` and `directive` both claim `:`), and **NULL** when none does |
| `opener->delim_char == closer->delim_char` | **which opener matches** | `formula` needed four sentinel BYTES (0x01–0x04) to keep `$x$` from matching `$$x$$`, and `directive` a fifth (0x08) |
| `openers_bottom[length % 3][delim_char]` | **where the opener memo lives** | declared `[3][128]`, indexed by a byte the **public** push accepts unconstrained; `openers_bottom[2][200]` is offset 456 into 384 elements |

A dense `markdown_core_delimiter_rule` answers all three: eleven rules, four
core and seven from the three extensions that push delimiters; the owning
extension is a pointer **on** the delimiter; matching is `opener->rule ==
closer->rule`; and the memo is `[3][MARKDOWN_CORE_DELIM_RULE_COUNT]`. All five
sentinel bytes are gone — no byte below 0x20 is special anywhere in the engine.

##### D33 — a new defect, and it is the sharpest thing this step found

The arm chain read

```c
if (extension)                                    ... else
if (delim_char == '*' || delim_char == '_')       ... else
if (delim_char == '\'' || delim_char == '"')       ...
```

and **nothing followed it**. A delimiter matching none of the three left
`closer` exactly where it was, fell into the `!opener_found` removal below,
freed it, and read it again on the next turn of the loop.

- With `can_close` only: **ASan `heap-use-after-free`, READ of size 8 in
  `process_emphasis`.**
- With `can_open` set: nothing frees it and **the loop never ends.** That was
  measured first, by accident, as a probe that would not terminate.

The unreachable case is "a byte whose owner cannot be found", which is exactly
what byte-keyed ownership produces. No in-tree extension reaches it, because
each pushes a tag it also declares — but the push is **public** and takes the
byte from the caller, so it is one call away, and §4.1.3 predicted a *NULL
dispatch* here without noticing that the fall-through is worse. **D33 is
recorded in §2 with its witness.**

It is closed twice over: the owner comes off the delimiter so the lookup cannot
fail, and the chain gains a final `else` that advances. The second is the one
that matters — "unreachable by construction" through three `else if`s is
reintroduced by the next rule someone adds without a handler.

And `push_delimiter` now refuses a rule outside the enum. The parameter is
typed, but C converts anything to an enum and the push is public, so the bound
that sizes `openers_bottom` is enforced rather than assumed; an unnamed rule is
not a delimiter and its text node stays as ordinary text.

##### Gate

`stray_delimiter` in `tests/api/main.c` builds an extension that pushes a
delimiter **no one can handle** — once with a real rule and a NULL owner, once
with a rule outside the enum — and asserts the parse finishes. It is a
contract violation on purpose: the point of D33 is that the engine must survive
a misbehaving extension, because the public push cannot stop one.

| mutant | result |
|---|---|
| delete the final `else` arm | `correctness` **68/69, api_engine SegFault**; `correctness-asan` **59/60**, *heap-use-after-free at `inlines.c:856` in `process_emphasis`* |
| delete `push_delimiter`'s rule bound | `correctness-asan` **59/60**, *stack-use-after-scope at `inlines.c:908` in `process_emphasis`* |
| put `0x01` back in a byte set | the special-chars audit: *declares 0x01, a control byte* |

The special-chars audit's sentinel **exemption** became a **prohibition**: a
byte below 0x20 in any of the three sets is now a failure, because there is no
longer any such thing as a delimiter tag that is also a byte.

##### Output-neutral, and that is measured rather than hoped

Removing the sentinels changes what a literal 0x01 in a document does — it no
longer ends a text run and is no longer offered to `formula`'s inline hook —
and **no golden moves**, because `markdown_core_consolidate_text_nodes` merges
the split run back before any consumer sees the tree. Checked directly before
the change: `a␁b` through the CLI at `--profile gfm-extended` gives one
`Text literal="a\u0001b"` either way. The cost the split represented was real
and invisible; that is the same reason D2 had no output signature.

**Gates after.** `correctness` **69/69** · `correctness-asan` **60/60** ·
`correctness-ubsan` **60/60** · `conformance` 2/2 · upstream parity 817/817 with
7/7 · mdast 54/54, backlog 24/24 · fuzz-parity 300/300 · scope-sanity 14 ·
position oracles 0 / 45 / 109 · reference-order 2 rows, still red ·
canonical-ast 28/47/6 · public surface · special chars · attach order · plan
graph 22/45 · source lists 27, 4 of 5 · topology · format-c · format-cmake.
**Zero golden rows moved, and no fixture, spec or golden file touched.**


##### 3.4 — the descriptor is a `static const` object in a fixed table, and seven files are gone

**What an extension was.** A heap object built at run time by
`markdown_core_syntax_extension_new` and sixteen setters, allocated from a
hidden process-global allocator (`core/syntax_extension.c`'s `_mem`), handed to
a process-global registry keyed by NAME (`core/registry.c`) through a
process-global plugin object (`core/plugin.c`), filled in behind a
process-global once-flag (`core/once.c`), and reached by string lookup.

**What it is.** One `static const markdown_core_syntax_extension` per
extension, in `.rodata`, named by a symbol, referenced by a fixed table in
`extensions/core-extensions.c`. **Every hook takes a `const` descriptor**, so
"carries no mutable state" is a fact the compiler checks rather than a
convention — 94 declarations across the tree changed to say so.

**Deleted outright**, and each because the thing it served no longer exists:

| file | what it was |
|---|---|
| `core/registry.c` + `.h` | the process-global list and `markdown_core_find_syntax_extension` |
| `core/plugin.c` + `.h` | the plugin object registration went through |
| `core/syntax_extension.c` | `_new`, `_free`, sixteen setters, and a hidden `_mem` global |
| `core/once.c` + `.h` | the once-flag that guarded registration, and nothing else |

Also gone: `markdown_core_register_plugin`,
`markdown_core_list_syntax_extensions`,
`markdown_core_core_extensions_ensure_registered` and its eight call sites, and
`priv` / `free_function` — a private-state pair **no extension in this
repository ever used**. **23 source files where there were 28** before Step 3.

**Nobody looks anything up by name any more.** A literal name becomes the
descriptor symbol; the three harnesses that take extension names from *data*
(both fuzzers and the sweep) map name → **bit** and hand the bit set to
`markdown_core_core_extensions_attach`, which is the one thing that turns a set
into a sequence. That is a real fix and not only tidiness: those loops attached
in the order of their own name list, which is **a second attach order**, and a
second attach order is D15.

**What "cannot express an order" means exactly.** The CLI and the facade — the
two product entry points, and everything every binding goes through — can only
pass a bitmask. `markdown_core_parser_attach_syntax_extension` still exists,
takes a `const` descriptor, and is **not in the export map**, so no consumer of
the shipped library can reach it. A statically-linked test can, deliberately:
3.3's D33 gate has to build an extension that misbehaves, and no real one does.
`audit-extension-attach-order.mjs` is what keeps that honest — it asserts the
**library** has exactly one attach site.

##### Two more audits stopped reading, and both said so

Repairing the audit at 3.2 did not inoculate the others. Both source-scanning
audits went **red** on this commit:

- `audit-extension-special-chars.mjs` — its reader looked for
  `create_*_extension` and a `set_byte_sets` call. Neither exists now. It caught
  itself only because 3.2 had given it a saw-nothing assertion; **that
  assertion is the reason this is a failure and not a silent pass**, and it is
  now stronger: it counts the descriptors it read and requires **six**, so
  reading five is as loud as reading none.
- `audit-extension-attach-order.mjs` — the table used to hold `"name"` strings
  and the pairing was against `markdown_core_plugin_register_syntax_extension`
  calls. It now reads `&MARKDOWN_CORE_EXTENSION_*` out of the table and pairs it
  against the descriptors **defined** in `extensions/*.c`, which is the same
  law the other way round: an extension cannot become attachable without being
  given a position.

| mutant | result |
|---|---|
| move `table` to the front of `CORE_EXTENSIONS[]` | *must end with `table` (Q9); it ends with `directive`* |
| drop `tasklist` from the table | *`tasklist` defines a descriptor and has no place in CORE_EXTENSIONS\[\]* |
| a byte set the reader cannot find | *read N extension descriptors and expected 6* |

**Gates after.** `correctness` **69/69** · `correctness-asan` **60/60** ·
`correctness-ubsan` **60/60** · `conformance` 2/2 · upstream parity 817/817 with
7/7 · mdast 54/54, backlog 24/24 · fuzz-parity 300/300 · scope-sanity 14 ·
position oracles 0 / 45 / 109 · reference-order 2 rows, still red ·
canonical-ast 28/47/6 · public surface · special chars · attach order · plan
graph 22/45 · **source lists 23 sources, 4 of 5** · topology · format-c ·
format-cmake. **Zero golden rows moved, and no fixture, spec or golden file
touched** — by a commit that deletes seven files and changes thirty-six.

##### What Step 3 still owes

One clause of §4.1's row: *"The public `delimiter` struct, annotated 'Exposed
raw for now' since 1.0, is hidden behind accessors."* Thirty field reads across
the three extensions that push delimiters, over eight fields. It is 3.5.


##### 3.5 — the `delimiter` struct is opaque, and Step 3 is done

`markdown-core-extension-api.h` spelled the struct out under the comment
*"Exposed raw for now"* from 1.0 until here, which made **every field part of
the extension surface**. The three extensions that push delimiters read **eight
fields between them and write none**, so the whole exposure bought eight
one-line accessors — and buys back the freedom to change the representation,
which Step 8 needs.

The definition moves to `core/delimiter.h`, private to core and included by
`inlines.c` alone; the extension API keeps `typedef struct delimiter delimiter;`
and eight `const`-taking readers. **37 field accesses across three extensions**
became calls. The opacity is checkable rather than asserted: a translation unit
that includes only the extension API and writes `d->can_open` does not compile —
*"incomplete definition of type 'delimiter'"*.

Step 3's requirement is now true in every clause: a `static const` descriptor
in a fixed compile-time table, no registration, no name lookup, no mutable
state, a bitmask a caller cannot order, three declared byte sets, a delimiter
that names its rule, compile-time node types and flag bits, and no
process-global mutable state anywhere in the extension path.

**Gates after.** `correctness` **69/69** · `correctness-asan` **60/60** ·
`correctness-ubsan` **60/60** · `conformance` 2/2 · upstream parity 817/817 with
7/7 · mdast 54/54, backlog 24/24 · fuzz-parity 300/300 · scope-sanity 14 ·
position oracles 0 / 45 / 109 · reference-order 2 rows, still red ·
canonical-ast 28/47/6 · public surface · special chars · attach order · plan
graph 22/45 · source lists 23, 4 of 5 · topology · format-c · format-cmake.
**Zero golden rows moved.**


---

#### 4.14.3b Step 3b landed: the check the shipped library did not run, and D34

**Q13's suspicion was right, and the witness is one line of C.**
`markdown_core_enable_safety_checks` defaulted to **off** and only the api
test's `main()` ever turned it on, so what shipped answered:

```
-- safety checks OFF (what the shipped library does) --
append_child(q, q)                  returned 1, parent == self: YES
prepend_child(r, r)                 returned 1, parent == self: YES
append_child(a,b)=1 then (b,a)=1 -> a->parent == b: YES (a cycle)

-- safety checks ON (what the test suite does) --
append_child(q, q)                  returned 0
prepend_child(r, r)                 returned 0
append_child(a,b)=1 then (b,a)=0 -> no cycle
```

A library that makes a cycle on request while its own tests deny it is not
testing the library. The flag and its declaration are deleted; the ancestor walk
is unconditional.

##### D34 — a node CAN be its own sibling, and the flag never covered it

Writing the gate found a second hole the requirement names and Q13 does not.
`markdown_core_node_insert_before(node, sibling)` guards with
`S_can_contain(node->parent, sibling)`, and **with `sibling == node` that walk
starts at the parent and never meets the child**, so it answers yes. Measured on
the tree before the fix, with the flag in either position:

```
append_child(a,b)  = 1
insert_before(b,b) = 1
  b->next == b : YES      b->prev == b : YES
  a->first_child == b : yes   a->last_child == b : no
  walking a's children stops after 10 steps (bound 10)
```

An unbounded sibling list, and a parent whose `first_child` and `last_child`
disagree. Any traversal of `a`'s children runs forever. **D34 is recorded in
§2**; the fix is `node == sibling` refused in both `insert_before` and
`insert_after`, and the ancestor walk cannot be made to cover it because the
node is not its own ancestor — it is its own *neighbour*.

##### Cost, measured on this tree rather than repeated

Q13 says *"unmeasurable on four workloads, 10.7% on one already-pathological
path the engine takes 36 seconds to parse"*. The direction is confirmed; the
10.7% is not reproduced by anything here, and no input in this repository takes
36 seconds:

| input | flag off | unconditional | |
|---|---|---|---|
| 2,000-deep blockquote, 4 KB | 8.87 ms | 8.95 ms | **+0.9%** |
| 200 paragraphs 200 deep, 81 KB | 24.68 ms | 25.54 ms | **+3.5%** |
| 690 KB of benchmark samples | 22.80 ms | 22.69 ms | −0.5% |
| 995 KB of plain paragraphs | 6.61 ms | 6.48 ms | −2.0% |

The last two are noise. The walk is O(depth) per link and the parse's depth is
the document's nesting, which is what the two blockquote rows measure.

##### Mutant kills

| mutant | result |
|---|---|
| put the ancestor walk back behind a default-off flag | `api_engine` fails **8** assertions — the six new ones **and `create_tree`'s tests 313 and 314**, which passed only because `main()` set the flag |
| drop the `node == sibling` guard | `api_engine` fails 2: *cannot be inserted before / after itself* |

The first row is worth reading twice: two assertions that were already in the
suite were testing a configuration the product never ran.

**Gates after.** `correctness` **69/69** · `correctness-asan` **60/60** ·
`correctness-ubsan` **60/60** · `conformance` 2/2 · upstream parity 817/817 with
7/7 · mdast 54/54, backlog 24/24 · fuzz-parity 300/300 · scope-sanity 14 ·
position oracles 0 / 45 / 109 · reference-order 2 rows, still red ·
canonical-ast 28/47/6 · public surface · special chars · attach order · plan
graph 22/45 · source lists 23, 4 of 5 · topology · format-c · format-cmake.
**Zero golden rows moved.**


---

#### 4.14.5 Step 5 landed: the event contract is total, and three walks were relying on it not being

**`S_is_leaf` was a LIST, not a property.** Eight node types had their `EXIT`
suppressed, so a `FOOTNOTE_REFERENCE` with no children got an `EXIT` and a
`TEXT` with no children did not, and every walk in the engine had to know
which. It is deleted; every node now yields exactly one `ENTER` and exactly one
`EXIT`.

**The mutation rule names a node, not an event**, and it is now written in the
public header beside `markdown_core_iter_new`: *while walking, the only node
that may be freed is the one whose `EXIT` is current* — that is exactly the
moment the iterator's lookahead names something outside the node's own subtree,
and the only such moment. **Three walks were freeing or splicing at `ENTER`,
and all three were safe only because of the suppression list:**

| walk | what it did at `ENTER` | what it does now |
|---|---|---|
| `markdown_core_consolidate_text_nodes` | merged a `TEXT` run and freed the merged-away nodes and the emptied node | works at `EXIT`; brings each merged-away node to its own `EXIT` before freeing it, then `markdown_core_iter_reset(iter, cur, EXIT)` so the final drop is legal under the rule and the lookahead is recomputed from the survivors |
| `S_strip_html_comments` | freed an `HTML` / `HTML_BLOCK` node | frees at `EXIT` |
| `autolink`'s `postprocess` | spliced new siblings in after a `TEXT` and emptied it | works at `EXIT`, **which is also the behaviour it always had**: the lookahead at a node's `EXIT` is the sibling that followed it before the splice, exactly what a suppressed `EXIT` gave `ENTER`. Doing it at `ENTER` with the contract total makes the walk descend into the autolinks it just created — two fixture examples said so |

The api test's `iterator_delete` was a fourth: it freed `CODE` at `ENTER`, which
is a test asserting the suppression list rather than the contract.

**Gate.** `iterator_contract_is_total` walks a document containing one of every
formerly-suppressed kind — thematic break, HTML block, indented code block,
text, soft break, hard line break, inline code, inline HTML — with a stack, and
asserts four things: every node is entered once, exited once, each `EXIT`
closes the `ENTER` it belongs to, and the walk ends with nothing open.

| mutant | result |
|---|---|
| restore `S_is_leaf` | **12 of 69 suites fail**; `api_engine` fails 12 assertions — the five new ones **and** `iterator_delete`'s and all four `strip-html-comments` assertions, because the EXIT-based frees never fire |

**Zero golden rows moved**, which is the right answer for a change to the
*order* events arrive in and not to what the tree contains.

##### What Step 5 does not close, and one defect it found

- *"No zero-length `Text` node exists in a finished tree"* — **true since
  0a.14**; no golden carries `literal=""` on a `Text`.
- *"A merged run's scope is the union of what it merged, line and column"* —
  **true since 0a.14** (D12).
- *"no node carries `0:0..0:0` as a stand-in"* — ~~**one row left**, the
  split-off table lead's paragraph~~ **TRUE since Step 10** (§4.14.10), which
  gave that paragraph the place its first and last bytes were written. What
  remains in `specs/scope-sanity/ledger.json` is one NEGATIVE row, an empty
  table cell, which is the representation defect and not a sentinel.
- *"One function computes a position from a byte range"* — not landed here.
  **The block half landed at Step 10** as `markdown_core_parser_content_place`,
  and the inline phase reads it through one function; what Step 8 still owns is
  deleting the counters that now consult it. Writing a third one at Step 5 was
  the disease Q22 names, and Q22 is answered.
- The ledger's **11 negative rows** had no owner named anywhere. Reading them
  found that **four of them are one defect**: an HTML block whose terminator is
  on its own opening line ends **one line before it starts**. That is **D35**,
  and §4.14.5a lands it.


#### 4.14.5a D35: an HTML block that ends one line before it starts

**Found by reading the eleven negative rows in
`specs/scope-sanity/ledger.json` for an owner.** They had none — the ledger
names the *representation* defect (a closed interval cannot spell an empty
range) and no step. Ten of the eleven turned out not to be that at all.

`finalize` (`core/blocks.c`) ends a block at `parser->line_number - 1`, which
assumes the block was closed **by a later line**. That is true of every block
that needs a following line to end it, and false of an **HTML block of type 2
to 5** — comment, processing instruction, declaration, CDATA — whose terminator
can be on its own first line. Measured:

```
$ printf 'para\n\n<!-- c -->\n' | markdown-core --profile gfm
└── HTMLBlock scope=3:1..2:0 literal="<!-- c -->\n"
```

The block starts on line 3 and ends on line 2, at column 0 — and
`parser->last_line_length` there is the length of the **blank line before it**.
A thematic break and an ATX heading on the same line are correct, because they
are finalized when the *next* line arrives.

**The fix is one clause**: also take the current line when
`parser->line_number == b->start_line`. The witness becomes `3:1..3:10`, the
last byte of its own literal.

**Ten golden rows moved, and the claim is mechanised.** Every moved row is an
`HTMLBlock`, its literal is unchanged, its start is unchanged, its old end was
strictly before its start, and its new end is on the start line at or after the
start column. A script checks all five for all ten; the eye then reads nothing.
Three of the ten had ended at `0:0` — line zero, which is why the places oracle
counted them separately.

**Two ledgers move with it**, which is the point of having them:

| ledger | before | after |
|---|---|---|
| `specs/scope-sanity/ledger.json` | 14 rows | **4** — and `spec.txt` leaves the ledger entirely |
| `specs/positions/places.json` | 109 rows | **106**, `end-at-line-ending` 61 → 58 |

What is left in scope-sanity is one sentinel (the split-off table lead's
paragraph, Step 10's), **one** negative — `TableCell scope=3:6..3:5`, an empty
cell, which is the representation defect in its pure form — and two partial.
**Step 10 took that 4 to 1**: the sentinel and both partial rows were the same
paragraph and its children, and the negative is all that is left (§4.14.10).

##### Two stale numbers found by counting rather than reading

- `places.json`'s own prose said *"One hundred and nine rows in six families"*
  and then listed `61 + 19 + 9 + 9 + 6 + 2`, **which is 106**. The total was
  right and `continuation-line-content-offset` had been undercounted by three
  since whenever it grew. Both are corrected from the data.
- `--update` on `specs/positions/places.json` also **normalised pre-existing
  escape drift** — `\u2014` written as a literal em dash in three unrelated
  `closedBy` strings — exactly as §0's trap says it would. Those three lines in
  the diff are not movement.

##### Mutant kills

| mutant | result |
|---|---|
| revert the one clause | `correctness` **67/69** (`spec_commonmark` and `extensions_gfm`), `audit-position-places` reports **3 rows APPEARED**, and `audit-scope-sanity` fails its only-shrink rule |

**Gates after.** `correctness` **69/69** · `correctness-asan` **60/60** ·
`correctness-ubsan` **60/60** · `conformance` 2/2 · upstream parity 817/817 with
7/7 · mdast 54/54, backlog 24/24 · fuzz-parity 300/300 · **scope-sanity 4** ·
position oracles 0 / 45 / **106** · reference-order 2 rows, still red ·
canonical-ast 28/47/6 · public surface · special chars · attach order · plan
graph 22/45 · source lists 23, 4 of 5 · topology · format-c · format-cmake.
Neither parity oracle moves: neither compares positions.


##### 15A.2 — Q30: the Swift model's child edges are typed, and `audit-ast-projections` is GREEN

**The drift was one shape repeated sixteen times.** Every Swift kind declared a
flat `children: [any Markup]`, and eleven leaves declared
`children: [any Markup] = []` — a field that is always empty. The contract names
the edge per kind: `content`, `items`, `label`, `header`, `rows`, `cells`. Kotlin
and ES already did; Swift was one binding behind, which is §4.1.2's whole point
about why deferring the bindings is how the drift happened.

| was | is |
|---|---|
| `children: [any Markup]` on twelve kinds | `content: [any Markup]` |
| `children: [any Markup] = []` on nine leaves | **deleted** |
| `List.children` | `List.items: [ListItem]` — a list owns list items and the type now says so |
| `List.isTight`, `ListItem.isChecked` | `tight`, `checked` |
| `CodeBlock.isFenced`, `isClosed` | `fenced`, `closed` |
| `Code`, `CodeBlock` had **no** `mode` | `mode: PlacementMode` |
| `Directive.labelCount: Int?`, `DirectiveBlock.labelCount: Int?` | `label: [any Markup]?`, and `DirectiveBlock` gains `content` |

**`labelCount` was the tell.** The contract says `label: [Markup]?` and the
Swift model kept an `Int`, because with one flat child list there was nowhere
to put the nodes. The C facade has named the two runs since 1.0 —
`markdown_core_node_directive_first_label_child` and `_first_content_child` —
and nothing used them. Now `Markup.directiveLabel(from:count:)` and
`directiveContent(from:)` do, and a written-but-empty label stays `[]`,
distinct from `nil`, which is what the contract requires and an `Int?` could
only encode by accident.

`Table.init` and `TableRow.init` lose their hand-written `as? TableRow` /
`as? TableCell` loops to one generic `Markup.typedChildren(from:)`.

**The dump is byte-identical**, and that is what the conformance suite proves:
`MarkdownCoreConformanceTests` compares Swift's `TreeDumper` output against
`specs/canonical-ast/*.ast`, the same goldens the C dump is checked against, and
it passes unchanged. The walker had to learn the same child order Kotlin's
already had — for a `DirectiveBlock`, **label first, then content**, because the
dump's `children=` counts both.

| mutant | result |
|---|---|
| rename Swift's `List.items` back to `children` | *Swift: List does not declare items* |
| rename Kotlin's `ListItem.checked` to `isChecked` | *Kotlin: ListItem does not declare checked* |

**`scripts/audit-ast-projections.mjs` is green.** It was one of §0's two
known-red rows and one of §4.8's named gates, and §4.1.2 is vindicated in
detail: it reported *"16 Swift-only failures and zero Kotlin or ES failures"*,
which is not era skew but one binding a full era behind the other two.

##### A required CI health check that fails at HEAD and is in nobody's list

`scripts/format-swift.sh --check` runs `swift format lint --strict` and
**exits 1 at `46e20f2`**, with **184** findings, every one of them
`[AllPublicDeclarationsHaveDocumentation]`. The pinned version matches
(`6.3.0`), so it is not toolchain skew; `.github/workflows/ci.yml:182` runs it
as an unconditional step of the required *Health Check - Swift* job. §0's
known-red table does not name it, §4.8 says "formatters, linters, repository
audits" must be green, and no step owns it.

15A.2 takes it **184 → 170** — the eleven leaves lost a public field each — and
does not try to close it, because closing it means writing 170 doc comments and
most of them can only restate the signature. That is exactly the pass this
repository has already rejected once. It is **Q41**.


##### 15A.3 — one audit, and the six surfaces are twelve readers

§4.1 asks for *"**one** audit \[that\] checks all six projection surfaces
against \[the contract\] — C header, C dump, Kotlin bridge + decoder + model, ES
bridge + export list + decoder + model, Swift model + dumper, and the
canonical-AST manifest"*. Until here it read **three**: the three models. A
decoder that forgot a kind, a dumper that could not name one, or a wire enum
one short was invisible to it and visible only if some test happened to parse
that kind.

The six surfaces are twelve concrete readers, because most of them are more
than one file:

| surface | readers |
|---|---|
| C header | the `markdown_core_node_kind` enum |
| C dump | `markdown_core_node_kind_name`'s string table, **and `dump_fields`'s per-kind field emission** |
| Kotlin | `WireKind` enum, `WireMarkupDecoder` arms, `TreeDumper` visits, the model |
| ES | `wire/kinds.ts`, `wire/node-decoder.ts`, `index.ts`'s export list, `tree-dumper.ts`, the model |
| Swift | `Walker`'s `ChildrenVisitor`, `TreeDumper`, the model |
| manifest | `specs/canonical-ast/manifest.json`'s `coverageRequirements.kinds` |

Every reader is checked **both ways**: a kind the contract has and the surface
does not, and a kind the surface has and the contract does not. And every one
carries the saw-nothing assertion 3.2 and 3.4 taught this repository to write —
*"read no kinds at all — this reader is looking at the wrong thing"* — because
a reader that matches nothing is how three audits in this programme have
already turned into sentences.

**The C dump's fields need one stated rule**, and it is the only interesting
one: the dump prints every contract field **except** the ones that ARE the
child structure (`content`, `items`, `header`, `rows`, `cells` are the tree and
the `children=` count). `label` is the exception to the exception — it is
structural and the dump prints its **length**, because a directive's label and
its content are two runs of one child list and nothing else on the line says
where the first ends.

**It found no further drift**, and that is the result: after 15A.2 every other
surface already named all 28 kinds. §4.1.2's reading — *"16 Swift-only failures
and zero Kotlin or ES failures ... one binding a full era behind the other
two"* — is confirmed exactly.

| mutant | result |
|---|---|
| rename `MARKDOWN_CORE_KIND_TABLE_CELL` in the C header | *C header kind enum: does not name TABLE_CELL* |
| misspell `"FootnoteReference"` in the C dump's name table | *C dump kind names: does not name FootnoteReference* |
| stop printing ` level=` for `Heading` | *C dump: Heading never prints level* |
| misspell a `WireKind` arm in the Kotlin decoder | *Kotlin decoder: does not name SOFT_BREAK* |
| delete `Image` from the ES export list | *ES export list: does not name Image* |
| misspell a kind in the canonical-AST manifest | *canonical-AST manifest: does not name TableCell* |

**Gates after.** Every §0 gate green; `audit-ast-projections.mjs` reads
`28 kinds over 12 surfaces, the C dump's fields, the prose table, and 3
models`. No engine file, no binding file and no golden was touched by this
sub-step: it is all audit.


##### 15A.4 — Q29: `mode` is deleted from FIVE kinds, not four

**Q29's reason is right and its number was one short.** A field whose value is
implied by its type is ceremony the surfaces must keep in step; Q29 names
`Code`, `CodeBlock`, `Directive` and `DirectiveBlock` and keeps `mode` on
*"`Formula`/`FormulaBlock` where it is genuinely variable"*. Measured:

- `Formula` is genuinely variable — the corpus has 12 `embedded` and 6
  `standalone`.
- **`FormulaBlock` is not.** The corpus has 12 `standalone` and zero
  `embedded`, and it is not a corpus accident:
  `markdown_core_extensions_set_formula_mode` **refuses** any value but
  `standalone` for a `FORMULA_BLOCK` node (`extensions/formula.c:100`). Its mode
  is as constant as a `CodeBlock`'s.

So five kinds lose it and `Formula` keeps it, which is Q29's own rule applied to
what the engine actually enforces.

**Every claim Q29 makes about the decoders was verified before acting on it**,
and every one was true: Kotlin hard-codes `PlacementMode.STANDALONE` for
`CodeBlock` and `PlacementMode.EMBEDDED` for `Code`; ES hard-codes
`mode: "standalone"` and `mode: "embedded"`; and ES then **asserts** the
constant — `if (fields.mode !== "embedded") throw` for `Directive`,
`if (mode !== "standalone") throw` for `FormulaBlock`. Three of those throws are
gone with the field.

**Twelve surfaces moved in one commit**, which is what 15B's standing rule
means in practice:

| | |
|---|---|
| contract | `canonical-ast.json` and the prose table; the PlacementMode invariants table keeps one row |
| C | `dump_fields` stops printing it; **`markdown_core_node_directive_properties` loses its `mode` out-param** — a public-header change, and §4.1.5 says the surface is free until 3.0 |
| Kotlin | model ×5, decoder, dumper, **and the JNI bridge stops writing the byte** |
| ES | model ×5, decoder, dumper, bridge, **and `es_node_directive_mode` is deleted from the exported-function list** |
| Swift | model ×5, `DirectiveValues`, dumper |
| goldens | 11 fixtures and 5 `.ast` files |

**195 golden rows moved, and the claim is mechanised**: every moved row differs
from its predecessor by the removal of exactly ` mode=embedded` or
` mode=standalone` and nothing else, and every one is one of the five kinds —
97 `CodeBlock`, 59 `Code`, 16 `Directive`, 12 `FormulaBlock`, 11
`DirectiveBlock`.

##### A seventh copy of the contract, found by deleting a field from the first

`scripts/check-canonical-ast-fixtures.mjs` carried its **own hand-written
kind→fields table** — 28 rows, inline in the checker — and it is what failed
when the contract lost `mode`. It now derives that table from
`docs/specs/canonical-ast.json` by the same rule the projections audit uses, so
the count of hand-written copies goes from seven to one.

The manifest's `placement.embedded` / `placement.standalone` coverage states
also had to move: they are demonstrated by a ` mode=` in the dump, and after
Q29 only the `formulas` case has one. `blocks`, `completeness` and `structure`
declared them and no longer earn them.

**Mutant.** Putting ` mode=standalone` back for `CodeBlock` gives
*"C dump: CodeBlock prints mode, which the contract does not give it"* and
**4 of 69 suites red**. That check is new here: the C dump's field comparison
was one-directional until this sub-step, so a field the contract had **deleted**
and nobody removed was invisible. Q29 is exactly that case, which is why the
check and the deletion land together.

**Gates after.** Every §0 gate green · `audit-ast-projections` 28 kinds over 12
surfaces · `check-canonical-ast-fixtures` 28/47/6 · Swift 8 tests + consumer +
0 lint violations · Kotlin `:jvmTest` · ES node tests + `tsc` + `eslint` ·
`format-c` · `format-cmake`. **195 golden rows moved, all mechanically
verified.**


---

#### 4.14.6 Step 6 landed: a mutant that was the correct code, and a backlog entry that closed for the wrong reason

**Footprint.** §4.1 sizes Step 6 at *"~60 · deletions across 18 files"*. It is
**34 files, +400/-266**. The survey that preceded it predicted 22 files for the
option deletion alone and was right about the shape: the width is in the
bindings, and all three had to be built and run.

| | §4.1 says | measured |
| --- | --- | --- |
| files | 18 | 34 |
| lines | ~60 deletions | +400 / -266 |

**What the step is.** Three rulings, one step. Q14 says attachment is the only
gate, so `MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS` and
`_LATEX_FORMULA_DELIMITERS` are deleted rather than defaulted: `formulas` turns
on `$`, `$$`, `` $`...`$ ``, `\(...\)` and `\[...\]` together. Q18 says a
formula body that begins and ends with a space or line ending, and is not all
whitespace, loses one from each end. Q29's `mode` deletion had already moved
these same fixtures at 15A.

The option deletion reaches the C header, the facade, the CLI, the C tests, all
three binding models, the shared manifest and two audit scripts. The bit
positions in the ES and Kotlin masks were renumbered rather than left as a hole
at 8 and 9: both sides of each mask live in this repository, and a reserved gap
that nothing can explain is worse than a renumber nothing can observe.

**Q18's phrasing is what misled.** "Strip one leading and one trailing
space-or-line-ending" reads as two independent strips. It is not:
`extensions-formula-github.txt` pins `text $$ mid$$ text` as `literal=" mid"`,
which only the both-or-neither reading gives. The oracle row is what settled
it, and the implementation comment now says so at the function.

**A mutant that turned out to be the correct code.** The first cut of Q18's
all-whitespace test read `if (!formula_pad_byte(data[i]) && data[i] != '\t')`
— a tab did not make a body count as having content. Mutant **M7** deleted the
tab clause and **survived every gate**, so a pin was written for `$$ \t $$`,
regenerated from the engine, and pinned `literal=" \t "`. The mdast gate then
went red: remark says `literal="\t"`. So does this engine's own CommonMark code
span — `` ` \t ` `` gives `literal="\t"`, because CommonMark's test is
"consists entirely of spaces or line endings" and a tab is neither. **The
mutant was the fix.** The clause is gone, the pin says `"\t"`, and the comment
records that a code span and a formula answer this identically.

This is the case the method exists for. The mutant did not prove a gate gap; it
proved the code wrong, and only because the pin it motivated was checked
against two oracles rather than regenerated and believed.

**Two arms deleted for being unreachable, one predicate kept for stating the
rule.** Q18 words its rule over line endings, and a CRLF is one — so the first
cut collapsed a CRLF to one byte at each end. Measured: with **both** arms
disabled, `text $$\r\nx\r\n$$ text` is byte-identical, and so is a lone-CR
document and the block form. The line reader hands inline content LF-only. The
arms are deleted, because no fixture can reach them to keep them honest.
`formula_pad_byte` keeps its `'\r'`: it states the rule rather than an
algorithm, and the comment says which is which and what has to come back if the
feed ever stops normalising.

**Mutants.** Seven on the padding rule, six killed, one proved equivalent.

| | mutant | correctness | mdast |
| --- | --- | --- | --- |
| M1 | both-or-neither → either-side | 1 failed | killed |
| M2 | all-whitespace exemption deleted | 1 failed | killed |
| M5 | a line ending is not padding | 2 failed | survives |
| M6 | the backslash form does no stripping | 1 failed | survives |
| M8 | the tab clause put back | 1 failed | killed |
| M9 | `size < 2` → `size < 1` | — | survives |
| M10 | one end stripped, not two | 2 failed | killed |

**M9 is equivalent, not a gap.** A one-byte body that passes the first guard is
a single pad byte, which the all-whitespace test then catches; `size < 2` and
`size < 1` cannot differ. **M5 and M6 survive the mdast gate and that is
correct**: M6 only touches the backslash forms, and the mdast corpus
deliberately excludes LaTeX-delimiter rows because remark has no opinion there.
Correctness kills both.

Q14's own gate is live too: attaching formula regardless of the option
(`if (1 || options->formulas)`) fails **two** tests by name —
`spec_commonmark` and `extensions_formula_option_gates`.

**Standing rule 2, and a gate that reported the wrong reason.** §4.1.8 called
this correctly: Step 6's two `baselineBacklog` entries *"close by leaving the
mdast corpus"*. Their inputs are the two LaTeX rows that the 1.0 baseline filed
under `extensions-formula-github.txt`, which the corpus reads;
`specs/oracles/` files them under `extensions-formula-option-gates.txt`, which
it does not. They still diverge from remark and would still diverge if
restored.

**The gate did not say that.** It said *"backlog entry now AGREES with
remark"* — for both — and it had no way to tell the two cases apart: the
`unreachable` branch existed for registered divergences and not for backlog
entries, so an entry that left the corpus was reported as one that settled. A
plan that says "closes by leaving the corpus" and a gate that says "now agrees"
are two different claims, and only the plan's was true. It does now:

- an entry whose input is still in the corpus and no longer diverges reports as
  settled, as before;
- an entry whose input **left** the corpus reports as unreachable, and says
  that retiring it is a decision to record rather than a silence to accept.

The two are recorded in `retiredBacklog` with the reason, and a new check fails
if a retired input ever comes back into the corpus. **That check kills no
mutant** — measured: with the check deleted and one retired input pasted back
into `corpus.md`, the gate still fails, because the input is in neither
`expectedDivergences` nor `baselineBacklog` and lands in `divergent` as an
unregistered difference. What the check adds is the *name* of the cause. The
split between settled and unreachable adds no kill either, for the same reason:
both branches fail. Both are recorded here as reporting improvements, not as
gates.

Backlog: **24 → 22**, and all 22 remaining still diverge.

**Fixture rows that moved.** Three fixtures were already at their oracle
content from the survey; this step added three pins beyond the oracle and
regenerated one row.

- `extensions-formula-github.txt`: **one new example** (`text $$ \t $$ text`),
  written with a placeholder and regenerated by `spec_runner --rewrite`. Exactly
  one row moved, twice: first to `literal=" \t "` from the defective engine,
  then to `literal="\t"` after the tab clause came out. No other row moved.
- `extensions-formula-latex.txt`: **two new examples** pinning Q18's rule on the
  backslash forms, which no oracle row covered.
- `extensions-conflicts.txt`: two examples lost the now-unknown
  `dollar-formula-delimiters` tag.

Every other difference from `specs/oracles/` is one of the two staleness classes
§4.1.8 already names: Q29's `mode=` removal, and D26's `SoftBreak scope=0:0..0:0`.

The counts that moved with the one added example: upstream parity 827 → **828**,
mdast 61 → **62**, scope sanity 5051 → **5056** scopes, containment 3995 →
**4000**, places 4119 → **4123**. Every ledger count held: 4 unresolved, 45
containment rows, 106 place rows, 0 inline-sourcepos rows, 2 reference-order
rows.

**Three things found on the way that are not Step 6.**

1. **`lint:c` was not in my gate loop, and it was red.** `scripts/lint-c.sh` is
   a `-Werror` Debug build, and Step 3b's `static const` descriptors made
   `S_llist_append_checked` discard qualifiers — two errors, invisible to the
   default, ASan and UBSan presets. Fixed by typing the helper
   (`S_extension_list_append`) so the one unavoidable cast sits on one line with
   the reason. **The gate is now in the loop.** It is the fifth trap: a preset
   that builds clean is not the preset CI runs.
2. **`eslint` walked `.claude/worktrees/`** — 20 leftover agent worktrees, 1.8
   GB, checkouts of commits from the closed history — and reported **699 errors**
   nobody owns. Git ignores that directory; eslint did not. Added to
   `globalIgnores`. The worktrees themselves were left alone.
3. **`packages/es-markdown-core/scripts/build.mjs` hid its own failure.** When
   `emcc` is not on `PATH` the spawn never starts, so `status` is `null` and
   `stdout` is `undefined`, and the script died inside `process.stderr.write`
   with `ERR_INVALID_ARG_TYPE` — naming the stream, not the missing compiler. It
   now prints `result.error`.

**Q41 moved, and the last two untriaged checks were triaged.**
`scripts/format-swift.sh --check` is at **163**
`AllPublicDeclarationsHaveDocumentation` findings, down from 170; no other rule
fires. Still red, still registered. `pnpm audit:ci` and `pnpm format:es:check`
— the two §0 listed as *"not yet triaged by era"* — are both era skew, measured:
the first wants every workflow action pinned to a commit SHA and the baseline's
workflows use tag refs; the second reports **100 files**, none of them engine
sources. Neither is engine state. The second is a required CI step with no
owner and 100 files is a decision, so it is now **Q42**.

**Gates.** All green: correctness 69/69, ASan 60/60, UBSan 60/60, conformance
2/2, every audit, `lint-c`, and all four linters. All three bindings built and
run: ES node + conformance, Swift macOS + conformance, Kotlin JVM + macOS-arm64
native, each with conformance.

---

#### 4.14.7a Step 7.1: the directive grammar, and the fallback that was not one

Step 7 is the largest step in the plan and lands in two commits. **This one is
the grammar** — what the parser accepts and what it produces. The other is the
**surface**: `attributes=[…]`, `DirectiveLabel` as a visible node, `label=`
deleted, the JSON round-trip gone, and the bindings that project all of it.

**Where the step started.** Every one of the oracle's 50 examples was run
against HEAD before anything was changed. With Q29's `mode=` stripped (it was
deleted from `Directive` and `DirectiveBlock` at 15A.4, so the oracle's
`mode=embedded` is documented staleness, not a gap), **4 passed and 47 failed**.
Classified by first difference: 27 `label=`/`DirectiveLabel`, 13
`attributes=[…]` spelling, and **20 that were the grammar itself**.

**After this commit: 7 of the grammar's 20 are gone and the other 13 are the
surface's.** Re-run against the same stripped oracle, the remaining 44 failures
are 22 attribute spelling, 21 `DirectiveLabel`, and **one** that is neither:
example 15's `SoftBreak scope=0:0..0:0`, which is D26 staleness — the oracle is
wrong there and this engine is right.

**Ten rules, each with its oracle row.**

| rule | before | after |
| --- | --- | --- |
| a malformed attribute block leaves the directive standing | `:n{#}` was one Text node | `Directive name="n"` + `Text "{#}"` |
| `#name` / `.name` are `id` / `class` | `:n{.a}` was text | `attributes=[class="a"]` |
| a shorthand value ends at the next marker | — | `{.a.b}` is two classes |
| an empty shorthand takes the block down | — | `{#}` and `{.}` are malformed |
| `class` accumulates, everything else last-wins | `class="blue"` | `class="red green blue"` |
| an attribute name may not BEGIN with punctuation | `{:_a}` was accepted | malformed |
| a symbol is punctuation for that rule | `{a$b}` was accepted | malformed |
| `.` `:` `-` `_` are name characters from the second on | `{a:b}` started a nested directive | one name, `a:b=""` |
| an `=` promises a value | `{a=}` was `a=""` | malformed |
| an unquoted value holds no `<` `>` `=` or backtick | `{a=b<c}` was `a="b<c"` | malformed |
| a quoted value needs whitespace or `}` after it | `{a="x"b=1}` was two attributes | malformed |
| a text directive's colon has no colon beside it | `x ::a y` was `x :` + a directive | text |
| a directive name may not BEGIN with `-` or `_` | `:-a[]` was a directive named `-a` | text |

**The grammar is applied to code points, and the engine already had the
predicate.** `markdown_core_utf8proc_is_punctuation` is what "punctuation"
means here — it answers for ASCII through `ispunct`, which is why `$`, `:`, `_`
and `-` are all punctuation and a digit is not, and it answers for the rest of
Unicode through its own table, which is why `{中文=1}` is a name. There is no
second table in `directive.c`. One four-line predicate pair replaced
`is_attr_name_char`, and `attribute_name_is_valid` — the public setter's
validator — now *re-runs the scanner* rather than restating its rule, so the
setter and the parser cannot drift.

**`class` accumulation had to UNLINK, not deactivate.** The first cut marked the
folded duplicates inactive and left them in the list. The duplicate normalizer
that runs next walks the whole list and swaps the first occurrence's value for
the last one's, so `{.a.b}` came out `class="b"` — the accumulation undone by
the pass it was supposed to precede. The folded entries are freed and unlinked,
and the count is decremented, so the normalizer sees one `class` and does
nothing to it.

**Two tests were asserting the old grammar, and one of them segfaulted.**
`api_engine`'s directive test parses `:-a[]{…}` — a name that begins with a
hyphen, which is exactly what the new rule rejects — so the directive came back
NULL and the test dereferenced it. Renamed to `:a[]`, with the accumulated
`class` in its expected string and two new cases for the leading `-` and `_`
(the setter shares `scan_name`, so it got the rule for free).
`pathological_directive_unclosed_attributes` asserted that `:x{` × 20000
produces **zero** directives. Under the fallback rule it produces 20000, each
followed by the prose `{`. What that case guards — 20000 unterminated blocks
must not make the scan quadratic — is unchanged, so the shape assertion moved
rather than went away.

**The fixture was rebuilt from the oracle.** It had 22 examples and the oracle
has 50. Rather than hand-adding rows, the fixture is now the oracle's own text
with Q29's `mode=` removed and the expected blocks regenerated, plus **D21's
three pins at the end** — the container-fence rows, which the oracle has not
got. **13 of the oracle's examples are held back to the surface commit**: every
one carries an attribute VALUE, and their only disagreement with remark is the
spelling. That was verified rather than assumed — remark's projection renders
`class="a  b"` and `a="b{c"` for the two hardest, matching this engine's
semantics byte for byte, and its own sorted bracket form is what Q19 asks the
dump to become. **41 examples, 41 passing.**

**Mutants: thirteen, eleven killed, two owed and named.**

| | mutant | correctness | mdast |
| --- | --- | --- | --- |
| D1 | degradation removed | 2 failed | killed |
| D2 | a name may start with punctuation | 4 failed | killed |
| D3 | `.` `:` `-` `_` are not name characters | 3 failed | survives |
| **D4** | **a shorthand value runs past a marker** | **—** | **SURVIVES** |
| D5 | an empty shorthand is accepted | 1 failed | killed |
| D6 | `class` does not accumulate | 2 failed | survives |
| **D7** | **a class separator before every value, not every non-empty one** | **—** | **SURVIVES** |
| D8 | an `=` does not promise a value | 1 failed | killed |
| D9 | a quoted value needs no separator | 1 failed | killed |
| D10 | an unquoted value may hold `<` `>` `=` and a backtick | 1 failed | killed |
| D11 | a colon run may start a directive | 1 failed | killed |
| D12 | a name may begin with `-` or `_` | 2 failed | killed |

**D4 and D7 are the two whose witnesses are in the 13 held back** — `{.a.b}`
and `{.a class="" .b}`. They are owed to the surface commit, where those rows
land, and they are the only thing this commit leaves unproved. D5, D8, D9, D10
and D11 all survived everything until the 18 attribute-free oracle examples
were adopted; that adoption is what killed them, and it is why the fixture grew
in the same commit as the rules.

**Standing rule 2: backlog 22 → 20.** Two entries closed by *agreeing* with
remark, which the gate's new settled/unreachable split confirms: `:invalid{=value}`
with `:open{title="value}` (the fallback rule) and `:a-[]` / `:-a[]` / `:_a[]`
(the name rule). Step 7 owns **13** of the remaining 20.

**Counts.** 18 examples added: upstream parity 828 → **846**, mdast 62 → **80**,
scope sanity 5058 → **5128** scopes, containment 4004 → **4053**, places 4125 →
**4177**. Every ledger count held: 4 unresolved, 45, 106, 0, 2.

**Gates.** All green: correctness 69/69, ASan 60/60, UBSan 60/60, conformance
2/2, every audit, `lint-c`, all four linters, and all three binding suites.

---

#### 4.14.7b Step 7.2: the label was a node all along, and three copies of one number

The second half of Step 7. 7.1 landed the grammar; this is the surface. **57
files, +1,048 / −826.** With 7.1 that puts Step 7 at 62 files against §4.1's
*"~530 written · +150 net"* — the line count is close, the file count is not,
and the difference is the twelve projection surfaces.

**The whole directive oracle now reproduces.** 46 of 51 examples pass, and the
five that do not are all `SoftBreak scope=0:0..0:0` — D26 staleness, where the
oracle is wrong and this engine is right. The working fixture IS the oracle's
text now, with Q29's `mode=` removed, those seven SoftBreak positions corrected,
and D21's three container-fence pins appended because the oracle has not got
them.

**THE LABEL WAS ALREADY A NODE.** `MARKDOWN_CORE_NODE_DIRECTIVE_LABEL` has been
in the tree since 1.0; the facade *spliced it out*. `get_first_child` skipped
past it into its children and `get_next_sibling` climbed back out, so a
directive's label reached every binding as a **count on the parent** and a run
of children with no container — which is why the model said `label: [Markup]?`
and the dump said `label=2`. Making it visible is a deletion: `is_label`, both
splice branches, and the two accessors that existed only to name where the
label's children began and ended.

**Its scope now spans its brackets, and that is what makes an empty label a
place.** Content-only made `[]` a *negative* range — end one column before start
— because there was nothing between the brackets to point at. `:red[]:` reads
`1:5..1:6` now. The label's own content did not move with it: `internal_offset`
is the field for exactly this (a heading's `#` and a table cell's leading pipe
use it), and without it the block path's label text landed one column early,
which the oracle caught on two rows.

**Attributes are a sequence, and the JSON round-trip is gone.** 256 lines of it:
a renderer, an escaper, a JSON string parser with its own surrogate-pair
handling, and a cached `attributes_json` chunk on every directive node. What
replaces it is three functions over the list the parser already holds. The
public shape:

```
markdown_core_node_directive_properties(node, &name, &has_attributes, &count)
markdown_core_node_directive_attribute_at(node, index, &name, &value)
```

`has_attributes` is not a convenience: `:n` wrote no container and `:n{}` wrote
an empty one, a count of zero cannot tell them apart, and the old `null` versus
`"{}"` said so.

**Q19's sort is a linked-list merge sort, and the reason is failure.** The
duplicate normalizer above it already has a no-allocation fallback for when its
index cannot be built; a sort that could fail to allocate would hand back an
UNSORTED list with nothing to say so. Bottom-up merge over the list itself
cannot fail. remark's own projection is sorted, which is what the mdast oracle
compares against — the sort was verified against it rather than assumed.

**Standing rule 2: backlog 20 → 7, and Step 7's thirteen all closed.** Every one
of them was an attribute-bearing directive whose disagreement with remark was
the spelling; the 13 fixture rows held back at 7.1 landed here and every one
agrees. What remains is Step 9b's six and Step 10's one.

**Three copies of one number, and three hand-written exceptions.**

- `scripts/audit-public-surface.sh` asserted each binding's visitor had **28**
  methods, in three places, with the number written out. Adding a 29th kind made
  all three say the same wrong thing at once. The count is read from the
  contract now.
- `check-canonical-ast-fixtures.mjs` and `audit-ast-projections.mjs` each
  decided which contract fields the dump prints with a regex naming four kinds
  by hand **plus an explicit `label` exception** — because a label was a count.
  Both now ask whether a field's type names a kind, which the contract answers.
- The fixtures gate's field splitter read `attributes=[a="1" b="2"]` as TWO
  fields. It blanked quoted strings and not bracketed groups; it does both now.

None of those three was found by reading. Each was a gate going red on a change
it was written to notice, which is the whole argument for having them.

**`escaping.json` was renamed to `escaping.attribute-value`, and one case lost
it.** The state checks that the dump escapes a quote inside an attribute value;
`inlines.ast` declared it while demonstrating no such value, which only became
visible when the JSON string stopped supplying quotes for free.

**Mutants: nine, all killed.**

| | mutant | correctness | mdast | conformance |
| --- | --- | --- | --- | --- |
| D4 | a shorthand value runs past a marker | 1 | killed | — |
| D7 | a class separator before every value | 1 | killed | — |
| S1 | attributes not sorted | 3 | killed | — |
| S2 | the block label's scope is content-only | 1 | — | killed |
| S3 | the inline label's scope is content-only | 1 | — | killed |
| S4 | the label's content offset dropped | 1 | — | killed |
| F1 | an absent attribute container reads as empty | 2 | — | killed |
| F2 | the attribute separator dropped | 1 | killed | killed |
| F3 | the label hidden again | 2 | killed | killed |

**D4 and D7 are the two 7.1 left owed**, and the rows that kill them are the
ones this commit landed. S2, S3 and S4 are invisible to the mdast oracle because
it does not compare positions — the canonical-AST goldens are what catch them,
which is the second time this branch has found that division of labour holding.

**Counts.** 13 more examples: upstream parity 846 → **859**, mdast 80 → **93**,
scope sanity 5128 → **5190** scopes, containment 4053 → **4089**, places 4177 →
**4224**. Every ledger count held: 4, 45, 106, 0, 2. Q41 is at **164**, up one
from 163 — `DirectiveAttribute` and `DirectiveLabel` are new public
declarations and the rule wants comments on their members.

**Gates.** All green: correctness 69/69, ASan 60/60, UBSan 60/60, conformance
2/2, 29 kinds and 48 fields, every audit, `lint-c`, all four linters, and all
three binding suites including Kotlin macOS-arm64 native.

---

#### 4.14.7c Step 7 closes: Q20 answered by the oracle, Q14's other half, and 41 measured differences

The last of Step 7. Two questions were open and both are settled by
**measurement against micromark-extension-directive 4.0.0**, not by preference.

**Q14's other half.** `MARKDOWN_CORE_OPT_DIRECTIVE` is gone from the header, the
extension, the facade, the CLI and four C tests, and `directive_enabled` with
it. The gate is live: attaching the extension regardless of `options->directives`
fails **three** named tests — `spec_commonmark`, `extensions_gfm` and
`extensions_directive_option_gates`. The option bit is not renumbered into;
`gfm-extended` now differs from `gfm` only in which extensions it attaches,
which is what Q14 says a profile is.

`extensions-directive-option-gates.txt` was retitled with it. Its prose claimed
to *"attach the directive extension without enabling its parser option"*, which
it never could: the ctest entry passes no `--option`, the examples carry no
tags, and the runner starts from nothing attached. §4.1.8 had that row open.

---

**Q20: are character references decoded in directive attribute values?**

**YES, and the oracle says exactly where.** `mdast-util-directive` decodes in
three places and three only — an attribute's value, and the `#id` and `.class`
shorthand values — with `parseEntities(…, {attribute: true})`. A NAME is not
decoded. This engine now decodes at the same three, through
`houdini_unescape_html_f`: the sixth call site of the engine's one entity
decoder, alongside a link destination, a link title and a fenced block's info
string.

Decoding runs **after** the raw scan and never during it, which is what makes
`{a=x&#125;y}` one attribute whose value contains a `}` rather than a block
that ended early, and `{a=b&#32;c}` one attribute rather than two.

**The narrower question — is the semicolon required? — was measured
exhaustively**, every named entity in this engine's table through both sides:

| form | inputs | differ |
| --- | ---: | ---: |
| named, with `;` | 2125 | **0** |
| named, without `;` | 2125 | **106** |
| numeric, both bases, with and without `;` | 104 | 55 |
| oddities (`&`, `&;`, `&#;`, `&AMP;`, nine digits, …) | 16 | 1 |

**Zero differences on every one of the 2125 `;`-terminated named references.**
The entire difference is the semicolon, and it comes to 162 inputs: HTML5's 106
legacy names, the numeric forms without one, and a numeric form of more than
eight digits.

**And micromark disagrees with itself there.** Measured against remark, a
semicolon-less `&amp` is LITERAL in ordinary text, in a link title, in a link
destination and in a fenced block's info string, and decodes only inside a
directive attribute value — because `mdast-util-directive` reached for an HTML
*attribute* rule for something that looks like an HTML attribute. That rule
appears nowhere else in its own parser.

So: **this engine requires the semicolon**, which is CommonMark's rule and the
rule at its own five other decode sites. Importing the sixth would put a second
entity rule inside `extensions/directive.c`, disagreeing with the five that
share one. Registered as `directive-attribute-reference-semicolon` in
`specs/mdast-parity/deltas.json` with the measurement — which is what Q20's own
text said to do if the semicolon half were declined.

---

**Then the grammar was swept against micromark's source, and it was not
conformant.** Five independent readings — names and colon forms, attribute
names, attribute values, labels, degradation and interaction — each probing
both implementations on concrete inputs, and every claimed difference handed to
a separate reader told to refute it. **41 survived refutation.** Four were
repairs and land here.

| | what micromark does | what this engine did |
| --- | --- | --- |
| **the directive name is code points** | any code point that is not whitespace and not punctuation, `-`/`_` from the second on | `[A-Za-z0-9_-]+`, so `:café` was a directive named `caf` and the text `é`, and `::café` was not a directive at all |
| **an attribute name may start with `-` or `_`** | `between` ends the block on punctuation *except* those two | every punctuation start rejected |
| **a shorthand value is not a name** | ends at `#`, `.`, `}` or whitespace; rejects `"` `'` `<` `=` `>` and a backtick; consumes everything else | reused the attribute-NAME scanner, so `{.a&b}` was malformed |
| **an unquoted value holds no quote** | `valueUnquoted` fails on `"` and `'` as well as `<` `=` `>` and a backtick | both quotes were value characters |

The name rule turned out to be the rule already here. micromark's
`factory-name.js` is four lines, and its two states are exactly
`name_cp` (not whitespace, not punctuation) and `attr_name_start_cp` (that plus
`-` and `_`) — the predicates the attribute grammar was already built from at
7.1. So the fix is a deletion: `scan_name` stops calling the generated
`_scan_directive_name`, and that ASCII scanner, its `directive_name_char` rule
and its declaration are gone from `ext_scanners.re`, `.c` and `.h`. **Two name
rules became one predicate pair.**

**Mutants: eleven on this commit, all killed, and two of them were re-aimed
after surviving.**

| | mutant | correctness | mdast |
| --- | --- | --- | --- |
| Q14 | directive attached regardless of the option | 3 failed | — |
| E1 | attribute values are not decoded | 1 | killed |
| N1 | the name continues byte-wise | 1 | killed |
| N2 | the name may start with punctuation | 2 | killed |
| N3 | the name may end with `-` or `_` | 2 | killed |
| V1 | an unquoted value may hold a quote | 1 | killed |
| K1a | a shorthand value rejects nothing | 1 | killed |
| K1b | a shorthand value does not end at a marker | 1 | killed |
| K1c | an empty shorthand value is accepted | 1 | killed |
| K2a | a name may not start with `-` or `_` | 2 | killed |
| K2b | `.` and `:` may start a name too | 1 | killed |

**K1a survived its first run and the reason is worth keeping.** The pin was
`{.a"b}` — and the raw brace scan tracks quoting, so that input is already
malformed one step earlier and never reaches the reject set at all. `=`, `<`,
`>` and the backtick are the four this rule alone decides; the pin is
`` {.a`b} `` now. **N1 survived its first run because the mutant was nearly a
no-op** — it made only the FIRST code point byte-wise, and a directive name's
first character is ASCII in every pin. Re-aimed at the continuation loop it
dies, on a pin whose witness is an en dash: one code point of punctuation and
three bytes of none.

---

**The other 37, classified, none of them silent.**

| class | witnesses | what it is |
| --- | --- | --- |
| ~~the label lexer~~ | `:b[http://e.com]`, `` :b[a`b]`c] ``, `*a :b[c*]`, `:a[b\]`, `:a[b` unclosed, 33-deep nesting, 5 more | **D36, CLOSED at 7e (§4.14.7e).** micromark finds the `]` lexically; this engine pushed a delimiter and let the inline pass pair it, so anything that reached the bracket first won. It scans at the colon now, and all nine witnesses match remark. |
| **whitespace class** | a tab, a form feed, U+2028 in names and values — 11 witnesses | micromark's preprocessor gives a tab the code `-2` and its `regexCheck` bails on `code > -1`, so **a tab is a name character there**. That is an artifact of its own tokenizer, not a rule anyone wrote. |
| **C0 controls in names** | `:a\x01b`, `:n{\x01=1}` | same shape: a control is neither whitespace nor punctuation to it, so micromark takes it into a name. |
| **the punctuation class** | `:n{a€b}` | micromark's `unicodePunctuation` covers symbols as well as punctuation; this engine's is CommonMark **0.29's**, P only — and cmark-gfm 0.29 is its pinned upstream oracle, so changing it moves the emphasis flanking rules and the whole base language with them. **Not Step 7's to change.** |
| **container block rules** | `> :::a` with a lazy line, `  :::a` with an indented body | a lazy continuation closes the container there; the opening fence's indent is stripped from every content line. Block-level, adjacent to D21. |
| **quoted value continuation** | `:n{a="b\n   c"}` | micromark keeps the continuation line's leading whitespace in the value. |
| **an escaped colon** | `\::b[c]`, `\::a` | the ban on a colon beside a colon is lifted when the first came from a character escape. |
| **a table cell's raw bytes** | a directive in a table cell whose value holds an escaped pipe | micromark hands the directive the cell's raw bytes, so GFM's escaped pipe reaches the value. |

The last four are one or two witnesses each and none of them is in the mdast
corpus, so **the gate is silent about them** — which is the reason they are
written down here rather than left to be rediscovered.

**Counts.** 9 examples added: upstream parity 859 → **871**, mdast 93 → **105**,
scope sanity 5190 → **5233** scopes, containment 4089 → **4114**, places 4224 →
**4255**. Every ledger count held: 4, 45, 106, 0, 2. Backlog unchanged at 7.

**Gates.** All green: correctness 69/69, ASan 60/60, UBSan 60/60, conformance
2/2, every audit, `lint-c`, all four linters, and all three binding suites.

---

#### 4.14.7d D36's autolink witness, closed by the third proposal

The owner read D36 and said the label case should be fixed by **moving
`autolink` to the end of the attach order**. It is not, and measuring why led
to a fix that is.

**Proposal 1 -- attach order. Measured dead.** With `autolink` last instead of
first, four probes are **byte-identical**: `:b[http://e.com]`,
`[http://e.com]`, `[http://e.com](/x)`, `:b[a http://e.com b]`. The two
extensions are never consulted at the same byte. `autolink` wins at the `:` of
`http://`, where `directive` is offered the byte and declines for want of a
name after `//`; having won, it consumes through the `]`, so the byte that
would have closed the label is never dispatched to anyone. Order decides who
gets a *contested* byte, and this is not a contest.

Isolating it does confirm the cause: with the directive extension attached
ALONE, `:b[http://e.com]` is a proper directive.

**Proposal 2 -- the `in_bracket` guard. Measured dead, and it looked right.**
`autolink`'s `match` already declines inside a link or image bracket; a
directive label is a delimiter rather than a bracket, so teaching the guard to
see an open label is the obvious narrow repair. It fixes `:b[http://e.com]`
**and breaks `:b[a http://e.com b]`**, where the URL inside the label must
still become a `Link` — remark produces one, and so did this engine before the
guard.

**Proposal 3 -- bound the SCAN, not the match.** The pair above looked
contradictory: the label's contents must be inline-parsed, autolinks included,
while its terminator must already be known. But only the *scan* needs bounding.
A delimiter rule may declare its closer **structural**, and a forward scan asks
the core before crossing a byte:

```c
int markdown_core_inline_parser_byte_is_protected(markdown_core_inline_parser *, unsigned char);
```

`autolink`'s two URL scans stop at whitespace, at `<`, **or at a byte something
else is holding open** — and never learn whose byte it is. Which rules have a
structural closer is decided in the core, next to the rule list that already
names `MARKDOWN_CORE_DELIM_RULE_DIRECTIVE_LABEL`, so no extension knows
another's answer. Six probes, all matching remark exactly:

| input | before | after (= remark) |
| --- | --- | --- |
| `:b[http://e.com]` | `Text ":b["` + `Link "http://e.com]"` | `Directive` > label > `Link` |
| `:b[a http://e.com b]` | already right | unchanged |
| `:b[a www.e.com]` | already right | unchanged |
| `:b[x] then http://e.com/a]b` | already right | unchanged |
| `~~a http://e.com/x]y~~` | already right | unchanged |
| `see http://e.com/a]b now` | already right | unchanged |

**Two things went wrong on the way and both are worth keeping.**

**It was QUADRATIC, and the first measurement of that was against a stale
binary.** The obvious implementation walks the delimiter stack per byte asked.
On `*a ` × N beside a URL of length N: **0.04 / 0.13 / 0.58 s** at N = 5000 /
10000 / 20000, against a flat **0.00** before — 4.5× per doubling. The subject
now counts structural-closer delimiters per rule, so the query is O(rules) and
the same inputs are flat to N = 80000. §3's rule is that anything re-walking
work proportional to the input is off-model however correct, and this was.

The stale binary matters more than the number. Two `git stash` cycles left
objects newer than the restored sources, so a build that reported success ran
the old code; the timings taken from it were meaningless and the correctness
probe silently showed the OLD tree. **`rm -rf build/<preset>` before any
measurement that spans a stash**, and read the behaviour, not just the exit
code. This is §0's mtime trap wearing different clothes.

**The count has to track the SCAN, not the stack.** Counting delimiters still
pushed leaves a label's `]` protected for the rest of the paragraph, because
the stack is not emptied until `process_emphasis` runs — after the whole
subject has been read. `:b[x] then http://e.com/a]b` lost the `]b` from its
URL. Openers seen minus closers seen is what the question means, and the scan
is strictly forward, so seeing the closer is the answer.

**That defect existed for one build and no gate caught it**, which is what the
mutants then said: three of five survived the first pin set. The witnesses they
asked for are the three rows above that read "already right" — the closed
label, the `www.` scan (a second, identical scan site the first pins never
reached), and strikethrough as the control that only a LABEL's closer is
structural. Five mutants, five dead.

**Counts.** 6 examples added to `extensions-conflicts.txt`: upstream parity 871
→ **877**, scope sanity 5233 → **5274** scopes, containment 4114 → **4151**,
places 4255 → **4290**. mdast unchanged at 105 and the backlog at 7 — the
fixture is not in that corpus. Every ledger count held: 4, 45, 106, 0, 2.

**Gates.** All green from a clean rebuild of all three presets: correctness
69/69, ASan 60/60, UBSan 60/60, conformance 2/2, every audit, `lint-c`, all
four linters, and all three binding suites.

---

#### 4.14.7e D36 closed: the bug was in STARTING a directive, not in anything downstream

**The owner was right twice and I was wrong twice.** The first correction —
"move `autolink` to the end of the attach order" — was wrong about the remedy
and right that the diagnosis was off. The second — *"the issue is not how you
iter the nodes, the issue is how you start a directive"* — named the defect
exactly, and everything at 7d was a side path.

**The bug.** `:name[label]` was started by PUSHING A DELIMITER for `:name[` and
betting a `]` would turn up later to pair with it. micromark does not bet. Its
`directive-text.js` runs `effects.attempt(label, afterLabel, afterLabel)` — it
SCANS the label at the colon, and **both branches continue**. A label that
closes is a label; one that does not is prose; the directive stands either way.
That is the same shape 7.1 gave the attribute block, and the label never had it.

Two consequences follow from the bet and both were defects. When no `]` arrives
the directive is LOST — a differential over 29 start-shaped inputs put the whole
difference from remark in exactly two rows, `:b[c` and `:b[`. And while the bet
is open **the label has no boundary**, so whatever reaches the `]` first takes
the directive with it. That is D36.

**Scanning at the colon consumes the bytes**, so no other extension is ever
offered them. There is nothing to protect, nothing to order, and no iterator to
change:

| witness | before | now (= remark) |
| --- | --- | --- |
| `:a[b` | one Text node | `Directive a` + `[b` |
| `:b[` | one Text node | `Directive b` + `[` |
| `a :b[c f` | one Text node | `a ` + `Directive b` + `[c f` |
| `*a :b[c*]` | `Emphasis` swallowing `a :b[c` | `*a ` + `Directive b`, label `c*` |
| `` :b[a`b]`c] `` | label held `a` + Code `` b] `` + `c` | label `` a`b ``, then `` `c] `` |
| `` :a[`[`x]y] `` | — | `Directive a`, label Code `[` + `x]y` |
| `a :b[c\\] d` | — | `a ` + `Directive b`, label `c\` |
| `:b[http://e.com]` | `:b[` + Link `http://e.com]` | `Directive b`, label = Link |
| 33 nested brackets | a label | prose, as micromark caps it |

**Nine of nine match remark.** D36 is closed and **Q43 is answered — lexical**,
here rather than at Step 8.

**What went away.** Eleven functions, one delimiter rule's only use, one
dispatch byte, one extension hook, and the whole of 7d:

```
make_name_only_directive   make_delimiter_text        match_directive_delimiter
find_directive_opener      scan_parsed_attributes     match_label_closer
remove_delimiters          set_attributes_from_wrapper make_empty_label_node
insert_label_directive     insert_directive
DIRECTIVE_LABEL_DELIM   the `]` in `.dispatch`   .insert_inline_from_delim
markdown_core_inline_parser_byte_is_protected   delimiter_structural_closer
subject.protected_open  and its push-side counting
```

**8 files, +214 / −409.** `directive.c` alone is −413/+~120. The extension no
longer pushes a delimiter at all, and `]` is nobody's business but the core's —
which is what makes `[a](b)` inside a label behave like any other link.

**The label's inlines are parsed against the label**, the way a table cell's are
and the way this directive's BLOCK forms always did: `make_label_node` copies
the raw bytes into `label_node->content`, sets `internal_offset = 1`, and the
parse runs on that buffer. `process_inlines`' walk cannot reach a node created
during a paragraph's own inline pass — `markdown_core_iter_next` computes its
next event eagerly, so when it hands out `ENTER(paragraph)` it has already seen
`first_child == NULL` — so the parse is driven from the extension. That is
re-entrancy into `markdown_core_parse_inlines`, and it is safe because the
subject is a stack local: nothing is shared but `parser->mem` and the sticky
OOM flag.

**The oracle fixture passed 66/66 with no golden regenerated.** The lexical
start changes nothing that was already pinned; it only fixes what was broken.
Five new pins were added for what it fixes, and one registered divergence
—`autolink-after-failed-label`— **closed**, because the two now agree.

**The depth cap was off by one and comparing caught it.** micromark counts the
INNER brackets and fails at the 33rd (`if (code === 91 && ++balance > 32)`);
`scan_label` starts its depth at 1 for the label's own `[`. The first cut
dropped the label at 32 inner brackets where the reference drops it at 33. Both
sides now agree at 31, 32, 33 and 34.

**Mutants: six, all killed.**

| | mutant | correctness | mdast |
| --- | --- | --- | --- |
| L1 | a failed label kills the directive | 2 | killed |
| L2 | the label's inlines are not parsed | 3 | killed |
| L3 | the label's content offset dropped | 2 | survives |
| L4 | no depth cap | 1 | killed |
| L5 | the cap is off by one | 1 | killed |
| L6 | the construct is not consumed | 2 | killed |

**Two process failures, both mine.** The assessment I launched let its agents
edit the working tree — an `int pending_enter` lookahead experiment in
`core/iterator.c` and a partial rewrite of `directive.c` — which the owner saw
before I did. It was killed and reverted; **a workflow that may build must be
told to work in a copy, or given `isolation: "worktree"`.** And 7d's whole
mechanism was designed around a diagnosis that named the wrong layer. The
measurements in it were sound and its conclusion was not, which is what a
diagnosis that stops at the first mechanism that *could* explain the symptom
buys you.

**Counts.** 5 examples added: upstream parity 877 → **882**, mdast 105 → **110**,
scope sanity 5274 → **5301** scopes, containment 4151 → **4174**, places 4290 →
**4312**. Every ledger count held: 4, 45, 106, 0, 2. Backlog unchanged at 7.

**Gates.** All green from a clean rebuild of all three presets: correctness
69/69, ASan 60/60, UBSan 60/60, conformance 2/2, every audit, `lint-c`, all four
linters, and all three binding suites.

---

#### 4.14.15A Step 15A: one contract, and the surfaces that project it

##### 15A.1 — the contract is machine-readable, normative, and out of the archive

**Where it was.** `docs/deprecated/specs/canonical-ast.md` — a Markdown table
parsed by a regex, in the directory this document's own first paragraph calls
archive and *"nothing there is normative"*, read by **four executable policy
files**.

**Where it is.** `docs/specs/canonical-ast.json`: 28 kinds, 53 fields, each with
a name, a type and a nullability bit, in canonical order, plus the three enums.
`docs/specs/canonical-ast.md` moves out of the archive with it and becomes the
**prose companion** — the core rules, the coordinate model, ownership, the
attribute grammar, everything a table cannot say.

**And the prose is checked.** Its kind/field table is a second copy of the
contract, so `audit-ast-projections.mjs` now compares it against the JSON kind
for kind, field for field, **in order**. The two cannot drift.

| mutant | result |
|---|---|
| drop `content` from `Heading`'s row in the prose | *`Heading` reads [level] and the contract says [level, content]* |
| swap `Text` and `SoftBreak` in the prose table | *its table names a different set or order of kinds than the JSON* |

**Everything that pointed into the archive was repointed**, and doing that found
**two citations that outlived their documents**:

- `scripts/check-generated-scanners.sh` cites `docs/deprecated/specs/c-naming.md`
  — **which is not in this repository.**
- `scripts/audit-scope-sanity.mjs` cites two review documents under
  `docs/deprecated/reviews/` — **that directory does not exist.**

Both now say so in place rather than pointing at nothing. Fifteen files were
repointed, including `specs/canonical-ast/manifest.json`,
`specs/coverage/policy.json`, both parity `deltas.json`, the scope-sanity
ledger and the Kotlin build's packaged-docs list.
`docs/specs/test-architecture.md` moved too, because `specs/coverage/policy.json`
names it as its contract.

**What still points into `docs/deprecated/`, and why it stays.** Exactly two,
both release plumbing and both already §4.1.5's:
`check-release-version.mjs` and `audit-ci-policy.sh` read
`docs/deprecated/releases/$(cat VERSION).md`, and the Android runtime's Gradle
build packages a migration document. §4.1.5 lists the release-notes path as one
of the seven release gates, owner **15C**. Nothing normative is left in the
archive.

**Gates after.** Every §0 gate green and unmoved, plus the three binding
baselines this step established and can now hold: **Swift** `swift test`,
**Kotlin** `:jvmTest` (Gradle via Android Studio's JBR, which
`scripts/lib/discover-toolchain.sh` already finds), and **ES** node tests
against a wasm build from the **vendored** emsdk at
`.tools/emsdk/4.0.23`, which is not on `PATH` by default.
`audit-ast-projections.mjs` is still red, on the Swift model alone, which is
15A.2.



#### 4.14.10 Step 10 landed: an offset into a content buffer is not a column

**The requirement, restated as the thing that was missing.** A block's content
buffer is the concatenation of the line slices `add_line` copies into it with
the container prefix stripped, and the engine kept exactly two numbers about
where those bytes came from -- the block's `start_line` and `start_column`.
Every consumer that had an offset into that buffer and wanted a place added the
offset to the column. That is right only while the block is one line long, or
while every line of it begins in the same column. `"> foo"` followed by `"bar"`
strips two bytes from the first line and none from the second.

**The mechanism, and it is the one §4.1 named.** `parser->line_marks`: one
`markdown_core_line_mark` -- `(content_offset, line, column)`, twelve bytes --
appended per `add_line`, and a node carrying the index and length of its own
run. `markdown_core_parser_content_place(parser, node, offset, &line, &column)`
binary-searches that run, so a caller asking once per inline node pays
log(lines in the block) and nothing re-walks the document (§3).

**Two properties made it small, and both were checked rather than assumed:**

- **A block's marks are contiguous**, because only the deepest open block takes
  lines and opening another closes it. That is an `assert` in
  `S_record_content_mark`, it is compiled into the ASan preset (no `-DNDEBUG`,
  and the assertion text is in the binary), and `correctness-asan` runs 60/60
  with it live. So the run is two ints on the node rather than an owner pointer
  per mark.
- **A cut off the front of the content is a rebase, not a rebuild.**
  `resolve_reference_link_definitions` drops the definitions it harvested;
  `S_rebase_content_marks` moves the run's head past what went away and
  advances the column of the slice the cut landed inside. O(lines in the
  block), once.

**Five consumers, and the fifth is the one the ledgers demanded.**

| consumer | before | after |
|---|---|---|
| the split-off table lead | `0:0..0:0` -- the last sentinel in `specs/scope-sanity/ledger.json` | the place its first and last bytes were written |
| the table itself | the paragraph's start, which is the LEAD's first line | the header row's own line and column |
| the recovered header row and its cells | `start_column + content_offset`: `\| a \| b \|` on line three reported at `1:10`, a column line one does not have | `place(cell->start_offset)` |
| the paragraph whose front was consumed (D18) | the line counted from the dropped newlines, the column left alone with a note saying it was right in every corpus case | both coordinates from the map |
| **the inline phase's line origin** | `column_offset = -pos`, i.e. every line of the block starts where the block does | `S_reseat_column_origin`, i.e. where the map says that line starts |

**The fifth needs its own justification, because Q22 assigns the inline
projection to Step 8.** Three places in this repository -- `specs/positions/places.json`'s
`continuation-line-content-offset` family, nineteen rows in
`specs/positions/containment.json`, and §4.1.3's own sentence about the residue
Stage 0a leaves -- all name **Step 10** as the owner of the continuation-line
column class. Q22's recommendation splits producer from projector and says
*"8 projects through it"*. Both are satisfied by what landed: there is still
exactly one map, `S_line_start_column` is the only thing that reads it for the
inline phase, and the four sites that used to assign `column_offset` were four
spellings of one sentence -- *the negated content offset of the line the cursor
now stands on* -- so they became one call each. Step 8 still deletes
`block_offset`, `column_offset`, `adjust_subj_node_newlines` and
`count_newlines`; it now deletes them against a map that already exists instead
of inventing a second one.

**What it cost, measured on this machine rather than estimated.**

| | before | after |
|---|---|---|
| `sizeof(markdown_core_node)` | 176 | **184** -- two `int`s, and the four-byte hole after the `footnote` union absorbs one of them |
| `bench large_document@128 / @256 / @512` | 21.605 / 45.171 / 89.974 ms | 20.146 / 41.231 / 84.352 ms |
| `bench deep_nesting@32768` | 2.448 ms | 2.171 ms |
| `bench adversarial_links@65536` | 19.975 ms | 19.467 ms |
| peak RSS, 2.3 MB input, `--profile gfm-extended` | 91,734,016 B | 91,750,400 -- 91,799,552 B, **+0.02% to +0.07%** |
| `leaks --atExit` over `extensions.txt` + `spec.txt` | 0 | **0** |

**Every "after" timing is faster than its "before", and that is noise, not a
speed-up.** The baseline was run twice on the same build and spread 21.605 to
20.075 ms on one case; the honest statement is that the change is smaller than
this machine's run-to-run variance on every workload measured.

**The ledgers, and the direction each moved.**

| ledger | before | after |
|---|---|---|
| `specs/positions/places.json` | 106 | **79** -- 27 cleared, **0 appeared**. `split-off-table-lead` (6) and `continuation-line-content-offset` (22) both leave; one of the 22 did not move and is re-filed |
| `specs/positions/containment.json` | 45 | **31** -- 14 cleared, 0 appeared. It also reports **0 child relations skipped** where it used to skip the lead's, so it judges more than it did |
| `specs/scope-sanity/ledger.json` | 4 | **1** -- the last sentinel and both partial rows go, and what is left is the one negative range, an empty table cell |
| mdast reconstruction backlog | 7 | **6**, all Step 9b's |
| upstream parity | 882/882, 7 divergences | **885/885, 8** -- `table-lead-authored-spelling` |

**The lead keeps its authored spelling, and that is a behaviour change, not a
position change.** The lead used to be run through `unescape_pipes`, which is a
CELL transformation: a pipe a cell escaped is not a pipe the cell contains. The
lead is not a cell. `pre \\| lead` above a table therefore reached the inline
phase as `pre \| lead`, whose surviving backslash escaped the pipe, and the
paragraph came out one character short of what was written. remark agrees with
the fix -- it is `specs/mdast-parity/corpus.md:207`, the last backlog entry that
was not Step 9b's -- and cmark-gfm does not, so the difference is registered.
Registering it required a **corpus addition**: no fixture had an escaped pipe in
a split-off lead, and `check-upstream-parity.mjs` fails an `expectedDivergence`
its corpus never reaches.

**Fifty golden rows moved, three examples were added, and the claim is
mechanised.** 36 in `spec.txt`, 10 in `extensions.txt` (all in one example), 4
in `regression.txt`. The ten and the four were read by hand. For the 36, the
statement checked by machine is that **the source bytes at a `Text` or `HTML`
node's own scope must spell its own literal**, over every single-line literal
the source spells verbatim: **1,037 of 1,134 agreed before, 1,069 after, 31
went from disagreeing to agreeing and NOT ONE went the other way.** The
remaining nine moved rows are breaks, an `Emphasis` end and a `Link` end, which
that statement cannot reach and which `audit-position-places.mjs` covers
instead.

**One prediction in §4.2's own tables came true at the column it named.** D19's
residue row said `spec.txt` example 518 was *"still wrong but better -- the true
source column is `2:12` and the two-column shortfall is the continuation line's
stripped indent, Q22/Step 10's"*. It now reads `Link scope=1:1..2:12`. That row
is the closest thing this stage has to a pre-registered prediction, and it is
the reason to write residues down with the number they should become.

**That measurement also sizes a blind spot this document already named.** Of
the 40 moved rows in `spec.txt` and `regression.txt`, the places oracle could
see 21. Ten of the rest were *well-formed columns on the wrong line origin* --
`aaa`, then thirteen spaces and `bbb`, reported the `bbb` at column 1 of a
sixteen-byte line. Column 1 is a place. R7 says a well-formed but wrong position
sails through the ratchet and this is what that looks like at scale.

**Nine mutants, and one of them kills nothing.**

| mutant | what went red |
|---|---|
| M1 restore `unescape_pipes` on the lead | `extensions_gfm`; upstream parity **883/885**; mdast parity **109/110** |
| M2 the lead keeps no position | `extensions_gfm`; places **+1** |
| M3 the table keeps the paragraph's start | `extensions_gfm`; containment **+1** |
| M4 header row and cells read a content offset as a column | `extensions_gfm`; places **+13** |
| M5 D18's line only, column left alone | `regression_commonmark`; places **+1**; containment **+1** |
| M6 the map's binary search excludes an exact line start | `spec_commonmark`, `extensions_gfm`; places **+14**; containment **+2** |
| M7 the marks are not rebased after a cut | `spec_commonmark`, `regression_commonmark`; places **+1** |
| M8 the synthesized tab spaces get no mark of their own | **NOTHING. 69/69, every oracle green** |
| M9 the inline phase does not read the map | `spec_commonmark`, `regression_commonmark`; places **+21**; containment **+14** |

**M8 is worth the space, because the reason is structural and not a gap in the
corpus.** Every path that reaches `add_line` for a paragraph or a heading first
runs a BYTE-wise `S_advance_offset`, which clears `partially_consumed_tab`; only
the three kinds that call `add_line` directly can carry it in. Instrumented over
the 885 corpus examples and 1,400 generated tab inputs, the branch is entered
**404 times and the block is a code block (368), an HTML block (23) or a formula
block (13) every one of them** -- none of which takes a position from the map.
The mark stays, with that measurement written above it in `blocks.c`: it is
defensive, and the code says so rather than implying a gate watches it.

##### What Step 10 does NOT close, and the ruling it asks for

**The nine autocompleted table cells stay, and this is Q44.** A cell the table
extension completes at the end of a short row has **no source bytes at all**.
Both spellings a coordinate pair can give it were built and measured, in the
shape §4.2.5 used for D26:

| spelling | places | containment | scope-sanity |
|---|---|---|---|
| today, `L:0..L:0` | 9 zero-column | 18 | 0 |
| the empty range past the row's last byte, `L:len..L:len-1` | **9 → 9**, zero-column becomes off-column | **−18** | **+9 negative**, which its only-shrink rule refuses |
| the row's last byte, `L:len-1..L:len-1` | **−9** | −18 **+6 sibling-overlap** | 0 |

The first is a pure transfer. The second nets twenty-one registered rows for
six, and buys them by **claiming a byte that already belongs to the cell before
it** -- which is 11a's L1 broken before 11a is written. The cause is the one
`specs/scope-sanity/ledger.json` states in its own `purpose`: *the dump has no
spelling for "no position" that does not borrow a coordinate.* So this is a
ruling about vocabulary before it is a repair, it gets **Q44**, and its owner is
**11a**, where a node owning zero regions first becomes a thing that can be
said. The rows are re-attributed in both ledgers rather than left `unassigned`.

**One row in `places.json` was misfiled, and Step 10 is how that was found.**
The `continuation-line-content-offset` family had 22 rows; 21 cleared and one
did not. It is not that class: a text run containing a backslash escape reaches
one column too far, because the escape is two source bytes and one content byte
while the run's end is measured in content. It is re-filed as
`escaped-byte-content-offset` and belongs to Step 8, together with the one
`containment.json` sibling-overlap row that is its second symptom.

##### Two claims in this repository were falsified by measuring them

- **`regression.txt` predicted that both rows of its D18 example would move
  here. Neither did.** The definition's line and the surviving line have the
  same stripped prefix in that input, so the column D18 declined to touch was
  already right. The prose is corrected in place, and the example that *does*
  separate the two rules had to be written -- `"> [a]: /x"` lazily continued by
  `"bar"`, which strips two bytes from the definition's line and none from the
  paragraph's. At the baseline it reports `Paragraph 2:3..2:3` around a `Text`
  at `2:1..2:3`: a paragraph that starts after its own only child.
- **`specs/mdast-parity/deltas.json` said "when this list is empty, Stage 0 is
  done".** §0 and §4.6 both say the opposite and have for longer. The note is
  corrected to say what the backlog measures.

**Gates after.** Every §0 gate green and non-vacuous, with the three presets
rebuilt from scratch first: `correctness` 69/69, `correctness-asan` 60/60,
`correctness-ubsan` 60/60, `conformance` 2/2, upstream 885/885 with 8/8
divergences reproduced, mdast 110/110 with a 6/6 backlog, fuzz 300/300,
scope-sanity 1, inline-sourcepos 0, containment 31, places 79,
reference-order 2 (still red, still Step 9a's), plan graph 22/45,
`audit-ast-projections` green with no canonical `.ast` row moved, `pnpm -w run
lint` clean across C, Swift, Kotlin and ES, and `leaks --atExit` 0. Binding
suites: **Swift** `swift test` and its conformance run green; **ES** node tests
and conformance green against a wasm build from the vendored emsdk at
`.tools/emsdk/4.0.23` -- which is not on `PATH`, and the run fails with
`spawnSync emcc ENOENT` until `emsdk_env.sh` is sourced; **Kotlin**
`:jvmTest` green.


#### 4.14.9a1 Step 9a.1: the definition anchor rule, and a family that moved rather than shrank

**One line, and the rule it makes true is §5.1's.** A footnote definition used
to be added at `parser->first_nonspace + matched + 1` — the byte *after*
`[^label]:` — so it was the one block in this engine that began after its own
marker rather than at its own first byte. `[^footnote]:` alone on a line is
twelve bytes and the definition began at column 13, which is not a column.

```
before   FootnoteDefinition scope=1:13..3:0 id="footnote"
after    FootnoteDefinition scope=1:1..3:0  id="footnote"
```

**Sixteen golden rows moved and the claim is mechanised**: for every
`FootnoteDefinition` in `regression.txt` and `extensions.txt`, the two bytes at
its reported start must be `[^`. **0 of 16 satisfied that before, 16 of 16
after.** No other row moved in either file, and `spec.txt` has no footnote
definition at all. One canonical `.ast` row moved with them
(`specs/canonical-ast/inlines.ast`), which is what `conformance` is for.

**The children did not move, and that is Step 10 paying for itself.** A
definition's body is a child paragraph whose inline positions come from the
content-to-source map, not from the definition's `start_column`, so moving the
definition's anchor nine columns left changed nothing inside it. Before Step 10
this same edit would have dragged every inline in every footnote body with it.

**`internal_offset` on a footnote definition is dead and stays.** It is read in
exactly one place — `markdown_core_parse_inlines`'s block offset — and
`contains_inlines` is false for a `FOOTNOTE_DEFINITION`, so nothing reads it.
It is left alone because deleting it is §5.3's list, not this sub-step's.

**A ledger family closed and the total did not move**, which is worth stating
because the opposite is the natural read. `specs/positions/places.json`'s
`content-start-past-marker-line` was two rows, and both were exactly this
defect. Both starts are now places. Both rows are still registered, because
their *ends* are `zero-column` for an entirely different and unowned reason —
a block that ends at a line ending. **A row in that ledger is one node, not one
fault.** The two rows join `end-at-line-ending` (58 → 60) and the family they
came from is deleted; the total stays 79.

**Mutant.** Restoring `+ matched + 1`: `regression_commonmark` red,
`extensions_gfm` red, **`conformance` 0/2** (both the facade and the CLI dump
of `inlines.ast`), and the places oracle reports **14 rows moving** — the seven
it cleared and the seven it re-registers at the old coordinates.

**Gates.** All of §0 green: correctness 69/69, asan 60/60, ubsan 60/60,
conformance 2/2, upstream 885/885 with 8/8, mdast 110/110 with a 6/6 backlog,
scope-sanity 1, containment 31, places 79, inline-sourcepos 0.


#### 4.14.9a2 Step 9a.2: the definition stays where it was written, and a registry entry nobody read

**Four behaviours changed and they are one rule.** A footnote definition is a
block node where its `[` was written, in the container it was written in, and
nothing runs after the parse that moves, reorders, drops or re-parents it.
`core/blocks.c` loses `process_footnotes` — all three passes, 168 lines — and
`core/footnotes.c` and `.h` are deleted outright. **+144 / −316 across `core/`
and `extensions/`.**

| input | before | after |
|---|---|---|
| `> a[^n] b`, blank, `> [^n]: note`, blank, `tail` | the definition is **hoisted to the document root**, after `tail`, while its own scope still says line 3 | it stays inside the block quote, in document order |
| `[^orphan]: still a definition` | **`Document children=0`** — the whole definition is dropped | a `FootnoteDefinition` with its paragraph |
| `x[^*y*] tail` | one flat `Text "x[^*y*] tail"`; the `Emphasis` the core parser built is **freed** | `Text "x[^"`, `Emphasis`, `Text "] tail"` |
| `[^a]: one` … `[^a]: two` … `see [^a]` | both kept, but emitted in **first-reference order**, so the tree runs 3:1, 5:1, 1:1 | both kept, in source order |

**The map is a set of labels and owns nothing.** `parser->footnote_defs` holds
normalized labels and no nodes, which is the whole difference between it and
the map `process_footnotes` built: that one owned a node per entry and used
registration order as the tie-break for a repeated label, so on `EXIT` a
definition nested inside another closed first, won the label, and the outer one
was freed with everything written in it (D11). **A consequence worth measuring
rather than asserting: registration order now decides nothing.** Moving the
`create` call from the block opener into `finalize` — from open to close, the
exact question D11 turned on — leaves **every suite and every oracle green**.
The code comment says that, instead of implying a gate watches it.

**The call rule is the other half, and it is what makes the failure path
ordinary.** A `[…]` is a footnote call only if it opens with a raw `^` **and
the document defines that label**; the definition set is complete before any
inline is parsed, so "defines" is answered over the whole document. An
undefined label is then an unmatched `[`, which CommonMark specifies
normatively — remove the delimiter-stack entry, emit a literal `]`, touch
nothing inside. §5.7 has the argument and Q2 is the ruling.

**Two golden files moved, 33 rows, and the claim is mechanised.** 25 rows in
`regression.txt`, 8 in `extensions.txt`. The mechanical statement is **document
order**: for every pair of consecutive siblings in every dumped tree, the
second must not start before the first. Over `regression.txt` and
`extensions.txt` that goes **10 violations → 6**, and all four that cleared are
`FootnoteDefinition`. The six that remain are autocompleted table cells at
column 0 — **Q44's family, already carried** — and `spec.txt`'s two are the
same. Nothing else in the repository is out of document order.

**An allocation-failure defect, found by the sweep and fixed here.** The new
definition set is the second map in the engine with a sticky `oom` flag, and
nothing folded it into `parser->oom`. A normalization it could not allocate
answers *"this label is not defined"*, which degrades a footnote call to prose
— and the parse reports success. `regression_fallback_oom_sweep` caught it at
allocation 201 of 431: `quote with footnote` came back as
`quote with footnote[^fn] and `. One `if` at the convergence point in
`markdown_core_parser_finish`, beside the `refmap` one it belongs with.

##### A registry entry nobody read, and the gate that now reads it

**Step 10 registered a second entry for a divergence this repository had
already written down, and every gate stayed green.**
`specs/upstream-parity/deltas.json` carries `pendingDeltas` and
`pendingExpectedDivergences`: entries describing a divergence a reconstruction
step has not created yet, each naming its `pendingStep`, each saying *"that step
is not done until this entry is back and reproducing"*. §4.6 tracks the count.
One of them was `table-split-lead-spelling`, `pendingStep: Step 10`, keyed to
the exact input `pre \\| lead` — and Step 10 did not look, invented
`table-lead-authored-spelling` for the same difference, and left the pending
entry in place. The registry then described one divergence twice.

**Nothing read `pendingDeltas`.** Not `check-upstream-parity.mjs`, not
`check-mdast-parity.mjs`, not `fuzz-parity.mjs` — it was prose. So the gate now
reads it: a pending input that has **started** diverging fails, naming the step
that owes the activation. Verified by putting the entry back into
`pendingDeltas` and re-running — the gate reports
`PENDING divergence ... has started reproducing, so the step that creates it has
landed: Step 10`. The duplicate is deleted, the original entry is activated
under its own id, and its `landed` note records what happened.

**THE CLEAN-UP, and what it found on the other side of the registry.** Merging
the duplicate back was the small half. The registry's PROJECTED entries were
held to no rule at all — an input-keyed divergence has to still reproduce, but a
projection could describe a difference this engine does not have and nothing
would notice. **`reference-definition-node` was doing exactly that**: registered
in `deltas`, implemented by `applyUpstreamReferenceModel`, and acting on **0 of
885 corpus examples**, because this engine produces no `ReferenceDefinition`,
`LinkReference` or `ImageReference` yet. It was describing **Step 9b's model as
though it had landed.** It is in `pendingDeltas` now, marked `projected` and
naming Step 9b, and the gate holds both halves to the same rule:

- an ACTIVE projection that acts on no corpus input fails — *a projection that
  never acts describes a difference this engine does not have*;
- a PENDING projection that starts acting fails, naming the step that owes the
  activation, exactly as a pending input does.

`own-extensions` is the one exemption and it is stated in the code: it is not a
tree rewrite but the **corpus profile** — the extension fixtures run under
`--profile gfm` so the comparison is of one language — so there is nothing for a
normalizer to report. The gate prints `registered projections: 3/3 acted, 1
pending` on every run, so a projection going quiet is visible rather than
assumed. Both checks were proved by mutant: putting the inert entry back into
`deltas` fails, and stopping a live projection from reporting fails.

**`scripts/fuzz-parity.mjs` needed a fragment exclusion, and its comment was
already carrying two defects.** The upstream oracle's `excludeFragments` gains
`"[^"`, for the reason the mdast oracle's list has carried it all along:
whether a footnote call resolves depends on a definition elsewhere in the
document, recombination separates the two by construction, and since this
sub-step an unresolved call keeps its interior where upstream flattens it. The
three fuzz divergences this caused were all that one class. While editing that
comment: a four-line block in it was **duplicated verbatim**, and its `\\|`
clause named `table-split-lead-spelling` — which was, at the time it was
written, a delta this repository did not have. Both corrected.

##### Mutants

| mutant | what went red |
|---|---|
| M11 a call opens on the caret alone, defined or not | `regression_commonmark`, `extensions_gfm`; upstream **884/885**; mdast **109/110** |
| M12 a definition registers when it CLOSES, not when it opens | **NOTHING**, and that is the finding above |
| M13 the definition set's allocation loss does not converge | `regression_fallback_oom_sweep` and `..._chunked` |
| M10 (9a.1) the definition starts after its own marker | `regression_commonmark`, `extensions_gfm`, `conformance` 0/2, places 14 rows |

##### What 9a does NOT close, and one re-attribution

**D9 moves to Step 9b, and this is a correction to this document rather than a
deferral.** Four places say 9a — §2's index, §4.8, §4.0's *"which is Step 9a's
model change"* and the 25-line comment at `core/map.c` — and two say 9b: the
requirement row (*"The map holds no resource, so D9's expansion budget has
nothing to charge and is deleted"*) and §4.12's paragraph on the discard-and-retry
double charge. **The two are right, and the reason is in §4.0's own sentence:**
*interning the destination has no owner at the baseline that outlives the
refmap.* The refmap is freed in `markdown_core_parser_dispose`; the document
outlives it and holds only `root`; so a `Link` that borrows its destination
from a map entry dangles. Deleting the copy therefore requires either giving
the map a new owner — which 9b then deletes — or the reference holding **no**
destination, which is 9b's `LinkReference`. There is no 9a-shaped fix, and
building the ownership transfer would be two implementations of one fact, which
is the disease Q22 names. Both of D9's gates stay exactly as 0a.8 left them:
`audit-reference-order-independence.mjs` registered red with two rows, and
`reference_expansion_bound` green at 0.999x.

**D30 stays carried and its owner is unchanged** (9b/11c delete the mechanism;
the §4.13.9 allocation sweep pins it). It is worth noting that the sweep is not
a formality: it is what caught the new defect above, in the same run.

##### Standing rule 2

**Backlog 6 → 5, and the entry that closed was filed under the wrong step.**
`specs/mdast-parity/corpus.md:69` — `[^orphan]: still a definition` — was
registered `closedBy: Step 9b`. §4.6 said it belonged to 9a and §2's progress
meter said the JSON was right. **§4.6 was right**, which the entry proved by
closing here. The five that remain are all Step 9b's.

**Gates.** correctness 69/69, asan 60/60, ubsan 60/60 (three presets rebuilt
from scratch), conformance 2/2, upstream 885/885 with **10/10** divergences,
mdast 110/110 with a 5/5 backlog, fuzz 300/300, scope-sanity 1,
inline-sourcepos 0, containment 31, places 79, reference-order 2 (still red,
still 9b's now), plan graph 22/45, source lists **22 sources** (was 23;
`footnotes.c` is gone from all four live lists), `leaks --atExit` 0, `pnpm -w
run lint` clean, and the Swift, Kotlin and ES binding suites green.


#### 4.14.11a Step 11a: the concrete record set, and the law the owner added

**The deliverable.** A parse now produces, beside the tree, a record set in
which **every byte of the normalized source is in exactly one region**, each
region with exactly one owner and one of three roles — `MARKER` for the bytes
that made the owner what it is, `CONTENT` for the bytes that went into its
content buffer, `DISCARDED` for the bytes it read and kept nowhere. The parser
retains the normalized source and its line index, because a region is a byte
range in **that** and not in whatever buffer the caller fed.

```
$ printf '> foo\n> bar\n\n# head #\n' | markdown-core --concrete
concrete source=22 lines=4 regions=8
region  0 2 MARKER    0.0   block_quote   "> "
region  2 4 CONTENT   0.0.0 paragraph     "foo\n"
region  6 2 MARKER    0.0   block_quote   "> "
region  8 4 CONTENT   0.0.0 paragraph     "bar\n"
region 12 1 DISCARDED 0     document      "\n"
region 13 2 MARKER    0.1   heading       "# "
region 15 4 CONTENT   0.1   heading       "head"
region 19 3 DISCARDED 0.1   heading       " #\n"
```

**THE FOURTH LAW, and it is the owner's.** §11.5/11.7 narrow Stage 1 to *"make
the tree readable at a line boundary, without ending the parse and without
paying the document"*, and 11a's three published laws constrain the record
set's **result**, not when it is built — so a close-time construction would
satisfy all three and recreate the quadratic cheat one level down. The fourth
law says it cannot:

> **L4. The concrete records are complete for lines 1…N once line N has been
> fed.**

**One mechanism satisfies all four, and two of them by construction.**
`S_claim_region` attributes the line in hand up to a byte offset and moves a
cursor there. The cursor only moves forward, every claim starts where the last
one ended, and the end of `S_process_line` sweeps whatever is left. So **L1**
(the regions tile the line) is the statement that nobody bypassed the cursor;
**L3** (concatenation reproduces the source) follows, because every region is a
range *of* the source; and **L4** holds because there is nowhere else to claim
from. The gate checks all four anyway — *by construction* is a property of
today's code, and the gate is what makes it a property of tomorrow's.

**Measured: L1 and L3 have no rows at all.** 2,746 regions over 885 examples,
zero gaps, zero overlaps, zero shortfalls. **L4 has eleven**, over 1,200
line-boundary prefixes, and all eleven are one fact (below).

##### Three attribution rules, each of them measured before it was written

The first reading of the gate was **139 rows**. Three rules took it to 45, and
each one is a claim about who a byte belongs to rather than a fix to a symptom:

| rule | what it replaced | rows |
|---|---|---|
| indentation ahead of an opener belongs to the **container**, not to the block being opened | an indented code block's four spaces were its own `MARKER` and began four bytes before its own scope | **−52**, plus 28 list items and more |
| indentation stripped ahead of a line's content belongs to the container that stripped it | it was the block being written into, and on that block's first line it preceded the block's start | **−11** |
| L2's second clause — *descendants lie inside their ancestor's `CONTENT`* — is **11b's** | applied to BLOCKS it reported 78 rows, every one a `table_row`, because a block child of a block is not inside its parent's content in any sense a block partition can express: a block quote has no content buffer, and a table's content regions are the paragraph's from before it was retyped | **−78** |

The wide form of the third was measured before it was narrowed, which is the
rule §0 asks for. The clause is about **inline** regions inside the block whose
content they were cut from, and there are none until 11b.

##### Two defects found by building it

- **A region naming a freed node**, which is a map owning a node (D11) one
  indirection further out. The formula extension replaces a fenced code block
  whose info line says `formula` with a formula block and frees the old node;
  every region naming it dangled, and `--concrete` under ASan reported the
  use-after-free on `$$x+y$$`. `markdown_core_parser_transfer_regions` is how a
  replacement says the bytes moved with the construct, and the same mechanism
  hands a destroyed paragraph's bytes to its parent.
- **A latent defect in Step 10's own rebase.** `S_rebase_content_marks` chose
  the last mark at or before the cut, which is right unless the cut takes
  *everything* — and then it kept that mark and advanced its column past the
  end of its own line. A paragraph of nothing but reference definitions
  therefore reported a `start_line` on the last line it consumed, and through
  that, the disown scan started below the regions it was meant to find and left
  them naming a freed node. It takes a `remaining` argument now. **Nothing
  observed it before**: the node is freed on that path, so the wrong start_line
  reached no golden.

##### What it costs, measured rather than estimated

| | before | after |
|---|---|---|
| peak RSS, 2.3 MB input | 91,799,552 B | **100,335,616 B, +9.3%** |
| `bench large_document@128 / @256 / @512` | 19.989 / 40.240 / 80.192 ms | 22.001 / 43.579 / 86.314 ms, **+10.1% / +8.3% / +7.6%** |
| `bench deep_nesting@32768` | 1.900 ms | 2.025 ms, +6.6% |
| `bench extensions@400` | 8.681 ms | 8.997 ms, +3.6% |

**This is the only step so far whose cost is above the machine's own noise, and
it is the deliverable rather than an accident**: the memory is the normalized
source the requirement says the document retains, plus one 24-byte region per
claim and four bytes per line. Per line the work is one buffer append and a
constant number of claims — nothing re-walks anything (§3).

##### The forty-five rows, and the eleven

`specs/concrete/records.json` carries them in six families with the ledger's
usual rule: `class` is analysis, `closedBy` is measurement, and a family whose
owner is unassigned is one where the step that moves the rows proves the
attribution by moving them. Three families name **11c** — a paragraph destroyed
for holding only definitions, the lines a harvest consumed, and the table lead's
content — and all three are the same underlying fact: **a definition does not
yet own its bytes.** That is precisely 11c's requirement, and it is the whole of
L4's exception: whether `[foo]: /url` is a destroyed paragraph or a surviving
one depends on whether a later line follows it, so those bytes' ROLE changes
after their line was fed. Not a record created late, moved, or deleted — and
11c deletes the case.

##### Gate and mutants

`scripts/audit-concrete-records.mjs`, in §0's list and in `scripts/dev/gates.sh`.

| mutant | what went red |
|---|---|
| M14 the line's remainder is never swept | concrete records: **117 `L1 gap` rows and 18 `L3 does-not-reproduce-source`** |
| M15 indentation ahead of an opener is the new block's | **96 `L2 region-before-owner` rows** |
| M16 a replaced block does not hand over its records | 3 `L2 owner-scope-is-not-a-place` rows, **and AddressSanitizer reports a heap-use-after-free** |

**None of the three moves a single golden row**, which is the point: the record
set is beside the tree, and no oracle that reads the tree can see it.

**Gates.** correctness 69/69, asan 60/60, ubsan 60/60, conformance 2/2, upstream
885/885 with 10/10, mdast 110/110 with a 5/5 backlog, fuzz 300/300,
scope-sanity 1, inline-sourcepos 0, containment 31, places 79, **concrete
records 45**, reference-order 2, plan graph 22/45, source lists 22, formatters
and linters clean, and an ASan sweep of `--concrete` over every corpus example.


#### 4.14.8a Step 8.1: an inline position is a projection, and the first row where this side is right

**One function replaced the arithmetic.** Every inline maker already took two
byte OFFSETS into the block's content buffer and turned them into columns by
addition — `offset + 1 + column_offset + block_offset` — which is right only
while the whole block is one line beginning where the block does.
`S_place_inline` asks requirement 10's map twice instead, once per end. That is
the model the requirement states: *a position is a projection of the byte range
the node covers, not a counter each handler maintains.*

Everything the old arithmetic needed a correction term for is answered by
asking twice: a span that crosses a line ending, a continuation line with a
different stripped prefix, a construct whose end is on a later line than its
start.

**Ten golden rows moved, and seven of them close a defect this document names
and assigns to Step 8.** `specs/positions/places.json`'s closing note says it
outright:

> `a <b`, newline, `c> d` gives `HTML scope=1:3..2:1` for a literal whose last
> byte is at `2:2`, so the closing byte belongs to no node — and `2:1` is a
> perfectly good place. … it belongs to Step 8.

It now reads `1:3..2:2`. **Mechanised: of the seven multi-line raw-HTML nodes in
`spec.txt` and `regression.txt`, ZERO had a scope end that named their literal's
last byte before, and SEVEN do after.** Every one of the seven was losing its
closing `>` to no node at all.

**`specs/positions/inline-sourcepos.json` goes 0 → 9, and that is the point of
it.** The oracle compares every inline position against cmark-gfm's and read
zero for the whole of Stage 0a — including on the rows that were wrong, because
this engine agreed with upstream about them. Its value was always that it would
move the moment the two stopped agreeing; what nobody had said is that the first
such moment would be an improvement. Six rows are the raw-HTML closing byte,
where upstream still loses it. **A row there is a disagreement, not a defect,
and each one's `closedBy` says which.**

**Three rows are a lateral move and are recorded as one.** A code span crossing
a line ending now ends at the end of the line its last byte is on — `1:3..2:4` —
where it used to say column 0 of the line after — `1:3..3:0`. Both are outside
`audit-position-places.mjs`'s rule; the new one names the right line. Three rows
move out of `end-at-line-ending` and into `multi-line-span-ends-at-a-line-ending`
and the total stays 79, which is the same shape 9a.1 recorded and is recorded
here for the same reason.

##### One thing was built, measured and NOT taken

**A code span's extent is its CONTENT and not its construct**, so `` ``foo`` ``
begins at the byte after its opening ticks — a column that does not exist on a
two-byte line, and the nine `unmatched-code-span-literal` rows in
`places.json`. Emphasis covers its asterisks and a link covers its brackets; a
code span is the one construct that does not cover its own delimiters.

Covering them was built and measured, and **the first reading of that
measurement was wrong; §4.14.8c corrects it.** It clears **three** `places`
rows — the multi-line code spans — and **none** of the nine, which are `Text`
nodes and a different defect. It moves **thirty-seven** rows in
`inline-sourcepos.json`, because upstream reports the content extent for every
code span and not only the multi-line ones. That is a ruling about what a node
covers, in the shape Q40 took, and it is not a side effect of the projection.
**Q45**, and the measurement is written above the line in `handle_backticks` so
the next reader does not re-derive it.

##### What is not done yet

`adjust_subj_node_newlines` and `count_newlines` still exist, and so do
`subj->column_offset` and `subj->block_offset`: they are the FALLBACK, reached
by a block whose content was SET rather than fed — a table cell, the paragraph a
table was split out of, a directive label. Those have no marks to project
through. The node repair inside `adjust_subj_node_newlines` is already
conditional on the projection being unavailable, because with the projection in
place it counted the newlines a second time — measured, and it is what a
two-line code span reported the moment `make_literal` started projecting. The
four extensions still build positions from `get_line` + `get_column` and do
line-local arithmetic on the column. **8.2 gives the synthesized-content blocks
marks and deletes all of it.**

**Gates.** Every §0 gate green: correctness 69/69, asan 60/60, ubsan 60/60,
conformance 2/2, upstream 885/885 with 10/10, mdast 110/110 with a 5/5 backlog,
fuzz 300/300, scope-sanity 1, containment 31, places 79, concrete records 45,
**inline-sourcepos 9**, reference-order 2.


#### 4.14.8b Step 8.2: the counters cease to exist, and nothing moved

**`subj->block_offset`, `subj->column_offset`, `adjust_subj_node_newlines` and
`count_newlines` are gone**, along with the line-and-column bookkeeping inside
`markdown_core_inline_parser_set_offset`. So are the two hand-written
delimiter-run constructors. **Step 8 in total: +304 / −173 across `core/` and
`extensions/`**, against the requirement's estimate of +330 / −245.

**NOT ONE GOLDEN ROW MOVED, and not one ledger row moved with them.** places 79,
containment 31, inline-sourcepos 9, concrete records 45, scope-sanity 1, all
identical before and after. That is the strongest available evidence that the
projection reproduces the arithmetic exactly wherever the arithmetic was right —
8.1 had already moved every row where it was not.

**What made the deletion possible is one line.** The counters survived 8.1
because a block whose content was **set** rather than fed has no marks to
project through — a table cell cut out of a row, a directive's label, the
paragraph a table was split out of. `markdown_core_parse_inlines` now gives any
such block one mark before parsing it:

```c
markdown_core_parser_mark_content(parser, parent, parent->start_line,
                                  parent->start_column + parent->internal_offset);
```

**That derivation IS the term it replaces.** `block_offset` was
`start_column - 1 + internal_offset`, applied to every offset in the block;
saying it once as a mark is what lets the term go. Two blocks needed more than
the default:

- the **split-off table lead**, whose content is a slice of the paragraph's and
  can be several lines long, so it ADOPTS the marks for those lines
  (`markdown_core_parser_adopt_content_marks`) — copied, never shared, because
  two nodes naming one run is an alias and an alias between two trees is the
  shape §1 records six times;
- **table cells**, which take one mark at the place their first content byte was
  written, read out of the row's own map for a header cell and out of the line
  in hand for a body cell.

**One residue, and it is pinned by a golden already.** A cell's content is
`unescape_pipes`'d, so it is one byte shorter per `\|` than the source it came
from, and one mark cannot say that: after an escaped pipe a cell's inline
positions are short by the number of dropped backslashes.
`extensions.txt` pins it — `Text scope=3:3..3:27` for a 27-byte source span —
and it is a REFINEMENT of one region into several, which is 11b's vocabulary
rather than this step's.

**One constructor for a delimiter run.** `formula` and `strikethrough` each had
their own, and they disagreed about where the cursor was when they ran, so each
computed the run's columns from a different end — `formula` from the run's first
byte, `strikethrough` from one past its last.
`markdown_core_inline_parser_make_delimiter_text(parser, from, to)` is told the
range instead. `strikethrough` also loses a 101-byte stack buffer it was
filling with `~` characters only to copy them back out as the literal; the
literal is a slice of the block's own content and needs no copy at all.

**Mutant.** Removing the default mark — a block whose content was SET gets no
map — fails `extensions_directive` and `extensions_conflicts` and moves a
containment row.

**Gates.** Every §0 gate green and every ledger unmoved: correctness 69/69,
asan 60/60, ubsan 60/60, conformance 2/2, upstream 885/885 with 10/10, mdast
110/110 with a 5/5 backlog, fuzz 300/300, scope-sanity 1, inline-sourcepos 9,
containment 31, places 79, concrete records 45, reference-order 2.


#### 4.14.9b0 What Step 9b actually costs, measured before it was started

**Scoped, not landed.** This is here because §0 says everything needed to pick
the work up cold lives in this file, and because the shape of 9b is not what
its requirement row implies.

**Three node kinds, and a node kind is forty-five files.** Counted by naming
one that already exists: `FootnoteDefinition` appears in **45 files** across C,
Kotlin, ES and Swift — the type enum, the facade kind enum, the C dump, the
wire kind enum and decoder in two languages, four dumpers, four visitors, four
models, the TypeScript `dist/*.d.ts` that ship beside the sources, the canonical
manifest, the contract JSON, the contract prose and the dump grammar. Standing
rule 4 requires all of them in ONE commit with the regenerated `.ast` goldens.
`scripts/audit-ast-projections.mjs` checks **twelve** of those surfaces by name,
which is what makes the count trustworthy rather than a guess.

**So 9b splits in two, and each half is a standing-rule-4 commit on its own:**

- **9b.1 — the definition is a node.** One new kind, `ReferenceDefinition`,
  carrying `label`, `destination` and `title`. References still resolve to
  `Link`/`Image` with the destination copied in, so nothing else moves. This is
  also what closes **11c's** half and **the whole of L4's exception in
  `specs/concrete/records.json`** — the eleven rows there are one fact, that a
  paragraph holding only definitions is destroyed, and a definition that owns
  its own bytes is never destroyed.
- **9b.2 — the reference is a node.** `LinkReference` and `ImageReference`
  carrying the association and the form, no destination; the map loses its
  resources; **D9's budget has nothing left to charge and is deleted**, which is
  the whole of D9's fix and the reason §4.14.9a2 re-attributed it here.

**Three things are already in the tree and were not obvious.**

1. `scripts/lib/upstream-cmark.mjs` already carries
   `applyUpstreamReferenceModel`, which collects `ReferenceDefinition` nodes,
   drops them, and resolves `LinkReference`/`ImageReference` against them before
   comparing. **The upstream gate is already written for the post-9b world** —
   era skew from Step 0's `scripts/` restore, in this repository's favour for
   once. The delta `reference-definition-node` is registered and describes the
   target state in the present tense.
2. It also fixes the **vocabulary**: `ReferenceDefinition` has `label`,
   `destination` and `title`; a reference has `label`. Q5's rename of the
   footnote `id=` to `label=` belongs with it.
3. **There is no free core BLOCK type value below the extension range.** Core
   runs to `0x000a` and `table`/`formula`/`directive` occupy `0x000b`–`0x000f`,
   so a new core block kind takes `0x0010`. The internal type value is NOT the
   wire ordinal — `WireKind` numbers the FACADE's kinds — so the gap costs
   nothing but must be written down, because the natural assumption is that the
   next value is free.

**What 9b does NOT need.** No concrete record work: every byte it stores is
available at parse time, which is §4.1.4's struck `9b → 11a` arrow, and it still
holds. Its dependencies — 9a and 10 — are landed.


#### 4.14.8c Step 8.3: one line, ten rows, and a correction to 8.1's own measurement

**A claim in §4.14.8a was wrong and this sub-step is where re-measuring it found
the defect it was wrong about.** 8.1 recorded that covering a code span's
backticks *"clears three `places` rows and every one of the nine"*. It clears
three. The nine `unmatched-code-span-literal` rows are `Text` nodes, not `Code`
nodes, and they were a different defect entirely — which is what looking at them
one at a time showed, and what the first reading had not done. The wide-rule
measurement is also corrected: **thirty-seven** inline-sourcepos rows move under
that change, not thirteen.

**The defect, and it is one line.** An UNMATCHED backtick run stands as its own
literal, so it covers its own bytes. It took both offsets from `subj->pos` —
**one past the run** — so the literal was placed one column right, and
consolidation then carried that end onto the whole merged text run:

```
`hi`lo`          seven bytes
before   Text scope=1:5..1:8 literal="lo`"      column 8 does not exist
after    Text scope=1:5..1:7 literal="lo`"
```

**Ten registered rows cleared and none appeared**, in two ledgers:

| ledger | before | after |
|---|---|---|
| `specs/positions/places.json` | 79 | **69** — `unmatched-code-span-literal` (9) and `escaped-byte-content-offset` (1) both leave, and the second was never the escape class |
| `specs/positions/containment.json` | 31 | **21** — the same ten, which reached one column past their own paragraph |

**A second mis-attribution went with it, and it was mine.** Step 10 refiled the
one `continuation-line-content-offset` row that did not clear as
`escaped-byte-content-offset`, reasoning that a backslash escape is two source
bytes and one content byte. That reasoning was plausible and wrong: the row is
`` `not code` `` on a line beginning with a backslash, and it is this defect.
**The ledger recorded a guess in the field that is supposed to hold a
measurement**, and the correction is written into `places.json` where the guess
was.

**Fifteen golden rows moved and the claim is mechanised.** Fourteen in
`spec.txt`, one in `extensions-directive.txt`, every one a `Text` whose literal
begins or ends with a backtick. Of the eight that are single-line verbatim
literals the checker can judge, **8 went from disagreeing with their own source
bytes to agreeing and none went the other way** (978 → 986 agreeing). The other
seven carry escapes or entities the check excludes by construction.

**Mutant.** Restoring `subj->pos` for both offsets moves ten `places` rows and
fails `spec_commonmark` and `extensions_directive`.

**What this says about the ledgers.** `closedBy` is measurement and `class` is
analysis, and the analysis was wrong twice here — once about which step owned
the nine (right: Step 8) and once about what caused them (wrong: not the
delimiters, the placement). Both were caught by the same thing: a step landing
and the rows NOT moving.


#### 4.14.8d Step 8.4: Q45 answered — a code span covers its backticks

**Owner ruling, 2026-08-23:**

> *"For Q45, it's definitely a defect or bug inherited from cmark. It should
> cover backtick according to all the other inline element rules, right?"*

**Yes, and the rule the ruling appeals to is visible on one line:**

```
*bar* and [a](/u) and `foo` and ~~s~~
├── Emphasis      1:1..1:5     covers *bar*     — asterisks included
├── Link          1:11..1:17   covers [a](/u)   — brackets and parens included
├── Code          1:24..1:26   covers foo       — backticks at 23 and 27 owned by NOBODY
└── Strikethrough 1:33..1:37   covers ~~s~~     — both tilde pairs included
```

`make_code` is now given the construct's range instead of its content's. The
same line now reads `Code scope=1:23..1:27`.

**Fifty-five golden rows moved and EVERY ONE OF THEM IS A `Code` NODE** — 34 in
`spec.txt`, 12 in `extensions.txt`, 9 in `regression.txt`, plus one each in
`smart_punct.txt`, `extensions-formula-conflicts.txt` and
`specs/canonical-ast/inlines.ast`, and one assertion in `api_engine`. Zero rows
of any other kind moved, which is the cleanest statement available that the
change touches exactly the construct it names.

**Mechanised: the source bytes at a `Code` node's own scope must start and end
with a backtick. 0 of 55 before, 51 of 55 after.** The four that remain are all
code spans inside a table cell with an escaped pipe — the residue §4.14.8b
already names and pins, where the cell's content is `unescape_pipes`'d and so is
one byte shorter per `\|` than the source it was cut from. **The checker had to
index the source in BYTES to say this**: a no-break space is one character and
two columns, and a character-indexed first draft reported two false failures on
`` `\u00a0` ``.

**Ledgers.**

| ledger | before | after |
|---|---|---|
| `specs/positions/places.json` | 69 | **66**, and it is down to TWO families — `multi-line-span-ends-at-a-line-ending` is gone, because a code span that crossed a line ending used to end AT the line ending (its content's last byte was the newline) and now ends at its closing tick, which is a place |
| `specs/positions/inline-sourcepos.json` | 9 | **40** — 34 `Code` rows added, 3 cleared |

**The inline-sourcepos ledger going up is the ruling, not a regression.** It
compares this engine's inline positions against cmark-gfm's, it read zero for
the whole of Stage 0a — *including on the rows that were wrong* — and every one
of its forty rows is now a row where this side is right and upstream is not.
Thirty-four are this ruling; six are Step 8.1's raw-HTML closing byte. Each
row's `closedBy` says which side is right.

**Q45 is answered and closes.** Q40 stands unchanged: a line ending is a place
only for a node that IS one, and no code span sits on one any more.


#### 4.14.9b1 Step 9b.1: the definition is a node, and the role its bytes carry is forced by L4

**A link reference definition is a block node at the byte where its `[` was
written, in the container it was written in, and it owns every byte it read.**
One new kind, `ReferenceDefinition`, carrying `label`, `destination` and
`title`. References still resolve to `Link`/`Image` with the destination copied
in, so nothing else moves — 9b.2 is what makes the reference a node and takes
D9's budget with it.

```
before   Document scope=1:1..3:5 children=1
         └── Paragraph scope=3:1..3:5 …
after    Document scope=1:1..3:5 children=2
         ├── ReferenceDefinition scope=1:1..1:11 label="foo" destination="/url" title=null children=0
         └── Paragraph scope=3:1..3:5 …
```

**Eighty-one golden examples moved and the claim is mechanised.** 78 in
`spec.txt` (85 new rows), 2 in `regression.txt`, 1 in `extensions.txt`. The
mechanical statement is that **every one of them differs from its old text by
added `ReferenceDefinition` rows and by nothing else**: strip the
`ReferenceDefinition` lines from the new dump, normalise the tree-drawing
prefixes and the `children=` counts, and the result is byte-identical to the old
dump for **81 of 81**. One canonical `.ast` row moved with them
(`specs/canonical-ast/blocks.ast`), which is what `conformance` is for, and its
`.md` gained the definition **ahead of** the footnote definition so the
footnote's own scope did not move with it.

##### THE ROLE A DEFINITION'S BYTES CARRY IS FORCED BY L4, and this is the finding

The harvested bytes keep the role they already had — `CONTENT` for the line's
text, `DISCARDED` for a continuation line's stripped indent. Only the OWNER
changes. That reads like a stylistic choice and it is not one:

> A prefix of the document that stops before the destination reads `[foo]:` as
> ordinary paragraph CONTENT. The whole document reads the same bytes as a
> definition. If becoming a definition changed the role, every such prefix
> would attribute those bytes differently from the whole — which is exactly
> what L4 forbids.

Measured. **Mutant M16** makes a definition's own bytes `MARKER`, which is the
reading §4.14.11a's role vocabulary invites (*"the bytes that made the owner
what it is"*). `correctness` 69/69, `conformance` 2/2, upstream 887/887 — and
**six L4 rows appear**, every one of them an input whose first line is not yet a
definition (`[foo]:` with the destination on the next line, `[Foo bar]:` with an
angle-bracket URL below, a title that opens on one line and closes on another).

##### `specs/concrete/records.json`: 45 rows to 28, and NOTHING appeared

Both definition families closed, and the second one was 11c's:

| class | law | rows | was owned by |
|---|---|---|---|
| `definition-paragraph-destroyed` | L4 | **11 → 0** | 11c |
| `consumed-definition-line` | L2 | **6 → 0** | 11c |

Seventeen findings cleared over eleven inputs, none appeared. **A definition
that owns its own bytes is never destroyed**, so nothing hands a paragraph's
regions to its parent as `DISCARDED` any more, and the paragraph that survives
a harvest no longer has a region starting twelve bytes before its own scope.
§4.14.9b0 predicted the L4 half; the L2 half came free with it because both were
one fact.

**What did NOT close, and it is not this step's.** A paragraph whose first
SURVIVING line is indented still owns that line's indent, and its scope now
begins after it — one `region-before-owner` of the pre-existing
`indent-after-container-closed` family, which is unowned and 8 rows. Before this
step the same input had FOUR such regions rather than one, so 9b.1 shrinks the
class without closing it. No corpus input reaches it.

##### The region surgery: one pass, one move, and a segfault a corpus gap hid

A run of unindented definitions is a **single region** — `S_claim_region`
extends rather than appends when owner, role and cursor line up — so the run has
to be cut into as many pieces as there are definitions. Cutting once per
definition would move the tail of the region array once per definition, which is
quadratic in a paragraph with many definitions and many regions after them, and
that is off-model (§3). So the pieces are counted, the array is grown once, the
surviving rows are **right-aligned inside the window they will occupy**, and the
pieces are written forward over them; the write pointer can never overtake the
read pointer because every source row produces at least one piece.

`lo` is found by bisection rather than by scanning from row zero, for the same
reason: a paragraph that opens with a definition would otherwise cost the whole
document.

**The boundary rule.** A definition takes the bytes from its own opening bracket
to the end of the last line it read. The indentation between one definition's
last line ending and the next one's bracket goes to the block's **PARENT** —
which is where the FIRST line's indentation already went, before the block
existed to claim it. Giving it to either definition puts a region outside its
owner's scope.

##### Two corpus gaps, both found by mutant and both closed with a fixture row

| mutant | what it does | before the new rows | after |
|---|---|---|---|
| **M17** the indent before a definition goes to that definition | region starts 2 bytes before its owner | **NOTHING** — 69/69, every ledger holds | `audit-concrete-records.mjs` reports one `region-before-owner`; `correctness` still 69/69 |
| **M18** drop the right-alignment memmove | the forward fill reads rows it has overwritten | **NOTHING** — 69/69, 0 rows appear, and `--concrete` **SEGFAULTS** on an input no fixture had | `audit-concrete-records.mjs` exits 1 |

Both gaps are the same shape: *no fixture had two link reference definitions in
one paragraph.* Every multi-definition case in this repository indents at least
one of them, and an indent breaks the merge, so the arithmetic that cuts a
merged region had no witness at all. Two examples in `regression.txt` close
them, and both add **zero** ledger rows.

**M18 is killed by `audit-concrete-records.mjs` and by nothing else, for a
structural reason**: the region set has exactly ONE reader today — the CLI's
`--concrete`. A corrupted region array is invisible to the facade, to both
parity oracles and to every golden, because nothing else ever dereferences a
region's owner. That is requirement 12's job and it is worth writing down now.

##### D30's node half is closed, and the sweep could not see it until it was told

**Q7 and Q26: the destination is REQUIRED.** An allocation that loses the
destination or the title now fails the parse instead of producing a definition
that lies about where it points, and the two paths that cannot place a
definition set the failure bit rather than dropping the node while the reference
map still resolves the label.

**`fb_node_payload_equal` compares payloads only for the types it lists, and
returns 1 for everything else.** A new kind added without an arm is therefore
compared as *equal*, and the allocation-failure sweep — the gate §4.13.9 exists
for — passes on a wrong tree. Measured, and this is not a hypothetical:

| mutant | result |
|---|---|
| **M19** a definition commits a destination or title that was lost | `regression_fallback_oom_sweep` and `..._chunked` RED |
| **M19 + the comparator arm disabled** | **69/69.** Every suite green on a document with an empty destination and `parser->oom` clear |

The arm is thirteen lines and it is the only thing in the repository that sees
it.

##### The dump grammar had drifted three ways, and nothing read it

`docs/specs/canonical-ast-dump.md`'s field-order table is a **third copy of the
contract** — after `canonical-ast.json` and the prose table in
`canonical-ast.md` — and `audit-ast-projections.mjs` read the other two and not
this one. Measured against the engine:

- a `mode` on `CodeBlock`, `Code`, `DirectiveBlock` and `Directive`, which
  **Q29 deleted at 15A.4**;
- a `label` on the two directive kinds, plus a paragraph calling it *"a scalar
  presence field"*, which stopped being true when **Step 7** made the label a
  node — and the doc's own worked example printed `mode=embedded … label=1`,
  which this engine has not emitted since;
- **no row for `DirectiveLabel` at all.**

All three fixed, and the table is now checked kind for kind and field for field
against the contract, in order. Proved by mutant twice: taking `mode` off
`Formula` and taking `DirectiveLabel` out of the no-fields row each make the
audit exit 1. This is the same hole 15A.3 closed for the kind surfaces, one
document further out.

##### Two gates §4.8 names were not in `scripts/dev/gates.sh`

`audit-ast-projections.mjs` and `audit-source-lists.mjs` are both on §4.8's
checklist and neither was in the script that claims to run every gate. Both
added; the header still says the binding suites are not there.

##### The numbering gap, and the boxing, both measured

Core block types run to `0x000a` and `table`/`formula`/`directive` occupy
`0x000b`–`0x000f`, so `MARKDOWN_CORE_NODE_REFERENCE_DEFINITION` is `0x0010`.
`tests/api/main.c`'s `node_type_values` already asserts that the *n*-th block
type has value *n*, so the gap is gated by a test that existed before it.

**The payload is boxed, and §5.8's cost argument is now a measurement on this
tree**: `chunk` 16, `link` 32, `code` 40 — the widest arm `node.as` has — and
three chunks are 48. Compiled both ways: the node is **168 bytes boxed and 176
inline**, so storing it inline would cost **8 bytes on every node in the
document** to carry a payload that appears once per definition.

##### The upstream projection was already written, and activating it is the whole edit

`applyUpstreamReferenceModel` has been in `scripts/lib/upstream-cmark.mjs` since
Step 0's `scripts/` restore, and 9a.2 moved its delta into `pendingDeltas`
because it acted on **0 of 885** corpus examples. It now acts on 78 of them, and
the gate 9a.2 added said so before anything else did:

```
upstream parity FAILED: PENDING projection `reference-definition-node` has started acting,
so the step that creates it has landed: Step 9b - one reference model, the node model.
```

The entry is back in `deltas`, its `landed` note records that only the
definition half has landed, and the run prints `registered projections: 4/4
acted`. **No second entry was written**, which is the mistake §4.14.9a2 was
about.

##### Standing rule 2: the backlog does not move, and that is on purpose

**5/5 still diverging, all five Step 9b's.** Every one of the five is a
`Link`/`LinkReference` disagreement, not a definition one — verified by reading
the tree for each: `[ref]: /r "T"` now produces exactly mdast's `definition`
node, and what remains is `Link destination="/r"` where remark has
`linkReference label="ref" form="full"`. 9b.2 deletes all five. A sub-step that
closes none of its parent step's entries is not a violation of the rule; landing
9b with them still listed would be.

##### Gates

`correctness` 69/69, `correctness-asan` 60/60, `correctness-ubsan` 60/60 (both
sanitizer trees deleted and reconfigured first), `conformance` 2/2,
`canonical-ast` **30 kinds, 51 fields, 6 cases**, `ast-projections` 30 kinds over
12 surfaces, `source-lists` 22 sources, `public-surface`, `special-chars`,
`attach-order`, `plan-graph` 22/45, fuzz 300/300, upstream **887/887** with
10/10 divergences and **4/4** projections, mdast 110/110 with a 5/5 backlog,
scope-sanity 1, inline-sourcepos 40, containment 9, places 57, **concrete
records 28**, reference-order 2 (still red, still 9b.2's), `test-topology`,
`format-c`, `format-cmake`, `lint-c`, `pnpm -w run lint`, `leaks --atExit` 0 over
`regression.txt` + `spec.txt`, and the Swift, Kotlin and ES suites green.
`scripts/format-swift.sh --check` is **164 findings, unchanged** — the new Swift
type documents every public declaration it adds.


#### 4.14.9b2 Step 9b.2: a reference names its definition, and D9 closes

**The other half of §5.1.** `LinkReference` and `ImageReference` are nodes
carrying an association and the form they were written in, and **no
destination**. The destination is stated once, at the definition. Five kinds
now carry the association — those two, `ReferenceDefinition`,
`FootnoteDefinition` and `FootnoteReference` — and the dump speaks one
vocabulary for all five (Q5: `label=`, never `id=`).

```
before   Link scope=3:5..3:15 destination="/r" title="T"
after    LinkReference scope=3:5..3:15 label="ref" identifier="ref" form=full
```

##### D9 IS CLOSED, and both of its gates changed state in the same commit

`markdown_core_map_lookup` carried a running budget — `max(100000, input size)`
bytes summed over successful lookups — because resolving a reference COPIED the
definition's destination and title into the node. The budget was the defect: it
made **whether a reference resolves depend on how many resolved before it**, and
the contamination crossed labels. Deleting it alone was measured at **204.678x**
output growth and was never the fix.

A reference that names its definition copies nothing, so there is nothing to
charge:

| gate | before | after |
|---|---|---|
| `audit-reference-order-independence.mjs` | **REGISTERED RED**, 2 rows | **GREEN, 0 rows** — and still fail-closed: M23 puts a budget back and both rows re-appear |
| `reference_expansion_bound` | 0.999x | **0.399x** |

The bound gate had to be rewritten or it would have passed by measuring nothing:
it counted `link_properties`/`image_properties` bytes, and after this step a
reference has none, so it would have reported **0.000x** on a model it no longer
describes. It now counts the association on every reference kind as well, which
is what the tree actually stores.

**`markdown_core_reference` is gone.** Both maps are label sets with one free
function between them; `markdown_core_map_entry` loses `size`, the map loses
`ref_size` and `max_ref_size`, `chunk_clone` in `core/inlines.c` has no caller
left, and `core/blocks.c` loses the eight lines that armed the budget.
**D30 closes with it** — an entry that carries no url and no title cannot commit
one it lost.

##### The mdast backlog reaches ZERO, and the second fuzz oracle turns green

All five remaining entries were this half, and all five closed here. The gate
said so itself before the JSON was touched: *"backlog entry now AGREES with
remark … delete this entry in that same commit."*

**`node scripts/fuzz-parity.mjs --oracle mdast` was one of §0's six known-red
checks and is now 300/300.** Measured on both sides of this commit rather than
assumed: stashed to 9b.1 with a clean rebuild it fails; restored it passes. It
was red for exactly the reason the backlog existed.

**And the mdast oracle now compares more than it did.** `identifier` joins the
compared fields on all three reference kinds, and §5.6's *"footnote label bytes
are compared by nobody, on either side"* is answered — `FootnoteDefinition` and
`FootnoteReference` compare `label`, because mdast's `label` is the authored
spelling and so is this side's. `identifier` is deliberately NOT compared for
the footnote kinds: this side keeps the leading `^` and mdast does not (§5.2),
and a one-byte difference belongs in a register rather than in a gate.

##### The upstream projection groups by `identifier`, and that is stronger

`applyUpstreamReferenceModel` folded the raw label in JavaScript. It cannot:
**`toLowerCase()` is not a full case fold**, so `[SS]` and `[ẞ]` — which cmark
matches and this engine matches — would resolve to different definitions in the
normalizer and report a divergence this engine does not have. It now groups by
the `identifier` the engine states.

That is not trusting the engine about the thing being checked, and the mutant
says so: **M22** builds the identifier from the raw label with no fold at all,
and upstream parity goes **887/887 → 875/887**. A reference that names the wrong
definition still resolves to the wrong destination here, and a reference that
names none stays `Text` where upstream has a `Link`.

##### Ninety-four golden examples moved, and the claim is mechanised

78 in `spec.txt`, 12 in `regression.txt`, 4 in `extensions.txt`, plus two
canonical `.ast` files. Every changed line is one of exactly three rewrites:

1. a `Link`/`Image` becomes a `LinkReference`/`ImageReference` **at the same
   scope**, with `destination`+`title` replaced by `label`+`identifier`+`form`;
2. a footnote's `id="X"` becomes `label="X" identifier="^x"`;
3. a `ReferenceDefinition` gains `identifier=`.

Applied as a canonicalisation and compared: **94 of 94 examples, 0
unexplained.**

##### The numbering rule, stated because the block half looked like a rule and was not

`MARKDOWN_CORE_NODE_REFERENCE_DEFINITION` is `0x0010` because the extension
BLOCK types run to `0x000f`. The two inline reference kinds are `0x000f` and
`0x0010`, because the extension INLINE types stop at `0x000e`. **The two classes
are numbered independently and the class bits are what separate them** — reading
9b.1's gap as "core starts at 0x0010" would leave `0x000f` permanently unused
and break `node_type_values`, which asserts that the *n*-th type in each class
has value *n*. That test was written before either kind existed and it is what
makes both statements checkable.

##### `markdown_core_node_association`, and the accessor test that killed nothing first

One function, one switch, all five kinds, and it refuses every other node. §5.2
requires the dispatch rather than a common-initial-sequence read, and the reason
is not standards lawyering: a definition's association is **boxed** and the other
four are inline, so a uniform read would take a POINTER for `chunk.data`.

The new `association_accessor` test walks a corpus and asserts that exactly five
nodes answer and every other refuses. **Its first version killed nothing**:
M24 adds `MARKDOWN_CORE_NODE_LINK` to the switch — reading a `markdown_core_link`
as an association — and the suite stayed 69/69, because the corpus had no inline
link in it. An inline `Link` and an inline `Image` are now in that corpus
deliberately; they are the two kinds nearest to answering by accident, because
their union arm is a pair of chunks too. With them M24 fails three assertions.

##### Mutants

| mutant | what went red |
|---|---|
| **M20** a collapsed reference reports `form=full` | `spec_commonmark`, `extensions_gfm`, `conformance` 0/2, mdast **109/110** — and upstream **887/887**, because upstream has no form to compare |
| **M21** the footnote identifier loses its `^` | `regression_commonmark`, `extensions_gfm`, `conformance` 0/2 |
| **M22** the identifier is the raw label, unfolded | `spec_commonmark`, `extensions_gfm`, mdast 109/110, upstream **875/887** |
| **M23** a running budget goes back into `markdown_core_map_lookup` | `audit-reference-order-independence.mjs` reports **both rows appearing**; also `facade_concurrent_stress` and both OOM sweeps, which is the crude mutant's own `static`, not the property |
| **M24** a sixth kind answers the association accessor | `api_engine`, three assertions — **and nothing at all until the corpus gained an inline link** |

##### Gates

`correctness` 69/69, `correctness-asan` 60/60, `correctness-ubsan` 60/60 (both
sanitizer trees deleted and reconfigured), `conformance` 2/2, `canonical-ast`
**32 kinds, 62 fields, 6 cases**, `ast-projections` 32 kinds over 12 surfaces,
`source-lists` 22 sources, `public-surface`, `special-chars`, `attach-order`,
`plan-graph` 22/45, **both** fuzz oracles 300/300, upstream **887/887** with
10/10 divergences and 4/4 projections, mdast 110/110 with **an empty backlog**,
scope-sanity 1, inline-sourcepos 40, containment 9, places 57, concrete records
28, **reference-order 0 and green**, `test-topology`, `format-c`,
`format-cmake`, `lint-c`, `pnpm -w run lint`, `leaks --atExit` 0, and the Swift,
Kotlin and ES suites green. `scripts/format-swift.sh --check` moves **164 →
155**: the two footnote types were rewritten and now document every public
declaration they have (Q41's count shrinks; the check is still red and still
owned).


#### 4.14.9b3 The scoping pass on `end-at-line-ending`, and why it is not a position fix

**Not a step — a measurement, taken before 11b builds on top of it.**
`specs/positions/places.json` was down to ONE family, 57 rows, and no step owned
it. It has an owner now: **11c**, and the reason is that the fix is not the
one-liner it looks like.

**The mechanism, stated exactly.** `finalize`'s third branch ends a block at
`(line_number - 1, last_line_length)` — the line before the one that closed it.
Where blank lines are TOLERATED, that line can be blank, and `last_line_length`
is then 0:

```
- a          List      scope=1:1..2:0     the list's last byte is at 1:3
             ListItem  scope=1:1..2:0
             CodeBlock scope=1:5..2:0     (for `    code` + a blank line)
```

A block quote closed by the same blank line reports `1:1..1:3` and is right,
because a blank line CLOSES it — the family is exactly the blocks that survive a
blank line and are closed one line later.

**The one-line fix works, and it is not enough.** Walking `end_line` back over
the trailing blank run — bounded by the run and stopped at the block's own first
line — was built and measured:

| oracle | rows appearing | rows clearing |
|---|---|---|
| `audit-position-places.mjs` | **0** | **51 of 57** |
| `audit-scope-containment.mjs` | 0 | 0 |
| `audit-scope-sanity.mjs` | 0 | 0 |
| `audit-concrete-records.mjs` | **37** | 6 |

**The 37 are the finding.** Every one is `region-after-owner`, and they say the
same thing: the block's SCOPE stopped before the blank line while its REGIONS
did not. 12 are a `list_item` and 5 a `list` still owning the blank line as
`DISCARDED`; 12 are an indented `code_block` still owning it as **`CONTENT`** —
which is a second defect the wrong scope was hiding, because
`remove_trailing_blank_lines` takes those bytes back OUT of the content buffer
and the region set was never told.

So the position and the record set are one fact and have to move together: **a
block ends at the last byte it took, and owns no region after it.** That is a
region-ownership rule at a block boundary, which is 11c's subject — 11c already
owns "the block partition is total for real documents" — and it needs the
end-of-input branch to walk back too, or a prefix ending in the blank run
attributes those bytes differently from the whole document and L4 fails.

**All 57 rows now carry that owner** instead of `unassigned`. The probe is
reverted; nothing in this commit changes the engine.

##### SECOND ATTEMPT, after 11b, and it is recorded so the third does not start over

The full shape was then built — the walk-back in BOTH end branches, the trailing
run handed to the block's PARENT as `DISCARDED`, and the row the cut lands
inside SPLIT, because a run of blank lines inside an indented code block is one
`CONTENT` row with everything before it. Measured at each turn:

| version | places CLEARED | concrete rows APPEARING |
|---|---|---|
| walk-back only | 51 | 37 `region-after-owner` |
| + hand the tail back | 52 | 54 (29 L4 block-role, 12 `code_block` CONTENT) |
| + split the straddling row | 52 | 271 — because the handback then fired for EVERY block and took its own line ending |
| + fire only where the end MOVED | **52** | **32** (26 L4 block-role, 6 L2) |

**The last one is the closest and it still fails, and the reason is decisive:**
26 of the 32 are `prefix-block-role-differs`, and **11b's L4 block half is at
zero**. That half is 11a's law unweakened, and the whole point of splitting L4
in two (§4.14.11b) was that the BLOCK attribution keeps it. Trading a family of
57 position rows for 26 rows of the one law that has none is not a step
forward, so the attempt is reverted again rather than landed at 32.

What the third attempt needed was the piece none of these had: **why the walk
fires in a PREFIX where it does not fire in the whole document.** The witness is
`    Foo⏎    ---⏎⏎    Foo⏎---⏎` at byte 15 — the `\n` ending line 2 — which the
prefix of lines 1–3 called `DISCARDED` and the whole document calls `CONTENT`.
**§4.14.11c2 started there and closed the family**: the walk was right and both
of its details were wrong. Working the witness by hand rather than by count is
what found them, and the owner asking for the example is what prompted it.


#### 4.14.11b Step 11b: the inline phase owns its bytes, and two laws that say so

**The deliverable.** Every byte of every block's CONTENT region is owned by
exactly one INLINE node or by the block itself, in source coordinates. A
paragraph's record set is no longer one row:

```
a *em* `c` b
region 0 2 CONTENT  text        1:1..1:2
region 2 1 MARKER   emphasis    1:3..1:6
region 3 2 CONTENT  text        1:4..1:5
region 5 1 MARKER   emphasis    1:3..1:6
region 6 1 CONTENT  text        1:7..1:7
region 7 1 MARKER   code        1:8..1:10
region 8 1 CONTENT  code        1:8..1:10
region 9 1 MARKER   code        1:8..1:10
region 10 2 CONTENT text        1:11..1:12
region 12 1 CONTENT paragraph   1:1..1:12
```

**NOT ONE GOLDEN ROW MOVED.** 11b adds records; it does not change the tree.
`spec.txt`, `regression.txt`, `extensions.txt`, every `.ast`, both parity
oracles and all three position ledgers are byte-identical across this commit.

##### The role rule is one sentence, and it decides every case the requirement lists

> A byte is **CONTENT** when it survives into the owner's literal **as itself**,
> and **MARKER** when it does not.

That is not a restatement of the requirement's list — it *derives* it. A
matched delimiter run, a matched bracket, a backslash, an entity, a
destination, a title and a smart-punctuation substitution all fail the test; an
UNMATCHED `*` passes it, because the node's literal is that byte. It is also
11a's own rule for blocks, applied one level in.

##### The mechanism: claims, painted, then one refinement

The inline phase CLAIMS ranges of the block's content as it reads them, and the
claims are resolved when the block's inlines are done — because emphasis and
brackets are settled at the END of the block, and a `*` that turns out to open
an emphasis was a `Text` node when it was read. **LATER CLAIMS WIN**, which is
exactly that: `S_insert_emph` re-claims the delimiter bytes for the node it just
made, and so does `handle_close_bracket` for a link's brackets.

Resolution is three passes and one move, all bounded by the block: paint one
claim index per content byte, group the paint into runs and cut them at the
block's own line marks — a content range that crosses a line ending is TWO
source ranges, because the next line's stripped indent lies between them — then
rewrite the block's CONTENT rows, counting first, growing once and
right-aligning the survivors, which is the shape §4.14.9b1 already used and for
the same reason.

**A claim for the BLOCK ITSELF is forced to `CONTENT`**, whatever the caller
asked for. 11b REFINES a block's content into inline owners; it does not
re-label it. Calling a byte the inline phase read and kept nowhere `DISCARDED`
makes the BLOCK-level attribution depend on the inline phase, and that breaks
L4 — measured, eleven rows, all of them a prefix whose block had not yet grown
the construct.

##### THE CLAIM LIST IS A STACK, and the reason is one call

`markdown_core_parse_inlines` is REENTRANT: `extensions/directive.c` parses a
label's inlines from inside the paragraph's own inline pass, because
`process_inlines`' walk cannot reach a node created during that pass. A single
flush would have resolved the outer block's half-made claims. Each call records
the claim count it entered with and resolves only its own.

The same call needed one more line: the label's inline parse walks the block's
ANCESTORS to find the region its content was cut from, and the directive that
will hold the label is still a return value at that point — so `node->parent`
is set there rather than left to `append_child` a moment later. Without it every
node inside a label owned nothing.

##### Three soundness filters, and each one is a fact about the engine

| filter | what it answers |
|---|---|
| the LIVE SET, over the whole tree, after the last rewrite | a claim's owner may have been FREED — an extension that matches a delimiter pair builds its own node and frees the text nodes the run was; consolidation frees every text node but the first; the autolinker splits a text node in its POSTPROCESS, at `finish`. A region naming a freed node is read by `S_write_concrete`, so it is a use-after-free and not a wrong label |
| the SCOPE walk, up the ancestor chain | a pointer freed by a postprocess can be REUSED by a node the same postprocess allocates, so membership proves the owner is *a* node and not that it is *the* node. One step up is not enough: measured, three rows a thousand bytes past their owner |
| CLIPPING a piece to the region it falls in | a claim can span a refinable region and one a nested refine already wrote; taking it whole overlaps, which L1 says out loud |

**Consolidation had to become parser-aware, and then cursored.** It frees every
text node but the first of each run, and those nodes own regions. Transferring
them one at a time scans the region array from the node's own line — which
narrows nothing when 300,000 `<!--` are 600,000 text nodes on ONE line.
Measured as a **TIMEOUT** in `pathological_unclosed_comment`. A forward-only
cursor shared across the run makes the whole run cost the run.

##### Two new laws, and one of them is the only thing that can see 11b at all

**L5 — an inline node's scope is exactly the bytes it and its descendants own.**
L1 through L4 are all satisfied by a record set that names no inline node:
*"every byte of a block's CONTENT is owned by an inline node OR BY THE BLOCK"*
is true of the day before this step, and so is the tiling, and so is the
completeness. **Mutant M26 deletes the entire refinement**: `correctness` 69/69,
`conformance` 2/2, upstream 887/887, mdast 110/110, every position ledger
holding — and `audit-concrete-records.mjs` reports **324 rows appearing**.
Nothing else in the repository moves.

L5 needs the tree, which the record rows cannot give it, so `--concrete` now
prints a `node` row per node in preorder — the RAW tree's, the same traversal
`S_write_node_path` walks, not the facade's, which hides the directive label.

**L6 — an inline node's OWN regions carry the role its kind admits.** A `Text`
owns no marker unless it is an entity, an escape or a smart substitution, so its
vocabulary is open and the law says nothing about that kind; every other kind is
exact. **Mutant M27** reports an emphasis's opening delimiter as CONTENT:
`correctness` 69/69 and **154 L6 rows appear**. Without L6 the role assignment
had no oracle at all.

##### L4 had to be told the truth about inline records, in two halves

An inline record is made when its BLOCK closes, so the block a prefix leaves
open cannot have the same ones — `$$` on its own line is a paragraph and
`$$⏎x⏎$$` is a formula, and the same bytes are CONTENT in the first reading and
MARKER in the second. No ordering of the parse avoids that: the construct is not
there yet.

So L4 is now checked in two halves, and the split keeps 11a's law intact:

- the **BLOCK attribution** over **every byte** — an inline region only ever
  refines a byte the block already called CONTENT, so the coarse role is
  recoverable and the original law is unweakened. **0 rows.**
- the **INLINE refinement** over the bytes in a block the prefix had already
  closed — **5,246 of 27,734 (19%)**, and the run prints both numbers, because a
  bound that quietly swallowed the document would read as agreement.

**Eight rows survive even that**, and they are not a defect: whether `[a]` is a
reference or prose depends on a definition that may appear on the LAST line, so
no prefix can attribute those bytes the way the whole document does. Registered
as `reference-recognition-is-document-scoped`, so a row appearing anywhere else
still fails.

##### What is registered, and the three causes

`audit-concrete-records.mjs` goes 36 → **267 rows**, and the growth is the two
new laws finding what the block model leaves behind. **2,255 of 2,462 inline
nodes (91.6%) are exactly covered**; 207 are not, and 24 carry a repaired role.
Three named causes, all of them block-level facts 11b refines on top of, all
owned by **11c**:

1. the **table** extension gives a row line the role MARKER, so a cell has no
   refinable CONTENT region and nothing inside it owns anything — the same shape
   as `split-off-table-lead-content`, which 11c already owns;
2. the **autolink** extension rewrites the tree in its postprocess, AFTER the
   block whose regions it invalidates has been refined, so its pieces are
   reseated up to the block;
3. a text run **rtrims** its trailing spaces out of its literal while its SCOPE
   still covers them, so the run owns fewer bytes than it claims to — giving
   them to the run instead costs two L4 block rows and needs the scope decided
   first.

##### What the extensions do claim

`strikethrough` claims both tilde runs for the node it retypes in place;
`formula` claims its two delimiter runs and the body between them, because
`free_nodes_through` frees every node the span was built from; `directive`
claims its label's brackets, which are the label's scope and not its content.
The floor under all of them is a re-claim to the BLOCK before any extension
delimiter handler runs, so a handler that claims nothing still cannot dangle.

##### Gates

`correctness` 69/69, asan 60/60, ubsan 60/60 (both trees deleted and
reconfigured), `conformance` 2/2, canonical-ast 32 kinds / 62 fields,
projections 32 over 12 surfaces, both fuzz oracles 300/300, upstream 887/887
with 10/10 and 4/4, mdast 110/110 with an empty backlog, scope-sanity 1,
inline-sourcepos 40, containment 9, places 57, **concrete records 267**,
reference-order 0, `pnpm -w run lint`, `leaks --atExit` 0, and the Swift, Kotlin
and ES suites green.


#### 4.14.11c Step 11c: the requirement was already true, and now something says so

**A step whose engine change is nothing, and that is the finding.** 11c's row
reads: *"A reference definition and a footnote definition own their source
bytes, so the block partition is total for real documents. A definition that
lost a duplicate-label contest keeps its bytes."* **Step 9b.1 made all of it
true** — the definition became a node at the byte where its `[` was written,
owning every byte it read, and a definition that loses a label is a node like
any other because nothing picks a winner at parse time (§4.14.9a2). What was
missing was not the behaviour. It was anything that would notice if it stopped.

**So 11c is the gate, and it is L5 with two block kinds added.** L5 says an
inline node's scope is exactly the bytes it and its descendants own; applied to
`ReferenceDefinition` and `FootnoteDefinition` it says exactly 11c's sentence.
**115 definition nodes across the corpus, 0 rows.** No other block kind is
checked there, and the reason is stated in the code: a container block's bytes
are its children's by construction, and the wide form of that clause reported 78
`table_row` rows at 11a saying nothing.

**One fixture, for the second sentence.** `[a]: /1⏎[a]: /2⏎⏎[a]` — the first
definition wins the label, the second resolves nothing, and both are nodes that
own their own bytes. Upstream keeps neither.

**Mutant M29**: only the first definition of a run becomes a node.
`spec_commonmark` and `regression_commonmark` red, and **19 concrete rows
appear**.

##### What 11c carries forward, and it is not this requirement

Four families are now owned by 11c and none of them is the sentence above. They
are recorded where they were measured rather than restated here:

| family | rows | measured in |
|---|---|---|
| `end-at-line-ending` — a block ends at the last byte it took and owns no region after it | 57 (`places`) | §4.14.9b3, twice, with the numbers of four attempts |
| `inline-scope-not-covered` and `inline-role-repaired-to-content` — the table row's MARKER line, the autolinker's postprocess, a text run's rtrimmed tail | 287 + 45 | §4.14.11b |
| `split-off-table-lead-content` | 3 | 11a |
| `html-block-end-before-its-content`, `indent-after-container-closed`, `thematic-break-line-ending`, `atx-closing-sequence` | 26 | 11a, unassigned until now |

##### Gates

`correctness` 69/69 (the new fixture takes `regression.txt` to its own count),
asan 60/60, ubsan 60/60, `conformance` 2/2, canonical-ast 32 kinds / 62 fields,
projections 32 over 12 surfaces, both fuzz oracles 300/300, upstream **888/888**
with 10/10 and 4/4, mdast 110/110 with an empty backlog, scope-sanity 1,
inline-sourcepos 40, containment 9, places 57, concrete records 267,
reference-order 0, formatters and linters clean.


#### 4.14.12a Step 12.1: one parse under two views, in C

**The C half of requirement 12.** `markdown_core_document_root` is now
`markdown_core_document_semantic`, and beside it the document keeps the other
view: `_source`, `_line_count`, `_line_start`, `_region_count`, `_region_at`.
The rename is deliberate and is the whole of the ABI break §4.1's row 12
budgets — one name per thing, and `root` was a name for the semantic view that
did not say it was one.

**The law is in the header, and it is checkable rather than aspirational:**

> Every byte of `markdown_core_document_source` lies in exactly one region, and
> every region has exactly one owner in the semantic tree.

So the pair is complete: the tree MAY omit bytes — a fence's backticks are in no
literal, an ATX heading's closing hashes are in nothing at all — and the concrete
view may not.

##### The view moves; it is not copied

The parser's `source`, `line_starts` and `regions` were released at `finish`,
which is what 11a's own comment said requirement 12 would change. They are now
**moved** into the document, at the one moment they are both complete and still
owned: after every rewrite, before the reset. The regions name NODES, so the
document owns both and frees them together, regions first.

`markdown_core_parser_retain_concrete` is how a caller asks; the CLI does not,
which is why its `--concrete` still prints straight from the parser and its
stack-allocated facade document zeroes the field.

##### One name collision, and it is worth writing down

The internal region type and the public one are different shapes — `bufsize_t`
against `size_t`, an owner that is mutable against one that is not — so the
internal struct is `markdown_core_region_record` now and the public one takes
the plain name. The ROLE enum is shared and is guarded the way
`markdown_core_reference_form` is: one definition, two headers that both admit
it.

##### The gate, and where it does NOT run

`facade_test` gains `check_two_views`, which asserts the law through the public
surface and nothing else — the regions tile the source with no gap, no overlap
and nothing past the end; every owner is reachable from the semantic root; every
line but the first begins after a line ending; and line zero, a line past the
end and a region past the end are all refused. The corpus is deliberately one of
everything the tree omits.

**Mutant M30** drops the last region on the way out of the parser.
`correctness` reads **69/69** — and `conformance` fails with *"every byte of the
source is in exactly one region"*. `facade_native` is labelled `conformance`,
not `correctness`, so the preset most work is done under cannot see this law at
all. `scripts/dev/gates.sh` runs both, which is why it is gated; anyone running
only `ctest --preset correctness` is not testing requirement 12.

##### What 12 still owes

The bindings. *"The concrete view survives being copied into value types and the
handle being freed"* is 12.2's sentence, and it needs a region to name its owner
by something that survives the copy — a node PATH, which is what `--concrete`
already prints and what a pointer cannot be.

##### Gates

`correctness` 69/69, asan 60/60, ubsan 60/60, `conformance` 2/2, canonical-ast
32 kinds / 62 fields, projections 32 over 12 surfaces, `public-surface` (which
compares the header against both export lists and so catches a symbol added to
one), both fuzz oracles 300/300, upstream 888/888, mdast 110/110 with an empty
backlog, every position ledger holding, `lint-c` — which caught the missing
initializer in `core/main.c` that no preset did — `pnpm -w run lint`, `leaks
--atExit` 0, and the Swift, Kotlin and ES suites green through the rename.


#### 4.14.12c Step 12.2: the bindings, and the name `Document` goes where C always had it

**Requirement 12 is complete.** All three bindings return two total views, both
copied into value types, and each has a test that reads a region after the
native handle is freed — the requirement's own sentence and the one part of it
a C test cannot make.

##### THE OWNER'S SECOND RULING: the pair is `Document`

§4.14.12b's ruling settled reading 1 and left one name to pick. My answer was
`ParsedDocument`, on the ground that the markup root's model type must be
spelled `Document` because `audit-ast-projections.mjs` requires each model to
declare a type named for every contract kind. **The owner took the third option
instead: the PAIR is `Document` and the ROOT is renamed.**

It is the better answer and C is the argument. `markdown_core_document` has
always been the parse result; `markdown_core_document_semantic` returns a
`markdown_core_node` whose KIND is `DOCUMENT`. The bindings had given the root
the name C gives the pair, and 12.2 is where that shows.

| | before | after |
|---|---|---|
| the parse result | *(no value existed)* | **`Document`** — `semantic`, `concrete`, `parse`, `ownerOf` |
| the markup root | `Document` | **`DocumentRoot`** |

**Everything else keeps saying `Document`,** and that is deliberate: the kind
name, this repository's contract table, the dump string in every golden,
`MARKDOWN_CORE_KIND_DOCUMENT`, `WireKind.DOCUMENT`, `"document"` in the ES wire,
and every `visitDocument`. **One spelling moved, in one place**, so the
projections audit carries ONE named mapping rather than a pattern:

```js
const MODEL_NAME = new Map([["Document", "DocumentRoot"]]);
```

Not a rule. A list of one, which a second entry would have to be added to
deliberately. Three model projections and the two Swift surfaces that key on the
parameter type read through it; the Kotlin and ES dumpers key on
`visitDocument(` and did not move.

##### THE C FUNCTION 12.2 NEEDED, and the measurement that asked for it

`markdown_core_document_region_owner_path` answers for ONE region, and a binding
copies every one. **Looping it costs 96.8 ms against a 30.8 ms parse** on this
document's own source — 52853 regions — because a node knows its parent and not
its own index among its siblings, so every call counts previous siblings from
scratch. That is §3's rule broken by a query.

**The thread-safety contract decided the shape.** `markdown_core.h` promises
that *"concurrent read-only access … to the same document from multiple threads
is safe"*, and `facade_concurrent_stress` gates it — so a lazy cache on the
document was out, and precomputing at finalize costs every consumer who never
asks. What is left is a call that does the whole pass at once and keeps its
memo on the STACK:

```c
markdown_core_document_region_owner_paths(document, paths, paths_capacity,
                                          offsets, offsets_capacity)
```

Regions come in source order, so a pass in that order can start where the last
one stopped: a 64-bucket direct-mapped memo of (parent, child, index), scanning
FORWARD from what it remembers. **119232 of 125655 path elements are memo hits,
and the whole forward scan is 21128 steps.** Both calls of the two-call
protocol — size, then fill — measure **1.13 ms**, against 96.8. The offsets are
written even when the paths are refused, so the caller learns the total from the
call that refused.

##### The three mechanisms differ and the values do not

| | how it crosses | what it costs |
|---|---|---|
| Swift | calls the C functions directly | `[UInt8]`, five `[Int32]` |
| Kotlin | one wire payload, magic `MKC2` → **`MKC3`** | the concrete view appended after the tree |
| ES | six new `es_*` accessors, each read in ONE crossing | `Uint8Array` + typed arrays |

**The regions are COLUMNAR and a `Region` is built when it is asked for.**
Measured density on real prose is **one region per 17 bytes** — `README.md`
484/8633, `canonical-ast.md` 840/13294, this document 40252/673903 — so an
object per region costs several times the source it describes, and the parallel
arrays cost about 25 bytes each. The C surface chose index addressing first
(`region_count` / `region_at`), and the bindings mirror it.

**`source` is BYTES** in all three, because region offsets index the normalized
source and handing back a `String` invites indexing it — §0's trap that reports
false failures on `\u00a0`.

##### `ownerOf`, and the descent that is not `content[i]`

A path of child indices is not a locator unless something resolves it, and the
value trees split some C child runs into named fields — a table's `header` and
`rows`, a directive's `label` and `content`. So each binding grew ONE shared
`children()` in the order the C tree holds them, **which the walker now walks
with too**, so the two cannot disagree, and `Document.ownerOf` descends with it.

##### Mutants

| mutant | what saw it |
|---|---|
| **M33** the memo returns its remembered index without advancing it | `conformance` fails on **"the one-pass paths are the same paths"** — and `correctness` reads **69/69**, because `facade_test` is not in that preset (§4.14.12a) |
| **M34** Kotlin: a table's rows before its header | `ConcreteTest` — `Position(line=5, column=3)` expected, `line=7` got: the header cell's `a` resolved to the body row |
| **M35** ES: the concrete view borrows WASM memory instead of copying it | the ES `concrete` suite, on re-reading after 300 further parses |
| **M36** Swift: the same reversal | the Swift `concrete` suite, same witness |
| **M37** ES: the same reversal | the ES `concrete` suite, same witness |

**AND ONE MUTANT THAT KILLS NOTHING, which is worth more than the five.** The
memo's backward-scan fallback — the arm that runs when the forward scan does not
find the child — is **unreachable today**. Deleting the check that guards it
leaves bulk and singular in agreement on **67797 regions across four
documents**. The 6423 misses measured earlier are all bucket COLLISIONS, which
take the backward path anyway. The arm stays, because it is what makes the memo
correct rather than lucky if region order ever stops implying owner order; it is
recorded here as dead so that nobody reads its survival as coverage.

##### Gates

`correctness` 69/69, asan 60/60, ubsan 60/60, `conformance` 2/2, canonical-ast
32 kinds / 62 fields, projections 32 over 12 surfaces, public surface, both fuzz
oracles 300/300, upstream 888/888 with 10/10, mdast 110/110, scope-sanity 1,
inline-sourcepos 40, containment 8, places 0, concrete records 277,
reference-order 0, `lint-c`, `pnpm -w run lint`, `leaks --atExit` 0 on
`facade_test`, and the binding suites: **ES 11 + 9**, **Swift 6 + 1 + 3**,
**Kotlin 11 + 4**.


#### 4.14.12b Step 12.2 part one: the locator a value type can keep, and the API question it exposes

**`markdown_core_document_region_owner_path`.** A region's owner is a
`const markdown_core_node *`, and every binding copies the tree into value types
and frees the handle — so a pointer is exactly the locator that does not survive
the copy. The path of child indices from the semantic root is the one that does:
`{}` is the root, `{0, 2}` the third child of the first.

It is an ORDINAL, which §5.8 rejected for the reference model, and the
difference is worth stating because the two look alike: §5.8's ordinal had to
survive `filter`, `slice` and `merge` on a tree a consumer edits, and this one
names a node inside ONE immutable snapshot. Nothing filters a parsed document.

`facade_test` walks every path back down and requires it to arrive at the owner
it came from; a path that does not fit the caller's buffer is refused rather
than truncated, and a region past the end has no path.

##### THE API QUESTION 12.2 EXPOSES, and it is the owner's

The requirement says the surface presents *"`document.semantic` … and
`document.concrete`"*. In C that is now literally true. **In the bindings it is
not, and cannot be without a break the requirement does not obviously
authorise**: `Document.parse(source)` returns the markup `Document` VALUE, and
consumers write `document.content`. There are two readings:

1. **the parse result gains both** — `Document.parse` returns
   `{ semantic, concrete }`. Literal, and it breaks every consumer, every
   binding test and every conformance suite in three languages.
2. **the document value IS the semantic view and gains `.concrete`** — nothing
   existing moves, `document.concrete.regions` is reachable, and
   `audit-ast-projections.mjs` permits it, because the model check is
   one-directional by design: *"the models may carry members the contract does
   not name"*.

**Reading 2 is what the bindings should take**, and it is recorded here rather
than taken silently, because it is a public-surface decision and §4.11's
precedent is that those are the owner's. The cost of reading 1 is measurable and
is the reason: three model declarations, three decoders, three dumpers, three
conformance suites and every consumer example.

##### OWNER RULING, 2026-08-23: READING 1. The recommendation above was WRONG.

*"We are under semver and targeting a new major version now. Why do you think
breaking a consumer is blocking under the current context?"*

It is not, and **§4.10 of this document already said so** — three paragraphs
that this record failed to apply:

> **The ABI break window is not a constraint.** … Shipping 3.0 from this base
> means the surface is free to change as the design requires, and the discipline
> that remains is only that it changes *deliberately* and the bindings follow.

`VERSION` is `3.0.0`. §4.1's row 12 budgets the break. The surface breaks at
Step 7, at 9b, at 12 and at 13 regardless (§4.5.6). **The cost I priced —
three declarations, three decoders, three suites, the examples — is a DIFF, and
I presented it as a compatibility problem.** Those are different things, and
under a major version only the second one counts. Nothing in the list is a cost
the major bump has not already authorised.

Reading 2 also has a defect the cost argument hid: it makes the markup
`Document` node carry a `concrete` field its contract does not have, permitted
only by the projection audit's model check being one-directional *by design*
(*"the models may carry members the contract does not name"*) — an allowance for
constructors and conveniences, not for a second total view of the parse.
Reading 1 needs no allowance.

##### What reading 1 costs, and the three things measurement decided

**The markup node keeps the name `Document`, and that is settled by a gate, not
by taste.** `audit-ast-projections.mjs` requires each of the three models to
declare a type named for every contract kind, and `Document` is a contract kind
whose name is also the dump string in every golden. So the PARSE RESULT is the
thing that needs a name. **SUPERSEDED by §4.14.12c**: the owner's answer is
that the PAIR takes `Document` and the root becomes `DocumentRoot`, which is
what C has always done.

**`.concrete` is INDEX-ADDRESSED, not an array of objects, and the C surface
chose that first.** `markdown_core_document_region_count` / `_region_at` /
`_region_owner_path` are index-based, and mirroring them is both faithful and
cheap. Measured density on real prose — `README.md` 484 regions / 8633 bytes,
`canonical-ast.md` 840 / 13294, this document 40252 / 673903 — is **one region
per 17 bytes**. Materialising a `Region` object per region eagerly costs roughly
**8× the source** in ES; the columnar form is start/length/role plus a flat
owner path with offsets, and costs about **25 bytes a region**. A `Region` value
is built on access and is still a value: the arrays are the copy, and they
outlive the handle.

**The source is BYTES.** Region offsets index the normalized source, and a
binding that hands back a `String` invites indexing it — which is the §0 trap
that reports false failures on `\u00a0`. `Concrete.source` is `Uint8Array`,
`ByteArray`, `[UInt8]`.

##### What 12 still owes, precisely

**Paid in §4.14.12c.** The three bindings, each: `Document` (`semantic`,
`concrete`) carrying `parse`, a `Concrete` value type (`source` as bytes,
`lineCount`/`lineStart`, `regionCount`/`region(index)`), a `Region` value type
(`start`, `length`, `role`, `owner` as the path), the copy at parse time, and a
test that reads a region AFTER the native handle is freed. ES reads the wasm
accessors directly, Kotlin needs the view on its wire, Swift calls the C
functions; the three mechanisms differ and the value types do not.


#### 4.14.11c2 `end-at-line-ending` closed: the direction was right and BOTH details were wrong

**`specs/positions/places.json` is EMPTY.** The oracle that began Stage 0a with
131 rows in six families, and was down to one family of 57 at §4.14.9b3, has
zero. The owner asked for a worked example to check the direction, and the
example is what found the error in it.

##### The defect, in three lines

```
- a⏎⏎b        List      1:1..2:0     its last byte is at 1:3
    code⏎⏎x   CodeBlock 1:5..2:0     its last byte is at 1:8
> q⏎⏎x        BlockQuote 1:1..1:3    RIGHT -- a blank line CLOSES a block quote
```

Every scope end in this engine names the LAST BYTE — `hello` is `1:1..1:5`.
Column 0 is not one. The family is exactly the blocks that survive a blank line
and are closed a line later, so `line_number - 1` lands on the blank.

##### The two errors, and neither was the direction

Attempts one to four (§4.14.9b3) stalled at 52 rows cleared against 32
appearing, of which 26 broke L4's block half. Working the witness by hand
instead of by count found both:

**ERROR ONE — the cut point.** The handback took the block's bytes from its
LAST BYTE, which takes its own line ending away too, and only from blocks whose
end walked back. Every other block in this engine owns the line ending of its
own last line: `hello` is one paragraph owning six bytes. The cut belongs at the
LINE BOUNDARY — the block owns its lines whole, and what it does not own is the
blank lines after them.

```
    Foo⏎    ---⏎⏎    Foo⏎---        byte 15 is the ⏎ ending line 2
prefix (lines 1-3)  cut at last byte:  DISCARDED, the document's
whole document                      :  CONTENT, the code block's
prefix (lines 1-3)  cut at line end :  CONTENT   <- agrees
```

26 L4 rows → 14.

**ERROR TWO — the recipient.** The bytes went to `b->parent`, and a block can be
finalized while a descendant of it is still OPEN: a list closes when a code
block opens beside it, and its item closes after — so the item handed its
trailing blank to a list whose end was already settled. They go to the nearest
ancestor that actually COVERS them, and an ancestor still open is fine because
its own handback runs later. 4 L2 rows → 0.

**And branch 2, which is H14.** With both corrected, five rows were left: four
setext headings and one unclosed fence inside a block quote, all reported at
`L:0` by `S_set_end_to_current_line` — §11.4's *"Branch 2 … reports the current
line, which is right for a fence closed on its own fence and wrong for a setext
heading closed later."* The same walk in that branch clears all five and moves
nothing. **`audit-scope-containment.mjs` goes 9 → 8 with it**: the fenced code
block inside a block quote no longer outreaches its own parent.

##### THE MUTANT THAT MATTERS

| mutant | what saw it |
|---|---|
| **M31** no walk-back at all | 57 places rows appear, and the goldens go red |
| **M32** cut at the last byte instead of the line boundary — *the error above* | `correctness` **69/69**, `audit-position-places` **green at 0** — and `audit-concrete-records.mjs` reports **17 rows**, 13 of them L4's block half |

M32 is the argument for 11b's L4 split in one line: the wrong cut is invisible
to every golden and to the position oracle, and the only thing that can see it
is the law that says a prefix and the whole document attribute a byte the same
way.

##### Thirty-seven golden examples moved, and the claim is mechanised

31 in `spec.txt`, 3 in `regression.txt`, 3 in `extensions.txt`, and three
canonical `.ast` files. Every changed line differs in exactly one way: a scope
END moved from `L:0` to a real last byte on an EARLIER line, with the start and
every other field unchanged. **37 of 37, 0 unexplained.**

##### Sixteen rows registered, and both are what they are

- **14 L4 `blank-line-is-interior-or-trailing-only-at-close`.** A blank line
  inside an indented code block, a fenced block, an HTML block or a loose list
  item is that block's CONTENT; the same blank line at the END is trailing and
  `remove_trailing_blank_lines` takes it back out. Which one it is depends on
  the line AFTER it, so a prefix that ends on the blank cannot agree. **Same
  shape as `reference-recognition-is-document-scoped`** and registered the same
  way.
- **2 L2 `document-does-not-reach-its-trailing-blank-line`.** A block that gives
  its trailing blank up hands it to the document, whose own end is the last line
  it had content on. Before the handback the block kept the byte and its own end
  was column 0 — which made the containment check pass **by accident**, and is
  the clearest statement of why the family was worth closing.

Two other families shrank as a side effect: `html-block-end-before-its-content`
8 → 7 and `indent-after-container-closed` 8 → 3.

##### Gates

`correctness` 69/69, asan 60/60, ubsan 60/60 (both trees deleted and
reconfigured), `conformance` 2/2, canonical-ast 32 kinds / 62 fields,
projections 32 over 12 surfaces, both fuzz oracles 300/300, upstream 888/888
with 10/10 and 4/4, mdast 110/110 with an empty backlog, scope-sanity 1,
inline-sourcepos 40, **containment 8**, **places 0**, concrete records 277,
reference-order 0, `lint-c`, `pnpm -w run lint`, `leaks --atExit` 0, and the
Swift, Kotlin and ES suites green.


#### 4.14.11a2 Q44 answered: an autocompleted table cell sits where it was completed

**Owner ruling, 2026-08-23, and it supplied the criterion the question was
missing:**

> *"The scope is used for consumer that tried to map element from CST/AST back
> to source. For these auto completed cells, you should just mark it at the
> place it auto completed."*

**That decides it, and it decides it against both spellings 11a had measured.**
Both of those asked *which coordinate pair is least wrong about the extent of
something that has no extent* — and the answer is that a scope is not an extent
claim, it is **where a consumer should look**. A cell that completion invented
should point at the place the completion happened: the end of its row.

```
| a | b | c |
| --- | --- | ---
| x
                                         before           after
TableRow  3:1..3:3
  TableCell "x"                          3:2..3:3         3:2..3:3
  TableCell (completed)                  3:0..3:0         3:3..3:3
  TableCell (completed)                  3:0..3:0         3:3..3:3
```

**Nine golden rows moved — two in `spec.txt`, seven in `extensions.txt` — and
every one is an autocompleted `TableCell`.** Mechanised: **every degenerate
`TableCell` scope must name a byte on its own line. 2 of 11 before, 11 of 11
after.** The two that already did are genuinely EMPTY cells the author wrote —
`| … | |` — which were always placed correctly and did not move; the checker
cannot tell them apart from the dump, which is why the claim is stated over both.

**Ledgers, and one of them goes up by design.**

| ledger | before | after |
|---|---|---|
| `specs/positions/places.json` | 66 | **57, and ONE family left** — `recovered-table-cell` is gone, and it is the only family this ledger has ever closed by a *ruling* rather than a repair |
| `specs/positions/containment.json` | 21 | **9** — eighteen cleared, **six added** |

**The six are the cost, and they are the reason the question needed an owner.**
An autocompleted cell now names the row's last byte, and the cell before it ends
on that byte too, so the two overlap. The alternative is an empty range at
column `len + 1`, which is off the line, or column 0, which is not a byte at
all. **Pointing at something beats pointing at nothing**, and the six rows are
registered in `containment.json` carrying that reasoning rather than hidden.

**Mutant.** Putting the cells back at column 0 moves nine `places` rows and
fails `spec_commonmark` and `extensions_gfm`.

**What this leaves.** `specs/positions/places.json` was down to **one** family:
57 rows of `end-at-line-ending`, of which 57 are a block's end — H14's
neighbour (§11.4). §4.14.9b3 scoped it and measured four attempts that all
stalled; **§4.14.11c2 CLOSED it and the ledger is now EMPTY.** The oracle that
began Stage 0a with 131 rows in six families has none.

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
| **A1** | The engine has one **failure model** as well as one allocator model: an allocation failure is a fact about a transaction, not a property a buffer acquires. `markdown_core_strbuf.oom` either ceases to exist (a `reserve` becomes the only fallible buffer operation and every write after it is infallible) or gains an explicit clear. Today `markdown_core_strbuf_clear` (`core/buffer.c:78-83`) does not lift it and nothing else does — measured, one refused grow silently swallows every later line into that block **with the allocator working again**. **LANDED at 3a.3** as the explicit lift, and it kills no mutant on its own — §4.14.3a says why, and what gate it earned instead. | **3a** ✅ |
| **A2** | No allocation path aborts. Delete `core/arena.c` and the two `markdown_core_arena_push`/`_pop` pairs at `extensions/table.c:342` and `:550`. §4.13.10. **LANDED at 3a.1**, together with the re-parse retry the pop implied and the CLI's `#if DEBUG`. | **3a** ✅ |
| **A3** | ~~`parser->oom` stops being one sticky bit meaning four things.~~ **Measured at 3a.3: it means ONE thing** — 70 write sites, three reads, and all three mean *the document lost bytes, abandon*. The "four things" described the session/streaming parser, which no longer exists. What remains is *a failure is a **returned status***, and 3a cannot land it: `markdown_core_parser_finish` reports loss by freeing the root because NULL is the only vocabulary the surface has. **CARRIED.** | ~~3a~~ **13**, facade half at **12** |
| **A4** | `S_strbuf_grow_by` (`core/buffer.c:34-36`) checks `add` against `INT32_MAX/2 - buf->size` **before** the sum. Today a negative target satisfies `target_size < buf->asize` at `:41` and returns without growing *and without poisoning*, after which `_put` memmoves past the end. Verified by direct call; needs a single put above ~1.07 GiB, which `append(chunk:)` makes reachable at `core/blocks.c:909`. **LANDED at 3a.2, and it needed TWO guards, not one** — poisoning on a non-positive target is not enough, because the wrapped sum is compared through the overflow flag and the guard never fires (§4.14.3a). | **3a** ✅ |
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
- [x] **Directive grammar conformance (Step 7) — deliverable #1. LANDED**, §4.14.7a–e. Nothing carried: **D36** closed at 7e and **Q43** answered with it.
- [x] **The formula fix (Step 6) — deliverable #2. LANDED, §4.14.6.**
- [ ] CST concrete records (11a, 11b, 11c) and diagnostics (13) — deliverable #3. **11a, 11b and 11c are LANDED** (§4.14.11a, §4.14.11b, §4.14.11c); 13 is not
- [x] The reference model (9a, 9b) and the positions that depend on it — **10, 9a and 9b are all LANDED** (§4.14.10, §4.14.9a1–9a2, §4.14.9b1–9b2)
- [ ] The facade (12), the null/empty rule (14) — **12 is LANDED WHOLE** (§4.14.12a–12c): two total views in C and in all three bindings, and §4.10's ruling that the break window is not a constraint is what let 12.2 take the literal reading; 14 is not
- [ ] Bindings, specs and docs regenerated (15)

**Defects** — **all thirty-six of §2** closed, or explicitly carried with a
named owner step and a registered known-red gate. ~~seventeen~~ was stale from
the revision before D18–D25 and §4.13's four were added, and ~~thirty-two~~ was
stale the moment Step 3.3 found **D33**. §2's own heading now says thirty-three,
its index table carries thirty-five rows (D1–D25, D27–D36) and **D26 is the
thirty-third** — measured at 0a.12, refused there, and landed at **0a.12b**
with the ruling it needed (Q40).

**Thirty-five are closed and one is carried.** D9 and D30 both **CLOSED at 9b**
(§4.14.9b1–9b2): D9's two gates changed state in one commit — the
order-independence oracle went from registered-red with two rows to green with
zero, and `reference_expansion_bound` from 0.999x to 0.399x — and D30 lost its
mechanism entirely when the map stopped holding a resource. What remains carried
is **D31** (Step 8; pinned as a golden row in `regression.txt`). **D36** was the fourth for three commits: found at 7c by
sweeping the grammar against micromark's own source, and **closed at 7e** once
the owner named the layer -- the defect was in STARTING a directive, not in
anything downstream. D27 was another and **closed at 3a.3**.

**Gates**, all green and none of them vacuous:
- [ ] `correctness`, `correctness-asan`, `correctness-ubsan` — each having
      actually run its tests, not merely exited 0 (§0's warning)
- [ ] `conformance`
- [x] upstream parity, and **both** fuzz oracles — the mdast fuzz oracle turned green at 9b.2 (§4.14.9b2)
- [x] mdast parity with an EMPTY backlog — **emptied at 9b.2**; ~~6 left, all Step 9b's~~
- [ ] scope-sanity, having only shrunk
- [x] `check-canonical-ast-fixtures`, `audit-public-surface`,
      `audit-ast-projections` — all three green; the third since 15A.2
- [ ] `check-generated-scanners` — known-red, owned by R9 then Step 3, and must
      be green or re-owned by close
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
| D9 | order-independence oracle (**registered red**, names Step 9a in its own text; the owner is **9b** since §4.14.9a2) + output-size bound in `complexity_runner.c` (green) | **both new** | n/a — the fix is Step 9b | **none.** With the budget deleted, every existing gate stays green while 1 MiB of input produces 68.7 GB of output |
| D10 | position half: `regression.txt` example 24 **already exists and pins the defect** — unpinning it is the gate. Byte half: new example `x[^a\nb] tail` + an `expectedDivergence` | half new | yes, both halves | **half.** No corpus input loses bytes here, and upstream loses the same bytes |
| D11 | new `regression.txt` example (the nested-duplicate reproducer) + an upstream **model** delta; sanitizers and `leaks --atExit` gate the ownership half | **new** | the minimal fix moves zero goldens, so the example is mandatory | **none.** Nothing in the corpus has a nested duplicate label |
| D15 | 2 order witnesses in `extensions-conflicts.txt` (a `:::note` and a `$$` block after an OPEN table) **+ the new `scripts/audit-extension-attach-order.mjs`** | **both new** | **LANDED 0a.11, measured**: putting `table` first takes the fixture 4/4 → 2 passed / 2 failed; a second attach site in `markdown_core_document_parse` is caught by **the audit alone**, with `correctness` 67/67 and `conformance` 2/2 | **none, and no corpus can be**: every fixture runs through the facade, so no fixture can compare the two attach orders. `conformance` runs the CLI and the facade against the same six canonical goldens but none of the six inputs is order-sensitive |
| D24 | 1 example in `extensions.txt` + activating `tasklist-checked-marker` in `specs/upstream-parity/deltas.json` | rows only | **LANDED 0a.11, measured**: restoring the `strstr` fails `extensions_gfm` **and** fails `check-upstream-parity.mjs`, which reports the divergence no longer reproducing | **upstream parity**, and only once the corpus reaches it: the registered input was in no fixture, so the JSON edit alone fails the gate's own reachability check |
| D18 | the 12 moved golden rows in `spec.txt` (177/179/184/185) and `regression.txt` **+ `audit-position-places.mjs`** | rows + oracle | **LANDED 0a.12, measured**: reverting it makes `spec_commonmark` **and** `regression_commonmark` red and the places oracle report 3 rows appearing | **none of the parity gates** — upstream has it identically. But it was **pinned, wrongly, by two golden files**, which §4.1.7's "invisible to every gate" did not say |
| D19 | `spec.txt` 518 for the newline half **+ a new `regression.txt` example for the start-line half + `audit-scope-containment.mjs`** | example new | **LANDED 0a.12, measured, and the halves separate**: the newline half is killed by `spec_commonmark` and the places oracle; **the start-line half was killed by NOTHING** — 67/67 and all three oracles green — until the new example made containment's parent/child rule fire | **containment, and only once the corpus has a multi-line link label.** No parity gate compares positions |
| D20 | the 3 moved rows in `extensions.txt` **+ `audit-position-places.mjs`** | rows + oracle | **LANDED 0a.12, measured**: `extensions_gfm` red and 3 rows appearing | **none.** A reversed range is not a containment violation; `audit-scope-sanity.mjs` reads goldens, not the engine, so it cannot kill the mutant |
| D32 | the 5 moved backslash-hard-break rows in `spec.txt` **+ `audit-position-places.mjs`** | existing rows | **LANDED 0a.12, measured**: `spec_commonmark` red and 5 rows appearing | **none, and upstream cannot be the oracle** — cmark-gfm reports the same columns |

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
**`pendingDeltas` reached 0 at 9a.2**, and the list only ever worked because a
reader looked: nothing in the repository read it, Step 10 landed its fix and
left `table-split-lead-spelling` pending while registering a second id for the
same difference, and every gate stayed green. `check-upstream-parity.mjs` now
fails a pending input that has started diverging (§4.14.9a2).

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
| **Q40** | Is a line ending a place? (D26) | **PROPOSED, taken at 0a.12b — YES, and only for a node that IS one.** A line of L bytes has L+1 boundaries and the last is where the line ending lives, so a `SoftBreak` or `LineBreak` at column L+1 is a place. The GENERAL form was measured before it was rejected: admitting L+1 for every kind would have excused **twelve** rows already in `specs/positions/places.json` — eleven `Text` and one `Emphasis` — that are wrong for other reasons. The narrow form excuses none of them, because no break node was ever registered there. `scripts/audit-position-places.mjs` carries the rule and says so. | 0a.12b | Step 5, which owns the dump spelling that would make the question moot |
| **Q38** | Does removing the empty `Text` node get registered against upstream, and how? | **TAKEN at 0a.14 — as a PROJECTION, not as inputs.** Without it `check-upstream-parity.mjs` reads 806/817 with eleven inputs diverging (eight autolink, three hard-break and shortcut-reference), which is §4.2.3's number reproduced. The difference appears wherever the construct does, so it is a model difference and a list of inputs would go stale as the corpus grew: `normalize` drops an empty-literal `Text` from BOTH sides, `empty-text-node` joins `NORMALIZED_DELTAS`, and both delta files carry the entry. **The cost is stated rather than hidden**: with it projected, re-introducing this side's empty node is invisible to upstream parity — measured — and the goldens are the only gate. | 0a.14 | — |
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

**H2 · The reference-expansion budget makes the parse of lines 1…*i* a function of lines *i+1*…** `max_ref_size = max(100000, total_size)` is set at `core/blocks.c:801-806` from a number only the last line knows, and spent monotonically at `core/map.c:307-309`. Measured twice, independently: a 4 248-byte document with 60 references to one definition resolves **24** of them; the same 62 lines followed by 303 001 bytes of unrelated filler resolves **60**. Separately, appending 368 890 bytes *after* a paragraph flips that paragraph from `Text "[b]"` to `Link "/short"`. *Unclassifiable because:* `total_size` is carried, but the value it feeds does not exist until the stream ends. *At a pause:* prefix equality is not merely expensive, it is **unattainable** — no carried state can supply a number that has not been fed. *Smallest change:* delete `ref_size`, `max_ref_size` and `entry.size` (**Step 9b**; §4.14.9a2 says why not 9a). **This upgrades D9 from "wrong output, pinned by two gates" to a Stage 1 prerequisite.**

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
