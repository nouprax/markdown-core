# Gates for incremental parsing

Status: steps 0 to 8 of the rollout implemented this harness: the stream
workloads, the correctness runner on every platform, the stream benchmark with
rules 6.3 and 6.4, the `buffer_to_ast` rule of the one-shot gate, `finalize`,
the stream oracles of 4.6 and, with step 8, rule 6.2 on the paragraph of
short lines. On 2026-10-14 the owner narrowed the
requirement to streaming ([plan](2026-09-29-incremental-parsing.md) section 1,
D7). On 2026-10-27 edits came back, on a finalized session and re-reading
only the damaged block (plan 12). Step E0 of that rollout added the edit
gates: the edit scripts (3.3), the edit oracles 4.11 to 4.13 and their faulty
subjects, and the edit benchmark's R column (5.1). Until step E1 gives the session its edit, the edit scripts run on
the `reparse` subject. This document specifies the
correctness suite and the benchmarks that decide whether each step of
[incremental parsing](2026-09-29-incremental-parsing.md) is done.

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

On 2026-10-14 the owner narrowed it to one workload:

> 我仔细理了一下我们的编辑需求，我们实际只有streaming的追加token更新一种需要高吞吐的实时编辑能力，因此，鉴于现在性能的现实，你可能需要彻底放弃现在的所有design，但是那个为修正表格的scope做的node runs的设计可能需要保留，我们需要正确的表达每个node的位置的能力。然后基于llm的token streaming追加场景设计一个高度优化的markdown解析器。

> 对于llm session streaming，你甚至可以在session finalize时再去做definition look up，因为token streaming过程中的文本跳变本来就是无法避免的。

> 也就是你需要加一个session.finalize的API，由调用方显式结束当前session。

Every gate below checks one of these properties for tail streaming:

- **Correct AST.** After `finalize`, the AST a session returns is the AST a
  fresh parse of the same text returns. Before it, each snapshot is the AST a
  fresh parse of the text so far returns, except where a definition lookup
  would see text written after the node that asks (plan 5.5), and it does not
  depend on how the text was split into appends.
- **Identifier.** A node's identifier is unique, keeps naming the same node
  across steps, and is never handed to a different node.
- **Minimal AST mutation.** A node is a new value exactly when its value
  changed.
- **Minimal re-parse.** The Ir of an append does not grow with the text
  already streamed, and no step costs more than parsing the whole text again.
  An edit re-reads only the damaged block and the sibling before it (plan
  12.3, 4.12).

The existing one-shot benchmark (`packages/markdown-core/benchmarks/README.md`)
answers "what does a parse cost". The gate therefore has the one-shot and
stream benchmarks and one correctness suite.
A step of the rollout is accepted only when each passes at the level section
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
  runs every step shows that the code keeps them.
- The complexity bounds of the plan's section 7.1 are the reason for the whole
  design. A bound that is first measured after the implementation is complete
  is a bound nobody designed against.

The gates also fix the vocabulary before the code exists: what a script is,
what "unchanged" means observably, what is counted. Each later step then
changes the subject under test, not the harness.

## 2. Subjects and the baseline

The harness drives a **subject** through one interface: open a session in a
coordinate unit with an initial text, call `append` and, from step 7,
`finalize`, then from step E1 `edit`, each of which returns the new document,
and close. There are three subjects:

| Subject | What it does | Role |
| --- | --- | --- |
| `session` | The engine's `MarkdownSession` (plan 4.4) | Under test, from rollout step 2 |
| `reparse` | Keeps the text, and on every step calls `Document.parse` on the whole of it | The oracle for correctness, the R column for benchmarks |
| `binding` | Steps the `session` subject's session through the MCB3 entries (plan 6.2) and releases each step's message | The B column of the stream benchmark, from rollout step 7 (5.1) |

`reparse` is the one parser, called the way an application calls it today. It
is the equivalence oracle and the reference cost an incremental append must
beat. Until the session API exists the harness runs `reparse` alone, which
already produces every baseline number and exercises the scripts, the text
model and the reports.

