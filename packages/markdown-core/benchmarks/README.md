# Parse stage benchmark

What it costs Markdown Core to parse a document, stage by stage, next to what
it costs a reference implementation to parse the same bytes. The comparison
exists to give an optimization somewhere to argue from: a claim that a change
made the parser faster should name a stage and a number.

The reference grammar and comparison contract are part of every measurement.
cmark supplies CommonMark controls and cmark-gfm supplies GFM controls. For
dialect constructs, [structurally proved pairs](#isomorph-pairs) preserve the
declared ownership/field correspondence; their A/R ratios do not establish equal
optimal parsing effort. Cases historically labelled **bound** are unmatched
descriptive controls, not mathematical bounds on an optimum. A production that
decorates a host rather than standing on its own
— an attribute list — is measured as a [split](#splitting-a-corpus-the-remainder-inside-its-hosts): its host is
compared without it, and its own cost is read in place, in every host the
grammar gives it, as the difference between two whole documents, against the
same grammar decoding it alone.

```sh
scripts/init-environment.sh --install oracle-cmark oracle-cmark-gfm   # once
node scripts/benchmark-stages.mjs
```

The driver builds all three engines, runs them under callgrind, and writes
`build/benchmark-stages/stages.md`, `stages.json`, and the raw dumps.

It also measures six [certified local operation contracts](../../../docs/architecture/benchmark-effort-boundaries.md)
and writes `build/benchmark-stages/effort/effort.{md,json}`, exact input bytes and
raw dumps. Both native implementations receive the same canonical entry state;
descriptor adapters count inside the operation, and preparation/release are
reported separately. VT/FF classification mismatches are split explicitly.
These local certificates do not upgrade the 43 unproved full-parser optima or
provide a coverage percentage of their parse costs.

An existing artifact can also be aggregated without rerunning the experiment:

```sh
node scripts/report-performance.mjs build/benchmark-stages build/performance-census
```

For an artifact measured with `--baseline-ref`, select its baseline explicitly:

```sh
node scripts/report-performance.mjs build/benchmark-stages build/baseline-census --baseline
```

The input remains the complete artifact directory. Baseline Core profiles live
under `baseline/`, while the reference profiles belong to the whole experiment
and are measured once. The reporter checks the paired reports' revision,
document/proof identity, runtime, reference binaries and shared measurements
before combining them; it does not search other directories for missing data.

This writes a Markdown census and a JSON accounting ledger: the same-input
reference cohort excludes unmatched fields; proved domains and reviewed
boundaries remain separate. It accepts `--case` subset artifacts and omits
contracts whose required documents were not measured. Whole-program self and
allocator self include the harness and must not be subtracted from parse-edge
costs. The complete parse lifecycle edge includes document teardown, while the
two stages below do not.

## The two stages

A parse has two paths worth optimizing separately, and they have different
shapes: one is a scan over bytes, the other is tree construction over the
buffers that scan produced.

| Stage | Markdown Core | cmark |
| --- | --- | --- |
| `source_to_buffer` | `markdown_core_parse_document_with_setup` → `S_parse_source` | `cmark_parser_feed` |
| `buffer_to_ast` | `markdown_core_parse_document_with_setup` → `S_finish_parse` | `cmark_parser_finish` |

cmark splits exactly these two paths across two public calls, so feeding the
whole document and then finishing gives a boundary that *is* the boundary
rather than an approximation of one. Markdown Core has no such API and does not
grow one for this: it parses one complete buffer per call, and the stage split
is read afterwards out of the recorded call graph.

Parser allocation, dialect attachment, element discovery, and tree release are
outside both stages. Their scaling differs: setup has a per-transaction cost,
while tree release scales with the owned nodes and values. The complete
parse-path count includes both; keeping it alongside the stages makes those
lifecycle costs visible. Empty-input allocation counts in the C API tests
cover the setup boundary separately.

What that exclusion costs is itself measured rather than asserted: the report
states the share of each engine's whole parse path the two stages cover. A
split that quietly stops covering the parse shows up as a growing remainder
instead of not showing up at all.

`S_parse_source` also runs nested inside `buffer_to_ast`, where mapped cell
inputs are read back through the ordinary block parser. That work belongs to
the AST stage and is counted there, because that is where the transaction
spends it.

That one function being entered from two stages is why the measurement is
shaped the way it is. Stage totals come from call *edges*, not per-function
inclusive totals, which would count the nested run under both stages at once.
The per-stage callee breakdowns need more than that: callgrind keys a profile
by function, so both entries share one node and one set of outgoing edges. The
driver runs with `--separate-callers=1`, which gives each context its own node,
and reads the breakdown from the node this stage actually entered. The driver
also refuses any breakdown entry that costs more than the stage containing it —
a part cannot exceed its whole, and that is precisely what a context-merged
breakdown reports.

## Why callgrind and not a clock

Instruction and data-reference counts do not depend on how fast the machine was
or what else was running on it, so a hosted runner is as good a place to
measure as a quiet laptop and a 2% difference is a real 2% rather than a
neighbour's build. The wall-clock pipeline this replaced could never make that
claim, which is why it could only ever be informational. What the counts *are*
a property of is set out below.

The counts are not time. Ir does not price a cache miss, a branch miss, or a
dependency stall, so a change that trades three instructions for one random
memory access improves the number without improving the parser. Dr and Dw are
reported next to it for that reason, and neither is a merge gate.

## The profile build

Both engines are compiled by the same compiler with the same flags, pinned in
the `benchmark` preset in `CMakePresets.json` and passed on to the cmark build
by the driver. The flags are Release plus two additions:

- `-g`, so callgrind can name what it measured;
- `-fno-inline-functions-called-once`, because `S_finish_parse` is called from
  exactly one place and is otherwise folded into its caller, which does not
  make the AST stage cheaper — it makes it unmeasurable;
- `-fvisibility=hidden`, because cmark sets it for its own build and Markdown
  Core's static objects do not. An engine compiled without it pays for
  indirection the other avoids: adding it moved Markdown Core's source stage by
  2.1% and its AST stage by 5.9% on `mixed-commonmark`, with cmark unchanged.
  That is a build difference sitting inside a number meant to be about parsers,
  and it is larger than most changes anyone would bring this benchmark to judge.

The report does not stop there and claim the two compile lines match, because
they do not. It reads every object of each linked engine out of that tree's
`compile_commands.json` and splits the
options into what both engines got and what only one of them did.

Every object, not the one holding the stage boundaries: a stage's cost is
inclusive, so it contains whatever the scanners and inline code did, and CMake
lets a single source carry its own options. `elements/CMakeLists.txt` gives ten
scanner sources `-Wno-unused-variable` through `set_source_files_properties`,
which is why Markdown Core's objects have two distinct compile lines and not
one. The report names the object count and the number of distinct lines, and
digests the whole per-file set, so a per-source option anywhere in an engine
moves the identity. Naming the target matters too, since Markdown Core compiles
`blocks.c` twice — into the shared library and into the static one the runner
links — and only the second is measured.

That split is what to read, and the report does not tell you it is harmless.
Warning flags cannot reach code generation. A define can: a `*_STATIC_DEFINE` is
a visibility switch, which is the same mechanism that made `-fvisibility=hidden`
worth 5.9% above. So a ratio here is a fact about the two parsers only as far as
those two rows are inert, and judging that is left to whoever reads the report
rather than asserted on their behalf.

What the driver does guarantee is the pinned set: it fails if either engine's
real compile line is missing any of it.

Nothing in the product is arranged for this. The engine has no measurement
mode, no phase hooks, and no benchmark-only code path; the profiling flavour
only keeps the boundaries that already exist from being optimized out of the
symbol table. The driver checks for them after the build and fails loudly
rather than reporting a missing stage as a cheap one.

## What the measurement is insulated from

The measured process gets an environment that is built rather than inherited: a
path, a home, a temporary directory, and the C locale. Nothing else is passed.

The environment reaches inside a measurement through several layers at once —
the loader reads `LD_PRELOAD` at exec time, glibc reads `GLIBC_TUNABLES` and
`MALLOC_PERTURB_` when it allocates, libc reads the locale when it classifies a
byte — and the stages allocate and call libc constantly, so none of it is a
rounding difference. Against one case's 35,066,966 Ir:

| exported | summary Ir |
| --- | ---: |
| `MALLOC_PERTURB_=42` | 51,807,936 |
| `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` | 35,101,488 |
| `LC_ALL=en_US.UTF-8` | 35,067,533 |

Hence an allowlist and not a list of variables to remove: a denylist has to name
every mechanism that can reach in, those are three of them in three different
layers, and the next one would be admitted silently. The locale is *set* rather
than dropped, because there is no "no locale" — libc falls back to C either way,
so naming it makes the measurement state its locale instead of depending on the
caller not having one.

They are excluded rather than recorded because what the counts should describe
is the pinned build parsing the corpus and not what the surrounding shell
arranged around it.

The build gets the same treatment, and for a sharper reason than the
measurement did: a compiler reads more than its command line. `CPATH` and
`C_INCLUDE_PATH` add include directories that appear on no compile line at all,
so a header can be swapped underneath a build while `compile_commands.json` —
where this report reads the engines' real options — shows character-for-character
the same command. The configure and build steps therefore run with `PATH`,
`HOME`, `TMPDIR`, the C locale, and `CFLAGS`/`LDFLAGS`, which the identity
deliberately honours and records. Nothing else reaches them.

The profiler's own configuration is isolated for the same reason. Valgrind takes
options from `~/.valgrindrc`, then `VALGRIND_OPTS`, then `./.valgrindrc`, before
its command line — so every option the driver does not pass explicitly is the
caller's to set, and those are exactly the ones that decide what gets counted. A
`~/.valgrindrc` holding `--collect-atstart=no` takes a measurement's summary to
**0** while the identity table stays identical. `VALGRIND_OPTS` is gone with
everything else unnamed; the two rc files are reached through `HOME` and the
working directory, so both point at an empty directory the driver owns.

The cmark side is linked from the static archive the driver just built,
with the library lookup constrained to static suffixes: CMake searches `.so`
ahead of `.a`, and configuring with `BUILD_SHARED_LIBS=OFF` does not remove what
an earlier shared build left in the tree. The oracle checkout is also required
to be free of untracked files, not merely of modified tracked ones — a stray
`src/config.h` is neither tracked nor ignored, and the source directory precedes
the build directory on the include path.

## The corpus

`corpus.json` names the documents. The CommonMark samples come from cmark's own
benchmark corpus, so those cases are input both engines were written against.
The rest are repository syntax written for this corpus, and their cases are
marked `extended`: Markdown Core is doing strictly more recognition work there,
so the number against cmark is a bound rather than a comparison.

A case marked `gfm` is measured against cmark-gfm as a same-job comparison, so
its sample must hold **only** syntax cmark-gfm implements. That is a judgement
about content rather than something the audit can check — inline notes and
custom task states build the same node kinds as the GFM constructs they are
spelled like, so a kind-level rule would catch the table caption and miss the
other two. Three samples got it wrong and were split: the `Table:` caption left
`block-table-pipe` for `block-table-caption`, the `^[...]` inline notes left
`inline-footnote` for `inline-footnote-inline`, and the `[?]` task state left
`block-tasklist` for `block-task-states`. Each was making this parser look
better than it is, because cmark-gfm read the dialect syntax as a paragraph
while Markdown Core built a construct from it.

Splitting the caption out also showed that the pipe table has **two** grammars,
not one. `try_opening_table_header` is the GFM path — a paragraph followed by a
delimiter row — and `table_parse_pipe_header` is the mapped-source path a
caption routes the table through. The caption was what drove the second one, so
the case bound to it never ran a line of it.

Which constructs the corpus reaches is a separate question from how it is
measured, and `scripts/audit-corpus-reach.mjs` is what answers it. A construct
no document builds is not reported as fast or as slow, it is not reported at
all — and the profile still reads like the whole parser. That audit is not a
coverage gate and must not become one: it states specific facts (every node kind
the dialect names is built by some document; each grammar the corpus must
measure runs in the case that exists to drive it; every sample has a case of its
own to be profiled in; every case still builds what it exists for; every
declared pairing witness holds; every split's two halves differ by the remainder
alone) rather than a percentage to climb.

A case whose declared kinds a CommonMark construct could also build is bound to
a grammar as well, because the kinds alone prove nothing there: a plain
blockquote builds `Callout`, a plain heading is a `Heading`, and an HTML comment
builds `Comment`. Replacing `block-callout.md` with `> an ordinary quote` used
to pass; it now fails at 12.7% of the callout metadata grammar, which is the
guard-clause figure.

A `samples` case is built by repeating its samples to at least `targetBytes`,
with a blank line between repeats so that repeating cannot merge the last block
of one copy into the first block of the next. Both engines get byte-identical
files, and the driver checks each engine's receipt against the file it was
given.

A `chain` case is one structure instead of many copies: `unit` repeated until
the document reaches its size, then `tail`. That distinction is what the
scaling comparison rests on. Repeating a document grows the number of
independent blocks and nothing else — the blank line between copies exists
precisely to stop them interacting — so a container nested D deep stays at its
original D however many copies are concatenated, and work quadratic in D would
still report linear growth in bytes. A chain case makes D the size, so D
doubles when the document doubles and a cost quadratic in D shows up as a 4x
growth ratio.

Each chain case names the dimension it scales, because the shape and the
dimension are not the same thing:

| Case | Unit | Scales |
| --- | --- | --- |
| `chain-quote-depth` | `> ` | block-container nesting depth |
| `chain-list-depth` | `- ` | list nesting depth |
| `chain-emphasis-run` | `*a_ ` | live delimiter-stack depth |
| `chain-bracket-open` | `[a ` | live bracket-stack depth |
| `chain-link-candidates` | `[a](b` | count of bounded failed link candidates |

A `generated` case is built from a numbered unit instead of from a sample file,
repeated until the document reaches its size. A `counted` case is built from a
unit too, but sized to the number of units a named `generated` partner actually
emitted rather than to a byte target. Those two modes exist for the halves of a
[declaration pair](#two-ways-a-pair-is-held), where the two spellings are
different lengths and an equal byte target would not be an equal workload. The
count is taken from what the partner emitted, so nothing has to be recovered
from the generated text by pattern — an earlier version counted headings with a
regular expression, which counts what *looks* like a heading, and a `#` inside a
fenced block is not one.

`chain-link-candidates` is the one to read carefully. It looks like a bracket
chain and is not: every `[` is closed by the `]` two bytes later, so no opener
survives and the live bracket depth is one at any size. What grows is the
number of failed link candidates, each of which is separately bounded — the
destination scan gives up after 32 unbalanced parentheses
([`link.c`](../elements/link.c), `manual_scan_link_url_2`). That is a real cost
dimension and worth measuring; it is just a count, not a depth, which is why
`chain-bracket-open` exists beside it.

Every case is also measured at twice the size, and the growth table names which
dimension grew. A stage whose cost is linear in what was scaled reports a
growth ratio equal to the byte ratio; anything else is a complexity finding,
which is a correctness question rather than a tuning one.

## Structural pairs and parser effort

The [structural isomorphism contract](../../../docs/architecture/benchmark-isomorphism.md)
defines output correspondence, not equal optimal parsing effort. A structural proof
must name its language domain, reversible source transformation, abstract
constructor/field mapping and composition argument. Concrete AST layouts may
differ; equal node counts or one successful sample substitution are insufficient.

The [parser-effort model](../../../docs/architecture/benchmark-parser-effort.md)
requires the complete correctness domain, costed primitive operations and
an optimum theorem, through bidirectional cost-preserving algorithm transformations
or matching lower bounds and achievable upper bounds.
All 43 current structural proofs are **unproved** against that stronger target.
No equal-effort cohort or median is published. Equal cost for two selected
recognizers would still be weaker than equality of the theoretical optima.

The single `pairs` registry distinguishes registered proofs, completed reviews and
pending proposals. The [30-pair review](../../../docs/architecture/benchmark-pair-review.md)
reconstructs 18 entries and partitions 12 at explicit boundaries. Forty-two restricted
production languages supplement the recursive `insertion-strong-v1` proof. Original
corpora remain measured diagnostics; they do not inherit the proof of a narrower
replacement. The review documents each source language, field map and remainder.

For a proved pair, the benchmark checks generated bytes at every requested scale
against the independent domain recognizer. The reference audit compares the
complete ordered tree, including literal content and explicitly mapped kinds,
from Core on both spellings and from the pinned reference. Unmapped kinds or
fields fail rather than being erased. Source-coordinate correctness remains a
separate parser invariant.

Candidate substitutions and node/field/state censuses remain regression
witnesses. Their diagnostics stay visible, but they do not establish structural
isomorphism or equal effort. `contract.review` names a completed
adjudication; `contract.pending` is reserved for unresolved proposals. Neither is
a proof of global impossibility. Twelve generated boundary baselines publish
signed full-minus-without costs, including interactions and changed byte lengths,
separately from the structural controls.

### Reading the factors

Let A = Core(dialect), B = Core(paired input), R = reference(paired input),
using total `source_to_buffer + buffer_to_ast` Ir at the paired workload sizes:

- A/B compares the two spellings in Core, including recognition and construction.
- B/R compares implementations on the same paired input.
- A/R is a descriptive cross-syntax quotient, never an equal-effort claim from an AST proof.

The report prints absolute A, B and R and each pair's missing cost obligation.
The former Grammar/Shape/Same-job interpretation is superseded by these arithmetic
labels. When B carries a field with no reference counterpart, A/B and B/R remain
suppressed. Lowering B alone lowers B/R and raises A/B while leaving A unchanged:
optimize actual costs, not one factor
in isolation.

The corpus's generation mechanisms remain independent of proof status: fixed
sample repetitions preserve their bytes, and `generated` / `counted` workloads
match unit counts. Neither pair half enters the mixed aggregates. Case selection
pulls in both halves and their sizing dependencies, including candidate pairs.

Stage reports use schema 4 and record the exact pairing contracts. The census
uses schema 2, removes `sameJob`, and separates original measurement identity
from current interpretation identity. Both the old 30-pair and the later
43-pair equivalent-work medians are withdrawn. Their raw counts remain evidence;
changing the interpretation is not a parser speedup. Remeasure both revisions
under the same corpus/toolchain for before/after claims.

### Scope and unmatched operations

A failed proposed mapping is not an impossibility proof. Reconstruct a reference
input first; if a shared restricted core can be proved, measure it separately.
Keep the remaining operation visible through an explicit corpus boundary. The
30-pair review records why the old mapping fails and exactly what every boundary
removes. Historical enumeration arguments in `unpairable` concern their named
production only and cannot certify a candidate by analogy.

Representation wrappers can differ, but fields, ordering, ownership and binding
operations require explicit correspondences. Replacing every kind name or simply
matching a census does not establish them. Constants such as a formula mode are
checked in separate proof domains; they are not ignored across a mixed domain.

## CommonMark syntax is not a CommonMark tree

`"dialect": "commonmark"` on a case asserts that its **syntax** is CommonMark. It
does not assert anything about the **output**, and those are different claims.

This dialect derives an identifier for every heading. A document of plain ATX
headings is therefore CommonMark source that does not parse to a CommonMark
tree, and dividing this parser's cost on it by cmark's published the price of a
feature cmark does not have as though it were the price of parsing a heading.
`block-heading` read 3.64x that way and it was not a comparison.

So `corpus.json` declares each **referenceless** field once, with why no
reference builds it, and every case declares the ones its tree carries:

```json
"referenceless": { "anchor": "Neither reference derives or stores a per-block identifier. …" }
```

A case carrying one lacks even matching output obligations on the same bytes.
A structural pairing must supply a corresponding reference operation, but that
alone proves no cost equality. `pair-anchor-dialect` retains diagnostic quotients
while its binding is measured through a declaration/host boundary.

A feature-absent diagnostic is still *ranked*, and separately: it sorts by its ratio against cmark
on its own bytes, it is the `Dialect-only (no reference)` group and contributes
to that group's median, and it is eligible for the cost table. What it never
does is enter an equal-effort group or claim a lower bound. Missing a feature
can change recognition of the remaining input; its quotient is diagnostic only.

An unmatched field on the **paired-input** half inflates B. It cancels out of
A/R, but it affects both A/B and B/R, so those two quotients are suppressed.
This algebra does not itself prove equivalence between A and R. The heading
input `pair-ldirective-common` carries a derived anchor, and that pair remains
historical diagnostics. Its replacement uses an image paragraph and has no
derived anchor.

The audit checks the declarations against the parser's dump **in both
directions**. A missing declaration publishes a bound as a comparison, which is
the original defect; a declaration for a field the tree does not carry demotes a
real comparison to a bound, which hides a regression behind a number nobody
ranks. The complexity shapes are checked at reduced depth, on a document rebuilt
from the same unit and tail at 64 repetitions rather than on a prefix cut mid-
structure.

## What subtraction cannot do, and why this is not that

The obvious alternative to a pair is to measure both engines on the same bytes
and subtract the dialect-only work from this side. It was tried, in 719 lines,
and abandoned. The record is worth keeping because it is the first thing anyone
proposes:

- **The boundary does not land on call edges.** Six successive review findings
  each found another mis-drawn one — an empty call costing 53 Ir, a cleanup
  path, a registration band inside a function that also does shared work, a
  base-name collapse, an observer's dispatch. Every one was correct, and the
  list never closed, because "this part is the feature and that part is shared"
  is not a property the call graph carries.
- **Subtraction cannot see second-order effects.** An allocation not made
  changes the heap that every later allocation meets.
- **Misclassification is invisible.** Calling this parser's own work "the
  reference does this too" hides exactly as well as the reverse, and no check
  catches it.

Compiling the feature out of a measurement build is worse still. `markdown_core.h`
states the contract: one dialect, every feature always on, nothing enables,
disables or configures a construct. A profile that switches a feature off is not
profiling this parser. **The feature set is immutable, so equivalence is the
corpus's job** — which is what an isomorph pair is.

## Splitting a corpus: the remainder inside its hosts

Not every production proved unpairable is a construct of its own. An attribute
list `{key=value .cls}` adds fields to the node its host built and no node
itself, so no document *is* the list: there is nothing to pair, and a bound on a
document that carries it is mostly the host — the bound on `inline-span` is
mostly the inline parser. The corpus used to hide that behind a pair that put
the list on one inline directive against a title on one image, which is two
productions in one row, and the number it published said nothing about either.

So the corpus **splits** it, and a split has three parts, because two would
hide a regression:

- the **host without the remainder** is a document with a comparison of its
  own — a pair half with the three numbers above, or a CommonMark case with a
  ratio of its own;
- the **remainder without a host** is the attribute runner's job: against
  lexbor's attribute tokenizer in `scripts/benchmark-attributes.mjs`, the one
  implementation that does the same job on it, and *alone*, through the same
  runner from the stage benchmark's own build tree, in the stage report;
- the **remainder inside every host** the grammar gives it, which is this
  section.

Measuring the first two alone is the failure the third exists to catch. An
optimisation can make the host cheaper on its own and the list cheaper on its
own while the seam between them — where the host's scan hands over to the
list's and takes the result back — gets dearer, and the document people write
is the one with both. A corpus split into a paired part and a bound part
reports the local optimum and never the global loss. The split's `with` half is
the `without` half with the remainder's bytes on every host node, and the
driver reports the remainder's cost **in place**, as the difference between the
two whole documents, per list — twice:

```
Ir per list = (Ir(with) − Ir(without)) / (units × each)          over the two stages
Whole path  = (path(with) − path(without)) / (units × each)      over the whole parse path
```

The second includes releasing what the list built, which the stages do not, so
a change that defers the list's work past a stage boundary shows as the gap
between the two widening rather than as a saving.

The reference for a row is the remainder **alone**: the same bytes, decoded by
the same grammar with no host around them, through
`markdown_core_attribute_runner` built in the same tree with the same pinned
flags, read on the edge `scripts/benchmark-attributes.mjs` reads — which covers
the scan, the decode and the release of each list, so it is compared with the
whole-path marginal. `In place / alone` is that quotient, and it is the
composition: what the host adds to decoding the list. A host that gets cheaper
in its pair while this rises has moved cost into the seam rather than removed
it, and that is the row a corpus split into a paired part and a bound part
would never print. The runner's objects are in the identity table for the same
reason the attribute benchmark puts them there: the measured edge is *into* the
runner, so its translation units are measured ones.

### The hosts

The hosts are the attachment table of `docs/specs/dialect/attributes.md`, one
row per site production, less two the table names and a split cannot hold: the
bracketed span, whose list is mandatory, so there is no span without one to
subtract; and the nameless container directive, whose opening fence *is* the
list, so it is the host and not a decoration. Its named form is the container
host, and both halves keep the nameless `::: {}` unchanged. That gives twelve:
seven whose `without` is a pair half, and five — a code span, a setext heading,
a resolving reference link, a link reference definition, an angle autolink —
whose `without` is a CommonMark case written for the purpose, with a ratio of
its own.

Where the grammar admits whitespace before the list — an ATX heading, which
removes the suffix together with the whitespace before it; a link reference
definition, which requires it — the host declares that `separator`, and it
belongs to the site, not to the list: the audit inserts separator plus bytes,
and the marginal contains the separator's scan, which is part of what that site
costs. Everywhere else the bytes follow the host with nothing between, because
a space before the list is what stops it attaching. The remainder's bytes are
declared once and are identical in every host; only the site varies.

The rows are one job, and the spread between them is the site production first
— an ATX heading removes a suffix from a line it has already scanned, an inline
directive reads the list where its label ends — and this implementation's seam
second. `Lands in` names the stage the difference fell in, *read off the
measurement*: which parser this implementation reads the list with, not which
the grammar assigns it to, and it is not the same for every block host. The
report also prints the marginal at the next size over this one (`x2 / x1`),
which should not move: a list costs what it costs however many there are.
Every number the report prints is recorded in `stages.json` under `splits`,
host by host with the alone measurement beside them, so a spread tracked across
runs is read from data rather than parsed back out of prose.

This is not the subtraction rejected above. Subtraction drew a boundary
*inside* one measurement, through a call graph that does not carry it, and the
boundary never landed on call edges. A split's boundary is in the corpus,
between two whole documents that differ by exactly the remainder's bytes; every
instruction of both is counted, so nothing is classified and nothing can be
misclassified; and second-order effects — an allocation the list makes that
every later allocation meets, a line the list makes longer — are *in* the
difference rather than hidden by it, which is the point of measuring in place.

Two things a difference does not show, stated so a row is read for what it is.
A cost both halves pay cancels in every difference column: a host that got
dearer on its own, with and without the list alike, shows in that host's own
ratio in the tables above and not here, and `With/without` moves toward 1.00x
on it, which reads as a small improvement of the list — so a row is read beside
its host's own number, never instead of it. And a row is the remainder's cost
at the corpus's unit shape: work a host does per extent rather than per list —
the attribute parser's memo is sized to the line or paragraph it scans — lands
in the row in proportion to the extent's length over the lists it holds, which
`each` and the unit fix. Rows are compared across runs at one corpus digest,
where that shape is constant, and `In place / alone` is a factor to watch move,
not a verdict on the host.

### What is held, and where

`corpus.json` declares a split once, under `splits`: the `remainder`, which
must name an `unpairable` proof, because a split exists where a pair cannot;
its `bytes`; the fields it `varies`; the grammar `states` it demonstrates in
every host; and its `hosts`, each naming the site, the `kind` of node the
remainder decorates, a `without` case, a `with` case, how many host nodes one
unit holds and, where the site takes one, the `separator`. The rules that need
only the manifest live in `scripts/lib/corpus-splits.mjs` — where
`benchmark-stages.mjs` reads the same declarations to decide what a `--case`
run builds, so the two cannot drift — and `scripts/tests/corpus-splits.test.mjs`
breaks each one in turn and checks it fails for the stated reason:

- the `with` unit holds the remainder's bytes exactly `each` times, every copy
  after the host's separator, and the `without` unit never; with every copy
  deleted, separator and all, the two units are byte for byte the same;
- both halves were generated to the same count of units, which is why the
  `with` case is `counted` against the case its `without` is sized by, and both
  declare they build the kind the host names;
- the `with` half is `extended`, is not a pair half and publishes no ratio of
  its own — or the states it reaches would count as measured while
  `statesBoundByProof` says they cannot be — and the `without` half is never
  another host's `with`, or the difference would be one remainder over another.

`scripts/audit-corpus-reach.mjs` holds the rest against the parser, from the
one dump it takes of each document:

- the two documents parse to the **same tree** modulo the fields the remainder
  populates and the source spans, kind names included — unlike a substitution
  pair, a split's halves must build the same nodes, or the remainder did more
  than decorate;
- every `with` reaches every state the split names and no `without` reaches
  any: the states are what the remainder demonstrates in every host, and a host
  reaching one on its own would make the difference something other than that
  state's price. Whether a state is a bound is the ratchet's question, not this
  one — both attribute states are demonstrated here, while the one-member
  span replacements have explicit one-member field mappings;
- the nodes carrying a populated field are exactly `each` per unit on the
  `with` side, every one of them of the kind the host names, and none on the
  `without` side, which is what says the remainder landed on every host node,
  on nothing else, and on the node the host claims.

Each of those fails when broken, verified by mutating the corpus rather than by
reading the code. A `with` half is in no group of the ratio table, in no
median, and is read by no reference: its comparison is the difference, and a
cmark reading of a document holding a production cmark does not decode would
print a per-byte ratio for something that is not a comparison. Its growth row
stands, with a dash where the reference would be.

The split remainder and spread between hosts remain diagnostic evidence.
The separate source-stage gate compares each whole input against the event base
within one job; it does not assign a performance budget to a split remainder.

## The attribute grammar, against lexbor

Attributes are the production the pairs reach one member at a time:
`{#lane .stage k="v"}` decodes into an anchor, classes and ordered records, a
single member pairs against a link's suffix, and no reference production
decodes a delimited list of repeated members — `corpus.json` carries the
enumeration under `unpairable` — so the list has nothing in cmark or cmark-gfm
to pair with. The stage benchmark bounds it, and the
bound it reports on `inline-span` is mostly the inline parser around the
attributes rather than the attributes; it also measures the list *in place*,
as the [split](#splitting-a-corpus-the-remainder-inside-its-hosts) above, which
says what each host adds to the list by dividing the list in place by the list
alone — read through this driver's own runner, from the stage benchmark's build
tree. This driver is the other half of that split: the list alone, against the
one implementation that does the same job on it.

An HTML start tag's attribute list *is* the same job: a bracketed run split into
an identifier, a class run and key/value records, with quoting and character
references. [lexbor](https://github.com/lexbor/lexbor) implements that in C and
is written for speed, so it is the reference this grammar has.

```sh
scripts/init-environment.sh --install oracle-lexbor   # once
node scripts/benchmark-attributes.mjs
```

That install only *pins the source*, unlike the cmark oracles: the driver
compiles lexbor itself, from the pinned checkout, with the `benchmark` preset's
compiler and options, and checks every object of both baselines really received
the pinned flags — the two archives **and the two runner targets**, because
`bench_parse_attributes` is compiled into the executables rather than into
either archive and the measured edge is the edge into it. (The stage benchmark
is not in that position: its edges are internal to the parse transaction, so its
runner objects sit outside every stage it counts.) Every configure, build and
compiler probe runs under the same environment allowlist the stage benchmark
builds under, because `CPATH` and `C_INCLUDE_PATH` add include directories that
appear on no compile line at all. An archive built by the environment setup would carry whatever
the host defaults to, and the ratio would then depend on how the baseline
happened to be installed while the report claimed one compiler produced both.
Nothing is written into the checkout either — lexbor ships no `.gitignore`, so a
build tree inside it is several hundred untracked files, and the driver refuses
a checkout with local changes for the reason it refuses a dirty cmark one.

It is a separate driver, and separate for a reason the stage benchmark makes
plain: there both engines must get byte-identical files, and here they cannot —
neither implementation reads the other's spelling.

```
[text]{#lane .stage k="callgrind"}
<x id="lane" class="stage" k="callgrind">
```

A build tree is reused only while what produced it is what the report names.
CMake reuses unchanged objects, and "unchanged" is about sources rather than
about the compiler — upgrade it at the same path and both trees are reused while
the report records the new banner, or one is cleaned and the ratio compares two
compilers. So each tree carries a stamp of its toolchain and is discarded when
that stamp does not match. Both trees belong to this driver rather than the
preset's shared one, because the stage benchmark stamps that tree with a wider
identity than this report carries and the two would otherwise wipe each other's
out on every run.

Quoting is part of each specification rather than of how one side renders it.
Both grammars scan a quoted value and a bare one down different branches — this
parser tracks `quoted` and `unquoted` runs separately, lexbor has distinct
`attribute_value_double_quoted` and `attribute_value_unquoted` tokenizer states
— so a workload that quoted everything would leave both bare paths unmeasured,
and a spelling that quoted on one side only would compare two different scans
while still producing a matching census. One specification carries character
references for the same reason: both grammars decode them in a value and both
are charged for it, and the census compares the decoded text, so it also proves
they decoded the same thing.

Both inputs are generated from one list of specifications, so they cannot drift
into describing different attributes, and **both baselines must recover the same
attributes**: each writes a canonical census and the two are compared line for
line before any count is reported. A baseline that skipped a record, kept a
value raw or stopped early fails the run instead of posting a cheaper number for
doing less.

The measurement is the tokenizer, not `lxb_html_parse`: a document, a DOM tree
and interned elements have no counterpart on this side, and the ratio would be
against those instead. It stops where the comparison does, at tokens carrying
name/value pairs.

One runner reaches past the public facade, and it is the only one in
`benchmarks/` that does. It has to: a consumer reaches `elements/attributes.c`
only through a span, a heading or a link, each of which brings a whole inline
parse along — which is the measurement this exists to avoid. That is a property
of the runner, not of the product: the parser still has no measurement mode and
no benchmark-only path, and the entry points it calls are the ones `link.c` and
`heading.c` call.

The report records the resolved compiler, C library and profiler, a digest of
how the compiler says it was configured, and the compile and link flags read
back out of each tree's own cache — because `gcc` is a name PATH resolves to
whatever the image ships this month, and a table that cannot move when the
environment does cannot carry a comparability rule. That table is **narrower**
than the stage benchmark's: it does not digest the resolved code-generation
target or what glibc dispatches on from inside valgrind. Two attribute reports
agreeing on its rows is a weaker statement than two stage reports agreeing on
theirs, and the report says so rather than leaving a reader to assume parity.

lexbor is a performance baseline and nothing else. No behaviour is judged
against it, it registers no deltas, and it is not one of the parser oracles in
`specs/oracles/`.

## What the counts are a property of

Instruction and data-reference counts do not depend on how fast the machine was
or what else was running on it: re-running one commit on one toolchain
reproduces every number exactly. They are not independent of the toolchain — a
different compiler or C library emits a different instruction stream for the
same source — so the report records the resolved compiler, libc and valgrind
versions, and absolute counts are comparable only against a report whose
toolchain table matches.

The code generation target is recorded as the compiler's complete answer, not
as the `-march`/`-mtune` names. Those names are a label: on one host `native`
resolves to `sapphirerapids` with 81 feature switches enabled, and
`-march=native -mno-avx512f` resolves to the same two names with 68. Cache sizes
are not in `--help=target` at all — `native` writes them into the tuning params,
where they steer unrolling and prefetching. So the table shows the names for a
reader and a digest of every switch and param beside them, and the digest is
what the comparison rule runs on.

The C library's dispatch is part of it as well. glibc picks an implementation
per routine at load time from the CPU it detects — `__memcpy_avx_unaligned_erms`
and `__strlen_avx2` on this host, plain SSE2 variants on one without AVX2 — and
those instructions sit inside the stage costs, because both parsers call them
constantly and in different proportions. Two hosts agreeing on compiler, C
library, valgrind and compiler target can therefore produce different counts
*and* a different ratio.

What dispatch actually sees is not the raw host: valgrind masks CPUID, and glibc
under it reports `max_cpuid` `0xd` here against `0x1f` natively. So the report
asks the question through valgrind, of the loader that will run the measured
binary, and records a digest of the CPU features it answers with. The two views
digest differently, which is the check that it discriminates rather than
recording a constant.

It is recorded rather than pinned to a fixed capability set, because
constraining dispatch would need `GLIBC_TUNABLES` inside the measurement — the
one variable whose removal is load-bearing above — and would measure a C library
nobody runs. And it records the CPU features rather than the routines that were
chosen: the feature set is a property of the host, valgrind and glibc, while the
set of routines a parse happens to call is a property of the code, which would
make every commit incomparable with the one before it.

The corpus is part of that table too. The report carries one digest over every
document measured, content and all, because an edited `corpus.json`, an edited
sample, or a change to how documents are generated moves every count while the
parsers stand still — and a byte count cannot tell two different documents of
one size apart. Each case also carries its own document digest in `stages.json`,
so a corpus that moved can be narrowed to the cases that moved. Filtering with
`--case` changes the digest, which is the intended reading: a partial run did
not measure the same workload as a full one.

The build flags are part of that table, and they are read back out of each
tree's CMake cache rather than taken from the preset. CMake initializes
`CMAKE_C_FLAGS` from `CFLAGS` and `CMAKE_EXE_LINKER_FLAGS` from `LDFLAGS`, once,
at first configure — so an exported `-march=native` or `-static` outlives the
shell it was set in, and both the compile and the link line describe a binary
the preset alone does not. Both are recorded, both are compared between the two
engine trees, and a tree stamped with different ones is rebuilt rather than
reused.

The ratio is not exempt. A compiler upgrade need not change both engines by the
same proportion, so a toolchain roll moves the ratio too. **Toolchain and
measurement-input identities must match** for counts or ratios to be comparable.
Source/object inventories and binary hashes record the revisions being compared;
those may differ without changing the toolchain.
What holds inside one report is that both engines met the same compiler, so the
ratio there is a fact about the two parsers rather than about the build.

## Source-stage regression gate

CI calls this workflow as a dependency of `Required gates`. With
`--baseline-ref COMMIT`, the driver rebuilds the base engine using the current
harness, preset and pinned references, generates the current corpus once, and
measures both cores on those exact bytes in the same isolated environment.
Effective flags, compiler provenance and loaded runtime libraries must agree.
The distinct ordered compile-option sets must match, as must the options of
each surviving source path. Adding, removing or renaming a translation unit
does not itself break compatibility. Object counts, per-source options and
inventory digests remain separate provenance, recorded for each revision in
its own report rather than copied from head to base.
Both complete reports and Callgrind dumps are retained; the base report reuses
this run's measured reference results and identifies their actual binaries.

The attribute/lexbor comparison is an independent informational workflow. Its
setup, measurement and artifact failures do not enter the source workflow's
aggregate result, CI's required gates or the release dependency graph.

Every case/scale must have `source_to_buffer` Ir at most 1.02 times its base.
The 2% allowance is an explicit review budget, not a claim of measurement noise
or elapsed-time equivalence. There is no median/total-stage offset, dropped
regression row, or missing-input fallback. An intentional increase beyond the
budget requires review of the algorithm and requirement; the driver writes
both reports before failing. This guard does not replace allocation bounds,
semantic proofs, or adversarial complexity tests.
