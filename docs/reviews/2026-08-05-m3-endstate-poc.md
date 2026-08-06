# M3 end-state PoC — the substrate fork, and what a dumped scope is

Date: 2026-08-05. Base: `main` at `dc36cb3` (the merge of #94, closing the engine
hardening slice).

Provenance: a slice plan for M3 was drafted and then held against a stricter
question than "does each slice ship" — namely, does the *sum* of the remaining
milestones land on a repository with a single source of truth, no special-case
tuning, and O(edit) commit cost. Scored against that question the draft reached
one of seven stated invariants, so the plan was not refined; it was tested. Five
proof-of-concept spikes were built, three of them in throwaway worktrees that
modified real code. The spikes are discarded. Their measurements, and the
decisions those measurements force, are this document.

Every number below was produced by running something. Where a spike is cited,
its build and test output was reproduced against `dc36cb3` before the claim was
written down.

Verdict up front: the milestone ordering was wrong, and the reason is not
scheduling. **M3 becomes the substrate milestone.** It adopts
`extensions/source.c` as the live byte store and retires `core/text.c`, work
that only a comment ever assigned to M7. Separately, the PoCs found that a
dumped `scope=` is not always a position — 216 of 5,385 golden rows are a
sentinel or an arithmetic impossibility — and that this has one cause, which
extents remove by construction.

## The fork

> Does M3 build a standalone unit sequence alongside the two byte substrates
> that exist today, or does it perform the substrate unification now?

Two substrates exist. The live one is the flat contiguous `markdown_core_text`
(`core/text.c`), held at `extensions/session_internal.h:293`. The other is
`extensions/source.c`: a persistent AVL rope over stored bytes, with subtree
byte sums (`source_node.length`, `:26`), refcounted immutable buffers,
path-copying `concat`/`slice`, and structural sharing. It is 737 lines, it
carries **no entry** in `specs/coverage/policy.json` — that is, it is at 100% on
every required metric — and it is dark: `markdown_core_source*` is referenced by
`source.c`, `source.h`, and `tests/runners/source_runner.c`, and by nothing else
in the repository.

`extensions/source.h:22-24` says the session adopts it "at the M7 flip." That
sentence is a comment. It is not a gate, it is not in
`docs/specs/incremental-canonical-ast.md`, and it is not in the delivery plan's
M7 section, which describes the public C header, the delta engine, and the
four-binding move (plan:736-753) and says nothing about the byte store.

### Resolved: unification now

Four independent lines, none of which is a cost argument.

**1. The flip is measured, not estimated.** The spike replaced
`markdown_core_text` with `markdown_core_source` under `PERMISSIVE_BYTES`,
touching five files for **153 insertions and 34 deletions**. Reproduced here:
`cmake --build` clean, `ctest --preset correctness` **164/164**, and
`ctest --preset correctness-asan` exit 0. `markdown_core_text` has zero
remaining references in `extensions/`.

**2. There is no contiguity blocker.** The census of the flat store is eleven
call sites. The two that appear to need a pointer into the whole buffer are both
`markdown_core_parser_feed`, and the feed path is already streaming:
`S_parser_feed` (`core/blocks.c:1236-1305`) accumulates partial lines in
`parser->linebuf` and handles a CR/LF split across two calls
(`parser->last_buffer_ended_with_cr`, `:1246`, `:1294-1296`), while
`S_process_line` copies the fed bytes into `parser->curline` on entry
(`:2253-2257`) and works from that copy. No pointer into the caller's buffer
survives a call. The parser's entire coupling to the substrate is "hand me
bytes, any chunking."

The one place the flat shape is genuinely used is
`extensions/incremental.c:2237-2250`, the only backward byte scan in the
codebase: it walks left from the restart byte to find the previous line start.
It is bounded by one line, so it is not asymptotic, but it needs a reverse
cursor on the rope rather than repeated `byte_at`.