The harness keeps its own **text model**: a plain byte buffer to which it
applies every script step, independent of the subject. The subject's text
must equal it after every step.

Until step E1, the edit scripts run on `reparse`. It numbers every parse from
1, so it keeps no lineage, and the runner checks 4.1, 4.7 and 4.13 on it and
skips 4.2 to 4.5. The faulty subjects of section 8 prove that the edit
oracles can fail.

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
  (the corpus's word generator, both alphabets). Shapes:
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
  fence, HTML block, comment and directive on the first line); a
  10,000-item list; 1,000 nested block quotes; one definition with 10,000
  references; a heading label repeated so that anchor suffixes shift; an
  unclosed `**` early in a paragraph that is then streamed for 64 KB; a
  single-line paragraph streamed to 64 KB. Step 7 adds the shapes
  that streaming meets: a definition written after 10,000 of its uses, a
  document that opens a Properties envelope and never closes it, and an
  unclosed grid table. Step 8 adds one paragraph of short lines streamed to
  64 KB, whose inline content the settle point keeps from being parsed again
  per append (plan 5.8).

A composite document that joins parts records, for each part, the number of
root children a fresh parse of the part alone produces. The generator checks
that the composite's fresh parse has their sum, so a part cannot silently
change the meaning of its neighbour.

### 3.2 Stream scripts

A stream script appends the whole of a document, starting from an empty
session, in chunks, and from step 7 ends with `finalize`. Every chunk is
whole scalars, on every platform.

| Family | Chunking | Documents |
| --- | --- | --- |
| `tokens` | A fixed pseudo-random sequence of chunk sizes from 1 to 16 bytes, mean 4, each moved to the next scalar boundary | Scale families, grammar corpus |
| `scalars` | One scalar per chunk | Grammar corpus |
| `rows` | One physical line per chunk | Scale families |
| `splits` | Two chunks, split at every scalar boundary | Grammar corpus documents up to 2 KB |

### 3.3 Edit scripts

An edit script opens a session on its document, finalizes it, and then
edits it (plan 12.3). An edit is `edit(start, end, text)`. Its offsets are
UTF-8 bytes of the text before the edit, and they fall on scalar boundaries.
A runner in UTF-16 converts them through the text model, so the same script
tests both units. A session's edit takes cursors, and no cursor stands
between the CR and the LF of a CRLF: the runner edits from before the CR,
writing it back, an edit that starts there, and to after the LF, writing
it back, one that ends there, and 4.4's images follow that edit. A session
takes one edit per call, so the multi-cursor
`batch` family of the edit engine that step 5 removed is not restored.

| Family | Edits | Positions |
| --- | --- | --- |
| `typing` | One scalar per edit, 64 edits, with runs of backspace | Inside a paragraph, a list item at depth 3, a pipe table cell, a heading, a code block, a quote leaf, a footnote body |
| `lines` | Enter inside a paragraph; insert and delete a blank line between blocks; join two lines of a paragraph; merge two paragraphs | Beginning, middle and end of the document |
| `markers` | Add and remove `> `, `- `, `1. `, `# `, four spaces, a fence opener, a Setext underline, a table delimiter row, a definition term marker | Each block kind of the shape |
| `ranges` | Paste a 2 KB section; delete a section; select a paragraph's whole text and type a replacement | Middle of the document |
| `far` | Alternate single-scalar edits at the end of the first and the last text, 64 edits | Both ends |
| `declarations` | Change a reference destination; add and remove a duplicate reference label; add and remove a heading whose label collides; add and remove a footnote definition and an inline note | Declaration sites of `refs` and `prose` |
| `undo` | Each edit of `typing` to `declarations` followed by its inverse and then by itself again | As the original edit |
| `random` | Seeded mixture of inserts, deletes and replacements at line and byte granularity, including CR/LF splits and NUL, 16 seeds of 64 edits | Uniform over the text |
| `hard` | One script per hard case of plan 12.7: fences, comments, HTML blocks and directives opened and closed; a candidate opened before unchanged children; `>`, list markers and indentation typed and deleted; references, footnotes, heading labels and specimens added, renamed and removed; a heading with an automatic anchor; split and joined paragraphs and items; the rules that write backwards; the Properties envelope; a loose list made tight and an early item made loose in a long list | Small documents written for the case |

