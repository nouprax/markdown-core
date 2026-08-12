# The public model is a Document, and allocation failure is a parse error — 2026-08-09

Two decisions, taken on this date, and the evidence that decides them. They are
recorded together because the second follows from the first.

## The model

```
Document(markdown, options)   -> Document
document.commit(markdown)     -> Commit { Document, Delta }
```

**There is no `Session` in the public model.** A document is created from text
and options; committing hands it new text and returns the next document with
the delta between them.

**`commit` SUPERSEDES its receiver.** The document it was called on must not be
used again — not for a query, not for a node view, not for a scope. This is the
decision that makes the rest of the model cheap, and it is consistent with what
was already settled on 2026-08-07: a consumer that wants a revision to outlive
its commit takes a value copy, and the bindings already do
([`2026-08-07-requirement-audit.md`](2026-08-07-requirement-audit.md)).

### What it settles

**The lifetime question.** §4.2 as amended on 2026-08-07 blessed the shipped
behaviour — one document, reused in place — while §12 and gate 14.1.5 still said
closing a session invalidates no published document. Both cannot hold. Under
supersession the answer is stated once: the predecessor is invalid, and nothing
in the engine has to keep it alive.

**`Delta.edits`.** §9.2 said `edits` is "the caller's own edits, normalized".
That was never true of this engine: `extensions/session.c:872-894` coalesces
every edit into one dirty range and a net length at `edit()` time, so the script
is destroyed before commit. With full-text commit there is no caller script to
normalize. `edits` becomes what the engine determines — the minimal replaced
span between the previous text and this one — which is what §2.1's path C
actually wants for highlighting.

**Streaming.** §8.2 forbids a streaming-only node, tail, type, opcode, domain,
cache class, invalidation branch, or parser algorithm. Full-text commit makes
that true by construction: there is one operation and streaming is calling it
again. The cost is a common-prefix/suffix scan to locate the change, `O(n)` per
commit. For a 100 KB document delivered in 1000 chunks that is roughly 50 MB of
`memcmp` in total, milliseconds, against a parse that is orders of magnitude
dearer. It does not justify a second entry point.

### `commit` takes text, not options

Options are fixed when the document is created and are immutable for its whole
lineage. **A `DocumentDomain` therefore never changes within a lineage.** §5.1
makes a parse-option change start a fresh domain, and under this model that is
not a commit at all — it is a new `Document(...)`, whose identities compare
equal to nothing that came before. Which is what a consumer expects when it
changes what the parser means: there is no delta to be had across it, and none
is offered.

This also collapses the two entry points into one. `Document.parse(source,
options)` was the one-shot path and `session.open(options)` the incremental
one; they are now the same call, and "a one-shot parse gets its own domain" is
just the general rule.

## Allocation failure

**On allocation failure the commit reports an error and the document is done.**
The caller holds the text; recovery is creating a document from it again. There
is no restoration and no retry promise.

### Where the clause it replaces came from

`e95aa17`, 2026-07-16, "[Feature] Support concurrence parse. (#5)". One-line
commit message. **`gh pr view 5 --json body` is empty. `gh issue view 5` is
empty.** The spec file was born in that same commit carrying the header "This
contract is not implemented yet", so the requirement was written before the code
and the machinery exists because the clause does.

No consumer was ever named. Not in the commit. Not in the spec. Not in
[`../migration/2026-07-15-v2-incremental-sessions-plan.md`](../migration/2026-07-15-v2-incremental-sessions-plan.md),
written in the same commit to record "why the design is shaped this way", whose
only mention (`:186-189`) restates the mechanism. Not among that plan's seven
numbered Decisions, which do not include it. And not in its "rejected
alternatives" appendix, which never weighed the cheap option.

### It is not the oracle's

cmark's answer to allocation failure is to kill the process:
`docs/migration/upstream-changelog.txt:1073` — "Abort on `strbuf` out of memory
errors ... Previously such errors were not being trapped." Both parity oracles
compare text to AST output over fixed corpora with a working allocator; neither
injects one, so **neither can hold an opinion about allocation behaviour at
all.** This clause is not inherited from cmark; it is a departure from it.

### And the engine already had the replacement, two days earlier

