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
hidden. Flip storms keep their ladder entry verbatim.

THE REFERENCE-APPENDIX ENTRY LEFT THE LADDER (2026-08-17). The harvest was
the only block-level construct this engine recognised at its CLOSE rather
than as it was fed, so a paragraph of definitions was re-consumed whole
once per tick for as long as it was the open leaf — 87 µs a tick rising to
713 µs over a 64 KiB stream, and 8.6 s to stream a 340 KiB article whose
references are written the way people write them. A definition is final
the moment a `[` stands where its title would have begun (a title opens
with `"`, `'` or `(` and never with `[`, so the parse that produced it read
nothing that is still arriving), and `add_line` takes it off the paragraph
there — leaving the close the last definition of the run alone.
Measured: 2.0 µs a tick, flat from 256 KiB to 4 MiB, and the article
linear at 74 ms. Same move as the fence's mint-at-open, one rung further
along the same ladder. Its own gates are `append_references_appendix` and
`append_amortized_references_appendix`, both FLAT, plus
`append_amortized_cited_article`; turning the settle off fails all three.
`definitions_spaced` is the arm that says the settle costs nothing where
there is nothing to settle.

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
After the funeral (2026-08-17) the pairing's middle became an alignment
and the growing fence left the honest ladder — its buffer moves instead
of being copied and its record witnesses it by length; see
`2026-08-14-l1-slices.md` §6.

## 7 · Non-goals (inherited verbatim)

Incremental edit of any kind; intra-unit incremental inline parsing;
delta/commit side channels; snapshot trees distinct from the living tree;
any mechanism whose soundness needs a pointer borrowed across the
invalidation window.

"Harvest prefix memoization" left this list on 2026-08-17, and it was
never the thing the ban was aimed at. What landed is not a memo of a
scan's result but the frontier rule this plan already runs on, applied one
level down: a definition a `[` already follows is settled, exactly as a
block a later line already closed is settled, and the close keeps only
what the next byte can still change. §1's ladder entry above.

## 8 · What the giant open leaf actually costs (2026-08-17 measurement)

The prose wall's non-goal above is still the right call, but the reason
recorded for it is not the reason the shape is slow, and anyone who picks
it up should start from the measurement rather than from the name.

A tick on a giant open PARAGRAPH costs 0.8× to 2.2× a full one-shot parse
of that whole paragraph (prose 32 KiB–256 KiB; emphasis-dense the upper
end). On this one shape the living tree therefore saves nothing at all
against reparsing the document from scratch every tick — it IS that
reparse, plus the pairing.

Where the tick goes, at 131 KiB: the byte scan is 16% of it (a single
131 KiB line with one text child costs 176 µs), and the other 84% is
PER-INLINE-CHILD work — the retract frees every child, the publish
allocates them all again, the identity step stamps each subtree (which
re-hashes every byte of the paragraph a second time) and pairs it against
the retired one, which is then freed. 13107 children cost 923 µs of the
1099 µs tick.

So the work is not "resume the scan"; it is "keep the children", and that
needs the open leaf to have a SETTLED PREFIX of its own inline stream. The
bound on that prefix is the earliest still-completable construct: a live
emphasis delimiter, a live bracket, an unmatched backtick run, an
unmatched `<`, and the last line (whose trailing spaces can still become a
hard break, and which `chunk_rtrim` reaches). Children entirely before
that bound keep their nodes, their ids, their hashes, their concrete
records, their probes and their diagnostics, and the tick re-derives only
what follows it.

That bound has to be authored by the core AND by every inline extension —
each brings its own scanners and `close_probe`, and each must say how far
back a byte it has not seen yet could reach. That is a contract change of
the same weight `opaque_size` was for blocks, which is why this is a
milestone and not a step. Anything narrower — a fast path for chunks with
no special characters, say — is the special case this design does not
have anywhere else, and would buy the common case at the price of the
property that makes the rest of the engine easy to reason about.

### 8.1 · The bound, built and measured (and why it is not in the tree)

