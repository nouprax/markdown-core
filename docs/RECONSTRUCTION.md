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
| Landed | Steps 0 and 1, and §4.0's re-ordering |
| Engine | byte-identical to `580d10c` (tag v1.0.3) **except** `core/main.c`, which gained `--profile` |
| `VERSION` | `1.0.3`, and it moves to `1.0.4` at the CLOSE of Stage 0a — the first commit range that moves behaviour |
| Next action | **Stage 0a**, §4.2, beginning with 0a.1 |

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

ctest --preset correctness -j 8            # 65/65
ctest --preset correctness-asan -j 8       # 57/57 — SEE THE WARNING BELOW
ctest --preset correctness-ubsan -j 8      # 57/57 — SEE THE WARNING BELOW
node scripts/check-canonical-ast-fixtures.mjs   # 28 kinds, 47 fields, 6 cases
bash scripts/audit-public-surface.sh
node scripts/check-plan-graph.mjs                # 22 steps, 42 edges, acyclic
node scripts/fuzz-parity.mjs --iterations 300                   # upstream, 300/300
node scripts/fuzz-parity.mjs --oracle mdast --iterations 300    # KNOWN-RED, see below
node scripts/check-upstream-parity.mjs     # 795/795 vs cmark-gfm 0.29.0.gfm.13
node scripts/check-mdast-parity.mjs        # 46/46, backlog 23/23 still diverging
node scripts/audit-scope-sanity.mjs        # 207 rows, only-shrink holds
node scripts/fuzz-parity.mjs --cases 300   # 300/300
```

**A sanitizer preset with no build reports GREEN having run nothing.** With
`build/asan` absent, `ctest --preset correctness-asan` prints
`No tests were found!!!` and **exits 0**. Run the configure and build lines above
first, and treat a sanitizer run that does not report `57/57` as a failure
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
| `node scripts/check-release-version.mjs --skip-swift` | **D17 is fixed**; what remains is an unexpected legacy tag (`codex-doc-pass-backup`), which is repo hygiene, not engine state. | release |
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

`VERSION` stays **1.0.3**. It is not carried forward from `2.0.0`: that major
was bought with the session API, which no longer exists here, and 1.0.3 is
simply the truth because this engine *is* 1.0.3 byte for byte. It moves to
**1.0.4** at the close of Stage 0a, which is now the first commit range that
moves behaviour.

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
    14  Step 7   — directive grammar conformance
     5  Step 9b  — the reference node model
     1  Step 9a  — definition retention
     2  Step 6   — formula
     1  Step 10  — the split-off table lead
    --
    23  remaining
```

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

### Sixteen defects live in the baseline

The first eleven were found by reading. **All eleven have since been built,
gated and reverted** on isolated worktrees at `8e76a94` — every claim below
about a line count, a moved golden row or a green suite is a measurement, not
an estimate. Doing that found five more (D12–D16), which are recorded here
rather than in a side file, because a defect the plan does not name is a defect
the plan will re-derive later at full price.

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

The systematic analysis is the first deliverable, not a preliminary: **an
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

