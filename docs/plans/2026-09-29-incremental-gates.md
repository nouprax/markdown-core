# Gates for incremental parsing

Status: proposed design. Nothing in this document is implemented. It
specifies the correctness suite and the two benchmarks that decide whether
[incremental parsing](2026-09-29-incremental-parsing.md) is done, and it lands
before any engine work so that every rollout step is measured against the same
harness from its first pull request.

## Requirement under test

The owner's requirement, as stated:

> We need to do a thorough and systematical design to markdown-core to make it
> support incremental edit in code editor/llm streaming scenario. Which means
> for random edit and tail streaming, we should make sure the re-parse and AST
> mutate is minimum. Please note, this packages downstream consumer is
> SwiftUI, Compose, and React, which means the AST update is self described
> via AST's identifier and equatable itself, you should not provide diff as
> the result. the only result should be a new AST(can be in place update or a
> new one, I do not limit the direction to avoid you misunderstanding and add
> some constraints I did not intended).

Every gate below checks one of these properties, for the two workloads the
requirement names (random edits and tail streaming):

- **Correct AST.** The AST a session returns is the AST a fresh parse of the
  same text returns.
- **Identifier.** A node's identifier is unique, keeps naming the same node
  across edits, and is never handed to a different node.
- **Minimal AST mutation.** A node is a new value exactly when its value
  changed.
- **Minimal re-parse.** The Ir of a local step does not grow with the
  document, and no step costs more than parsing the whole text again.

The existing one-shot benchmark (`packages/markdown-core/benchmarks/README.md`)
answers "what does a parse cost". The gate therefore has three benchmarks
(one-shot, edit, stream) and one correctness suite for the two new workloads.
A step of the rollout is accepted only when all four pass at the level section
7 assigns to that step.

Sections 1–2 say why the gates come first and what they are measured against.
Section 3 defines the workloads, section 4 the correctness oracles, sections
5–6 the measurement and the gate rules, section 7 how the gates tighten step by
step, section 8 how the harness proves it can fail, and section 9 where they
run. Section 10 lists the work items and section 11 the owner's decisions.

## 1. Why the gates come first

The incremental plan puts its gates in its last rollout step. That order lets
the earlier steps land without the evidence the design depends on:

- Step 1 adds ids, deep equality and Swift per-node records, and replaces
  absolute scopes with relative extents. Every one of them costs the one-shot
  parse something, and the current gate sees only `source_to_buffer`. Nothing
  today would notice the AST stage getting slower.
- Each later step claims one of the properties above. Only an oracle that
  runs every edit shows that the code keeps them.
- The complexity bounds of the plan's section 7.1 are the reason for the whole
  design. A bound that is first measured after the implementation is complete
  is a bound nobody designed against.

The gates also fix the vocabulary before the code exists: what an edit script
is, what "unchanged" means observably, what is counted. Each later step then
changes the subject under test, not the harness.

## 2. Subjects and the baseline

The harness drives a **subject** through one interface: open a session in a
coordinate unit with an initial text, call `edit` (a batch of
non-overlapping edits against the text before the batch) or `append`, each
of which returns the new document, and close. There are exactly two subjects:

| Subject | What it does | Role |
| --- | --- | --- |
| `session` | The engine's `MarkdownSession` (plan 4.4) | Under test, from rollout step 2 |
| `reparse` | Keeps the text, and on every step calls `Document.parse` on the whole of it | The oracle for correctness, the R column for benchmarks |

`reparse` is the one parser, called the way an application calls it today. It
is the equivalence oracle and the reference cost an incremental edit must
beat. Until the session API exists the harness runs `reparse` alone, which
already produces every baseline number and exercises the scripts, the text
model and the reports.

The harness keeps its own **text model**: a plain byte buffer to which it
applies every script step, independent of the subject. The subject's text
must equal it after every step.

## 3. Workloads

