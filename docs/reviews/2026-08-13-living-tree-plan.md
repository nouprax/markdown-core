# The living tree: O(affected) append for an append-only engine

Status: ADOPTED AND LANDED — see §6 for where each milestone stands.
Baseline when written: branch `new-and-append-only`
(PR #103), where the API is `new` + `append` and nothing else. Supersedes
the parser-tail fork of `2026-08-12-streaming-plan.md` §4.2 (buried by its
erratum E2); revives that plan's per-settle refine, probe/flip, and gate
designs unchanged. Grounded in a 21-agent audit of the post-#103 tree
(2026-08-13): the one-line side-effect inventory, the diff/hash consumer
census, and the ownership sweep referenced below all carry file:line
evidence.

## 1 · The bound (the requirement, formalized)

**A stream's amortized total must be the same order as one full parse of
the final text.** Let the stream deliver N bytes in ticks of ~c bytes.
Required: Σ per-tick cost = O(one_shot(N)) up to a constant, for real
corpora. Today's shipped append fails it four ways per tick, each
O(document): the byte join (source.c never accumulates — every append
copies all bytes into a fresh buffer), the full reparse, the full-tree
hash stamp (blocks.c:2319, unconditional, ~6–11% of parse time), and the
full-tree diff. A k-tick stream is O(k·N): the quadratic the requirement
forbids.

The audit's census (diff-hash dimension) proves the cleanliness half and
the performance half are one motion: **no arm of diff.c is deletable while
two trees exist** (late-arriving definitions keep the hash prefix sweep,
the hash suffix sweep, and the positional middle all reachable at every
nesting level), and the subtree-hash apparatus has exactly two decision
readers — the two sweeps. Delete the second tree and the whole apparatus
follows.

## 2 · The architecture

**One chain = one living tree.** The parser's tree is never copied, never
diffed, never finalized-and-replaced: `append` feeds bytes and the tree
grows in place. A document handle is a thin view {chain, revision,
length watermark, generation}; `root` answers with the chain's one root
object at every revision.

This is contract-legal by the words already shipped: *"the receiver's
node handles become invalid the moment the next mutation begins."* In-place
mutation is exactly the permission that sentence grants. The fork design
died of two-tree sharing — the sibling seam, the presentation root, the
full-build→warm id handover were all artifacts of settled nodes shared
between a warm tree and per-tick snapshots. One tree has none of them:

- **Identity is object identity.** Ids are minted once, at node creation,
  from a chain counter. No pairing, no handover, no resurrection by
  construction. D9's "the diff is the only author of identity" becomes
  vacuous — there is nothing to author.
- **Revisions are stamped by touch.** The feed knows what it changes: the
  open chain's nodes as lines extend them, nodes created, nodes retyped,
  units re-refined by a definition flip, and every ancestor up the spine.
  Subtree-covering falls out of the ancestor walk.
- **Settled units refine once, in close order** — the per-settle refine of
  the buried P2 slice S3a (`warm_refine_settled`, commit b0e4662 in the
  abandoned branch's history: per-unit inline parse + unit-only
  postprocess), minus its hash stamps.
- **Definition flips** re-refine mentioning units via the probe lists of
  the streaming plan's P4, unchanged: unit keeps its id, its inline
  children retire and mint (they are new objects).
- **The source accumulates.** The chain owns one growable byte buffer;
  a snapshot's text is a length watermark into it. Kills the per-tick
  O(N) join before anything else does.

Total: O(N) feed + O(N) settle-refines + Σ O(open leaf)/tick + flip work.
The leaf term is the honest exception inherited from the old plan: a
paragraph that grows to P bytes pays O(P²/c) inline re-refinement across
its lifetime, because intra-unit incremental inline parsing stays a
non-goal (built once, fuzz-diverged, reverted). Real prose has bounded
paragraphs; the prose-wall degradation is documented and gated, not
hidden. Reference-appendix harvest and flip storms keep their ladder
entries verbatim.

## 3 · The half line and the one-line undo journal

The single hard problem the fork was invented for survives architecture
changes: the projection must include the buffered partial line, but truly
processing it commits irreversible state. The living tree's answer is not
a clone — it is a **journal**: process the half line FOR REAL, recording
undo entries for everything it touches; at the next append, replay the
journal backward, then feed the held bytes plus the new chunk normally.

This is buildable because one line's effect surface is finite and now
enumerated — the audit's 39-entry inventory
(`2026-08-13-line-effects-inventory.md`, committed alongside this plan; it
becomes the journal's specification when L2 starts) classifies every write
site three ways:

- **Append-shaped** (undo = truncate/pop): content strbuf appends with
  their 0–3 tab stand-in pads, line_marks rows, per-node concrete records
  (eleven capture sites), diagnostics rows.
- **Replacing** (undo = restore saved value): line_number, last_line_length
  (consumed by the NEXT line's finalizes — must restore), parser->current
  (six write sites), the sticky oom/capture_lost bits, per-line scan
  scalars (self-restoring at next line start; saved only for completeness),
  curline (cleared at next line start; finalize branches on its size).
- **Structural** (undo = operation log): add_child links (detach youngest),
  finalize cascades — reachable five ways inside one line — with their
  OPEN-flag clear, end-coordinate writes, and per-type payload (fence info
  detach, content detach, harvest: ReferenceDefinition insert_before into
  the PARENT list, definitions-only paragraph vanish, footnote
  registration), retypes (setext, table conversion), spine flag writes
  (LAST_LINE_BLANK down the chain), TABLE_VISITED, list tightness, and the
  line_mark_count reset-to-zero at paragraph open (rows must be copied out,
  storage is reused).

**The journal is total or the tick falls back — and during the transition
the fallback keeps today's semantics.** A construct whose journal support
has not landed sends THAT tick down D6 (full rebuild + diff for id
handover, exactly the shipped path), so diff.c and the hash machinery stay
alive precisely as long as any fallback can fire, and die in the milestone
that proves totality. No contract-edge pointer borrowing anywhere in any
phase.

**The oracle is exact restoration:** the warm-state fingerprint (INV2's
design, extended to cover the tree spine) must be BIT-IDENTICAL after
journal replay to its pre-half-line value, asserted on every tick of every
replay in the harness, not sampled. A journal gap is a deterministic gate
failure, not a heisenbug.

## 4 · What dies (the cleanliness ledger)

With totality proven (fallback rate 0 across the corpus + fuzz):

- `extensions/diff.c` — the whole file (its two provably-dead guards at
  :136/:142 need not even be fixed first).
- `node->subtree_hash`, `markdown_core_node_stamp{,_tree}`,
  `markdown_core_hash_mix/bytes`, the unconditional O(tree) stamp at
  blocks.c:2319, and the four `hash_value` extension hooks.
- The er harness's two-tree double walk, reshaped: capture digests from
  the head BEFORE the append, compare the tree after. Bindings'
  value_mirror gates already capture at decode time and survive unchanged.

Independent of the living tree (audit-verified zero-caller or
lockstep-duplicate; executable now as L0):

- source.c: batch arm, shrink arm, overlap validation, `copy_bytes`,
  `byte_at`, the stats counters nobody reads; `document_set_text`'s splice
  reduces to `source_new`.
- document: `total_lines`/`last_line_length` dead fields;
  `chain->generation`/`chain->next_revision` lockstep duplicates (keep
  one); per-document `series` copy (chain outlives every handle); the
  per-append host-CSPRNG read minting a salt that is immediately
  overwritten.
- ES: `requireLive()` dead guard; the unreachable `!released` guard before
  reclaim.register.
- Kotlin: the decode-failure window that leaks the native successor
  (Document.kt:222 — the payload's handle has no owner until decodeWire's
  build callback runs); the `append` doc overclaim ("per-append cost
  O(changed)" — true of decode only until this plan lands).
- Contract: the "supports only free" vs "nodes readable until the next
  mutation begins" tension in markdown_core.h — reword to match reality
  (free plus read-only access until the next mutation begins); the
  "whichever mutation" plurals; ~30 stale edit/commit/session-era comments
  (source.h's plan-section coordinates, document_internal.h's
  MARKDOWN_CORE_SESSION_INTERNAL_H guard, arena.h's commit lifecycle,
  diff.c's false "not done here yet" banner);
  `docs/specs/sessions-and-deltas.md` archived (it defines commit/fork
  semantics the header now contradicts line-for-line).

## 4.5 · Erratum E3 — a superseded handle answers for no tree

Recorded 2026-08-14, during L1. §5 below says a superseded view "supports
free and read-only access, both only until the next mutation begins", and
that sentence carries an ambiguity the living tree removes: does the
mutation that SUPERSEDED a handle end its read right, or only the one after
it? Every reader so far assumed the looser answer — an api case dumped a
receiver while its successor was the head, and the old two-tree harness
walked superseded snapshots.

The looser answer cannot survive one tree. What a superseded handle would
show is the text it described plus everything appended since, which is not
the document it names. So the strict answer is the design: **the tree goes
the moment the handle is superseded**, and what remains is free (at any
time, from any thread) plus the three scalars that say which document it
was — revision, series, length. Root answers NULL; dump and diagnostics
answer as they would for no document.

The cost was one test, which now captures the head's dump BEFORE the
mutation and then asserts the narrowing itself. The bindings needed
nothing: all three decode at build time and never call back in for a
superseded document — the reason a consumer keeps decoded values rather
than native handles is exactly this.

## 5 · Doctrine

"No object reuse" (requirement audit #98) is REPLACED for the one
mutation that exists: a chain owns one living tree; a document is a
revision-watermarked view; a superseded view supports free and read-only
access, both only until the next mutation begins. The projection oracle is
untouched: at every revision, dump ≡ one-shot parse of bytes-so-far. The
binding update protocol — (id, revision, extent) pruning, wire REUSE,
destructive mirror — is untouched; it never depended on which side of the
FFI the nodes were fresh on.

## 6 · Milestones

- **L0 — cleanliness now.** §4's independent list: dead code out, stale
  claims fixed, contract tension reworded, Kotlin leak window closed,
  sessions-and-deltas archived. No behavior change; every suite green.
- **L1 — the living tree, prose-warm.** Chain-owned source accumulation;
  the tree grows in place with ids-by-identity + revisions-by-touch; er
  harness reshaped to capture-then-compare; journal framework + the
  fingerprint-restoration gate; warm ticks for the prose shapes
  (paragraph continuation, blank lines, new-paragraph opens — truncation
  and add_child/finalize log entries only); every other shape falls back
  to D6 per tick (diff alive); fallback-rate counter gated.
- **L2 — journal totality.** The full inventory: finalize cascades,
  harvest (insert/vanish), retypes, spine flags, extension effects
  (TABLE_VISITED, look-back, tightness), per-settle refine revival,
  probe/flip re-refinement. Fallback rate driven to zero on canonical +
  spec + fuzz corpora.
- **L3 — the funeral and the bound.** Delete diff.c + the hash apparatus +
  the stamp; contract/doctrine text finalized; the amortized-bound gate
  lands: stream N bytes at token-sized ticks, assert
  Σ tick_cost / one_shot(N) ≤ K with K flat across size doublings —
  the executable form of §1's requirement. Bench arms for the honest
  ladder (prose wall, appendix, flip storm) record their documented
  shapes.

**Where the milestones stand (2026-08-16).** L0 merged (#104); L1, L2's
totality and L3's bound gate merged together as #105: every close is
retractable, definitions flip exactly the units that asked (through the
probes threaded as an index), the amortized gate holds prose and the
nested list flat. The funeral followed on `living-tree-l3`: an extension
that opens blocks and allocates payloads must describe them or is not
attached, so no build can end in a state it cannot reopen, and with that
`final`, the rebuild, `diff.c`'s tree diff, the tick ledger and the
whole-tree stamp are deleted — an append is one tick, and a build stamps
only the subtrees the frontier will pair (a one-shot parse: the run its
close publishes, not the tree). What §4 lists and this keeps: the pairing
machine over two
child lists and the subtree hash it sweeps on, because the tail a tick
re-derives from the held line must PAIR against the tail it replaces (the
contract's "an empty append moves no revision"), and that is the frontier
diff, stamped on exactly what it pairs. See `2026-08-14-l1-slices.md` §4–5.

## 7 · Non-goals (inherited verbatim)

Incremental edit of any kind; intra-unit incremental inline parsing;
harvest prefix memoization; delta/commit side channels; snapshot trees
distinct from the living tree; any mechanism whose soundness needs a
pointer borrowed across the invalidation window.