**3. Deferring the substrate is deferring the milestone, not the cost.** The
four ordered side indices — the clean index
(`extensions/session_internal.h:226-237`), the per-kind definition index, the
suffix document-children shift, and `pipeline->line_offsets` — all reduce to
prefix-sum, predecessor, or rank queries over one document-wide sequence, and
three of the four reduce to nothing at all, their answer falling out of the
splice. But the reduction buys O(edit) **only if that sequence is persistent**,
and a persistent sequence over source is the substrate question. There is no
ordering in which it is answered later.

**4. The flat store is itself an O(document) per-edit cost.**
`markdown_core_text_edit` memmoves. Measured, keystroke at the document
midpoint, edit and commit per keystroke:

| document | edit, flat | edit, rope | commit, flat | commit, rope |
| --- | --- | --- | --- | --- |
| 359 KB | 2.51 µs | **0.43 µs** | 18.5 µs | 19.2 µs |
| 2.75 MB | 19.1 µs | **0.39 µs** | 79.0 µs | 80.0 µs |

Edit cost stops depending on document size. Commit cost moves by 1.3–3.7%,
inside the noise band the paired-benchmark convention already uses.

### What the fork does not decide

Unifying the byte substrate is not the same as fusing unit boundaries into it.
The fusion spike found the usual objection — that rope leaf windows are chosen
by edit history (`source.c:136-147`, `:266-283`) and so never align with unit
boundaries — to be **false as stated**, but false only because the splice must
then be unit-aligned, which forces the source structure to be built after the
parse rather than before it. That precondition is invisible in the type system,
and a single raw byte apply violates it silently: bytes stay correct while the
unit count drifts and compounds. It therefore needs a gate, not a comment. The
grain is also load-bearing: at AST-leaf/CST-region grain the fused form measures
41.30 B/unit resident, about 1.6× the document; at line-and-whitespace grain it
is roughly 9×, which would make the fusion memory-negative.

## A dumped scope is not always a position

This was not the question the PoCs were asked. It is the largest thing they
found.

### The defect

A closed `(line, column)` interval cannot express an empty range. A zero-width
node has no honest spelling in that representation, and the engine resolves the
problem two different ways, both of which are frozen in goldens.

`core/inlines.c:2494-2520` shows the arithmetic and the choice in one function:

```c
static void S_update_text_sourcepos(markdown_core_node *node) {
    if (node->start_line == 0) return;
    if (node->as.literal.len == 0) {
        node->start_line = 0; node->start_column = 0;   // :2500-2503
        node->end_line = 0;   node->end_column = 0;
        return;
    }
    int end_column = node->start_column - 1;            // :2508
    for (...) end_column++;                             // zero iterations
```

For an emptied node the loop runs zero times and `end_column` is
`start_column - 1`. Rather than emit that, the function zeroes the node.

Elsewhere the same value is emitted. Measured over
`packages/markdown-core/tests/fixtures/*.txt` and `specs/canonical-ast/*.ast`,
5,385 `scope=` rows:

| class | rows | composition |
| --- | --- | --- |
| sentinel `0:0..0:0` | **199** | `SoftBreak` 145, `LineBreak` 18, `Text` 36 |
| negative, `end < start` | **17** | `HTMLBlock` 13, `Text` 2, `TableCell` 1, `FootnoteDefinition` 1 |

216 rows, 4.0% of the position output. The negative class reproduces live:

```
$ printf '<style>p{color:red;}</style>\n' | markdown-core
Document scope=1:1..1:28 children=1
└── HTMLBlock scope=1:1..0:0 comment=false literal="<style>p{color:red;}</style>\n" children=0
```

The document is one line of 28 bytes. Its only child reports that it ends on
line 0.

### The sentinel carries four meanings

`line == 0` is read at eight sites and means four different things, across two
structs, disambiguated by a flag and a type test:

| read | meaning | disambiguated by |
| --- | --- | --- |
| unsealed node | soft or hard break: no position by construction | `SEALED_RELATIVE` absent (`extensions/session.c:289`) |
| unsealed node | a `Text` emptied by `markdown_core_node_unput`: position lost | indistinguishable from the above |
| sealed node | delta zero — starts on the parent's line, entirely legitimate | `SEALED_RELATIVE` present, plus `BLOCK_P` (`extensions/incremental.c:2965`, `:2973`) |
| `sourcepos_cursor` | the cursor has no position basis | a different struct (`extensions/autolink.c:193`, `:210`, `:215`, `:230`) |

### What it costs beyond the sentinel

The consumed bytes have no owner. For `aaa\nbbb` the dump is
`Text 1:1..1:3`, `SoftBreak 0:0..0:0`, `Text 2:1..2:3`: the newline at line 1
byte 4 belongs to no node. For the two-space hard break `aaa␣␣\nbbb` the dump is
`Text 1:1..1:5` for a literal of `"aaa"` — the break's own markup bytes are
charged to the preceding `Text`.

Both are incompatible with the extent model, and not as a matter of taste.
§7.2's sequence carries subtree byte sums; the single-truth property the fused
substrate is chosen for is that `Source.length` and the extent byte sums are the
same number by construction. A byte owned by nothing breaks that identity, and a
byte charged to the wrong owner breaks §11.1's rule that markup material belongs
to the node whose grammar consumed it.

### Why extents dissolve it

A `SourceExtent` is an identity plus a length. Length zero is expressible. The
emptied `Text` becomes a zero-length extent at a real place; the break becomes
the extent of the newline it consumes; the two hard-break spaces become the
break's own extent rather than the neighbour's. No sentinel is required, so no
disambiguation is required, so the four meanings stop colliding.

This is why the earlier framing of `SoftBreak 0:0..0:0` as a compatibility
obligation to upstream was wrong and is withdrawn. It is not a compatibility
contract. It is what a representation does when asked a question it cannot
answer.

### Cost of the correction

Goldens only. Neither oracle compares scope positions —
`docs/specs/test-architecture.md:297-298` states it, and
`scripts/check-upstream-parity.mjs:63` runs upstream `--to xml` with no
`--sourcepos`, so the field is not in the comparison at all. There is no
registered divergence to file: the ceremony exists for observable parity
differences, and positions are projected away on both sides. What the correction
owes is the regenerated goldens and a statement of position semantics in
`docs/specs/canonical-ast.md`.

Until then, `specs/scope-sanity/ledger.json` records the 216 rows as a budget
that may only shrink, enforced by `scripts/audit-scope-sanity.mjs` in
`verify:core`. When every count reaches zero the ledger and the script are
deleted rather than kept at zero.

## Two live defects the PoCs surfaced

**D1 — `markdown_core_source_apply` has no `size_t` overflow guard, and it
becomes reachable on adoption.** `core/text.c:36-39` refuses an edit whose
result would wrap (`if (length > SIZE_MAX - kept) return false;`).
`extensions/source.c` has no such check anywhere, by written policy:
`extensions/source.h:53-58` argues that the operand sum "cannot wrap `size_t`"
and that "there are deliberately no unreachable overflow guards: a branch no
input can take is untestable." That reasoning is sound only while the sole
caller is a unit gate that never passes an adversarial length. The session's
public `markdown_core_session_edit` takes `length` straight from the caller, and
the repository already tests exactly this input at
`packages/markdown-core/tests/api/main.c:2065-2070`
(`markdown_core_session_edit(session, 1, 1, "y", (size_t)-1)`). Under the spike
it is an ASan `heap-buffer-overflow WRITE of size 8` at `source.c:55`:
`sizeof(source_buffer) + SIZE_MAX` wraps to 15, a 15-byte region is allocated,
and 16 bytes of header are written. The branch is not untestable; the guard
belongs in `markdown_core_source_apply`.

**D2 — an unresolved multi-line footnote bracket loses content, and the loss is
inherited.** On `dc36cb3`:

```
$ printf '[^foo\nbar]\n\n[^foo]: def\n' | markdown-core
    └── Text scope=2:1..2:4 literal="[^f]"
```

The authored text is `[^foo\nbar]`; the surviving literal is `[^f]`. Upstream
cmark-gfm 0.29.0.gfm.13 produces `[^f]` for the same input, so this is inherited
rather than introduced. It has never been caught because it is a position-driven
read (`core/inlines.c:1539-1540`) and the parity gate does not compare
positions — the same blind spot that hid the scope defect above. The blockquote
form `> [^foo\n> barbaz]` additionally resolves a reference whose label contains
a newline against the definition `[^foo]`.

Both are repaired inside M3 by the slices that already touch those sites, not as
separate work.

## The invariants, honestly scored

| | invariant | reachable | where |
| --- | --- | --- | --- |
| I1 | exactly one structure owns the committed source bytes | **yes** | M3.1, measured |
| I2 | no coordinate is stored in the tree or any side table | **yes**, restated | M3.4 — the achievable form is "no coordinate outlives the scan of its region", which plan:380-393 already commits to |
| I3 | exactly one ordered index | **yes** | M3.3; three of four structures reduce to nothing |
| I4 | coordinate-space conversion happens at one place | yes, with cost | M3.2 — the normalized↔stored delta becomes a sequence aggregate |
| I5 | no position special cases | **yes** | M3.4 — no profile escape hatch is needed once length zero is expressible |
| I6 | per-commit cost proportional to the edit and the reparsed region | yes, with cost | M3.3 removes the dominant violator; the footnote first-use count needs separate work |
| I7 | one physical CST, records riding nodes | already held | M2.5; must not regress |

Two corrections to earlier scoring are recorded here because they changed
decisions. `markdown_core_session_seal_positions` is **not** an O(document)
per-commit cost: on the incremental path it is called on `pipeline->root` and on
each dependent's staged shell (`extensions/incremental.c:2574-2576`), so it is
O(reparsed region), and the sealed parent-relative encoding is the mechanism
that makes position conversion O(edit) today rather than debt to be removed. The
dominant per-commit violator is instead the suffix document-children line shift
at `extensions/incremental.c:2942-2949`: on a 4 MB head insert it is 98.4% of a
16,676 µs commit, against roughly 0.6 µs for a same-length edit. The extent
sequence deletes that loop outright.

## Permanent residue

One item, and it is language-mandated rather than design debt.

**Parse-time coordinates.** CommonMark defines block indentation in columns, so
tab expansion is semantic, and it defines list tightness in terms of the line
being processed. `core/blocks.c:2140` is the clean example: the tight/loose
computation asks whether a still-open container opened on the line currently
being scanned. That is a grammar question, and no extent formulation answers it,
because the extent does not exist until the block closes. The residue is
therefore `parser->line_marks` (`core/parser.h:107-124`) plus the open-block
start coordinate M3.4 introduces — both parser lifecycle state that dies with
the parser. The invariant to hold is not "no coordinate anywhere" but **no
coordinate outlives the scan of its region**, and its checkable form is the
quarantine audit that lands with M3.4.

Three items that look like residue and are costs to be paid, recorded so they
are not filed as permanent: the footnote first-use numbering count, which is
O(total sites) today (`extensions/footnote.c:447-598`) against the §14.5.7 bound
of "proportional to that set"; every loop in `commit_full`, which is O(document)
by construction and correct, because I6 is a per-incremental-commit invariant;
and the `STRICT_UTF8` profile, which loses its only production caller when the
session adopts `PERMISSIVE_BYTES` and must then be deleted or given a declared
future consumer, because `specs/coverage/policy.json` rule 6 forbids parking
unreachable code in the ledger.

## Action register

1. **M3.0 — this document, the scope-sanity ratchet, and the plan's M3 section.**
   Locks the reordering before code moves, in the shape #93 used.
