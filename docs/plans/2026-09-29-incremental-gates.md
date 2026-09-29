# Gates for incremental parsing

Status: proposed design. Nothing in this document is implemented. It
specifies the correctness suite and the two benchmarks that decide whether
[incremental parsing](2026-09-29-incremental-parsing.md) is done, and it lands
before any engine work so that every rollout step is measured against the same
harness from its first pull request.

The existing one-shot benchmark (`packages/markdown-core/benchmarks/README.md`)
answers "what does a parse cost". Incremental parsing adds two workloads that
it cannot see:

- **Edit**: a code editor replaces any range with any text.
- **Stream**: a language model appends text to the end.

The gate therefore has three benchmarks (one-shot, edit, stream) and one
correctness suite for the two new workloads. A step of the rollout is accepted
only when all four pass at the level section 7 assigns to that step.

Sections 1–2 say why the gates come first and what they are measured against.
Section 3 defines the workloads, section 4 the correctness oracles, sections
5–6 the measurement and the gate rules, section 7 how the gates tighten step by
step, section 8 how the harness proves it can fail, and section 9 where they
run. Section 10 lists the work items and section 11 the decisions for the
owner.

## 1. Why the gates come first

The incremental plan first put its gates in its last rollout step. That order
lets four steps land without the evidence the design depends on:

- Step 1 adds ids, deep equality and Swift per-node records, and replaces
  absolute scopes with relative extents. Every one of them costs the one-shot
  parse something, and the current gate
  sees only `source_to_buffer`. Nothing today would notice the AST stage
  getting slower.
- Steps 2–5 each claim a property (R1 equivalence, R3 minimal replacement, R4
  stable identity, R2 bounded re-reading). Section 5.10 of the plan argues
  those properties; only an oracle that runs every edit shows that the code
  keeps them.
- The complexity bounds of the plan's section 7.1 are the reason for the whole
  design. A bound that is first measured after the implementation is complete
  is a bound nobody designed against.

The gates also fix the vocabulary before the code exists: what an edit script
is, what "reused" means observably, what is counted. Each later step then
changes the subject under test, not the harness.

## 2. Subjects and the baseline

The harness drives a **subject** through one interface: open a session in a
coordinate unit with an initial text, apply `replace`, `append` or a batch,
read the current document, close. There are exactly two subjects:

| Subject | What it does | Role |
| --- | --- | --- |
| `session` | The engine's `MarkdownSession` (plan 4.4) | Under test, from rollout step 2 |
| `reparse` | Keeps the text, and on every step calls `Document.parse` on the whole of it | The oracle for correctness, the R column for benchmarks |

`reparse` is not a second algorithm. It is the one parser, called the way an
application calls it today, and it stays as the equivalence oracle (R1) and as
the reference cost an incremental edit must beat. Until the session API exists
the harness runs `reparse` alone, which already produces every baseline number
and exercises the scripts, the text model and the reports.

The harness keeps its own **text model**: a plain byte buffer to which it
applies every script step, independent of the subject. The model is the
source of truth for "the session's text"; the subject's text must equal it
after every step.

## 3. Workloads

All workloads are generated deterministically in-process from tracked
sources, like the grammar corpus. Nothing is downloaded and no corpus is
vendored (`packages/markdown-core/tests/corpora/README.md`). A workload is a
document and a script; its identity is the digest of both.

### 3.1 Documents

- **Grammar corpus.** Every document of the grammar corpus (848 documents
  over 194 certificates, ASCII and UTF-8 alphabets). It covers every feature
  and element, so every dependency row of the plan's section 3 is exercised
  somewhere. These documents are small (under 6 KB), which is what
  exhaustive correctness sweeps need.