**`VERSION`.** The line "`VERSION` moves to 1.0.4 at the close of Stage 0a" is now in question and is tracked as **Q27**: measured, at `VERSION=1.0.4` the ordering assertion in `check-release-version.mjs` is **unsatisfiable**, because the tag `v2.0.0` exists and the script requires every existing tag to be strictly less than `VERSION` when `v$VERSION` is absent. The honest options are to leave `VERSION` at `1.0.3` with an `Unreleased` heading in `CHANGELOG.md`, or to go to `3.0.0` early and accept that the release-notes file must exist from that commit. Do not adopt a version whose only job is honesty and whose effect is to make a gate permanently unreachable.

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
7:[3,15A]       10:[0a]         9a:[0a]         11a:[0a,5,10] 8:[3,6,7,11a]
9b:[9a,10,15A]  11b:[11a,8]     11c:[9b,11a]    12:[6,7,11b,11c,15A]
13:[7,12]       14:[9b,12,15A]  15C:[12,13,14]
```

**The check is executable, and it reads the edge list above rather than a copy
of it:** `node scripts/check-plan-graph.mjs`. It resolves every named dependency
to a real step and runs a white/grey/black depth-first walk that reports the
grey-on-grey *path* if one exists, rather than merely announcing that a cycle
does. Run on the list above: **22 steps, 42 edges, acyclic.** It is in §0's gate
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

Restating a port as a requirement exposes the decisions the port had already made for us. Each is recorded here so it is tracked rather than rediscovered mid-step. They belong in §9's table; recommendations are the restatement's, not the owner's.

| id | Question | Forced by | Recommendation |
|---|---|---|---|
| **Q11** | Does the repository adopt `InsertBraces: true`? | 2 | **Yes** — it is the whole content of Step 2 (measured: `format-c.sh --check` is already green). Footprint 2,393 diff lines across 36 files, 561 of them in `core/` + `extensions/`. **If no, delete Step 2.** Land the neutrality gate with it: normalized-disassembly equality, measured 29/29 objects identical. |
| **Q12** | Is the arena deleted, or made parser-owned? | 3a | **Delete.** Measured: ~7% CLI-only parse win, **+10–16% peak RSS**, `abort()` on allocation failure inside a library with a careful sticky-OOM discipline, total sanitizer blindness on the binary the parity oracles drive, and a demonstrated **480-byte leak in a parser that never asked for it** (a global `A != NULL` makes an unrelated default-allocator parse take `table.c`'s retry branch). Parser-owned is impossible without a document-owned lifetime model this engine does not have. Output-neutral: 7,251 comparisons, 0 differences. |
| **Q13** | Is the cycle check unconditional — and is it a defect or a refactor by-product? | 3b | **Unconditional**, and *"the shipped library makes `b->parent == b` on request while the test that denies it flips a flag nothing else flips"* reads exactly like D1–D16. Measured cost: unmeasurable on four workloads, 10.7% on one already-pathological path the engine takes 36 seconds to parse. **The owner may re-file it into Stage 0a; that is the only change to 0a this restatement would ask for besides Q25.** |
| **Q14** | One knob per extension, or two? | 3, 6, 7 | **One.** Attachment is the language. Delete `MARKDOWN_CORE_OPT_DIRECTIVE` and both formula delimiter options; keep formula's `dollar`/`latex` **sub-grammar** selection only if a use is stated, and today none is. |
| **Q15** | What is the **inline** dispatch precedence? | 3 | Q9 settles the *block* order (`table` last) and says nothing about inlines — `table` has no inline hooks at all. `autolink` and `directive` both claim `':'`, and first-non-NULL wins today. **Recommend: table order is also inline order, `autolink` before `directive`** (a bare `:` far more often begins a URL), stated in the commit and pinned by a fixture. |
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
| **Q27** | Does `VERSION` move to 1.0.4 at all? | 15C | **No.** Measured: the ordering assertion in `check-release-version.mjs` passes at `3.0.0` and **fails at `1.0.4`**, because `v2.0.0` exists and the script requires every tag to be strictly less than `VERSION`. `3.0.0-dev` fails the `stableSemver` assert. Leave `VERSION` at `1.0.3` and record the state in `CHANGELOG.md` under `Unreleased`, or go to `3.0.0` early. |
| **Q28** | Is `markdown_core_parser_feed_reentrant` deleted? | 11a | **Yes.** Zero in-tree callers, and it re-enters line processing with bytes that are in no source line — unrepresentable under L1. Keeping an entry point whose only purpose is to inject bytes no position can name, in the step that establishes that every byte has a position, is carrying a contradiction forward for no consumer. |
| **Q29** | Does `mode` survive on `Code`, `CodeBlock`, `Directive`, `DirectiveBlock`? | 15A | **No** — delete it from those four, keep it on `Formula`/`FormulaBlock` where it is genuinely variable. Both decoders prove the point: Kotlin and ES hard-code the constant and one of them then *asserts* the constant it just synthesized, and the Kotlin wire format does not transmit it. A field whose value is implied by its type is ceremony four surfaces must keep in step. |
| **Q30** | Do the bindings spell child edges typed (`content`, `items`, `label`, `header`, `rows`, `cells`) or flat (`children`)? | 15A | **Typed.** Kotlin and ES already do; Swift's flat `children` is what forces `labelCount: Int?`, forces `Table.init` to filter rows by `isHeader` and `preconditionFailure` if the count is not one, and forces `children: [any Markup] = []` onto eleven leaf kinds. Two of three bindings and the contract already assume it. |

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

### 4.2 Stage 0a — the defect stage

Ordered by three rules, in this precedence: **(1)** an oracle's first reading is
taken on the *unfixed* tree, so the two new position oracles land alone and
first; **(2)** memory-unsafety and data loss before wrongness; **(3)** a defect
whose *statement* changes under a later step lands before that step.

| # | Lands | Defects | C lines | Goldens moved | New gate | Cost |
|---|---|---|---|---|---|---|
| 0a.1 | Two oracles, no engine change | — | 0 | 0 | **inline-sourcepos oracle**, **scope-containment invariant** | ~120 script lines, ½ day |
| 0a.2 | Footnote data loss | D10, D11 | 19 | 1 + 1 ledger | 2 regression examples, 1 upstream model delta, 1 `expectedDivergence` | 1 day |
| 0a.3 | The out-of-bounds read | D4 | 1 | 0 | a debug assertion the existing ASan/UBSan presets trip | 1 hour |
| 0a.4 | Extension registration | D1, D2 | 3 (all deletions) | 0 | 3 mdast corpus rows, 3 engine examples, 1 structural invariant | ½ day |
| 0a.5 | Cross-extension interference | D8 | 6 | 0 | `extensions-conflicts.txt`, 2 examples | ½ day |
| 0a.6 | Positions | D3, D7 | 7 | 13 | 2 regression examples; 0a.1's oracle goes 13 → 1 | 1 day |
| 0a.7 | The two title defects | D5, D6 | 3 | 18 + 1 assertion | 1 regression example; activate `refdef-title-rewind` | ½ day |
| 0a.8 | **D9 pinned, not fixed** | — | 0 | 0 | order-independence oracle (**registered red**), output-size bound (green) | ½ day |
| | **total** | **10 fixed, 1 pinned** | **39** | **32 + 1 ledger** | **~13 examples, 4 oracles** | **~5 days** |

**0a.0 — reconcile the gates before touching the engine.** No engine change
except D17. Four items, all verified:

1. **Repoint `specs/`.** The doc move to `docs/deprecated/` repointed `scripts/`
   and `.github/` but NOT `specs/`, and the manifests are executable policy that
   name doc paths. `specs/canonical-ast/manifest.json` named two files that no
   longer existed, which is why the fixtures checker reported "manifest contract
   paths drifted". Ten files repointed. *Done.*
2. **Restore the two era-skewed checkers** to their baseline versions, per the
   rule in §0. Both green at baseline, both red at main's. *Done.*
3. **D17 — the version macro.** At `580d10c` the header declares
   `MARKDOWN_CORE_VERSION ((1 << 16) | (0 << 8) | 0)` — **1.0.0** — beside
   `MARKDOWN_CORE_VERSION_STRING "1.0.3"`. The shipped v1.0.3 tag therefore
   answers 1.0.0 to any consumer doing a compile-time version check. `main` has
   it consistent, so it was fixed later and the reset brought the defect back
   with the rest of the baseline. One line. It does not wait for the 1.0.4 bump,
   because it is wrong at 1.0.3 too.
4. **Register the known-red checks** in §0 with their owner steps, so a red gate
   is a statement rather than a surprise.

**0a.1 — the two oracles that must exist before any position is touched.**
Neither exists today and neither can be replaced by a corpus row.
(a) *Inline sourcepos vs upstream*: compare every inline `code` and
`html_inline` position from the already-pinned `cmark-gfm --to xml --sourcepos`
against our dump over all 671 `spec.txt` examples. ~25 lines of Node, no new
dependency. **Baseline reading: 13 mismatches.** After D3: 1 (spec example 200,
a table cell whose de-escaped content buffer differs from the source — the
pre-existing content-offset-as-column class, unrelated). (b) *Scope containment*:
for every node, `parent.start ≤ child.start` and `child.end ≤ parent.end`. This
one **cannot** be an upstream comparison, because upstream has D7 too.

**0a.2 — D10 and D11, the two data-loss defects, first among the fixes.**
D10 is two hunks: take the label from buffer offsets
(`markdown_core_chunk_dup(&subj->input, opener->position + 1, initial_pos -
opener->position - 2)` — byte-for-byte the expression `handle_close_bracket`
already uses 58 lines earlier for `raw_label`), and set
`fnref->start_line = opener->inl_text->start_line`. Underflow is impossible by
construction: the branch is entered only when `input[opener->position]` is the
`^`. Plus four lines in `blocks.c:625` positioning the `calloc`'d replacement
node. D11 is one word — `EXIT` → `ENTER` at `blocks.c:578` — plus an 8-line
sweep before `blocks.c:675` clearing `node` on every map entry whose node still
has a parent, because a duplicate that lost is still spliced where the author
wrote it and the *tree* owns it. **Neither hunk alone is enough**, measured: with
only the `ENTER` change the loss changes victim.

**Land D10 before D7 and the scope ratchet nets out.** D10 turns
`regression.txt:474`'s sentinel into a real position (17 → 16 for that file);
D7's continuation-line fixture adds one `SoftBreak` sentinel (16 → 17). The
only-shrink ratchet then never sees a growth and `specs/scope-sanity/ledger.json`
needs no recorded exception. *This composition is predicted from two separate
measurements — verify it in the 0a.6 commit, and if it does not hold, record the
exception the way the ledger's own `purpose` field already records this exact
precedent.*

**0a.3 — D4.** Swap the operands: `while (after_char_pos < subj->input.len &&
subj->skip_chars[peek_at(subj, after_char_pos)])`. The gate cannot be an output
test — the fix changes no output over 20,797 compared inputs — and it cannot be
ASan, which is blind here by construction. It is an assertion immediately before
the subscript, under `#ifndef NDEBUG`, which the existing `correctness-asan` and
`correctness-ubsan` presets then trip on `printf 'a *~~' | markdown-core
--profile gfm`. **Without it this fix is unfalsifiable by the current harness.**

