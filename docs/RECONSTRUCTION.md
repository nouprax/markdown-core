# Reconstruction

**This document governs.** Where it disagrees with any other document in this
repository, this one is right and the other is stale. Everything under
`docs/deprecated/` is archive: it describes engines and programs that either no
longer exist or have not been rebuilt yet. Nothing there is normative. A
deprecated document returns to `docs/` only when the step that makes it true
again has landed, and only by a deliberate commit that says so.

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
simply the truth because this engine *is* 1.0.3 byte for byte. It moves when
Step 4 first moves behaviour.

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
    14  Step 7  — directive grammar conformance
     6  Step 9  — one reference model
     2  Step 6  — formula
     1  Step 10 — the split-off table lead
    --
    23  remaining
```

**When this list is empty, Stage 0 is done.**

### The standing gate

> **No commit on this branch may leave `spec_commonmark` failing.** If a step
> legitimately moves spec output, the golden is regenerated in that same commit
> and every moved row is reviewed by hand and named in the commit message.

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

### Nine defects already live in the baseline

1. `set_emphasis(ext,1)` folds `$ \ : ] }` into `parser->skip_chars`, which
   `scan_delims` walks over — this kills CommonMark flanking.
   `formula.c:613`, `directive.c:1381`
2. `'}'` is registered as a special inline char that `match()` never consumes.
   `directive.c:1379`
3. `adjust_subj_node_newlines` is gated behind `OPT_SOURCEPOS`, which **nothing
   in the tree ever sets** — multi-line inline Code and HTML carry wrong end
   positions. `inlines.c:334-336`
4. A one-byte out-of-bounds read: `skip_chars[peek_at(subj, after_char_pos)]` is
   evaluated before the bounds test, and `peek_at` is unguarded. `inlines.c:492`
5. On the title-rewind path `title` still holds the scanned chunk and is written
   into the refmap. `inlines.c:1749-1765`
6. `make_autolink` writes `title = chunk_literal("")` where nothing was written.
   `inlines.c:219`
7. `make_autolink` omits `column_offset + block_offset`. `inlines.c:221-222`
8. `try_opening_table_header` returns `parent_container` on eleven non-opening
   paths. `table.c:325..457`
9. The reference expansion budget makes whether a reference resolves depend on
   how many resolved before it. `blocks.c:802-805`, `map.c:307-309`
10. **An undefined footnote call LOSES SOURCE BYTES**, in the default profile,
    because footnotes are on by default (`core/main.c:133`). Thirteen bytes in,
    nine characters out:

    ```
    $ printf 'x[^a\nb] tail\n' | markdown-core --profile gfm
    Paragraph scope=1:1..2:7 children=1
      Text    scope=1:1..1:7 literal="x[^] tail"
    ```

    The `a`, the newline and the `b` are in no node. The child also fails to
    span its parent. Cause: the underflow guard at `inlines.c:1352-1356` on the
    raw column-arithmetic slice the `noMatch:` path takes. The same path emits
    impossible positions — `[^~~x~~] tail` yields `Text scope=0:0..0:13`, line
    zero with column thirteen. Closed by Step 9 (see §5.7).
11. **A duplicate footnote definition can delete a block of content.**
    Definitions register on the iterator's `EXIT` event (`blocks.c:578`), which
    is post-order CLOSE order, and the map's tie-break is registration `age`
    (`map.c:189`). A duplicate nested inside another therefore closes first and
    wins over the one that opened earlier. The outer then has no reference, so
    the unreferenced drop (`blocks.c:668-671`) unlinks and frees it:

    ```
    Ref [^dup].

    [^dup]: OUTER opens first

        [^dup]: INNER closes first
    ```

    `"OUTER opens first"` is in no node. Closed by Step 9, which must also move
    registration to document order — see the falsifier in §5.8.

---

## 3. The roadmap

Three stages, in order. **Do not begin a stage before the one above it is
finished**, and in particular do not collapse Stage 1 into Stage 2 — that
collapse is what forced the two-tree shadow design last time.

### Stage 0 — Reconstruct

Re-apply, onto the 1.0 baseline and by hand, exactly three things:

1. directive support (grammar conformance),
2. the formula fix,
3. CST and diagnostic support.

Nothing else. The port list is §4.

### Stage 1 — Make the parser pausable at line boundaries

A major refactor whose goal is a property of the **parser's state machine**, not
of the tree: every piece of state the parser carries is explicit and preserved,
so the data flow can be paused at a breakpoint **after each line**, a snapshot
taken as the current output, and then continued.

Deliverable: **line-by-line append.**

The decisive property, and the one missed last time: **at a line boundary there
is no held partial line at all.** Stage 1 therefore has *zero* partial-line
complexity. Every problem that wrecked the previous attempt — running a held
line into a copy, un-running it, the delimiter engine seeing a second unit, an
inline scan straddling a boundary — does not exist in Stage 1.

Its oracle is exact and cheap:

> For every partition of the input **on line boundaries**, the resulting tree
> must equal a one-shot parse of the same bytes.

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

| # | Step | Diff. | Size | Depends on |
|---|---|---|---|---|
| ✅ 0 | Reset the engine to the 1.0 baseline | [CP] | — | — |
| ✅ 1 | **Restore the oracles before touching engine code** | [CP] | ~1,400 script lines | 0 |
| 2 | Formatter config, applied once | [CP] | mechanical | 0 |
| 3 | Static extension descriptors, no process-global state | [HW] | ~600 | 0–2 |
| 4 | Behaviour fixes that need no new architecture | [HW] | ~250 | 3 |
| 5 | Iterator contract, and the use-after-free it fixes | [CP] | ~120 | 3 |
| 6 | Formula — *deliverable #2* | [HW] | ~200 | 3–4 |
| 7 | Directive grammar conformance — *deliverable #1* | [HW] | ~330 | 3–4 |
| 8 | **Decision fork:** the unified delimiter engine | [HW] | ~1,100 | 3–4, 7 |
| 9 | **One reference model** | [HW] | +1,680 / −450 | 11 (hard) |
| 10 | Position defects that need line marks | [CP] | ~60 | 9 |
| 11 | CST: concrete records — *deliverable #3* | [CP]/[HW] | ~2,150 | 11a: none |
| 12 | CST facade, and the ABI break window | [HW] | ~400 | 6c, 7, 11 |
| 13 | Diagnostics — *deliverable #3* | [HW] | ~250 + ~130 | 7, 12 |
| 14 | The null/empty rule made structural | [CP] | ~40 | 12 |
| 15 | Bindings, specs, docs | [HW] | ~500 | 12–14 |

Notes that change the order or the risk:

- **Steps 0 and 1 have landed.** What Step 1 actually cost, recorded because
  the next person will hit the same thing: main's policies could not be copied.
  Two corpus fixtures had left with the cross-link/embed feature; three upstream
  divergences and two mdast divergences describe fixes not yet re-applied and
  moved to `pendingDeltas` / `pendingExpectedDivergences`, each naming the step
  that restores it; the mdast self-test canary asserted the padding-stripped
  literal, which the baseline does not produce, and now asserts `" mid "` until
  Step 6a flips it back. The CLI also gained `--profile`, which the harness
  invokes and the baseline lacked — a named option set only, with the extension
  attach *order* deliberately untouched, since reordering `table` is Step 4h.
- **Step 1 is non-negotiable and must not be deferred.** The parity oracles
  (`check-upstream-parity.mjs`, `check-mdast-parity.mjs`, `fuzz-parity.mjs`,
  `check-coverage.mjs` and the `specs/*-parity/` directories) arrive at
  `3d8d329` (#79). At the baseline there are **zero** of them. Every "the parity
  gates pass" claim in this repository's history is a statement about a harness
  the reset removed. `deltas.json` must be **re-pinned against the untouched
  baseline binary** — whatever cmark-gfm and mdast disagree about at 580d10c
  *is* the baseline delta set. Do not copy `main`'s; it encodes fixes not yet
  made.
- **Step 3 must come before every later step.** It removes the process-global
  registry and the `once`, which also makes a paused parser a plain struct in
  Stage 1.
- **Step 8 is the one genuine branch point.** It carries four syntax fixes and
  unblocks the CST's inline one-funnel property. Recommendation: take it, but
  only after 4–7 have landed and their goldens have been regenerated, so the
  largest risk is not entangled with a regeneration it did not cause.
- **Step 11a has no prerequisites at all** (see §6) and is the fastest proof
  that the CST verdict is right.

---

## 5. Step 9 — one reference model

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
 * `key` is the match key: full Unicode case fold, trim, collapse internal
 * whitespace — and for a footnote it KEEPS its leading `^`, so that a link
 * definition and a footnote definition of the same name cannot collide in a
 * consumer's single map.
 *
 * NORMATIVE: `key` is compared with memcmp over its bytes. It is never case
 * mapped, never NFC/NFD normalized, never re-encoded, and never used as a key
 * in a language map whose `==` has an opinion about Unicode. Bindings expose
 * it as an opaque byte-hashable, not as a string. */
