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
run. Section 10 lists the decisions for the owner.

## 1. Why the gates come first

The incremental plan first put its gates in its last rollout step. That order
lets four steps land without the evidence the design depends on:

- Step 1 adds ids, deep equality, relative spans and Swift per-node records.
  Every one of them costs the one-shot parse something, and the current gate
  sees only `source_to_buffer`. Nothing today would notice the AST stage or
  materialization getting slower.
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
| `batch` | 16 disjoint edits in one `batch`, as multi-cursor typing | Spread across the document |
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

### 4.2 Identity (R4)

- Ids are unique within the document, across every owned relation.
- Over the whole lineage, the harness keeps a map from id to kind and a set of
  retired ids. An id never changes kind, and a retired id never appears again.
- Two fresh parses of the same text are equal, ids included.

### 4.3 Reuse is exactly value equality (R3)

For every node `N` of the new document whose id also names a node `O` of the
previous document: `N` is the same object as `O` if and only if `N` is deep
equal to `O` (plan 4.2). The "only if" direction catches a stale node reused
after its value changed. The "if" direction catches a wasteful new object for
an unchanged value, which forces every consumer to re-render it. Every
ancestor of a new object is a new object.

"Same object" is object identity in Swift, Kotlin and ECMAScript. In C it is
an unchanged node `version` (plan 5.9); the harness snapshots the previous
document's per-node values, keyed by id, because C views are invalidated by
the next edit.

### 4.4 Unchanged subtrees are reused

4.3 allows an implementation to give an unchanged node a new id, which would
lose its view state. This oracle closes that gap without knowing the
algorithm. Using the plan's position mapping (5.2), each node `O` of the
previous document is mapped to the image of its source range. If the new
document has, at that image, a node of the same kind whose relative-span
subtree value equals `O`'s, then that node is `O` itself: same id, same object.
Relative spans make this precise: a node after an inserted line keeps its
value, while the first node after a changed sibling may change its lead, and
then is legitimately a new value with the same id.

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
bytes are never part of the text. An append whose bytes cannot complete the
pending sequence is rejected and leaves the session unchanged (4.9).

### 4.7 Batches

The dump after `batch(edits)` equals the dump after applying the same edits
one at a time from the last to the first (so earlier offsets stay valid), and
4.1–4.4 hold for the batch as one step.

### 4.8 Transactions (R7)

The allocator-seam OOM sweep runs every step of the correctness set's
`typing` and `tokens` scripts with a failure injected at every allocation
boundary. After each
failure the subject's text, dump, ids, versions and retained-state digest
equal the previous version's, and the unmodified step then succeeds. The same
sweep runs through each binding's two-phase publication (plan 6.1) with the
host allocation failing after the engine prepared the edit.

### 4.9 Invalid arguments

Ranges out of bounds, ends inside a scalar (a UTF-8 continuation byte, the
middle of a surrogate pair), ill-formed replacement text and impossible
pending continuations are rejected as invalid arguments, and the session is
bit-for-bit at its previous version.

### 4.10 Depth

Every binding runs the plan's deep-tree set (6.3) through a session: release
of a deep document whose subtree is shared with a newer version, equality of
two deep documents that differ at the deepest leaf, walking, scope lookup, hit
testing and `description`, on a thread with a small fixed stack.

### 4.11 Platforms and units

C and every binding run the same scripts in both units. A binding checks 4.1
against its own dumper, as conformance does today, and checks 4.3–4.5 with its
own object identity. The binding also counts the objects it materialized per
step; that count equals the number of new objects 4.3 observed.

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
every step. A longer script (a streamed megabyte) dumps after 64 steps spaced
evenly by position plus the last, and its total is measured from the edge.

The C benchmark measures the engine. Materialization in the bindings is gated
by deterministic counters (5.3), not by timings, as the testing architecture
already requires.

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
| `nodes_materialized` | Objects a binding built (per binding) |
| `journal_entries` | Transaction journal entries |

Counters are cheap, exact and platform-independent. They are where the plan's
bounds are enforced (6.2). Ir is where constant factors and the asymptotic
shape are checked (6.3–6.5).

## 6. Gate rules

A rule is either a bound derived from the design or a regression limit
against the base revision. No rule is a threshold on a measured number that
happened to look good.

### 6.1 Correctness

Every oracle of section 4 passes on every step of every script that runs it.
There is no tolerance.

### 6.2 Bounds, on counters

The generator knows the structure it built, so each script step carries the
quantities its bound needs: the damaged leaf's lines and bytes, the depth of
the edit point, the child counts along the path, and the number of references
to a changed declaration. The gate checks the plan's table 7.1 as exact
inequalities with named constant terms, for example for `typing` in a
closed paragraph:

- `lines_reread` ≤ lines of the paragraph + the lookahead of its kind;
- `inline_bytes` ≤ bytes of the paragraph;
- `nodes_new` = the set 4.5 asserts;
- `summaries_combined` ≤ (changed children + 1) × ⌈log₂ children⌉ per
  ancestor.