**0a.4 — D1 and D2, before Step 3 writes the descriptors.** Three deletions:
`formula.c:613`, `directive.c:1380`, `directive.c:1377`. Keep
`strikethrough.c:110`. The gate for D1 already exists and is only corpus-blind:
`check-mdast-parity.mjs` runs at `--profile gfm-extended` and remark gives the
right answer — proved with the script's own `--corpus` override, 0/3 before and
3/3 after on `foo:_bar_`, `foo$_bar_`, `a}*.foo.*`. `check-upstream-parity.mjs`
can **never** see D1: it runs `--profile gfm`, which is precisely the profile
that detaches both extensions. That is why 795/795 is green over a live
wrong-output defect. D2, once D1 is fixed, has no behavioural signature at all;
its honest gate is **structural** — every byte an extension registers in
`special_inline_chars` is either dispatched by its `match_inline` or is an
internal sentinel (`< 0x20`, used only as a delimiter tag). ~20 lines, and it
catches the next one too.

**0a.5 — D8.** Six `return parent_container;` → `return NULL;` at
`table.c:325, 329, 337, 354, 365, 372`. **Do not** take the tempting caller-side
fix in `blocks.c` that treats `new_container == *container` as a decline: the
success path at `table.c:457` legitimately returns the same node, because the
paragraph is retyped in place. **Do not** take the reorder the old plan proposed
as Step 4h either: moving `table` last was built and measured to be
output-identical to the return fix today (0 differences over 995 inputs), but it
works only because nothing is registered after `table` once it moves, and it
perturbs a global list instead of correcting the one function that lies. The
gate is a new `tests/fixtures/extensions-conflicts.txt` framed as an
independence property — *enabling `table` must not change how another
extension's block opener parses* — with the expectations taken from the
no-table parse. Measured: 0 passed / 2 failed at the baseline, 2 passed / 0
failed with the fix.