Every document of the correctness set gets every family but `hard`.

## 4. Correctness oracles

Every oracle is checked after every step of every script that runs it
(section 9). The oracles are stated against the public model, not against the
engine's internals, so they hold for any correct implementation and fail for
any incorrect one.

### 4.1 Equivalence

The subject's text equals the text model, and the canonical dump of the
subject's document equals the dump of a fresh `Document.parse` of the model
text in the same unit. The dump prints no ids and prints absolute scopes, so
it compares meaning and positions and nothing that depends on history. From
step 7 this oracle holds after `finalize`, and each snapshot before it is
checked by 4.6, because a snapshot's lookups see only what is written before
the node that asks (plan 5.5).

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

`Document.footnotes`, `Document.specimens`, `Document.references`,
`footnote(for:)`, `specimen(for:)` and `reference(for:)` for every label in
the text return the nodes corresponding to the fresh parse's answers, in the
same order (plan 8). The labels include duplicate reference labels and
reference labels that a heading's text also declares, so the winner of each
is compared.

### 4.2 Identifier

- Ids are unique within the document, across every owned relation.
- Over the whole lineage, the harness keeps a map from id to kind and a set of
  retired ids. An id never changes kind, and a retired id never appears again.
- A fresh parse numbers its nodes from 1 in canonical walk order (plan 4.1).
  Its ids are 1 through its node count, and two fresh parses of the same text
  are equal, ids included.

### 4.3 Minimal AST mutation

For every node of the new document, the harness classifies it against the
node of the previous document with the same id: **unchanged** when the two are
deep equal (plan 4.2), and **changed** when they differ or no previous node has
that id. `N` is the number of changed nodes. An ancestor of a changed node is
changed, because its children are part of its value.

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
terms the harness evaluates from the public model alone: the absolute scopes
of the snapshot and of `scope(of:in:)` on the new document, kinds, values and
owner relations. An append moves no byte, so a position of the old text is the
same position of the new one. An edit moves a byte by its **image**: a byte
before the edit keeps its offset, a byte after it moves by the edit's change
in length, and a byte it replaced has no image (plan 12.5). The harness
computes the expected id itself for every node of every step:

- The new document continues the old document.
- An old node's anchor is the first byte of its source that the step kept,
  at its image. A node with no source bytes, or whose bytes the edit
  replaced, has no anchor.
- Within the relation of a matched owner, a new node of the same kind whose
  source range contains an old sibling's anchor continues the earliest such
  sibling. A node's source range runs from the start of its first scope to
  the end of its last.
- A new node that continues an old node has the old node's id.
- Every other new node has an id the lineage has never seen. An old node
  that nothing continues is retired.

A continued node whose value equals its predecessor's is unchanged and outside
`N` (4.3). Extents are relative, so text that moves a node without touching it
leaves its value unchanged, except that a continued node that follows a
changed or inserted sibling in the same relation may get a new `lead`. The oracle predicts that `lead` from the fresh parse.

### 4.5 Scripted identity

Some steps have an exact expected outcome, written into the script as the sets
of kept, new and retired ids by position. The stream cases are the
consequences plan 5.9 and 8 list: typing at the end of a paragraph keeps its
id; a paragraph that becomes a Setext heading and then a paragraph again
takes a new id each time; a row appended to a table changes only the table,
the new row and the Document; an item appended to a long list changes only
the list, the new item and the Document.