All workloads are generated deterministically in-process from tracked
sources, like the grammar corpus. Nothing is downloaded and no corpus is
vendored (`packages/markdown-core/tests/corpora/README.md`). A workload is a
document and a script; its identity is the digest of both.

### 3.1 Documents

- **Grammar corpus.** The dialect encoding of every grammar corpus
  certificate, in both alphabets. It covers every feature
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
- **Adversarial shapes.** The inputs the plan's section 7.2 and testing
  section name, each at the scale sizes: a stray early opener (an unclosed
  fence, HTML block, comment and directive on the first line, toggled open
  and closed); a 10,000-item list edited in the middle; 1,000 nested block
  quotes edited at the deepest leaf; one definition with 10,000 references
  whose destination changes; a heading label repeated so that anchor suffixes
  shift; an unclosed `**` early in a paragraph that is then streamed for
  64 KB; a single-line paragraph streamed to 64 KB.

A composite document that joins parts records, for each part, the number of
root children a fresh parse of the part alone produces. The generator checks
that the composite's fresh parse has their sum, so a part cannot silently
change the meaning of its neighbour and move every edit site.

### 3.2 Edit scripts

A script is a sequence of steps. A step is `edit([(start, end, text)…])`, a
single edit being a batch of one, or `append(text)`, with offsets in UTF-8
bytes of the text before the step. Every offset falls on a scalar boundary and
every text is well formed, except in the invalid-argument cases (4.8).
Bindings convert offsets to their unit through the text model, so the same
script tests both units.

| Family | Steps | Positions |
| --- | --- | --- |
| `typing` | One scalar per step, 64 steps, with runs of backspace | Inside a paragraph, a list item at depth 3, a table cell, a heading, a code block, a quote leaf, a footnote body |
| `lines` | Enter inside a paragraph; insert and delete a blank line between blocks; join two lines; merge two paragraphs | Beginning, middle and end of the document |
| `markers` | Add and remove `> `, `- `, `1. `, `# `, four spaces, a fence opener, a Setext underline, a table delimiter row, a definition term marker | Each block kind of the shape |
| `ranges` | Paste a 2 KB section; delete a section; select a paragraph's whole text and type a replacement | Middle of the document |
| `far` | Alternate single-scalar edits at the first and last line, 64 steps | Both ends |
| `batch` | 16 disjoint edits in one `edit`, as multi-cursor typing, listed in a seeded shuffled order | Spread across the document |
| `declarations` | Change a reference destination; add and remove a duplicate reference label; add and remove a heading whose label collides; add and remove a footnote definition and an inline note | Declaration sites of `refs` and `prose` |
| `undo` | Each `typing` to `declarations` step followed by its inverse | As the original step |
| `random` | Seeded mixture of inserts, deletes and replacements at line and byte granularity, including CR/LF splits and NUL | Uniform over the text |

`typing`, `lines`, `ranges`, `far` and `batch` are **local**: the language
itself limits their effect to a bounded neighbourhood. `markers`,
`declarations` and `random` contain steps whose effect the language may spread
(plan 7.2); the gates treat them differently (section 6).

### 3.3 Stream scripts

A stream script appends the whole of a document, starting from an empty
session, in chunks. Every chunk is whole scalars, on every platform.

| Family | Chunking | Documents |
| --- | --- | --- |
| `tokens` | A fixed pseudo-random sequence of chunk sizes from 1 to 16 bytes, mean 4, each moved to the next scalar boundary | Scale families, grammar corpus |
| `scalars` | One scalar per chunk | Grammar corpus |
| `rows` | One physical line per chunk | Scale families |
| `splits` | Two chunks, split at every scalar boundary | Grammar corpus documents up to 2 KB |

## 4. Correctness oracles

Every oracle is checked after every step of every script that runs it
(section 9). The oracles are stated against the public model, not against the
engine's internals, so they hold for any correct implementation and fail for
any incorrect one.

### 4.1 Equivalence