**0a.6 — D3 and D7.** D3: delete the four-line `OPT_SOURCEPOS` guard and amend
`inlines.c:343` for the container-relative end column. D7: add
`+ subj->column_offset + subj->block_offset` to both assignments at
`inlines.c:221-222`. They compose — applied together the combined golden
movement is exactly D3's 13 rows and nothing more, and the interaction case
`` a `x\ny` <https://e.example/> z `` becomes correct where cmark-gfm
`--sourcepos` is still wrong. Three of D3's 13 new rows carry an end column of
**zero** (`Code scope=1:3..3:0`) for a content span ending on a newline. That is
byte-identical to cmark, so it is not a regression — but column 0 is not a place
either, and it is a **fourth class** the scope-sanity ledger does not classify
(it tracks sentinel, negative and line-zero). Name it in the ledger's `purpose`
field in this commit; do not let it enter the corpus unnamed.

**0a.7 — D5 and D6.** D5 adds one line inside the successful rewind: set `title`
to exactly what the no-title branch four lines above already sets
(`inlines.c:1755`, `markdown_core_chunk_literal("")`). No free is needed — the
stale chunk came from `markdown_core_chunk_dup`, which borrows. **Note the
tension and do not try to resolve it here:** that value is the written-and-empty
chunk, so the definition path already records an absent title the way D6 records
an absent autolink title. `inlines.c:1755` is therefore a *third* site of the
null/empty violation, alongside D6's `inlines.c:219` and D16's
`inlines.c:1300`. D5's job is to make the rewind path agree with its own
neighbour; making all three say "not written" is Step 14, and doing it here
would smuggle a rule change into a defect fix. Its gate is already written and waiting:
move `refdef-title-rewind` from `pendingDeltas`/`pendingExpectedDivergences` into
`deltas`/`expectedDivergences` and add the registered input to `regression.txt`.
Measured: 796/796 with `registered divergences: 1/1 inputs reproduced`. D6
deletes one line and moves 18 golden rows plus one assertion string in
`tests/api/main.c:1075`. **No parity oracle can ever police D6** —
`scripts/lib/upstream-cmark.mjs:174` maps `title:""` to `"null"` before
comparing, on all three of them. A golden dump is the only mechanism in this
repository that can hold that fix down, which is exactly why the 18 rows must be
regenerated *once*, deliberately, in a commit whose subject says so.