The edit cases, the `ids` family, are those the edit engine that step 5
removed held, with one edit per step: typing at the start of a paragraph
keeps its id; inserting `new\n\n` before a paragraph gives the new one a new
id and keeps the old one's; deleting a first word keeps the id; deleting a
sibling retires its id and does not hand it to the next; merging two
paragraphs keeps the first id; a paragraph that becomes a Setext heading is
a new node; a paragraph moved into a new quote is a new node; deleting a
nested inline note's outer closer makes the inner note a new node, and
deleting the outer opener then keeps it; inserting a line at the top of a
long document changes only the Document and the edited paragraph; changing
a heading anchor that links target changes only the Document and the
heading; typing in paragraph 5 of 1,000 changes exactly that paragraph, its
Text node and the Document.

### 4.6 Streaming

Until step 7 every append parses the whole text again, so after every chunk
4.1–4.4 hold against the model text, which is everything appended so far.
From step 7, after every chunk:

- **Chunking independence.** The snapshot equals the snapshot a new session
  returns for one append of the whole model text, ids aside: the dump,
  every scope and every definition query.
- **Snapshot meaning.** The snapshot equals a fresh `Document.parse` of the
  model text everywhere except at the places plan 5.5 allows. The harness
  computes them from the fresh parse alone, with its definition tables,
  anchors and scopes: every inline root that names a label whose definition
  starts after the root, and, in turn, every heading whose text holds such a
  root, every root that names that heading's label, and every heading whose
  automatic anchor an anchor in those places can change. Outside those
  places the dumps agree node for node.
- 4.2–4.4 hold against the previous snapshot.

After `finalize`, 4.1–4.4 hold against the last snapshot, and every later
`append` or `finalize` fails with the finalized status (4.7).

### 4.7 Errors

From step 7, an `append` or `finalize` after `finalize` fails with the
finalized status (plan 4.4). In C,
the allocator-seam sweep of plan 8 fails a sample of steps at every
allocation boundary; each call throws the out-of-memory error, and freeing
the session leaks nothing.

### 4.8 Deep trees

Every binding runs the plan's deep-tree set through a session: release of a
deep document while a newer version is alive, equality of two deep documents
that differ at the deepest leaf, walking, scope lookup, hit testing and
`description`, on a thread with a small fixed stack.

### 4.9 Platforms and units

C and every binding run the same scripts in both units. A binding checks 4.1
against its own dumper, as conformance does today, and 4.2–4.5 on ids and
values.

### 4.10 Fuzzing

A fuzz target decodes its input as a document and the appends that follow it
and checks 4.1–4.4 and 4.7 after every step. The current fuzz corpus seeds the document half.
From step E1 the input also decodes edits after the appends.

### 4.11 Edits

After every edit, 4.1 holds against a fresh parse of the model text, the
dump, every scope, the queries and the definition tables, and 4.2 to 4.5
hold against the document before the edit, with 4.4's anchors at their
image. An edit after `finalize` that names a range of the text succeeds.

### 4.12 Damage bound

A subject that states a damage bound reports, after each edit, the bytes of
the new text it read again. They hold the bytes the edit wrote and lie in
the text. A script states the bound of an edit whose re-read plan 12.3 fixes:
exactly (`reread FROM TO`), from the start of the damaged block's previous
sibling, or its own start when it has none, to the start of the first block
after the edit that the new reading shares with the old one; or as an upper
bound (`within FROM TO`), where the plan fixes the blocks around the edit but
not which of them is the damaged one. A regression that re-reads a whole
container fails either. `reparse` reads the whole text, so it states no
bound.

### 4.13 Edit errors

An edit while the session streams fails with
`MARKDOWN_CORE_SESSION_STREAMING` (plan 12.3). After `finalize`, an edit
whose range is reversed or ends past the text, or has an offset that is no
scalar boundary in the session's unit, fails with the out-of-bounds status:
in UTF-8 an offset at a continuation byte, in UTF-16 one between the two
units of a scalar. A refused edit answers no document and leaves the text as
it was, and the edit after it applies to that text.

## 5. Measurement

### 5.1 What is measured

The runners follow the one-shot benchmark's rules: callgrind instruction and
data-reference counts, the benchmark preset, the same build provenance and
isolation, and a measured edge instead of a timer. The stream runner loads a
document and a stream family, opens the subject on an empty text outside the
measurement, calls `bench_apply_step` once per chunk and then
`bench_finalize` once. For an edit workload it loads a document and its edit
scripts, and for each script opens the subject on the document and
finalizes outside the measurement, then calls `bench_apply_step` once per
edit.