The subject's text equals the text model, and the canonical dump of the
subject's document equals the dump of a fresh `Document.parse` of the model
text in the same unit. The dump prints no ids and prints absolute scopes, so
it compares meaning and positions and nothing that depends on history.

A node stores its relative extent, `Extent(lead, span)` in UTF-8 bytes (plan
4.3). `Document.scope(of:in:)` and `Document.node(at:in:)` compute absolute
positions from the extents and the source text the caller passes, in the
session's unit. For every node of the subject's document, `scope(of:in:)` with
the model text equals `scope(of:in:)` of the corresponding node of the fresh
parse. At every position where some node's scope starts or ends, and on each
side of it, `node(at:in:)` returns the node corresponding to the fresh parse's
answer at the same position. The fresh parse's own answers are checked once
against the scopes the canonical dump prints, which are UTF-8 columns, and in
UTF-16 against the same scopes converted through the model text.

`Document.footnotes`, `Document.specimens`, `footnote(for:)` and
`specimen(for:)` for every label in the text return the nodes corresponding
to the fresh parse's answers, in the same order (plan 8).

### 4.2 Identifier

- Ids are unique within the document, across every owned relation.
- Over the whole lineage, the harness keeps a map from id to kind and a set of
  retired ids. An id never changes kind, and a retired id never appears again.
- A fresh parse numbers its nodes from 1 in canonical walk order (plan 4.1),
  so two fresh parses of the same text are equal, ids included.

### 4.3 Minimal AST mutation

For every node of the new document, the harness classifies it against the
node of the previous document with the same id: **unchanged** when the two are
deep equal (plan 4.2), and **changed** when they differ or no previous node has
that id. `N` is the number of changed nodes. An ancestor of a changed node is
changed, because its children are part of its value. In C, every unchanged
node is the previous document's node, reused, and every node that is a new C
object is changed (plan R3, 8): a new object for an unchanged node is a
rewrite the step did not need.

The comparison uses a **snapshot** of the previous document the harness takes
before the step: each node's kind, id, scalars, extent and the ids of its
children, keyed by id, together with its absolute scope from `scope(of:in:)`
and the text before the step. The snapshot makes the check independent of
whether the engine updates in place or builds a new document. The extent is
part of the value; the absolute scope serves only the matching of 4.4.

### 4.4 Identity follows the matching rule

4.3 classifies nodes by the ids the subject assigned, so on its own it would
accept an implementation that gives a surviving node a new id and so loses its
view state. The plan states which old node each new node continues (5.9) in
terms the harness evaluates from the public model alone: the position mapping
of the step (plan 5.2), the absolute scopes of the snapshot and of
`scope(of:in:)` on the new document, kinds and owner relations. The harness
computes the expected matching itself, for every node of every step:

- The new document continues the old document (plan 5.9 starts matching
  from the reopened spine, whose root is the document).
- An old node's anchor is its first byte that survived the step. A node none
  of whose bytes survived has no anchor.
- Within the relation of a matched owner, a new node of the same kind whose
  source range contains the image of an old sibling's anchor continues the
  earliest such sibling.
- A new node that continues an old node has the old node's id. Every other
  new node, including every child of an unmatched owner, has an id the
  lineage has never seen. An old node that nothing continues is retired.

A continued node whose value equals its predecessor's is unchanged and outside
`N` (4.3). Extents are relative, so text that moves a node without touching it
leaves its value unchanged, except that the first continued node after a
changed or inserted sibling in the same relation may get a new `lead` (plan
5.3). The oracle predicts that `lead` from the fresh parse.

### 4.5 Scripted identity

Some edits have an exact expected outcome, written into the script as the sets
of kept, new and retired ids by position. They are the consequences the plan
lists (5.9, 8): typing at the start of a paragraph keeps its id; inserting
`new\n\n` before a paragraph gives the new one a new id and keeps the old one's;
deleting a first word keeps the id; deleting a sibling retires its id and does
not hand it to the next; merging two paragraphs keeps the first id; a
paragraph that becomes a Setext heading is a new node; a paragraph moved into
a new quote is a new node; unwrapping a nested inline note makes the inner note
a new node; inserting a line at the top of a long document changes only the
Document and the edited paragraph; changing a heading anchor that Links target
changes only the Document and the heading. Typing in paragraph 5 of 1,000 changes exactly that
paragraph, its Text nodes on the edited line and the Document.