The proof half was written and run before this note was: a `open_from`
watermark on the subject, lowered by every handler that can read to the end
of the buffer without deciding — an unclosed backtick run (its scanner
answers 0 only after reading through the whole buffer), an entity whose
bounded window reached the end, a `<` with no `>` ahead of it or whose
comment/CDATA/PI/declaration scan raised its SKIP flag, a `]` with a `(` or
a `[` behind it (every other byte ends the attempt after one look, and one
look at a byte that is present is a decision for good), and any extension
handler that looked at a byte and declined. A settle point is then a line
start with nothing open behind it, no bracket waiting, and a byte after it
— that last test is what keeps the final line out, since a trailing `\`
becomes a LineBreak the moment a line follows it.

MEASURED REACH over 64 KiB single paragraphs, as a fraction of the bytes
refined: prose 99.9%, code spans 99.9%, inline links 99.9%, shortcut
references 99.9%, entities 99.9%, autolinks 99.9%, an unclosed backtick
99.9% (the run itself is open; everything before it is not). Two shapes
came back 0%: a paragraph carrying EMPHASIS, and one carrying a `<` with
no `>` after it. The second is the conservative arm doing what it says.

THE FIRST IS THE BLOCKER, and it is where this stops. Emphasis is matched
at the END of a unit, so at a line start the pair a line closed is still
standing on the delimiter stack, unresolved, and the frontier cannot tell
it from a `*` that is still waiting. Real prose has emphasis, so the reach
on real prose is zero and the whole mechanism pays nothing.

The fix is a delimiter engine that reduces EAGERLY and KEEPS ITS RESIDUE:
match what can be matched now, leave what cannot standing for a closer that
has not arrived, and read an empty stack as the proof. Split out of
`markdown_core_delimiter_engine_process` — whose residue-clearing is one
`truncate_to_mark` at the end — that looks like a small change, and it is
not. Built as a residue-keeping pass it fails `spec_smart_punctuation`,
`regression_commonmark`, `equivalence_regressions`,
`extensions_cross_link_embed` and `fuzz_script_smoke`: an early pass
changes what the final pass produces even when nothing is settled by it, so
the reduction is order-sensitive in ways the "a closer's opener is the
nearest eligible one before it" argument does not capture. Two things seen
while it failed and worth starting from: `truncate_to_mark` is the only
writer that restores a lane's `tail` and `open_top`, so a pass that keeps
its residue leaves lane bookkeeping that nothing else puts back; and
`remove_record` does not decrement `engine->count`, so the record count is
not the live-delimiter count and cannot be the emptiness test.

SO THE MILESTONE'S FIRST SLICE IS THE DELIMITER ENGINE, not the inline
frontier: eager, residue-keeping reduction.

### 8.2 · It works, and the blocker was three defects in one data structure

Reducing eagerly IS equivalent to reducing once at the end. The five failing
suites were not CommonMark telling us the order matters; they were the
delimiter engine's LIVE CHAIN telling us it is only half tracked. Found by
shrinking the first divergence to ten bytes and then brute-forcing every
string up to length 6 over four markup alphabets — ~17 000 inputs — which
turned a list of failing tests into one pattern: `X<delim>\n<delim>Y<delim>`,
where line 1 ends in an unmatched delimiter and line 2 holds a complete pair
that stops matching.

1. THE LIVE CHAIN HAD A TAIL BUT NO HEAD. The closer pass began its walk at
   slot ordinal 1, which is the head only while nothing below the mark has
   been removed — true of every pass that CLEARS its residue, because the
   clearing reclaims the slots. A pass that keeps its residue leaves removed
   records in place, and a removed record's `next` still names what stood
   after it when it left: nothing. So the walk ended at the dead head and
   saw none of the records pushed since. `engine->head` now stands beside
   `engine->tail`, maintained by push, remove_record and truncate.

2. `engine_mark()` BUILT THE MARK FROM THE SLOT COUNT, not from the live
   tail's ordinal. `mark_is_valid` asserts the two agree, so a mark taken
   after any middle removal was born invalid and the next bracket close came
   back as "parser refinement invariant failed". A mark means "everything
   pushed after this record"; that is the live tail, whatever dead slots
   stand above it.

3. THE EMPTY-REGION EARLY-OUT RETURNED BEFORE THE TRUNCATION. A region with
   no live record but dead slots above left the slot count where it was, and
   the next unit's `begin` refuses a non-zero count — which took out the
   concurrency and registry suites, nowhere near the delimiters.

Plus one rule for the caller, which the frontier's settle point already
carries: NEVER REDUCE ACROSS AN OPEN BRACKET. The bracket's own region is
not closed, and reducing under it lets a closer inside the brackets match an
opener outside them.

VERIFIED: 97/97 × default, ASan and UBSan with the eager pass running at
every line start; zero divergence from the baseline tree over ~17 000
brute-forced inputs and the whole corpus.

MEASURED REACH with it on, settled over refined bytes, 64 KiB paragraphs:
prose 99.9%, EMPHASIS 99.9% (was 0%), strikethrough 99.9%, nested emphasis
99.9%, emphasis with code spans and links 99.9%. Still 0% for a paragraph
carrying an unmatched `*` — which is the frontier being honest, not the
mechanism failing.

WHAT IS IN THE TREE: the three fixes; `engine_reduce`, which reduces every
pair above a mark AND NOTHING ELSE; `markdown_core_delimiter_engine_process`,
which is the two operations in order — reduce, then `truncate_to_mark`, since
closing a region means both what pairs is paired and what is left ceases to
exist; `markdown_core_delimiter_engine_settle`, which is the reduce alone,
because a region still open keeps what found no partner; and
`subject_settle_delimiters` calling it at every line start where no bracket
is open. The two operations were conflated in one function before, which is
why closing was the only thing the engine could do and settling had no name. No flag and no switch — the settle is what the scan does, the way
settling closed blocks is what the feed does. It is exercised by every test
in the suite, because every unit with a delimiter and a line break runs it,
and its cost A/Bs inside noise on the emphasis and link samples (some faster:
the pass at the end has less left to walk).

THE SIX MUTANTS ARE ALL DEAD, each with a named witness. Two were owed and
cost a real correction: the extension arm had covered only a handler that
DECLINED, and `x :dir{a=b` on its own line followed by a `}` at the end of the
unit showed that a handler which SUCCEEDS on a truncated construct is just as
open-ended — the note now fires when a handler is consulted at all, before it
answers. The close-bracket arm turned out to be necessary after all, against
the guess written here that it might be dead: a link TITLE can span lines, so
`x [a](/url "title starts` followed later by `")` completes a link whose `[`
stands before the settle point. Reach with extensions is 8 of 31 texts and
core-only 19 of 31; the first number is what the `opaque_size`-shaped contract
buys back.

### 8.3 · The half that keeps the children, and the contract that pays for it

BUILT, AND THE STATE LIVES ON THE PARSER. The chain keeps one parser for the
document's life, so what a tick must remember belongs there and not copied
into the record: the first cut put the checkpoint in every record entry and
let a record's saved youngest child point at an INLINE node, and every step
that takes a unit's children then owed a correction to a record — six defects
in one day, all of them two owners of one fact disagreeing. A record names no
inline child now (`warm_undo_save` saves none for a block that owns inlines),
nor is the record itself the caller's: the parser holds it in whichever of
two slots says what state it is in — published (closed, and the record says
how to reopen) or retracted (open again, holding the retired runs the caller
pairs identities against) — so publish and retract take a parser and nothing
else, a settle with no retracted record IS the cold form, and a parser freed
mid-close takes its record with it.

WHAT A CLOSE ACTUALLY MUTATES, measured rather than argued. Snapshot every
node alive at the checkpoint, close, and diff: a close writes **6.8 fields
across 2.1 nodes** on average, and that is FLAT in the document — 4096
paragraphs, 32769 nodes, still 5 writes on 2 nodes. It is not flat in one
place only: a definition the close registers re-reads every unit that asked
about the label, which is 4097 nodes at 4096 askers, and that is the same
O(askers) the record's flip array already pays.

So the record is not expensive, but it is not cheap either, and it is bigger
than what it describes: a spine entry is 216 bytes against a node's 200, and
the record allocates 491 B per close (2529 B for a deep spine) to say what a
whole-node snapshot of the touched nodes would say in 415 B. The size is not
the argument against it. The argument is that its 25 fields are a list of
what a close MIGHT change, kept by hand, and
markdown_core_parser_warm_fingerprint exists to catch the times that list is
wrong — its own comment says a field nobody adds to it is a field it goes
blind to.

That blindness is real and now measured. `close_retract_exact` holds the
retract to every byte of every node alive at the checkpoint, plus the counts
behind the three vectors a node only points at. It passes — 830 closes, 0
nodes wrong — outside four exemptions the design states and this gate
confirms: the tentative inline children of a block still open, the child
links of the block that owns them, a unit the close flipped (and the old
children it kept for pairing), the settled inline prefix §8 hands forward,
and the memos a finalize fills on settled nodes. Dropping the memo exemption
leaves exactly one shape — CHECKED|ENDS_BLANK arriving where LAST_LINE_BLANK
already says yes — which is a memo agreeing with the state it memoizes.

It also earns its place. Leaving the close's extra marker record on a settled
node — deleting the retract's `node->concrete->count = entry->concrete_count`
— passed all 98 tests before this gate, and fails only this one after. A
second mutant, the youngest child's flags not restored, passes both, and the
field may be dead weight: nothing yet shows a close writing a bit there that
recomputation would not give back.

SO THE RECORD STOPPED NAMING FIELDS. A close may write anywhere in the node
it closes, and 25 hand-picked fields were the wrong answer to that even while
they were the right fields: they were right because something checked, and a
field added to a node tomorrow would be right by nobody. The record now keeps
the block WHOLE — `saved`, and `saved_last_child` for the settled youngest
child a blank line at the close writes onto — and the retract puts the whole
thing back. `type`, `flags`, both coordinate pairs, `internal_offset`, the
payload, the extension pointer and the youngest child's flags are gone from
the entry; nine fields became two snapshots.

THE LIST THAT REPLACED THEM IS THE EXCEPTIONS, WHICH IS WHY THIS IS DIFFERENT
FROM RENAMING THE PROBLEM. `warm_restore_node` copies the snapshot over the
node and then puts back only the bytes another owner already has: the links
its own list surgery just computed, the content buffer whose fate the
taxonomy decides (restoring a stale `ptr` would hand back memory a realloc
freed), the three vectors a node only points at, the extension pointer and
payload, the type — last, so an extension's `free_opaque` still reads the
node as what the close made it — and the facade's id, revision and hash,
which are younger than the snapshot and would retire ids a caller still
holds. A field with no second owner needs no line anywhere.

MEASURED BOTH WAYS, by adding a field to markdown_core_node and having
`finalize` write it. Nobody told the retract about it: it comes back, and the
gate passes. Put it on the exception list instead — the shape of forgetting —
and `close_retract_exact` names it: *"document byte 140 is in no named field
— a node grew"*, the only failure in 99. The gate asks its question of all
`sizeof(markdown_core_node)` bytes, not of a field list, so it stays true the
day a node grows. The entry costs about 500 bytes now against 216, roughly
1.1 kB per close against 491 B, and every benchmark gate holds.
and three small structs on the parser say everything: what the last publish
SETTLED, that value as CAPTURED AT THE RETRACT (what the identity step pairs
after), and what the refine now ending PROVED and where it BEGAN (what the
walks after a refine read). The step that knows which of its refines was the
unit still growing promotes the third into the first; nothing else may.

A unit still growing carries `markdown_core_inline_frontier` (parser.h):
where its settled prefix ends in the content, which child ends it, where the
refine that left it BEGAN, and what that prefix owns of the records, the
probes and the diagnostics. The retract keeps the prefix and retires only
what is unsettled; the refine resumes at the settle point, ADOPTS the unit's
record vector and re-asks the prefix's labels; a child that settles takes its
bytes with it (once, not per tick); the postprocess — consolidation,
materialisation, and an extension's own `postprocess_block` — walks from
where the refine began. One rule answers "what did this close append" for the
retract, the identity step and the refine walk alike, and a record's pointer
into a unit's children is checked by its READER (`markdown_core_warm_child_holds`)
rather than corrected by every step that can take children.

THE CONTRACT §8.2 NAMED IS WHAT MADE IT PAY, and it is two calls, both
opt-in with the sound default:
`markdown_core_inline_parser_note_read` — an inline handler says how far it
READ, and silence still means "to the end of the buffer", which is what the
engine assumed of every handler before; and
`markdown_core_parser_settled_inline_child` — a hook that walks a unit's
children may start after the prefix. Autolink implements both. Without the
first, one `w` in "words" consulted the autolink bucket and pinned the
frontier at byte 0, so the reach on real prose was 0% and the mechanism paid
nothing; without the second, a postprocess walked the whole child list every
tick and undid the saving on its own. The core's `<` handler is conservative
now in one more place: a tag scan that matched nothing is OPEN, because a
quoted attribute value may contain `>` and may span lines.

MEASURED, all extensions on, a 1 MiB single paragraph at 8 bytes a tick:
2.837 ms/tick before, 0.031 ms/tick after — 92x, against a one-shot parse of
5.0 ms. The bench's `append_giant_paragraph` moved from 0.69 ms/14.4 ms at
256 KiB/4 MiB to 0.0054 ms/0.052 ms. THE PROSE WALL IS GONE, and with the
definitions work of §8.1 so is the appendix's: every append arm now measures
flat.