Costs come from callgrind client requests (`CALLGRIND_DUMP_STATS`), which are
no-ops outside valgrind. The runner dumps after each of at most 1,024
**windows** of consecutive steps. A stream of at most 1,024 steps has one
step per window, so a window's cost is that step's cost. A longer stream (a streamed megabyte in
`tokens` or `rows`) splits into 1,024 contiguous windows of equal step count.
Its `reparse` column is measured at each window's last step, because
reparsing after every chunk of a megabyte stream is the quadratic cost the
design removes. A stream 6.3 measures at every step has one step per window
however long it is (decision G17). The finalize is one more window, after
the last. A session's
window counts the whole of its steps: an append is the session's streaming
parse. A `reparse` window counts the parse of each `Document.parse` in it,
its two stages `source_to_buffer` and `buffer_to_ast` as the one-shot
benchmark reads them. Creating the parser, starting and releasing its
transaction and freeing the document of the step before are not parsing, and
are in no window.

An edit script splits into windows the same way, each script on its own,
with no finalize window. The grammar corpus documents and each shape at its
smallest size run every edit family but `undo` (one `random` seed), one edit
per window, and a shape's larger sizes run `typing` and `far` in at most four
windows a script, because each `reparse` edit there parses up to a megabyte.
A `reparse` window counts the parse of its last edit, a fresh parse of that
edit's text; the edits before it in the window only replace the text. Until
step E1 only `reparse` takes edits, and edits are reported without a gate.

The gates measure the C engine. A binding builds its platform values from what
the engine returns (plan 6.1); bindings are gated for correctness (section 4).
A `binding` window counts what its messages add to the session's steps: the
encode of each document the window publishes and the release of its message.
That is the part of a binding's build that reads the C document, which Kotlin
and ECMAScript run per append and whose facade reads Swift's builder makes;
building the platform objects is not counted (G14). The benchmark runs it on
the streams of at most 1,024 chunks, and reports it without a gate.

### 5.2 Metrics per workload

| Metric | Definition |
| --- | --- |
| `step_ir` | Ir per window: p50, p95, max and total over the stream |
| `reparse_ir` | The same for the `reparse` subject: the parse an application runs today (5.1) |
| `binding_ir` | The same for the `binding` subject: the C side of a binding's build per append (5.1) |
| `oneshot_ir` | One-shot parse of the stream's final text, both stages |
| `speedup` | `reparse_ir / step_ir` per step, p50 and p5, for the streams 6.3 measures |
| `stream_ratio` | Stream total `step_ir` / `oneshot_ir` of the final text |

## 6. Gate rules

A rule is either a bound derived from the design or a regression limit
against the base revision. No rule is a threshold on a measured number that
happened to look good.

### 6.1 Correctness

Every oracle of section 4 passes on every step of every script that runs it.
There is no tolerance.

### 6.2 Flatness, on Ir

For `tokens` and `rows` on every scale shape, the rule compares positions
within one stream of `n` bytes. With `m(p)` the highest cost of every chunk window that ends at or before `p`, each
of `m(n/4)` and `m(n)` is at most `1.25 × m(n/16)`, which catches per-chunk
work that grows with the text already streamed, wherever in the stream it
occurs. The finalize's window is reported (decision G16).

The same rule covers the adversarial shapes whose cost the language keeps
local: the 10,000-item list and the 1,000 nested quotes. It excludes the
shapes whose cost plan 7.2 assigns to the language (the stray opener, the
Properties envelope left open, the unclosed grid table, the unclosed `**`, the
single-line paragraph) and the definition with 10,000 references, whose
`finalize` parses every root that uses it again. Those are bounded by 6.3.
From step 7 the stream rule covers the grid and multiline tables of the
`table` shape only up to their closing border, as plan 7.2 says, and from
step 8 it covers the paragraph of short lines (3.1).