- **Scale families.** Documents of one shape at four sizes, 16 KB, 64 KB,
  256 KB and 1 MB, built by repeating a fixed section with distinct words
  (the corpus's word generator, both alphabets). The same edit is applied at
  the same relative position at every size. Shapes:
  - `prose`: headings, paragraphs with inline markup, links to reference
    definitions, footnotes, short lists and code blocks.
  - `list`: one list whose items hold paragraphs, nested lists and code, the
    shape of a long model response.
  - `quote`: the same content inside a callout and three nested block quotes.
  - `table`: one pipe table with many rows, and one grid table.
  - `refs`: a few reference definitions and footnote definitions referenced
    from every section.
  - `flat`: thousands of short top-level blocks, which makes the document's
    own child collection large.
- **Adversarial shapes.** The inputs the plan's section 8 names, each at the
  scale sizes: a stray early opener (an unclosed fence, HTML block, comment
  and directive on the first line, toggled open and closed); a 10,000-item
  list edited in the middle; 1,000 nested block quotes edited at the deepest
  leaf; one definition with 10,000 references whose destination changes; a
  heading label repeated so that anchor suffixes shift; an unclosed `**`
  early in a paragraph that is then streamed for 64 KB; a single-line
  paragraph streamed to 64 KB.

A composite document that joins parts records, for each part, the number of
root children a fresh parse of the part alone produces. The generator checks
that the composite's fresh parse has their sum, so a part cannot silently
change the meaning of its neighbour and move every edit site. This is a
property of the generated input, checked once when the workload is built.

### 3.2 Edit scripts

A script is a sequence of steps. A step is `replace(start, end, text)`,
`append(text)` or `batch([replace…])`, with offsets in UTF-8 bytes of the text
before the step. Every offset falls on a scalar boundary unless the step is an
invalid-argument case (4.9). Bindings convert offsets to their unit through
the text model, so the same script tests both units.

| Family | Steps | Positions |
| --- | --- | --- |
| `typing` | One scalar per step, 64 steps, with runs of backspace | Inside a paragraph, a list item at depth 3, a table cell, a heading, a code block, a quote leaf, a footnote body |
| `lines` | Enter inside a paragraph; insert and delete a blank line between blocks; join two lines; merge two paragraphs | Beginning, middle and end of the document |
| `markers` | Add and remove `> `, `- `, `1. `, `# `, four spaces, a fence opener, a Setext underline, a table delimiter row, a definition term marker | Each block kind of the shape |
| `ranges` | Paste a 2 KB section; delete a section; select a paragraph's whole text and type a replacement | Middle of the document |
| `far` | Alternate single-scalar edits at the first and last line, 64 steps | Both ends |
| `batch` | 16 disjoint edits in one `batch`, as multi-cursor typing, listed in a seeded shuffled order | Spread across the document |
| `declarations` | Change a reference destination; add and remove a duplicate reference label; add and remove a heading whose label collides; add and remove a footnote definition and an inline note | Declaration sites of `refs` and `prose` |
| `undo` | Each `typing` to `declarations` step followed by its inverse | As the original step |
| `random` | Seeded mixture of inserts, deletes and replacements at line and byte granularity, including CR/LF splits and NUL | Uniform over the text |

`typing`, `lines`, `ranges`, `far` and `batch` are **local**: the language
itself limits their effect to a bounded neighbourhood. `markers`,
`declarations` and `random` contain steps whose effect the language may spread
(plan 7.2); the gates treat them differently (section 6).

### 3.3 Stream scripts

A stream script appends the whole of a document, starting from an empty
session, in chunks.

| Family | Chunking | Documents |
| --- | --- | --- |
| `tokens` | A fixed pseudo-random sequence of chunk sizes from 1 to 16 bytes, mean 4 | Scale families, grammar corpus |
| `bytes` | One byte per chunk | Grammar corpus |
| `rows` | One physical line per chunk | Scale families |
| `splits` | Two chunks, split at every byte offset | Grammar corpus documents up to 2 KB |

In C, chunk boundaries fall anywhere, including inside a UTF-8 scalar, which
exercises pending bytes (plan 4.4). Bindings append whole scalars, because
their strings have no partial scalars; their chunk boundaries move to the next
scalar boundary.

## 4. Correctness oracles

Every oracle is checked after every step of every script that runs it
(section 9). The oracles are stated against the public model, not against the
engine's internals, so they hold for any correct implementation and fail for
any incorrect one.

### 4.1 Equivalence (R1)

The subject's text equals the text model, and the canonical dump of the
subject's document equals the dump of a fresh `Document.parse` of the model
text in the same unit. The dump prints no ids and prints absolute scopes, so
it compares meaning and positions and nothing that depends on history.

A node stores only its relative extent: `Extent(lead, span)`, where `lead` is the UTF-8 distance from the end of its
previous sibling in the same relation (or from its owner's start, for the
first) and `span` is its UTF-8 length. `Document.scope(of:in:)` and
`Document.node(at:in:)` compute absolute positions from the extents and the
source text the caller passes (plan 4.3), and return them in the session's
unit; the extents themselves are UTF-8 in every unit, as the engine produces
them. On every step of the correctness set, for every node of the subject's
document, `scope(of:in:)` with the model text equals `scope(of:in:)` of the
corresponding node of a fresh parse of the model text in the same unit, and
`node(at:in:)` at the scope's start returns the corresponding node. The fresh
parse's own answers are checked once, in UTF-8, against the scopes the
canonical dump prints, which stay in UTF-8 columns, and in UTF-16 against the
same scopes converted through the model text. A document
answers these queries from its own values alone, so the previous document
with the previous text still answers exactly as it did before the step.
`Document.footnote(for:)` and `Document.specimen(for:)` are likewise computed
from the tree on demand, and for every label of the model text they return the
node that the fresh parse's answer names, or none when it has none.

### 4.2 Identity (R4)

- Ids are unique within the document, across every owned relation.
- Over the whole lineage, the harness keeps a map from id to kind and a set of
  retired ids. An id never changes kind, and a retired id never appears again.
- A fresh parse numbers its nodes 1, 2, 3, … without gaps in canonical walk
  order (plan 4.1), every id below 2^53. Two fresh parses of the same text are
  therefore equal, ids included.

### 4.3 Reuse is exactly value equality (R3)

For every node `N` of the new document whose id also names a node `O` of the
previous document: `N` is the same object as `O` if and only if `N` is deep
equal to `O` (plan 4.2). The "only if" direction catches a stale node reused
after its value changed. The "if" direction catches a wasteful new object for
an unchanged value, which forces every consumer to re-render it. Every
ancestor of a new object is a new object.

The comparison is always against a **snapshot** of `O` taken before the step:
its kind, id, scalars, extent and the ids of its children, keyed by id, on
every platform, together with its absolute scope from `scope(of:in:)` and the
text before the step. The extent is part of the value; the absolute scope is
not, and serves only the matching of 4.4. Comparing against the live object would be meaningless if a subject
mutated a published node in place, because `O` would already show the new
value. "Same object" is object identity in Swift, Kotlin and ECMAScript, and
an unchanged node `version` in C (plan 5.9), whose views the next edit
invalidates.

Published documents are immutable (R9). Every binding keeps the previous
document alive across the step and checks, after the step, that its
canonical dump against the text before the step and its per-node snapshot are
unchanged. A subject that edits a published
value in place fails here even when the new document is correct.

### 4.4 Identity follows the matching rule

4.3 constrains objects only where an id persists, so on its own it would
accept an implementation that gives a node a new id, changed or not, and
loses its view state. The plan states which old node each new node continues
(5.9) in terms the harness can evaluate from the public model alone: the
position mapping of the step (5.2), the absolute scopes of the pre-step
snapshot (4.3) and of `scope(of:in:)` on the new document, kinds and owner
relations. The harness therefore computes the expected matching itself, for
every node of every step:

- An old node's anchor is its first byte that survived the step. Within the
  relation of a matched owner, a new node of the same kind whose source range
  contains the image of an old sibling's anchor continues the earliest such
  sibling.
- Old and new nodes left between two consecutive matched pairs of one
  relation are paired in order by kind (slot pairing).
- A new node that continues an old node has the old node's id. Every other
  new node has an id the lineage has never seen. An old node that nothing
  continues is retired.

This holds whether the node's value changed or not, so an edited heading,
list item or table cell keeps its id exactly as an edited paragraph does. In
addition, a continued node whose subtree value equals its predecessor's is
the predecessor itself (4.3). Extents are relative, so text that moves a
node without touching it leaves its value unchanged. The one exception is
structural: the first continued node after a changed or inserted sibling in
the same relation may get a new `lead` (recomputed at convergence, plan 5.3), and
is then legitimately a new value with the same id. The oracle predicts that
`lead` from the fresh parse, so it needs no special case.

### 4.5 Scripted identity

Some edits have an exact expected outcome, written into the script as the sets
of kept, new and retired ids by position. They cover every consequence the
plan's matching rules list (5.9): typing at the start of a paragraph keeps its
id; inserting `new\n\n` before a paragraph gives the new one a new id; deleting
a first word keeps the id; deleting a sibling retires its id and does not hand
it to the next; merging two paragraphs keeps the first id; replacing a whole
paragraph's text keeps its id; a paragraph that becomes a Setext heading is a
new node; a paragraph moved into a new quote is a new node. For the plan's
example, typing in paragraph 5 of 1,000, the new objects are exactly that
paragraph, its Text nodes on the edited line and the Document.

### 4.6 Streaming

After every chunk, 4.1–4.4 hold against the model text, where the model text
is the longest complete-scalar prefix of the bytes appended so far. The pending
bytes are never part of the text. A chunk that extends the pending bytes to a
longer prefix of a valid sequence is accepted and leaves them pending, as when
a four-byte scalar arrives one byte per chunk in `bytes`. Only a byte that
cannot continue the pending sequence makes the append invalid; it is rejected
and leaves the session unchanged (4.9).

### 4.7 Batches

The dump after `batch(edits)` equals the dump after applying the same edits
one at a time in descending order of their start offsets, whatever order the
batch lists them in (so the offsets not yet applied stay valid), and
4.1–4.4 hold for the batch as one step.

### 4.8 Transactions (R7)

The allocator-seam OOM sweep runs every step of every script in the
correctness set (every edit and stream family, batches, declaration changes,
pending-byte chunks and the invalid arguments of 4.9) with a failure injected
at every allocation boundary. Each independently mutating path of the
transaction is therefore swept. After each
failure the subject's text, dump, ids, versions and retained-state digest
equal the previous version's. Then the unmodified step is retried: a valid
step succeeds, and an invalid step of 4.9 is rejected as invalid again, with
the session still unchanged.

R7 is the engine's contract. A binding is a pure projection of the engine's
result into immutable platform values for Compose, React and SwiftUI; it has
no transaction or failure contract of its own, so the sweep runs in C and no
gate injects failures into a binding's projection.

### 4.9 Invalid arguments

Ranges out of bounds, ends inside a scalar (a UTF-8 continuation byte, the
middle of a surrogate pair), ill-formed replacement text and impossible
pending continuations are rejected as invalid arguments, and the session is
bit-for-bit at its previous version.

### 4.10 Depth and concurrency

Every binding runs the plan's deep-tree set (6.3) through a session: release
of a deep document whose subtree is shared with a newer version, equality of
two deep documents that differ at the deepest leaf, walking, scope lookup, hit
testing and `description`, on a thread with a small fixed stack.

Every document index built lazily on first use, if the plan keeps any (a
cached position index behind `scope(of:in:)` and `node(at:in:)`, say), is
exercised by concurrent first use in Swift and Kotlin: several threads
released together by a barrier make their first `Document.scope(of:in:)` and
`Document.node(at:in:)` calls, and their first call into each other lazy
index, on the same fresh document of every step, and every answer must equal
the single-threaded answer. Each index is built exactly once per document:
the test build counts index constructions, and the count after the concurrent
first use is one per index, not one per thread. This runs under the thread
sanitizer where the platform has one. It also covers documents that share subtrees with the
previous version while that version is being read on another thread.

### 4.11 Platforms and units

C and every binding run the same scripts in both units. A binding checks 4.1
against its own dumper, as conformance does today, and checks 4.3–4.5 with its
own object identity. A binding builds its values from what the engine returns
and adds no bookkeeping of its own, so its construction cost is the platform's
own cost and is not gated (5.1).

### 4.12 Fuzzing

A fuzz target decodes its input as a document and a script and checks 4.1–4.4
and 4.9 after every step. The current fuzz corpus seeds the document half.

## 5. Measurement

### 5.1 What is measured

The runners follow the one-shot benchmark's rules: callgrind instruction and
data-reference counts, the benchmark preset, the same build provenance and
isolation, and a measured edge instead of a timer. The edit runner loads a
document and a script, opens the subject with the document outside the
measurement, and calls `bench_apply_step` once per step. The measured cost of
a step is the cost under that edge.

Per-step costs come from callgrind client requests (`CALLGRIND_DUMP_STATS`),
which are no-ops outside valgrind. A script of at most 1,024 steps dumps after
every step; every edit script is this size. A longer script (a streamed
megabyte in `tokens` or `rows`) dumps after each of 1,024 contiguous windows of
consecutive steps, so every step's cost lands in exactly one measured window.
Its `reparse` column is measured at each window's last step, because
reparsing after every chunk of a megabyte stream is the quadratic cost the
design removes and cannot be run.

Every step of those scripts still gets its own cost: the runner also replays
them natively in a build compiled with `-fsanitize-coverage=trace-pc-guard`,
whose callback adds one to a counter per executed edge. `step_edges`, the
counter's increase over one step, is deterministic like Ir and cheap enough to
record for every chunk of a megabyte stream, and `reparse_edges` is recorded
the same way at each window's last step. Edges are a different unit from Ir,
so the two are never compared with each other.

The gates measure the C engine only. A binding builds its platform values from
what the engine returns; that construction is the platform's inherent cost, the
same for any engine design, and no gate measures it or shapes the engine
around it. Bindings are gated for correctness (section 4), never for cost.

### 5.2 Metrics per workload

| Metric | Definition |
| --- | --- |
| `step_ir` | Ir per step: p50, p95, max and total over the script |
| `reparse_ir` | The same for the `reparse` subject: what an application pays today |
| `oneshot_ir` | One-shot parse of the script's final text, both stages |
| `speedup` | `reparse_ir / step_ir` per step, p50 and p5 |
| `stream_ratio` | Stream total `step_ir` / `oneshot_ir` of the final text |
| `retained` | Session-retained bytes after the script / text bytes |
| `transient` | Peak allocation within one step, in bytes |

Allocation figures come from the allocator seam, so they are exact counts,
not samples.

### 5.3 Work counters

Deterministic counters, in the style of the existing `input_line_work` and
`delimiter_work`, record per step:

| Counter | What it counts |
| --- | --- |
| `lines_reread` | Physical lines passed through the line machine |
| `inline_bytes` | Content bytes the inline parser scanned |
| `ledger_touched` | Ledger entries inserted, removed or rewritten |
| `summaries_combined` | Child summaries recombined during re-finalization |
| `registry_recomputed` | Registry entries whose winner or ordinal was recomputed |
| `lookups_invalidated` | Inline roots re-parsed for a resolution change |
| `finish_visited` | Nodes visited by finish steps and passes |
| `nodes_new` | Nodes whose version is the current edit |
| `journal_entries` | Transaction journal entries |
| `tree_visited` | Nodes of the shared balanced tree (text, ledger, registries; plan 5.1) visited by lookups, splices and shifts |

Counters are cheap, exact and platform-independent. They are where the plan's
bounds are enforced (6.2). Ir is where constant factors and the asymptotic
shape are checked (6.3, 6.4 and 6.6).

## 6. Gate rules

A rule is either a bound derived from the design or a regression limit
against the base revision. No rule is a threshold on a measured number that
happened to look good.

### 6.1 Correctness

Every oracle of section 4 passes on every step of every script that runs it.
There is no tolerance.

### 6.2 Bounds, on counters

Every activated counter has a bound, and every bound is written in quantities
the harness computes without the engine: from the fresh parses of the text
before and after the step (the `reparse` subject), the position mapping, and
the script. No bound contains a constant chosen by the implementer. The
quantities, per step:

| Quantity | Definition |
| --- | --- |
| `E` | The **language damage**: the union of the edited lines (the step's range widened to whole lines as in plan 5.2, before and after the step) and the smallest range of lines such that the block trees of the two fresh parses (kinds, depths and mapped start lines of every block) agree before it and after it. A content-only edit therefore still has the lines it touched as its damage |
| `U` | Lines and bytes of every leaf that intersects `E`: every leaf that owns an inline root (paragraphs, headings, table cells, terms, captions and the like), and the units plan 5.3 always re-reads whole (tables with their captions, code, HTML, comment, formula and directive blocks) |
| `B` | Blocks of either fresh parse that start inside `E`, at any depth |
| `A` | Ancestors of the damage: the blocks of either fresh parse that contain some connected region of `E`, counted once each over the union of every region's ancestor path. A batch whose edits land in several leaves has one path per region |
| `H` | The height bound of the shared balanced tree over `n` elements: ⌈log₂(n + 1)⌉ + 1, for the text (`n` bytes), the ledger and each registry (`n` entries) |
| `C` | Per ancestor of `E`: its child count, and how many of its children start inside `E` |
| `K` | Registry keys whose winner, family or ordinal differs between the two fresh parses: reference and heading labels, anchors, footnote labels, specimen ids |
| `R(K)` | Inline roots of the new fresh parse that look up a key in `K`, and their content bytes |
| `N` | New objects the step must produce, as 4.3 and 4.4 predict them |
| `T` | Text-tree journal entries the step may need: for each edit range, 1 + 2`H`, plus 1 for the pending bytes. The 1 is the entry that takes the replaced chunks (plan 5.11); the rest are the internal nodes on the paths to the range's two ends, at most `H` each. Inserted chunks are new allocations and need no entry. `T` depends on the text and the edit only, never on the text tree's chunk size |

The bounds:

| Counter | Bound |
| --- | --- |
| `lines_reread` | ≤ lines of `E` ∪ `U` + 1 |
| `inline_bytes` | ≤ content bytes of the inline roots in `U` + bytes of `R(K)` |
| `ledger_touched` | ≤ `B` + `A` |
| `summaries_combined` | ≤ Σ over ancestors (children in `E` + 1) × ⌈log₂(child count + 1)⌉ |
| `registry_recomputed` | ≤ declarations inside `E` + members of the families of `K` |
| `lookups_invalidated` | ≤ number of inline roots in `R(K)` |
| `finish_visited` | ≤ `N` + nodes of the inline roots re-parsed (in `U` or `R(K)`) |
| `nodes_new` | = `N` |
| `journal_entries` | ≤ `ledger_touched` + `registry_recomputed` + `lookups_invalidated` + `T` + `A` |
| `tree_visited` | ≤ `H` × (edit ranges + `lines_reread` + `ledger_touched` + `registry_recomputed` + `lookups_invalidated`) |

`H` is a height bound only for a tree whose nodes other than the root have at
least two children and whose leaves are all at one depth, the B-tree shape of
the plan's balanced tree (5.1). The gate requires that shape; the occupancy
rule of 6.5 is the same invariant. A binary tree balanced by rotation (AVL,
red-black) can be up to twice as tall and fails the bounds that use `H`.
`tree_visited` is what makes the O(log n) claim exact: each operation the step
performs on the tree costs at most one root-to-leaf path, so an O(log² n)
traversal fails here even when its Ir hides under the margin of 6.3.

For a stream chunk, `E` is the last line before the chunk together with the
lines it appends, and the inline term is the chunk plus the distance from the
frontier leaf's stable prefix (plan 5.6) to its end. The harness takes the
stable prefix as the earliest of plan 5.6's three candidates, each computed
from the fresh parse of the text before the chunk: the start of the leaf's last
line; the earliest opener that the fresh parse left as literal text in the last
inline root (an emphasis or other delimiter run, a bracket, a citation token or
a field); and the start of the earliest token that runs to the end of the
content (an unmatched backtick run, an unclosed HTML or comment token, a
formula without a closer). Reading openers from literal text can only move the
candidate earlier, so the bound can be loose but never too small.

These bounds are exactly as large as the language makes a step, so the
language-inherent cases of plan 7.2 get their real bound through the same
formulas: an unclosed fence makes `E` run to the end of the document, and a
changed definition puts its references in `R(K)`. Each activation row of
section 7 turns on the counters its step makes meaningful; a step that has not
yet removed a term (step 2 re-reads the whole document) is gated only on the
counters that row names.

### 6.3 Flatness, on Ir

For every local edit family (`typing`, `lines`, `ranges`, `far`, `batch`)
on every scale shape, **every step** costs at most logarithmically more as the
document grows. The scripts apply the same edits at the same relative
positions at every size, so step `i` of one size corresponds to step `i` of
another. With `c(s)` the cost of step `i` at size `s` and
`Δ = max(0, c(64 KB) − c(16 KB))`, the rule is

- `c(256 KB) ≤ 1.25 × c(16 KB) + 2Δ`, and
- `c(1 MB) ≤ 1.25 × c(16 KB) + 3Δ`.

A cost `a + b log n` rises by the same amount at every fourfold size step, so
extrapolating the first rise is exactly its growth: the O(log n) text tree,
ledger and registries pass at any branching factor and tree height. A linear
term that is a share `ℓ` of the step's cost at 16 KB rises by `3ℓ` of that
cost to 64 KB but by `63ℓ` to 1 MB, so the step fails once `54ℓ > 0.25`, that
is, once the linear part is more than about half a percent of the step. A
size-dependent cost on any single step (a lazy O(n) initialization on the first
edit, say) fails the same way, however few steps it affects. A finite set of
sizes with any margin cannot tell a small `log² n` term from a logarithmic one;
the Ir rule rejects polynomial terms, and the logarithmic structures are held
to exactly O(log n) per operation by `tree_visited` (6.2).

For `tokens` and `rows` on every scale shape, the chunks do not correspond
across sizes, so the rule compares positions within one stream of `n` bytes.
The band at position `p` is the chunks that end in the last tenth before `p`,
and `m(p)` is the maximum, and separately the p95, of the band's costs. With
`Δ = max(0, m(n/4) − m(n/16))`, the rule is `m(n) ≤ 1.25 × m(n/16) + 2Δ`: the
same logarithmic allowance, which catches per-chunk work that grows with the
text already streamed. A stream measured in windows (5.1) takes each window's
mean step cost as its sample.

The same rule covers the adversarial shapes whose cost the language keeps
local: the 10,000-item list edited in the middle and the 1,000 nested quotes
edited at the leaf. It excludes the shapes whose cost plan 7.2 assigns to the
language (the stray opener, the definition with 10,000 references, the shifted
anchor suffixes, the unclosed `**`, the single-line paragraph) and streaming
into the `table` shape, which re-reads the table at the tail per appended row.
Those are bounded by their counters (6.2) and by 6.4.

This is the gate that makes R2 observable: a step whose cost depends on the
document size fails it at any constant factor.

### 6.4 Never worse than reparsing

For every step of every script of at most 1,024 steps, including `markers`,
`declarations` and `random`, `step_ir` is at most 1.25 times `reparse_ir` of
the same step (decision G2). For a longer stream, each window's total
`step_ir` is at most 1.25 times its step count times the `reparse_ir` of the
window's last step. The text only grows within a stream, so that is at least
the sum of the window's per-step reparse costs, and a correct session that
rescans a growing prefix as a reparse would is never rejected. Every window
is 1/1,024 of the stream, so the bound is tight except in the first few
windows. A single expensive step inside a window is caught by the same rule in
edges, which applies to every step: `step_edges` is at most 1.25 times the
`reparse_edges` of the last step of its window. The margin pays for
matching, deduplication and the journal when an edit really does change the
whole document; beyond it, an incremental edit would be a regression against
the application that just reparses.

### 6.5 Retention

R8 asks that what a session retains is specified. The gate checks it
directly, with no measured number: after every step of the correctness set
and at the end of every benchmark script, once the harness has released every
earlier document it held, the session retains what the current text alone
implies. The allocator seam tags each session-owned allocation with its kind
from plan 5.1, and the harness compares the live counts with the fresh parse
of the current text:

| Kind | Bound |
| --- | --- |
| Tree nodes | = nodes of the fresh document |
| Block ledger entries | = its block nodes |
| Registry entries | = its declarations |
| Lookup index entries | = its registry lookups |
| Inline ledger entries | = its inline roots |
| Frontier | ≤ its open spine and the content of its open leaf |
| Pending bytes | ≤ 3 |
| Journal entries and parse scratch | 0 |
| Text tree bytes | ≤ 2 × those of a session opened on the same text |

A kind is checked from the rollout step that introduces it. The text tree is
the only retained structure whose shape depends on its history, and 2 is its
occupancy bound: every chunk and node other than the root is at least half
full, the invariant that keeps a balanced tree balanced. A session that keeps
an earlier tree, ledger entry or journal fails on the first step it does so,
whatever the script's length, so the rule holds from step 2, before there is
a session baseline.

### 6.6 Regressions

Against the base revision, measured with the current harness and workloads on
both sides as the one-shot gate already does, and always between the same
subject on both sides:

- each workload's per-step p95 and total `step_ir` are at most 1.02 times the
  base;
- `retained` and `transient` are at most 1.02 times the base;
- the one-shot gate keeps its `source_to_buffer` rule and adds the same rule
  to `buffer_to_ast`, per document.

When the base revision has no `session` subject, which is the case for the
pull request of rollout step 2, there is nothing of the same kind to compare
with. That pull request is gated by 6.2, 6.4 and 6.5 alone, reports its session
numbers beside the base's `reparse` numbers, and becomes the session baseline;
the 1.02 rules apply to `session` from the next pull request (decision G4).

Speedup and `stream_ratio` are reported, not gated. They follow from 6.3, 6.4 and 6.6,
and a fixed target on either would be a number chosen from a measurement.

## 7. Activation by rollout step

Each gate turns on when the step that makes it meaningful lands. Before that,
its numbers are reported with the `reparse` subject.

| Step | Correctness | Benchmarks |
| --- | --- | --- |
| 0 Harness (this plan) | Scripts, text model and pending-byte model self-tests; 4.1 with `reparse` | Edit and stream runners report the R column; one-shot adds the `buffer_to_ast` rule (6.6) |
| 1 Model | 4.2 for fresh parses; deep equality and 4.10 on fresh documents | One-shot budget for the model change (G1), then 1.02 per PR |
| 2 Sessions, whole-document restart | 4.1–4.12 on the correctness set, every platform, both units | 6.4 on every workload, which sets the session baseline for 6.6 (G4); 6.2 for `nodes_new`; 6.5 for the kinds it introduces |
| 3 Block restart and convergence | Unchanged | 6.2 for `lines_reread`, `ledger_touched`, `summaries_combined` and `tree_visited`, and for `inline_bytes` and `finish_visited` on shapes without declarations; 6.3 for the local edit families on shapes without declarations |
| 4 Session registries | Unchanged | 6.2 for `registry_recomputed` and `lookups_invalidated`, and for `inline_bytes`, `finish_visited` and `journal_entries` on every shape; 6.3 for the local edit families on every remaining scale shape (`prose`, `quote`, `refs`) and for the local steps of `declarations` |
| 5 Frontier and inline restart | Unchanged | 6.2 in its stream form for `lines_reread` and `inline_bytes`; 6.3 for `tokens` and `rows` |

From step 2 on, the one-shot benchmark measures `Document.parse` through the
session path it becomes (plan 4.4), so the one-shot gate also guards what the
session machinery costs a fresh parse.

The plan lists this harness as its rollout step 0.

## 8. The harness proves it can fail

A gate that has never failed proves nothing. The harness's own tests run each
oracle against a deliberately wrong subject and require it to fail with the
named oracle:

| Faulty subject | Must fail |
| --- | --- |
| Returns the previous document for one step | 4.1 |
| Renumbers every id on every step (a fresh parse with fresh ids) | 4.2 lineage, 4.4, 4.5 |
| Returns deep copies with the same ids | 4.3 "if" direction |
| Gives an edited heading a new id | 4.4 |
| Keeps a node object whose text changed | 4.3 "only if" direction |
| Reuses a retired id for a new node | 4.2 |
| Applies a step and then reports failure | 4.8 |
| Accepts an end inside a scalar | 4.9 |
| Re-reads the whole document on every step | 6.2 and 6.3 |
| Keeps every earlier document alive | 6.5 |

The faulty subjects wrap `reparse` (and, from step 1, fresh ids), live only in
the harness's test sources, and are never linked into a product target. The
benchmark driver's tests also feed synthetic profiles that violate each Ir rule
and require the gate to reject them.

## 9. Where the gates run

One generator module, next to `corpus.mjs` in `scripts/benchmark/`, defines
every document and script of section 3. It has two outputs, because the C test
graph has no scripting-language dependency and the benchmark has no tracked
inputs.

**The correctness set** is the generator's small output, tracked under
`specs/incremental/` and kept current by a `--check` audit, as the node-kind
tables are. Its documents are the canonical AST cases, which every platform
already consumes, and the scale and adversarial shapes at their smallest size.
It holds every edit and stream family, `splits` on every document up to 2 KB,
`random` with 16 seeds, the scripted-identity cases of 4.5, and the invalid
arguments of 4.9. Scripts are offsets and short texts, so the set stays small
enough to track.

- C runs it as `incremental_runner` under the `api` label, with 4.1–4.9, and
  the OOM sweep of 4.8 on every step.
- Swift, Kotlin and ECMAScript consume it through the same lifecycle that
  delivers the canonical manifest to their conformance runners, in both units,
  with 4.1–4.7 and 4.9–4.11.
- The fuzz target of 4.12 covers what a fixed set cannot.

**The benchmark workloads** are the generator's large output, built when the
benchmark runs, as the grammar corpus is: every grammar corpus document with
`typing`, `lines`, `markers`, `undo`, `random`, `tokens` and `bytes`, and every
scale and adversarial shape at all four sizes with every family but `undo`.
`random` runs with 16 seeds in both, so 6.4 is measured on arbitrary ranges
and not only on the scripted families.
Before measuring, the runner applies each workload natively, outside
callgrind, and checks 4.1–4.4 after every step. A workload that fails is not
reported, and the run fails, as the one-shot runner already refuses an empty
tree. Every benchmark run is therefore also a correctness sweep over the whole
grammar corpus.

The benchmarks run as `pnpm benchmark:edits`, sharing `run.mjs`'s build,
provenance, isolation and callgrind code. Outputs go to
`build/benchmark-edits`: `edits.md`, `edits.json`, the generated workloads and
the raw profiles. CI adds a job, "Measure - edits and streams", to the
benchmark workflow, and the trusted publisher adds its tables to the existing
PR comment. The report shows, per workload, the S column (session), the R
column (reparse), the speedup, and the flatness ratios by size.

## 10. Work items

Step 0 of the incremental plan is these items. Later steps activate the gates
as section 7 says, in their own pull requests.

- [ ] The workload generator: documents, edit and stream scripts, the
      composite-part check (3.1), and its `node --test` suite.
- [ ] The tracked correctness set under `specs/incremental/` and its
      `--check` audit.
- [ ] The C `incremental_runner` with the text model, the `reparse` subject,
      the oracles that apply before sessions exist, and the faulty subjects
      of section 8 that wrap `reparse`.
- [ ] The edit and stream benchmark runner, `pnpm benchmark:edits`, reporting
      the R column, with driver tests on synthetic profiles.
- [ ] The `buffer_to_ast` regression rule in the one-shot gate (6.6, G1).
- [ ] The "Measure - edits and streams" CI job and its tables in the PR
      comment.

## 11. Decisions for the owner

- **G1 One-shot budget for the model change.** Step 1 makes every fresh parse
  assign ids, store relative extents instead of absolute scopes and, in
  Swift, allocate one record per node. Proposed: step 1 may raise `buffer_to_ast` Ir up to 1.10 times the
  pre-step baseline per document, stated in its pull request, and
  `source_to_buffer` keeps its 1.02 rule. After step 1, both stages are at
  1.02 per pull request.
- **G2 The reparse margin.** Proposed 1.25 (6.4). A smaller margin forbids
  paying for matching on whole-document changes; a larger one hides a
  regression in the language-inherent cases.
- **G3 Flatness factor.** Proposed 1.25 as the margin on top of the
  logarithmic allowance across a 64 times size range (6.3). It is a statement
  of "no linear term", and it should not be loosened to pass a measurement.
- **G4 The session baseline.** Proposed: the step 2 pull request sets the
  session baseline under 6.2, 6.4 and 6.5 only, and the 1.02 regression
  rules apply to the session from then on (6.6).