### 4.6 Streaming

After every chunk, 4.1–4.4 hold against the model text, which is everything
appended so far.

### 4.7 Batches

The dump after `edit(edits)` with several edits equals the dump after
applying the same edits one at a time in descending order of their start
offsets, whatever order the batch lists them in (so the offsets not yet
applied stay valid), and 4.1–4.4 hold for the batch as one step.

### 4.8 Errors

A range whose start is after its end or whose end is past the text and two
overlapping edits of one batch are rejected as out of bounds; an offset at a
continuation byte in UTF-8 or between the two units of one scalar in UTF-16 is
rejected as inside a scalar (plan 4.4). In C, the
allocator-seam sweep of plan 8 fails a sample of steps at every allocation
boundary; each call throws the out-of-memory error, and freeing the session
leaks nothing.

### 4.9 Deep trees

Every binding runs the plan's deep-tree set through a session: release of a
deep document while a newer version is alive, equality of two deep documents
that differ at the deepest leaf, walking, scope lookup, hit testing and
`description`, on a thread with a small fixed stack.

### 4.10 Platforms and units

C and every binding run the same scripts in both units. A binding checks 4.1
against its own dumper, as conformance does today, and 4.2–4.5 on ids and
values.

### 4.11 Fuzzing

A fuzz target decodes its input as a document and a script and checks 4.1–4.4
and 4.8 after every step. The current fuzz corpus seeds the document half.

## 5. Measurement

### 5.1 What is measured

The runners follow the one-shot benchmark's rules: callgrind instruction and
data-reference counts, the benchmark preset, the same build provenance and
isolation, and a measured edge instead of a timer. The edit runner loads a
document and a script, opens the subject with the document outside the
measurement, and calls `bench_apply_step` once per step.

Costs come from callgrind client requests (`CALLGRIND_DUMP_STATS`), which are
no-ops outside valgrind. The runner dumps after each of at most 1,024
**windows** of consecutive steps. A script of at most 1,024 steps, which is
every edit script and every short stream, has one step per window, so a
window's cost is that step's cost. A longer stream (a streamed megabyte in
`tokens` or `rows`) splits into 1,024 contiguous windows of equal step count.
Its `reparse` column is measured at each window's last step, because
reparsing after every chunk of a megabyte stream is the quadratic cost the
design removes.

The gates measure the C engine. A binding builds its platform values from what
the engine returns (plan 6.1); bindings are gated for correctness (section 4).

### 5.2 Metrics per workload

| Metric | Definition |
| --- | --- |
| `step_ir` | Ir per window: p50, p95, max and total over the script |
| `reparse_ir` | The same for the `reparse` subject: what an application pays today |
| `oneshot_ir` | One-shot parse of the script's final text, both stages |
| `speedup` | `reparse_ir / step_ir` per step, p50 and p5, for scripts of at most 1,024 steps |
| `stream_ratio` | Stream total `step_ir` / `oneshot_ir` of the final text |

## 6. Gate rules

A rule is either a bound derived from the design or a regression limit
against the base revision. No rule is a threshold on a measured number that
happened to look good.

### 6.1 Correctness

Every oracle of section 4 passes on every step of every script that runs it.
There is no tolerance.

### 6.2 Flatness, on Ir