### 6.3 Never worse than reparsing

For every stream of at most 1,024 chunks, and for every stream of a shape
6.2 leaves to 6.3 at the shape's smallest size, measured at every step
whatever its length (decision G17), the total `step_ir` of its steps and its
`finalize` is at most 1.25 times the total `reparse_ir` of the same steps
(decisions G2 and G15). The highest ratio of one step is reported. Of every
other longer stream, `reparse_ir` is reported, and 6.2 bounds the chunk
windows of those of the scale shapes and of the shapes the language keeps
local. The
margin pays for matching when an append really does change the whole
document; beyond it, streaming would be a regression against the application
that just reparses.

### 6.4 Regressions

Against the base revision, measured with the current harness and workloads on
both sides as the one-shot gate already does, and always between the same
subject on both sides:

- each workload's p95, maximum and total `reparse_ir`, and `step_ir` on the
  streams 6.3 measures, are at most 1.02 times the base;
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
| 1 Model | 4.2 for fresh parses; deep equality and 4.8 on fresh documents | One-shot budget for the model change (G1), then 1.02 per PR |
| 2 Sessions, whole-document restart | 4.1–4.10 on the correctness set, every platform, both units | 6.3 on every workload, which sets the session baseline for 6.4 (G4) |
| 3 Reference nodes, pieces and content runs | Unchanged | One-shot budget for the step (G5), then 1.02 per PR |
| 4 Node runs | Unchanged | 6.3 on every workload; one-shot budget for the step (G10), then 1.02 per PR |
| 5 No edits | The edit scripts, the edit cases of 4.5, the batch oracle and the range errors leave with `edit` (G11) | The edit benchmark leaves; 6.3 on every stream; 6.4 at 1.02 |
| 6 Builders | Unchanged | As step 5 |
| 7 Streaming session | 4.6 and 4.7 for streams and `finalize`, every platform, both units; 4.1 after `finalize`; the streaming adversarial shapes | 6.2 for `tokens` and `rows` on every scale shape and adversarial shape that 6.2 does not exclude; 6.3 and 6.4 as step 5; the binding's cost per append reported (plan D5) |
| 8 Settle point | Unchanged | 6.2 for the paragraph of short lines too |
| E0 Edit gates | The edit scripts (3.3), 4.11 and 4.13 with `reparse`; the faulty subjects of the edit oracles | The edit benchmark reports the R column |
| E1 C edit | 4.1–4.5 and 4.11–4.13 on the edit scripts with the session, both units; edits in the fuzz input | Each edit's Ir beside a fresh parse of its text; the parse and stream gates stay at 1.02 |

From step 2 on, the one-shot benchmark measures `Document.parse` through the
session path it becomes (plan 4.4), so the one-shot gate also guards what the
session machinery costs a fresh parse. From step 6 that path is a commit and a
`finalize` (plan 5.10).

## 8. The harness proves it can fail

A gate that has never failed proves nothing. The harness's own tests run each
oracle against a deliberately wrong subject and require it to fail with the
named oracle:

| Faulty subject | Must fail |
| --- | --- |
| Returns the previous document for one step | 4.1 |
| Answers `reference(for:)` with the last declaration of a label instead of the first | 4.1 |
| Renumbers every id on every step (a fresh parse with fresh ids) | 4.2 lineage, 4.4, 4.5 |
| Keeps a node whose text changed | 4.1 |
| Reuses a retired id for a new node | 4.2 |
| Re-reads the whole document on every step | 6.2 |
| From step 7: answers a snapshot's lookup from a definition that arrived in an earlier append, wherever it was written | 4.6 chunking independence |
| From step 7: accepts an `append` after `finalize` | 4.7 |
| From step E0: renumbers every id on an edit | 4.4 |
| From step E0: answers an edit with the document before it | 4.1 |
| From step E0: re-reads the whole text on an edit while it states a bound | 4.12 |
| From step E0: takes an edit while it streams | 4.13 |
| From step E0: cuts an edit that names no range of the text to the text | 4.13 |