For streaming, per chunk: `lines_reread` ≤ 1 + lines closed by the chunk, and
`inline_bytes` ≤ chunk + the distance to the stable prefix, which the generator
computes from the document it streams.

The language-inherent cases of plan 7.2 get their real bound: opening an
unclosed fence re-reads to the end, a changed definition invalidates its
references. The bound is still exact; it is just large.

### 6.3 Flatness, on Ir

For every local edit family (`typing`, `lines`, `ranges`, `far`, `batch`) and
for `tokens` and `rows` on every scale shape, per-step p95 Ir at 1 MB is at
most 1.25 times the per-step p95 at 16 KB. The factor 64 in size leaves room
for the O(log n) text tree, ledger and registries (six more tree levels) and
nothing linear. For streams, the p95 of the chunks in the last tenth of the
document is at most 1.25 times the p95 of the chunks in the second tenth, which
catches per-chunk work that grows with the text already streamed.

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

For every step of every script, including `markers`, `declarations` and
`random`, `step_ir` is at most 1.25 times `reparse_ir` of the same step
(decision G2). The margin pays for
matching, deduplication and the journal when an edit really does change the
whole document; beyond it, an incremental edit would be a regression against
the application that just reparses.

### 6.5 Regressions

Against the base revision, measured with the current harness and workloads on
both sides as the one-shot gate already does:

- each workload's per-step p95 and total `step_ir` are at most 1.02 times the
  base;
- `retained` and `transient` are at most 1.02 times the base;
- the one-shot gate keeps its `source_to_buffer` rule and adds the same rule
  to `buffer_to_ast`, per document.

Speedup and `stream_ratio` are reported, not gated. They follow from 6.3–6.5,
and a fixed target on either would be a number chosen from a measurement.

## 7. Activation by rollout step

Each gate turns on when the step that makes it meaningful lands. Before that,
its numbers are reported with the `reparse` subject.

| Step | Correctness | Benchmarks |
| --- | --- | --- |
| 0 Harness (this plan) | Scripts, text model and pending-byte model self-tests; 4.1 with `reparse` | Edit and stream runners report the R column; one-shot adds the `buffer_to_ast` rule (6.5) |
| 1 Model | 4.2 for fresh parses; deep equality and 4.10 on fresh documents | One-shot budget for the model change (G1), then 1.02 per PR |
| 2 Sessions, whole-document restart | 4.1–4.12 on the correctness set, every platform, both units | 6.4 and 6.5 on every workload; 6.2 for `nodes_new`, `nodes_materialized`, `journal_entries` |
| 3 Block restart and convergence | Unchanged | 6.2 for block counters; 6.3 for the local edit families on shapes without declarations |
| 4 Session registries | Unchanged | 6.2 for registry counters; 6.3 on `refs` and the local steps of `declarations` |
| 5 Frontier and inline restart | Unchanged | 6.2 for streaming; 6.3 for `tokens` and `rows` |

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
| Keeps a node object whose text changed | 4.3 "only if" direction |
| Reuses a retired id for a new node | 4.2 |
| Applies a step and then reports failure | 4.8 |
| Accepts an end inside a scalar | 4.9 |
| Re-reads the whole document on every step | 6.2 and 6.3 |

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
  the OOM sweep of 4.8 on `typing` and `tokens`.
- Swift, Kotlin and ECMAScript consume it through the same lifecycle that
  delivers the canonical manifest to their conformance runners, in both units,
  with 4.1–4.7, 4.9–4.11 and the publication failures of 4.8.
- The fuzz target of 4.12 covers what a fixed set cannot.

**The benchmark workloads** are the generator's large output, built when the
benchmark runs, as the grammar corpus is: every grammar corpus document with
`typing`, `lines`, `markers`, `undo`, `tokens` and `bytes`, and every scale and
adversarial shape at all four sizes with every family but `undo` and `random`.
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

## 10. Decisions for the owner

- **G1 One-shot budget for the model change.** Step 1 makes every fresh parse
  assign ids, compute relative spans and, in Swift, allocate one record per
  node. Proposed: step 1 may raise `buffer_to_ast` Ir up to 1.10 times the
  pre-step baseline per document, stated in its pull request, and
  `source_to_buffer` keeps its 1.02 rule. After step 1, both stages are at
  1.02 per pull request.
- **G2 The reparse margin.** Proposed 1.25 (6.4). A smaller margin forbids
  paying for matching on whole-document changes; a larger one hides a
  regression in the language-inherent cases.
- **G3 Flatness factor.** Proposed 1.25 across a 64 times size range (6.3).
  It is a statement of "no linear term", and it should not be loosened to pass
  a measurement.
- **G4 Materialization in the flat shape.** In the bindings, publishing a new
  version rebuilds each changed node's child collection, so a stream into a
  document with thousands of top-level blocks builds a collection of that size
  per chunk. Every consumer framework also reconciles that collection per
  update, so this is the consumers' cost as much as ours. Proposed: gate the
  engine as flat (6.3) and gate `nodes_materialized` against the plan's
  O(d + F) term, reporting binding collection sizes, rather than changing
  the value model's collections to persistent ones.