**0a.8 — D9.** No engine change. Two gates, and the *statement* of the defect
recorded next to them, so that the next reader of `map.c:307` finds out from the
code what the 68 GB experiment cost to learn.

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

### 4.10 The release from this base is 3.0

**Owner ruling, 2026-08-20.** There is no 1.0.4 release. The version moves to
1.0.4 as an internal alignment marker so the numeric macro and the version
string stay honest with each other, and it carries no release obligation.

Two consequences worth stating, because the plan was written assuming otherwise:

- **The ABI break window is not a constraint.** R4 and Step 12 were built around
  batching six public breaks into one release so consumers broke once. Shipping
  3.0 from this base means the surface is free to change as the design requires,
  and the discipline that remains is only that it changes *deliberately* and
  the bindings follow. Step 12 keeps the "write the target header first" method
  and loses the "one window" urgency.
- **The release gates are off the critical path** until 3.0. `check-release-version`'s
  legacy-tag condition, the release notes and the README examples are a
  3.0 obligation, not a Stage 0a one.

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

**Defects** — all seventeen of §2 closed, or explicitly carried with a named
owner step and a registered known-red gate.

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
| D3 | regenerated `spec.txt` (13 rows) **+ the new inline-sourcepos oracle** | oracle new | yes — restoring the guard with the new goldens in place makes `spec_commonmark` FAIL | **none today.** Both parity gates compare rendered output; `audit-scope-sanity.mjs` reads 207 before *and* after, because it classifies only sentinel, negative and line-zero rows |
| D4 | `assert(after_char_pos < subj->input.len)` under `#ifndef NDEBUG`, tripped by the existing ASan/UBSan presets on `a *~~` | **new** | yes — kills the operand-order revert | **none, and no sanitizer either**: 0 ASan reports over 14,783 executions of the read |
| D5 | 1 example in `regression.txt` + activating `refdef-title-rewind` in `specs/upstream-parity/deltas.json`; `check-upstream-parity.mjs` then requires the divergence to still reproduce | rows only | yes — 796/796, `registered divergences: 1/1` | **upstream parity**, and only once registered: `regression.txt` is in the parity corpus, so adding the example without registering the delta fails the gate |
| D6 | the 18 moved golden rows, strongest at `extensions.txt:667` (both spellings of one construct, three columns apart on one line) | existing | the goldens are the gate | **none.** `scripts/lib/upstream-cmark.mjs:174` folds `title:""` to `"null"` before comparing, for all three parity oracles |
| D7 | 2 examples in `regression.txt` (blockquote pins `block_offset`, continuation line pins `column_offset`) **+ the new scope-containment invariant** | **both new** | yes — reverting the two lines makes `regression_commonmark` FAIL | **none, and upstream cannot be the oracle** — cmark-gfm reports the same wrong columns |
| D8 | new `tests/fixtures/extensions-conflicts.txt`, 2 examples, framed as *enabling `table` must not change another extension's block opener* | **new** | yes — 0/2 at baseline, 2/2 with the fix | **none.** The corpus tests one extension at a time: 761 of 798 examples enable nothing, and no example ever co-enables `table` with `formula` or `directive` |
| D9 | order-independence oracle (**registered red**, names Step 9a) + output-size bound in `complexity_runner.c` (green) | **both new** | n/a — the fix is Step 9a | **none.** With the budget deleted, every existing gate stays green while 1 MiB of input produces 68.7 GB of output |
| D10 | position half: `regression.txt` example 24 **already exists and pins the defect** — unpinning it is the gate. Byte half: new example `x[^a\nb] tail` + an `expectedDivergence` | half new | yes, both halves | **half.** No corpus input loses bytes here, and upstream loses the same bytes |
| D11 | new `regression.txt` example (the nested-duplicate reproducer) + an upstream **model** delta; sanitizers and `leaks --atExit` gate the ownership half | **new** | the minimal fix moves zero goldens, so the example is mandatory | **none.** Nothing in the corpus has a nested duplicate label |

**Four defects — D2, D4, D7, D8 — are invisible to every oracle in this
repository, and their gates are assertions and structural properties rather than
output comparisons.** That has a consequence worth stating in §8: a later
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
| **Q9** | What is the extension attach order? (D15) | **SETTLED 2026-08-20 — table LAST, with a test.** A decided order, not an inheritance: a table's row opener matches any line inside an open table, so every narrower claim attaches first. D15's CLI/facade disagreement is fixed in the same step. | owner | Step 3, 0a.5 |
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