`docs/specs/map-and-backslash-performance.md` was in the 1.0.1 baseline on
2026-07-14 and says `markdown_core_parser_finish` frees the tree and returns
NULL when any `oom` bit is set — a truncated document never masquerades as
success. That is "allocation failure is a parse error", as the v1 contract,
written two days before `e95aa17` asserted the other thing.

### The promise is unreachable, and where it is reachable it is a minority

Verified here:

- the default allocator **aborts** — `packages/markdown-core/core/markdown_core.c:14`
  and `:24`, `fprintf` then `abort()`;
- `markdown_core_session_open` hardwires it (`extensions/session.c:782-787`);
- **`markdown_core_mem` appears zero times in `include/markdown_core.h`.** The
  allocator-injecting variant is not public. All three bindings take the default
  path and none injects one.

So the transactional guarantee cannot be observed through the shipped public
surface — with one exception. `extensions/delta.c:12` and `:34` call `realloc`
**directly**, bypassing the aborting allocator, so the delta id arrays can
return NULL to a shipped caller. That window is real and it was measured: inside
one incremental commit, 3–68 allocations go through the aborting allocator and
2–16 through the recoverable one. **Roughly three quarters of the failures abort
regardless.** A guarantee that holds on a minority of the event that triggers it
is not something a consumer can code against.

### It is also false on one surface

After a failed commit, `markdown_core_session_reference_info` returns a freed
heap address as a `MarkupID` — measured at 2 of 600 queries, against 0 of 602 on
the successful control. `core/blocks.c:738` stamps `definition_node` with a raw
node pointer; pointer-to-id resolution happens only after the point of no
return; `reconcile_apply` has already spliced the staged entries into the live
map, and the abort path deliberately does not remove them. That function is
scheduled to disappear with the ownership flip (§16.8), which closes the window
as a side effect rather than by a guard.

## What follows for the engine

The transactional machinery exists to restore state that stops being public.
Under supersession there is nothing to restore: a failed commit leaves a
document the caller must discard, and the requirement collapses to what the v1
contract already said — do not crash, do not leak, report the error.

That deletes, when the flip lands: `incremental_rollback_splice` (40 lines),
`incremental_restore_graveyard` (13), the four restoration bools and their eight
asserts, the three `previous_*` saves paid on every successful commit, and the
point-of-no-return pre-reservation discipline that has now generated two
defects in one week — the double-spent bubbled reservation (`c270823`) and its
first fix's uncoverable failure arm.

**One correction to the plan's own design, which is worth taking either way.**
`2026-07-15-v2-incremental-sessions-plan.md:187` says "the living tree is
spliced only after all allocation-bearing work succeeded." The code splices
twice before its last fallible allocation — `detach_child_chain(pipeline->doc,
…)` at `incremental.c:2755` before the allocating adopt at `:2771`, and
`incremental_install_staged` at `:2935` before `footnote_refresh` at `:2945`.
The rollback functions exist to paper over that inversion. Moving the detach
after the allocations was measured to pass 172/172 default and 172/172 ASan on a
pinned tree, with control output byte-identical — the transactional property
falls out of the ordering instead of being restored.

## Method

Every claim above is a `file:line` read at `c270823`, a commit hash, or a
measurement. The provenance, the oracle question, the consumer question and the
cost were traced by five independent readers and two opposed adversaries; the
allocation-class counts and the `reference_info` defect were measured on a
pinned snapshot compiled in a scratchpad, never in the working tree.

Negative results, reported as such: no binding retries a failed commit — all
three throw. `grep -rniE "retry|retries"` across all three binding sources finds
one comment and no loop. Two of the three already ship a poison flag of their
own. The nine OOM gates cost 0.05 s in total, so nothing here is an argument
about test time.

## The CST should not resolve definitions — scoped, not yet done

Recorded on 2026-08-09 after the delta work, with the sites verified. **The
surgery is not started**: it cannot be split across a context without leaving
the parser half-changed, and the oracle move has to land in the same step.

### Why

`markdown_core_document_reference_info`, `footnote_info`, `footnotes` and
`footnote_references` have **zero binding callers** — the answers subsystem was
built for nobody. Deleting its `ANSWERS` delta part today removed a whole
emission pass. What remains is the resolution that happens during PARSING, and
it is the more expensive half:

- appending `[foo]: /url` at the end of a document RETYPES nodes anywhere
  earlier: `<text>see [foo]</text>` becomes `<text>see </text><link>…</link>`.
  Verified against cmark-gfm 0.29.0.gfm.13 directly.
- that is the LLM-streaming worst case — every chunk can retroactively rewrite
  the tree above it — and it is why "only edit-related nodes are affected"
  (2026-08-07's R7) is structurally unreachable, not merely unimplemented.
- it is also the source of the forward reference that made `ANSWERS`
  undecidable pairwise.

**The oracle does not require it.** CommonMark's normative text is about
rendered HTML; cmark's AST is cmark's own shape, and mdast keeps
`linkReference` unresolved and reverts it in `mdast-util-to-hast`. This repo
ships **no HTML renderer** — the parity runners compare our AST dump to cmark's
AST. So parse-time resolution is required by the SHAPE we chose for the parity
oracle, not by the oracle itself.

### The sites

- **link references**: `core/inlines.c:1493` — one `markdown_core_map_lookup`
  on `subj->refmap`; hit sets `is_reference`, miss does `goto noMatch`. The
  node stores only the LABEL, never the destination (the comment at `:1509`
  says why), so a `LinkReference` is already source-faithful. Resolution
  decides only whether the node exists.
- **footnote references**: `core/blocks.c:1970` passes `parser->footnote_defs`
  down. `core/parser.h:24-30` states the dependency outright: "this answer
  decides a node's type".

### The risk, stated before starting

`noMatch` is not just "emit the literal brackets" — it leaves bracket-stack and
delimiter state that later emphasis and bracket processing depend on. Making
every `[...]` a node changes that path, and 652 spec examples run through it.
The revert in `finalize` must reproduce the exact literal the author typed
around already-parsed inline children, which is what `mdast-util-to-hast` does
and is not a plain unwrap.

### The order

1. `markdown_core_document_finalize()` — bakes definitions into the tree:
   unresolved reference nodes revert to text, footnote numbering and first-use
   order are computed there rather than on every commit.
2. parse stops resolving; both sites emit unconditionally.
3. the parity runners compare `finalize(parse(text))`; goldens and spec
   fixtures pin the finalized tree.
4. the footnote index and the four id-addressed queries move behind
   `finalize`, where their answers actually belong.

## The iterator's leaf asymmetry is load-bearing by accident

Attempted on 2026-08-09 and **reverted**: making the walk symmetric (an EXIT
for every node, not only the eight types `S_is_leaf` excludes) breaks 9 gates,
two of them SEGFAULT.

The asymmetry itself has no reason of ours — `core/iterator.c` is a verbatim
inheritance from cmark-gfm 0.29.0.gfm.13, `S_is_leaf` and all, with no comment
either side. It is a renderer's distinction: a text node has no closing tag, so
an EXIT for it is a wasted event for every renderer upstream ships. And it is
a TYPE test, not a structural one — `iterator.c:57` gives an EXIT to a non-leaf
type that has no children, so the rule is "my type is on a list", not "nothing
is below me". That is why a consumer asking the structural question (am I
finished with this node) has to carry the list around to ask it.

**What makes it load-bearing**: `extensions/autolink.c:703` calls
`postprocess_text` on the ENTER of a TEXT node, and that call splits or
replaces the very node being entered. `iter->next` is computed before the
consumer sees the event, so under the asymmetry it already pointed at the
SIBLING and stayed valid across the mutation. Symmetric, it points at this
node's own EXIT — a node the consumer has just freed. Hence the segfaults, and
hence the walk missing the replacement nodes, which is what moved the spec
output.

So the asymmetry is hiding a mutate-during-iteration hazard rather than
justifying itself. Fixing it properly means making that consumer
iterator-safe — upstream ships `cmark_iter_reset` for exactly this — and that
is the prerequisite, not the symmetry change.

Patch kept out of tree at `scratchpad/iterator-symmetry.patch`; it is correct
in itself (measured 3.59x / 3.85x on many_duplicate_references, no scaling
cost from the extra events) and blocked only on the consumer.