The edit faults wrap the session, which takes no edits before step E1: their
edit opens a new session on the new text and finalizes it. Step E1 adds the
faults plan 12.7 lists, which only an engine that re-reads part of the text
can have, such as a resync that skips the state check.
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
It holds every stream family, `splits` on every document up to 2 KB, every
edit family of 3.3, the scripted-identity cases of 4.5 and the refused edits
of 4.13. Streams are rules over the documents and scripts are short texts,
so the set stays small enough to track.

- C runs it as `incremental_runner` under the `incremental` label, with
  4.1–4.8 and 4.11–4.13: one test per family the set's manifest declares, run
  in parallel, with a long family dealt into shards of one test each. Until
  step E1 the edit families run with `--subject reparse`.
- Swift, Kotlin and ECMAScript consume it through the same lifecycle that
  delivers the canonical manifest to their conformance runners, in both units,
  with 4.1–4.9 except the allocator sweep of 4.7.
- The fuzz target of 4.10 covers what a fixed set cannot.

**The benchmark workloads** are the generator's large output, built when the
benchmark runs, as the grammar corpus is: every grammar corpus document with
`tokens` and `scalars`, and every scale and adversarial shape at all four
sizes with `tokens` and `rows`; and the edit workloads of 5.1, a document and
its edit scripts each.

The benchmarks run as `pnpm benchmark:streams`, sharing `run.mjs`'s build,
provenance, isolation and callgrind code, and with `--baseline-ref` they
rebuild the base revision's engine with the current harness for 6.4. Outputs
go to `build/benchmark-streams`: `streams.md`, `streams.json`, the generated
workloads and the raw profiles. CI runs a job, "Measure - streams", in the
benchmark workflow against the pull request's base, and the trusted publisher
adds its tables to the existing PR comment. The report shows, per workload,
the S column (session), the R column (reparse), the highest S / R of one step,
the workloads that pass 6.4, the B column (binding) beside S, and the edits'
R column by family and size.

## 10. Work items

Step 0 was these items. Later steps activate the gates as section 7 says, in
their own pull requests.

- [x] The workload generator: documents, edit and stream scripts, the
      composite-part check (3.1), and its `node --test` suite.
- [x] The tracked correctness set under `specs/incremental/` and its
      `--check` audit.
- [x] The C `incremental_runner` with the text model, the `reparse` subject,
      the oracles that apply before sessions exist, and the faulty subjects
      of section 8 that wrap `reparse`.
- [x] The edit and stream benchmark runner, `pnpm benchmark:edits`, reporting
      the R column, with driver tests on synthetic profiles.
- [x] The `buffer_to_ast` regression rule in the one-shot gate (6.4, G1).
- [x] The "Measure - edits and streams" CI job and its tables in the PR
      comment.

Step E0 (plan 12.9) is these items:

- [x] The edit scripts of 3.3 in the generator and the correctness set, with
      the `ids`, `hard` and `rejections` families.
- [x] The harness's `edit` step and the damage bound of its subjects, the
      `reparse` subject's edit, and oracles 4.11 to 4.13 with 4.4's image.
- [x] The edit faults of section 8.
- [x] The edit benchmark: each edit's Ir for `reparse`, reported.

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
- **G5 One-shot budget for step 3. Decided 2026-10-05: 1.10.** Step 3 makes
  every parse record Reference nodes, the pieces of leaf blocks in
  containers and the content runs of inline roots. Its pull request may raise
  `source_to_buffer` and `buffer_to_ast` Ir up to 1.10 times the pre-step
  baseline per document. After step 3, both stages are at 1.02 per pull
  request.
- **G6 `flat` with the registries. Decided 2026-10-01: move to the session
  registries step.**
  `flat` has a heading in every section, and headings are declarations, so
  its 6.2 gate turned on with the session registries beside `prose`, `quote`
  and `refs`. Revised 2026-10-06: `flat` and every other 6.2 gate turn on in
  step 8 (G9). Superseded 2026-10-14 with the general-edit engine (G11).