2. **M3.1 — substrate unification.** Session on the rope; `core/text.c` deleted;
   D1's guard in `markdown_core_source_apply`; the reverse cursor; the
   `STRICT_UTF8` decision. The gate is a link failure: a build with
   `core/text.c` removed must link and pass. Retires the ledger entry
   `packages/markdown-core/core/text.c {lines: 9, functions: 0, branches: 5}`;
   `extensions/source.c` has no entry today and must gain none.
3. **M3.2 — the coordinate decision and the fused sequence.** No coordinate is
   stored in any form. Breaks are first-class units, which is what makes the
   partition total and therefore what makes the gate writable. Three gates:
   every splice cut is a unit boundary; the sequence agrees with the parse,
   validated after every mutation in the manner #94 established; retained
   boundary metadata is bounded, which the existing byte-only
   `MARKDOWN_CORE_SOURCE_MAX_AMPLIFICATION` does not cover.
4. **M3.3 — index subsumption.** All deletions. The head-insert case goes flat.
5. **M3.4 — position deletion.** The five `int`s leave `core/node.h:161-165`,
   every non-node coordinate carrier moves in the same slice, the scope-sanity
   ledger reaches zero and is deleted, and D2 is repaired. Goldens printing
   `scope=` regenerate; the semantics are written into
   `docs/specs/canonical-ast.md`.
6. **M3.5 — resolution path.** `Document.scope` resolves per node in O(log n);
   `markdown_core_document_scope_table` is demoted to an opt-in bulk API and the
   three binding resolvers stop materializing the whole map. Also decided here,
   because it becomes load-bearing here: `include/markdown_core.h:31-38` promises
   concurrent reads between mutating calls while rope refcounts are non-atomic
   (`extensions/source.c:69-72`), which is safe today only because readers never
   touch the source.

M7 absorbs nothing new. It loses the substrate adoption that only a comment ever
assigned to it, and with it the risk of changing the byte store inside the one
milestone the plan says cannot be split.

## Verified in delivery, and what it changed

M3.1 shipped the adoption. Three things the PoC could not settle were settled by
building it, and are recorded here so they are not re-derived:

1. **The overflow guard is per replacement, not per batch.** The first version
   accumulated a running total, reasoning that two individually representable
   replacements can sum past `SIZE_MAX`. Mutation testing refuted it: replacing
   the accumulation with a per-edit check left the batch case passing, because
   a total cannot approach `SIZE_MAX` until the buffers it counts have been
   allocated, and those allocations fail first. The batch arm was a branch no
   input can reach — the defect `extensions/source.h` already warns about — so
   the guard is now exactly the condition under which `buffer_new` would wrap.
   Deleting it turns `regression_source_span_validation` into a bus error.
2. **The read primitives carry no out-of-range arm.** `run_at` and `byte_at`
   shipped with one, and the coverage gate refused `extensions/source.c` at
   553/556 lines. No caller has such an arm to exercise: the scans loop below
   `markdown_core_source_length` and every probe tests its index first. Removed,
   with the precondition stated where callers read it.
3. **The backward walk is the one reverse scan in the engine**
   (`extensions/incremental.c`, the restart boundary). Without a reverse cursor
   it costs `O(log n)` per byte instead of `O(1)`, and it is bounded by one
   line, so it sits on no asymptotic path. A cursor is deferred until a
   measurement asks for it rather than added on suspicion.

Two open items are deliberately still open. `STRICT_UTF8` loses its only
production caller when the session takes `PERMISSIVE_BYTES`, but the contract
defines both profiles (§7.1) and gates the strict boundary from both sides
(§14.3.6), so it has a declared consumer and is not the dead code
`specs/coverage/policy.json` rule 6 is about; that reading should be written
into `source.h` rather than left implicit. And the paired benchmark for the
substrate swap is not yet run — the edit-cost figures above are from the PoC
harness, not from the repository's benchmark suite on all four platforms as
§14.7 requires.
