# Re-examining every decision against the requirements — 2026-08-07

A landing built on `landing-a-extents` — sixty commits, +9,216 / −1,503 across
fifty-six files — was reverted on this date. This records why, what was wrong
with the reasoning that produced it, and which decisions the contracts should
keep.

The branch is untouched in the repository. Nothing here is about the code being
wrong; almost all of it was correct and gated. It is about what it was for.

## The requirements, as stated

**R1. A source position is the input's row and column, and nothing else.** It
is exposed on the AST because some consumers reverse-look-up from a node to
the input. It carries no AST-semantic content.

**R2. An AST node's scope is unrelated to the content offsets of the elements
inside it.**

**R3. No consumer needs to see the bytes behind decoded text.**

**R4. The CST exists because embed and directive syntax can be malformed and
deserve local diagnostics.**

**R5. The boundary is the Markdown in front of us.** What an embed references
is not this engine's business.

**R6. UTF-8 is assumed and never validated.** Valid UTF-8 in, valid UTF-8 out;
anything else is out of scope.

**R7. In an edit, only the edit-related tree nodes are affected.**

## What those requirements need

One thing: **each node must have a resolvable, contiguous source span.**

That is the whole of it. R1 needs it. R4's diagnostic needs a span for the
construct it complains about. R7 is delivered by `proj` not containing position
(§9.1) — a positional shift emits nothing, whatever work produced it.

Per-byte ownership, a document-wide unit sequence, an aggregate tree over it,
the persistence of that tree, and a label space to key it are reachable from
none of them.

## Where they came from

Two clauses, each asserted rather than derived, and each the parent of
everything under it.

### §11.1's commit-cost bound

> `session.commit()` is measured independently of any consumer: …

Followed by ten measured quantities, and no statement anywhere in the contract
of which consumer needs the measurement to come out any particular way.

**"Only edit-related nodes are affected" and "commit cost is independent of
document size" are different claims.** The first is about the OUTPUT — which
nodes a consumer sees as changed, observable through `Delta` and through value
comparison in a pull-style UI. The second is about the WORK, which no consumer
can observe.

The first does not imply the second. An implementation may walk the entire
suffix after an edit, adding a byte delta to every later node's stored span,
and still satisfy the first exactly — because position is not a component of
`proj`, so the delta reports nothing and the UI re-renders nothing. It is
merely a walk.

The word that carries the slip is **"affected"**: it means *observed to have
changed*, and it was read as *touched*.

### §7.2's prescribed mechanism

> Resolution must be `O(log n)` … A persistent aggregate sequence keyed by
> private order-maintenance labels, carrying subtree byte sums, satisfies this

This prescribes a structure, in a contract, for a bound that §11.1 asserted
without a consumer. Everything the landing built follows from it in one line
each:

```
work must be O(edit)
  → nothing outside the region may be rewritten
    → concrete records must be region-relative
      → regions must partition every byte
        → a position must be a query over a sequence
          → the sequence must splice in O(log n)
            → aggregate tree, persistence, label space
```

§11.1 states the region definition's own reason as *"That is what makes the
bound hold"*. The bound has no reason. The region has the bound's.

## What the landing cost

- `extents.c` 796 lines, `extents.h` 380 lines, and ten gates over them;
- a per-byte ownership partition, and the region classification that defines it;
- the label space, its headroom constant, and its exhaustion fallback;
- persistence: refcounting, path copying, `MAX_BYTES_PER_UNIT`,
  `MAX_AMPLIFICATION`, and the source rope's window-into-immutable-buffer
  machinery;
- measured, on the paired C-host benchmark: **+2.0% (lorem1) to +31.4%
  (large_table@20000) on real workloads, +76.4% on deep_nesting@32768.**

And the four defects the landing reported finding and fixing — a unit that was
content-backed and not, a CRLF terminator's flag bits at a feed seam, an
autolink cursor, a quadratic single-line test — **do not exist without it.**
`main` contains zero occurrences of `S_emit_unit`, `S_claim_content`,
`stored_pos` or `S_merge_replacements`; its cross-run CR handling is three
lines that skip the LF.

## What persistence was for

Nothing reachable.

- **C**: `markdown_core_session_document` returns `&session->view`, reused in
  place on every commit. A caller cannot hold a predecessor.