- **G7 Ids of nodes taken into a new owner. Decided 2026-10-04: they keep
  their ids.** The general-edit engine's cursor found old nodes by position,
  so a list split by an inserted line took the items after the split whole
  into the new list. They kept their ids there, as for every taken node
  (4.4). Superseded 2026-10-14 with that engine (G11); the oracle's
  acceptance of either id left with edits in step 5.
- **G8 Positions of the local families. Decided 2026-10-04: text the
  language keeps local.** A grid table row, a table's header or border, a
  definition referenced from every section and a fence line spread an edit
  to the whole construct. The local families place their edits in
  paragraph, list item and pipe cell text; `markers`, `declarations`
  and `random` edit those other places, under 6.1 and 6.3. Superseded
  2026-10-14 with the edit families (G11).
- **G9 Benchmarks for steps 4 to 7. Decided 2026-10-06: reported, not
  gating; a performance step follows.** Steps 4 to 7 pass every correctness
  gate, and their benchmarks are reported without blocking a pull request.
  Step 8 is one performance refactor after step 7: it turns on 6.2 for every
  local edit family, `tokens` and `rows`, 6.3, and 6.4 for the one-shot and
  session subjects against the revision before step 4. After step 8, 6.4 is
  at 1.02 per pull request. Superseded 2026-10-14 (G11): each step of the
  streaming rollout meets the rules section 7 gives it.
- **G10 One-shot budget for step 4. Decided 2026-10-14: 1.10** ("3给1.1的预算").
  Step 4 makes every node, inline nodes included, carry runs of its own
  source. Its pull request may raise `source_to_buffer` and `buffer_to_ast`
  Ir up to 1.10 times the pre-step baseline per document. After step 4, both
  stages are at 1.02 per pull request.
- **G11 Streaming only. Decided 2026-10-14** (plan D7, "Edit删掉"). The edit
  scripts, the edit benchmark and the oracles and rules only edits use leave
  with `edit` in step 5. The stream gates of 4.6 and 6.2 turn on with the
  streaming session in step 7 and the settle point in step 8 (section 7).
- **G12 Parser creation. Decided 2026-10-17: in no window** ("不计建实例").
  `Document.parse` creates a parser on every call, so the `reparse` subject
  creates one on every step. Creating a parser is not parsing, and no window
  of either subject counts it, on either side of 6.4 (5.1).
- **G13 Reparse windows. Decided 2026-10-18: parsing only** ("只算解析"). A
  `reparse` window counts the two parse stages of each `Document.parse`, as
  the one-shot gate does; creating and releasing the parser are outside
  every window, on either side of 6.4 (5.1).
- **G14 A binding's cost per append. Decided 2026-10-19: the C side in Ir**
  ("只计 C 侧 Ir"). Callgrind measures the part of a binding's build that
  reads the C document, in the gates' unit on the gates' machine; building
  the platform objects is not counted. It is reported per append, without a
  gate (plan D5).
- **G15 6.3 over the whole stream. Decided 2026-10-19: the stream's total**
  ("按整条流总量"). What streaming must give is parsing far better than
  reparsing on every append, above all in the common LLM case
  ("streaming的解析性能要远好于每次append重复解析，尤其是LLM的常见场景").
  6.3 bounds each stream's session total by its reparse total, and the
  highest ratio of one step is reported.
- **G16 The finalize under 6.2. Decided 2026-10-18: the compromise**
  ("收尾窗口是我们的妥协，ref/link backtracking导致不这么做性能更差"). The
  finalize looks up the definitions that the stream's roots use, because
  going back over references and links while streaming costs more. 6.2
  compares the chunk windows of a stream, and the finalize's window is
  reported.
- **G17 The long streams of the shapes 6.2 leaves to 6.3. Decided
  2026-10-20: every step** ("逐段都量"). Most streams of those shapes are
  longer than 1,024 chunks. 6.3 measures each of them at the shape's smallest
  size at every step, the session and `reparse` alike, and bounds its total
  as it bounds a shorter stream's (5.1).
