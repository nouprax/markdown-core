# Parse stage benchmark

What it costs Markdown Core to parse a document, stage by stage, next to what
it costs cmark to parse the same bytes. The comparison exists to give an
optimization somewhere to argue from: a claim that a change made the parser
faster should name a stage and a number.

```sh
scripts/init-environment.sh --install oracle-cmark   # once
node scripts/benchmark-stages.mjs
```

The driver builds both engines, runs them under callgrind, and writes
`build/benchmark-stages/stages.md`, `stages.json`, and the raw dumps.

## The two stages

A parse has two paths worth optimizing separately, and they have different
shapes: one is a scan over bytes, the other is tree construction over the
buffers that scan produced.

| Stage | Markdown Core | cmark |
| --- | --- | --- |
| `source_to_buffer` | `markdown_core_parse_document_with_mem` → `S_parse_source` | `cmark_parser_feed` |
| `buffer_to_ast` | `markdown_core_parse_document_with_mem` → `S_finish_parse` | `cmark_parser_finish` |

cmark splits exactly these two paths across two public calls, so feeding the
whole document and then finishing gives a boundary that *is* the boundary
rather than an approximation of one. Markdown Core has no such API and does not
grow one for this: it parses one complete buffer per call, and the stage split
is read afterwards out of the recorded call graph.

Parser allocation, dialect attachment, element discovery, and tree release are
outside both stages. They are fixed cost that no document-size argument applies
to, so folding them in would produce a number that looks like parsing and
isn't. They are worth measuring — see the empty-input allocation counts in the
C API tests — but not here.

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
  make the AST stage cheaper — it makes it unmeasurable.

Nothing in the product is arranged for this. The engine has no measurement
mode, no phase hooks, and no benchmark-only code path; the profiling flavour
only keeps the boundaries that already exist from being optimized out of the
symbol table. The driver checks for them after the build and fails loudly
rather than reporting a missing stage as a cheap one.

## The corpus

`corpus.json` names the documents. All but one sample come from cmark's own
benchmark corpus, so the CommonMark cases are input both engines were written
against. `directive.md` is repository syntax that cmark does not recognize, and
its case is marked `extended`: Markdown Core is doing strictly more recognition
work there, and the ratio is not an apples-to-apples one.

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

## What the counts are a property of

Instruction and data-reference counts do not depend on how fast the machine was
or what else was running on it: re-running one commit on one toolchain
reproduces every number exactly. They are not independent of the toolchain — a
different compiler or C library emits a different instruction stream for the
same source — so the report records the resolved compiler, libc and valgrind
versions, and absolute counts are comparable only against a report whose
toolchain table matches.

The build flags are part of that table, and they are read back out of each
tree's CMake cache rather than taken from the preset. CMake initializes
`CMAKE_C_FLAGS` from `CFLAGS` and `CMAKE_EXE_LINKER_FLAGS` from `LDFLAGS`, once,
at first configure — so an exported `-march=native` or `-static` outlives the
shell it was set in, and both the compile and the link line describe a binary
the preset alone does not. Both are recorded, both are compared between the two
engine trees, and a tree stamped with different ones is rebuilt rather than
reused.

The ratio is not exempt. A compiler upgrade need not change both engines by the
same proportion, so a toolchain roll moves the ratio too. **Two reports whose
toolchain tables differ are not comparable at all** — not their counts, not
their ratios — and a difference between them cannot be read as a code change.
What holds inside one report is that both engines met the same compiler, so the
ratio there is a fact about the two parsers rather than about the build.