- **Bindings**: the ES snapshot is a decoded value tree, and asking it for the
  scope of a node from another revision throws — *"node value is from a
  different revision of this snapshot's session"*.

A consumer that wants a document to outlive a commit takes a value copy, and
the binding already does. §4.2's "remains readable after later commits" is
back-derived from that binding, not from a consumer.

## Decisions that hold

| § | Decision |
| --- | --- |
| 0 | One physical parser tree, the CST; the AST is its projection |
| 3.1 | Composition is downstream; an embed is an unresolved external reference — R5, stated before R5 was said in those words |
| 5.2 | `MarkupID` is not an index, offset, path, or hash |
| 6.3 | Parser answers are queries; "a fact that holds between nodes has no node field to live at" — R1's own argument, applied to relations |
| 7.2 | An extent carries no coordinates by construction |
| 7.3 | Source movement is not a change |
| 9.1 | Position is not in `proj` — **this is what delivers R7** |
| 9.3 | A part exists iff ignoring it would be wrong or cost more than `O(1)` |

## Decisions corrected

| § | Was | Now |
| --- | --- | --- |
| 6.1 | Every text field carries a `TextMap` back to source | `Utf8Text`, no map — no consumer asked to see the bytes behind decoded text |
| 7.1 | Validation on by default; invalid sequences replaced per line | Assumed, never validated; the guarantee is over legal input only |
| 7.2 | Resolution must be `O(log n)`; a persistent aggregate sequence satisfies it | The requirement is stated over the output; the mechanism is not prescribed |
| 11.1 | Commit cost independent of document size | The requirement is the published frontier; a walk is permitted, quadratic is not |
| 11.1 | Regions are defined "because that is what makes the bound hold" | Regions are defined for correctness: the smallest span that can be reparsed alone and give the same answer |
| 14.7 | Complexity gates reporting persistent nodes/bytes copied and every term against an AST-only baseline "with the CST permitted to move only the records-created term" | Removed. What it REQUIRED rather than reported moved to 11.1: `\|diffs\|` and delta application independent of unrelated nodes, and zero allocations and zero CST walks on ordinary access |
| 4.2 | A document is independent of later session commits and structurally shareable with adjacent revisions | Removed. A session hands out one reused view, so there is no predecessor to be independent of; sharing was there to make that unreachable clause cheap |

## What is owed

1. `SourceExtent` as an identity separate from `MarkupID` exists because
   coordinates go stale. If a node carries a span that a commit maintains,
   that separation is worth re-examining.
2. Concrete records are region-relative and their columns are
   normalized-line byte offsets. Whether that survives belongs with the public
   concrete interface, which does not exist yet.

## The method note

Three arguments were used in the first draft of this audit and all three are
invalid:

- **"It has an implementation and a gate."** Gates are ours. A gate proves we
  built it, not that it was needed.
- **"Only a test reads it, so it has no consumer."** Purpose comes from the
  requirement, not from who happens to read it today.
- **"Don't touch it if you are not in that code."** That is how an unexamined
  decision survives an audit whose purpose is to examine it.

## The one place a consumer for the removed clause is named

`§2.1`'s **path C** — "read the delta for location only" — says:

> a side-by-side editor highlighting the preview region a keystroke affected …
> It reads the entries and resolves `Document.scope` against the **retained old
> document and the new one**.

That is a stated consumer for holding a predecessor, and it is the
diff-highlighting editor scenario. Two things about it:

- **The C API does not deliver it today.** `markdown_core_session_document`
  returns `&session->view`, reused in place, so a C caller has no predecessor
  to resolve against. Path C is written for a capability that does not exist,
  which is why removing the clause it rests on broke nothing.
- **It may not need one.** `Delta.edits` (§9.2) already carries the byte-level
  difference. An editor that wants to highlight what a keystroke affected has
  the byte spans and the new document; what the OLD scope adds is the region to
  un-highlight, and a consumer that keeps its own previous render already knows
  that.

The clauses still written against a retained predecessor are `14.1.13`
("retained old documents keep their old answers after later commits and session
close"), `14.2.7` ("old node views resolve only through their retained old
document"), and `14.3.2` ("resolve exactly for old and new retained
documents").

**This is one decision, and it is not the parser's to make alone: does a
consumer get to hold a previous document?** If yes, path C stands and the API
owes it — and persistence comes back with it. If no, path C is restated on
`Delta.edits` and those three gates go.