For every local edit family (`typing`, `lines`, `ranges`, `far`, `batch`)
on every scale shape, **every step** costs at most 1.25 times as much at every
larger size as at 16 KB. The scripts apply the same edits at the same relative
positions at every size, so step `i` of one size corresponds to step `i` of
another. With `c(s)` the cost of step `i` at size `s`, each of `c(64 KB)`,
`c(256 KB)` and `c(1 MB)` is at most `1.25 × c(16 KB)`. A linear term that is a
share `ℓ` of the step's cost at 16 KB grows by `63ℓ` of that cost at 1 MB, so
the step fails once the linear part is more than about 0.4 percent of the
step. A size-dependent cost on any single step fails the same way, however few
steps it affects.

For `tokens` and `rows` on every scale shape, the chunks do not correspond
across sizes, so the rule compares positions within one stream of `n` bytes.
With `m(p)` the highest cost of every window that ends at or before `p`, each
of `m(n/4)` and `m(n)` is at most `1.25 × m(n/16)`, which catches per-chunk
work that grows with the text already streamed, wherever in the stream it
occurs.

The same rule covers the adversarial shapes whose cost the language keeps
local: the 10,000-item list edited in the middle and the 1,000 nested quotes
edited at the leaf. It excludes the shapes whose cost plan 7.2 assigns to the
language (the stray opener, the definition with 10,000 references, the shifted
anchor suffixes, the unclosed `**`, the single-line paragraph) and streaming
into the `table` shape, which re-reads the table at the tail per appended row.
Those are bounded by 6.3.

### 6.3 Never worse than reparsing

For every step of every script of at most 1,024 steps, including `markers`,
`declarations`, `random` and every stream of at most 1,024 chunks, `step_ir`
is at most 1.25 times `reparse_ir` of the same step (decision G2). A longer
stream is bounded per chunk by 6.2; its `reparse_ir` column is
reported. The margin pays for matching and deduplication when an edit really
does change the whole document; beyond it, an incremental edit would be a
regression against the application that just reparses.

### 6.4 Regressions

Against the base revision, measured with the current harness and workloads on
both sides as the one-shot gate already does, and always between the same
subject on both sides:

- each workload's p95, maximum and total `step_ir` are at most 1.02 times the
  base;
- the one-shot gate keeps its `source_to_buffer` rule and adds the same rule
  to `buffer_to_ast`, per document.

When the base revision has no `session` subject, which is the case for the
pull request of rollout step 2, there is nothing of the same kind to compare
with. That pull request is gated by 6.3, reports its session numbers
beside the base's `reparse` numbers, and becomes the session baseline; the 1.02
rules apply to `session` from the next pull request (decision G4).

Speedup and `stream_ratio` are reported. They follow from 6.2, 6.3 and 6.4,
and a fixed target on either would be a number chosen from a measurement.

## 7. Activation by rollout step

Each gate turns on when the step that makes it meaningful lands. Before that,
its numbers are reported with the `reparse` subject.

| Step | Correctness | Benchmarks |
| --- | --- | --- |
| 0 Harness (this plan) | Scripts and text model self-tests; 4.1 with `reparse` | Edit and stream runners report the R column; one-shot adds the `buffer_to_ast` rule (6.4) |
| 1 Model | 4.2 for fresh parses; deep equality and 4.9 on fresh documents | One-shot budget for the model change (G1), then 1.02 per PR |
| 2 Sessions, whole-document restart | 4.1–4.11 on the correctness set, every platform, both units | 6.3 on every workload, which sets the session baseline for 6.4 (G4) |
| 3 Block restart and convergence | Unchanged | 6.2 for the local edit families on shapes without declarations |
| 4 Session registries | Unchanged | 6.2 for the local edit families on every remaining scale shape (`prose`, `quote`, `refs`) and for the local steps of `declarations` |
| 5 Frontier and inline restart | Unchanged | 6.2 for `tokens` and `rows` |

From step 2 on, the one-shot benchmark measures `Document.parse` through the
session path it becomes (plan 4.4), so the one-shot gate also guards what the
session machinery costs a fresh parse.

## 8. The harness proves it can fail

A gate that has never failed proves nothing. The harness's own tests run each
oracle against a deliberately wrong subject and require it to fail with the
named oracle:

| Faulty subject | Must fail |
| --- | --- |
| Returns the previous document for one step | 4.1 |
| Renumbers every id on every step (a fresh parse with fresh ids) | 4.2 lineage, 4.4, 4.5 |
| Rewrites every node with the same ids | 4.3 |
| Gives an edited heading a new id | 4.4 |
| Keeps a node whose text changed | 4.1 |
| Reuses a retired id for a new node | 4.2 |
| Accepts an offset inside a scalar | 4.8 |
| Re-reads the whole document on every step | 6.2 |

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
arguments of 4.8. Scripts are offsets and short texts, so the set stays small
enough to track.

- C runs it as `incremental_runner` under the `incremental` label, with
  4.1–4.9: one test per family the set's manifest declares, run in parallel.
- Swift, Kotlin and ECMAScript consume it through the same lifecycle that
  delivers the canonical manifest to their conformance runners, in both units,
  with 4.1–4.10 except the allocator sweep of 4.8.
- The fuzz target of 4.11 covers what a fixed set cannot.

**The benchmark workloads** are the generator's large output, built when the
benchmark runs, as the grammar corpus is: every grammar corpus document with
`typing`, `lines`, `markers`, `undo`, `random`, `tokens` and `scalars`, and
every scale and adversarial shape at all four sizes with every family but
`undo`. `random` runs with 16 seeds in both, so 6.3 is measured on arbitrary
ranges and not only on the scripted families.

The benchmarks run as `pnpm benchmark:edits`, sharing `run.mjs`'s build,
provenance, isolation and callgrind code. Outputs go to
`build/benchmark-edits`: `edits.md`, `edits.json`, the generated workloads and
the raw profiles. CI adds a job, "Measure - edits and streams", to the
benchmark workflow, and the trusted publisher adds its tables to the existing
PR comment. The report shows, per workload, the S column (session), the R
column (reparse), the speedup, and the flatness ratios by size.

## 10. Work items

Step 0 is these items. Later steps activate the gates as section 7 says, in
their own pull requests.

- [ ] The workload generator: documents, edit and stream scripts, the
      composite-part check (3.1), and its `node --test` suite.
- [ ] The tracked correctness set under `specs/incremental/` and its
      `--check` audit.
- [ ] The C `incremental_runner` with the text model, the `reparse` subject,
      the oracles that apply before sessions exist, and the faulty subjects
      of section 8 that wrap `reparse`.
- [ ] The edit and stream benchmark runner, `pnpm benchmark:edits`, reporting
      the R column, with driver tests on synthetic profiles.
- [ ] The `buffer_to_ast` regression rule in the one-shot gate (6.4, G1).
- [ ] The "Measure - edits and streams" CI job and its tables in the PR
      comment.

## 11. Owner decisions

- **G1 One-shot budget for the model change. Decided 2026-09-29: as proposed.** Step 1 makes every fresh parse
  assign ids, store relative extents instead of absolute scopes and, in
  Swift, allocate one record per node. Proposed: step 1 may raise
  `buffer_to_ast` Ir up to 1.10 times the pre-step baseline per document,
  stated in its pull request, and `source_to_buffer` keeps its 1.02 rule.
  After step 1, both stages are at 1.02 per pull request.
- **G2 The reparse margin. Decided 2026-09-29: as proposed.** Proposed 1.25 (6.3). A smaller margin forbids
  paying for matching on whole-document changes; a larger one hides a
  regression in the language-inherent cases.
- **G3 Flatness factor. Decided 2026-09-29: 1.25, with no allowance for
  growth.** A step costs at most 1.25 times its cost at 16 KB at every size up
  to 1 MB (6.2). It is a statement of "no linear term", and it should not be
  loosened to pass a measurement.
- **G4 The session baseline. Decided 2026-09-29: as proposed.** Proposed: the step 2 pull request sets the
  session baseline under 6.3, and the 1.02 regression rules apply to
  the session from then on (6.4).