typedef struct {
    markdown_core_chunk label;
    markdown_core_chunk key;
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

This is a mixin, not a union of convenience: the arms share a common initial
sequence, so C guarantees `node->as.association.label` may be read for a node
holding *any* of them. One field, one offset, one read, five kinds — and the
guarantee is in the language, not in a comment.

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

### 5.7 D2 is settled: the interior of a failed footnote call is reparsed

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

### 5.8 D4 is settled: both kinds carry the match key

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
reference carries an identifier; it is which one. `label` + `key` replaces both
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

**Cost: the node struct gets smaller.** A reference `{label, key, form}` is 40
bytes — exactly the width of the widest existing payload arm (`code`), so the
union does not grow. The definition is boxed. Deleting `parent_footnote_def`
removes 8 bytes from *every* node: −800 KB on a 100,000-node document. The key
bytes are an ownership move, not a new allocation — the parser already
allocates exactly one per occurrence, and today frees them with the refmap at
teardown while the document keeps only `root`.

**Against mdast, checked afterwards:** convergent on the key, on keeping the
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

**The falsifier, and it has already fired once.** The case against an edge rests
on the winner being derivable as "group by key, first in document order". Today
it is not: registration is on the iterator's `EXIT` event, so close order beats
document order (defect 11). **Step 9 must move registration to document order**,
and the gate is: for every reference in the corpus, the definition selected by
that derivation is the definition the engine matched. If that proves
unfixable, the key is insufficient to identify a node and the definition needs a
`winner` bit.

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
`definition_anchor_position` (G2), `no_tree_rewrite_after_parse` (G3),
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

What *is* entangled, and is in scope: reference-definition records need Step 9;
table records need line marks; the inline one-funnel property needs Step 8.
Everything else — the whole block half, all ten block sites, the node slots, and
the region partition — needs nothing beyond the baseline.

---

## 7. Drop list

**Whole programs, not partial drops.** Named exhaustively so nothing creeps back
one hunk at a time. 23 commits are dropped whole.

### D-1 · The session / incremental / delta / streaming / append program

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

### D-2 · Out of stated scope

CrossLink `[[ref]]` and Embed `![[ref]]` (`cross_reference.{c,h}`). Self-contained
and low-risk *if scope ever widens*, but its two `inlines.c` hunks are deleted
again by the delimiter engine, so porting it and then taking Step 8 means
writing and unwriting the same code.

### D-3 · Behaviour-neutral churn with no consumer

The reference-map v2 rewrite (a one-shot harvests every definition before any
lookup, so the observable result is identical); the self-referential allocator
ABI; the 2.0.0 version bump; the formatter commit as a *diff* (take the config,
not the patch).

**One exception worth remembering:** the per-block `postprocess_block` pipeline
is dropped as a session artifact, **but it is the correct shape for a resumable
parser.** Port it deliberately when Stage 1 begins — not now.

### D-4 · Streaming-only fixes that look like engine fixes

List-tightness flag copying in the formula shell is inert in a one-shot, because
tightness is settled in `finalize` before postprocess runs.

---

## 8. Risks

| ID | Risk | Cheapest experiment | Cost |
|---|---|---|---|
| R1 | **The first behaviour change lands with no oracle.** Steps 4, 6 and 7 all move parse output. | Step 1 verbatim on the *untouched* baseline. The output is the delta pin. | ½ day |
| R2 | The delimiter-engine fork is the largest unknown — ~1,100 lines gating the CST inline funnel. | Land 11a alone, then attempt inline capture against baseline emphasis code for one fixture. If only the funnel test fails, the price of skipping is known exactly. | 2–3 days |
| R3 | ~~Footnote v1 deletion removes numbering~~ | **Retired.** See §5.4. Replaced by gate G6. | — |
| R4 | Six independent ABI breaks, unbatched. | Write the target public header first, as one diff against the baseline's 232 lines. | 1 afternoon |
| R5 | Removing `VALIDATE_UTF8` is a live product change; the facade sets it unconditionally. | Build twice, run the fuzz corpus and spec fixtures through the dump CLI, diff. | 1 hour |
| R6 | `strip_html_comments` removal has the same shape. | Same corpus diff, plus grep the bindings. | 1 hour |
| R7 | **Positions are invisible to every oracle.** Neither parity gate compares them — which is why these defects survived. | Port the scope-sanity ratchet and re-pin its ledger *before* Step 4. | ½ day |
| R8 | Unclear whether the iterative dump stack or the canonical walk is needed. | Dump a 50,000-deep blockquote; time the binding scope walk. | 2 hours |
| R9 | 20,459 lines of checked-in re2c output, and **no re2c invocation or version pin** in the build. | Regenerate from the untouched `.re` and diff. | 2 hours |
| R10 | The CST test debt is the bulk of Step 11 — a 7,067-line runner, half of it streaming. | Extract the 14 non-streaming cases into a standalone runner *at HEAD first*. | 1 day |
| R11 | Option-struct layout across three bindings. | Fold into R4; the bridge asserts fail loudly at build time. | — |

---

## 9. Open decisions

Each is stated as a question with a recommendation. **D2 and D4 are the two
where the recommendation is genuinely contestable** and should be settled by the
repository owner before Step 9 is written.

| ID | Question | Recommendation |
|---|---|---|
| D1 | Is an unreferenced definition kept? | **Keep both kinds.** Dropping requires the post-parse rewrite Step 9 exists to delete. |
| ~~D2~~ | Is the interior of a failed footnote call reparsed? | **SETTLED — yes.** Not by analogy: the construct that fails is an unmatched `[`, which CommonMark specifies normatively, and nothing authorizes an extension to free children core already built. The flattening also loses source bytes (defect 10). See §5.7. |
| D3 | Do footnote references carry a form? | **No.** mdast gives `footnoteReference` Association and not Reference; one call syntax means a field with one value. |
| ~~D4~~ | Does the node carry a normalized `identifier`? | **SETTLED — yes, on both kinds, plus the exported fold.** Derived from the lattice, not from mdast. The "export the normalizer instead" answer was wrong: the consumer has no engine to call, because every binding frees the handle and copies into value types. See §5.8. |
| D5 | Does the dump keep `id=` for footnotes? | **Rename to `label=`.** Two names for one field after unifying the field is the failure mode that produced three accessors. |
| D6 | Where does a footnote definition's scope start? | **At `[^`.** It is the only block in the engine opened past its own marker, and the binding docs already promise otherwise. |
| D7 | Is a definition's destination optional? | **Required.** The null is reachable only through allocation loss, where C and Swift currently disagree. Set the failure bit and emit no node rather than a node that lies. |

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
