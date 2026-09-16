# Parse stage benchmark

What it costs Markdown Core to parse a document, stage by stage, next to what
it costs a reference implementation to parse the same bytes. The comparison
exists to give an optimization somewhere to argue from: a claim that a change
made the parser faster should name a stage and a number.

A ratio is only a comparison where both parsers did the same job, so which
reference a case is read against is part of the measurement. cmark answers for
CommonMark, cmark-gfm for the GFM constructs, and for dialect constructs neither
implements, an [isomorph pair](#isomorph-pairs) puts a reference back in the
comparison by giving it the same workload in a grammar it has. A case with no
pair is reported as a **bound**, and a bound is never ranked against a
comparison.

```sh
scripts/init-environment.sh --install oracle-cmark oracle-cmark-gfm   # once
node scripts/benchmark-stages.mjs
```

The driver builds all three engines, runs them under callgrind, and writes
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
  make the AST stage cheaper — it makes it unmeasurable;
- `-fvisibility=hidden`, because cmark sets it for its own build and Markdown
  Core's static objects do not. An engine compiled without it pays for
  indirection the other avoids: adding it moved Markdown Core's source stage by
  2.1% and its AST stage by 5.9% on `mixed-commonmark`, with cmark unchanged.
  That is a build difference sitting inside a number meant to be about parsers,
  and it is larger than most changes anyone would bring this benchmark to judge.

The report does not stop there and claim the two compile lines match, because
they do not. It reads every object of each linked engine out of that tree's
`compile_commands.json` — 59 for Markdown Core and 19 for cmark — and splits the
options into what both engines got and what only one of them did.

Every object, not the one holding the stage boundaries: a stage's cost is
inclusive, so it contains whatever the scanners and inline code did, and CMake
lets a single source carry its own options. `elements/CMakeLists.txt` gives ten
scanner sources `-Wno-unused-variable` through `set_source_files_properties`,
which is why Markdown Core's 59 objects have two distinct compile lines and not
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
declared isomorph pair holds) rather than a percentage to climb.

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

## Isomorph pairs

cmark reads `++adds++` as a paragraph and `$x$` as text. The ratio against it on
one of those documents is the cost of *not* having the feature: it bounds what
the construct costs and cannot say whether the construct is slow, which is the
only question the profile exists to answer.

An isomorph pair answers it, by putting the **same workload** in front of the
reference in a grammar the reference has.

### The pairing criterion, and the one way to get it wrong

Two productions pair when they have the **same grammar shape**, and — where the
production has a subsequent operation — when that operation matches too. `$x$`
against `` `x` `` is the shape rule: a leaf inline opened and closed by a
one-byte run whose body is taken literally, differing only in the terminal. An
explicit anchor against a link reference definition is the logic rule: both
declare a name, bind it to a target, and the binding enters the document's table
of names.

**The pairing is read off the GRAMMARS. It is never read off either
implementation.** That is not a stylistic preference; it is what makes the
number mean anything. A pair designed by looking at what machinery each engine
happens to run, and choosing documents that make those machineries match, drives
the ratio to 1.00x by construction — it measures the equalisation, not the
parsers. The whole point is to hold the *grammar* constant and let the
*implementation* difference show. Holding the implementation constant and
letting the grammar vary is the same mistake inverted, and it is the one this
benchmark has actually made: a heading was once paired against a link reference
definition because both engines were observed to grow a reference map, and the
pair read 1.27x — a number that said nothing about either grammar.

So the argument for a pair is always a citation of the two grammars, and it is
written down in `corpus.json` as the pair's `claim`, printed verbatim in the
report. A pair whose claim cannot point at two productions is not a pair.

### Two ways a pair is held

A **substitution** isomorph is the same document under a change of marker. Both
halves are the same bytes and parse to the same tree:

| Dialect | Isomorph | Substitution |
| --- | --- | --- |
| `++adds++` `==mark==` `^sup^` `~sub~` | `**adds**` `**mark**` `*sup*` `*sub*` | `+` `=` `^` `~` → `*` |
| `%%hidden%%` | ` ``hidden`` ` | `%` → `` ` `` |
| `$x$` | `` `x` `` | `$` → `` ` `` |

A **declaration** isomorph is two spellings of one production that are *not* the
same bytes, so there is no substitution and no identical tree to compare. These
are declared as `logicalIsomorphs`:

| Dialect | Isomorph | Reference | What is the same |
| --- | --- | --- | --- |
| `A block #name#` | `[name]: #name` | `cmark` | one block, one name-to-target binding, entering a table of names |
| `[body]{.c}` | `[body](/c)` | `cmark` | bracket, inline body, bracket, raw paired-delimiter suffix decoded into the node |
| `[[target]]` | `[](/target)` | `cmark` | fixed delimiters, raw body not parsed as inline, one leaf holding a destination, no lookup |
| `![[img.png]]` | `![](/img.png)` | `cmark` | the same, with the embedding prefix in front of it |
| `(@label) body` | `[^label]: body` | `cmark-gfm` | a labelled opener owning block content, leaving ordinary content for a side list |
| `- [~] item` | `- [x] item` | `cmark-gfm` | one scalar in brackets at the start of item content, removed from it and stored on the item |
| `@key` | `www.key` | `cmark-gfm` | an unbracketed trigger at a word boundary, a raw run stored without inline parsing and without resolution |
| `$$ … $$` | ` ``` … ``` ` | `cmark` | a leaf block opened and closed by a delimiter line whose body is not parsed as Markdown |
| ` ```formula ` | ` ```text ` | `cmark` | the same fence with an info string that selects what the body means |
| `%% … %%` block | ` ``` … ``` ` | `cmark` | the same, with a two-byte delimiter |
| `--- … ---` envelope | ` ``` … ``` ` | `cmark` | the same again; recognised once per document, so it scales by member **lines** |
| `::note[Label]` | `## Label` | `cmark` | a leaf block whose marker run is consumed into a field and whose rest of line is inlines |
| `:::note … :::` | `> quoted` | `cmark` | a container block: a start condition, a per-line continuation condition, contents parsed as blocks |
| `> [!note] title` | `- [x] item` | `cmark-gfm` | a bracketed token at the start of a container's first line, taken out of the content and kept as a field |
| `Term` / `: body` | `- Term` / `  body` | `cmark` | a first line parsed as inlines plus block content bound to it by indentation, under one node |
| simple table | pipe table | `cmark-gfm` | a line cut into K ranges, each parsed as inlines, once per row |
| headerless simple table | pipe table | `cmark-gfm` | the same row production, with the head left empty rather than designated |
| one-column grid cell | loose list item | `cmark` | a per-line positional cut at a width established by an earlier line, remainder parsed as **blocks** |
| `Table: X` before a table | GFM header line | `cmark-gfm` | a paragraph-shaped line already scanned, retroactively claimed by a table a later line announces |
| `a)` `(A)` `iv.` `IV)` `#.` `(#)` | `1.` | `cmark` | an item opened by a sequence token and a delimiter, the token read once and fixing the list's start |
| `: X` after a table | a loose list item's continuation | `cmark` | a construct a blank line does not close, absorbing the next line that matches its continuation rule |
| `:note[body]` | `![body](/note)` | `cmark` | a sigil-introduced inline leaf carrying one raw run and one bracketed run parsed as inlines |
| `$$y$$` alone in a paragraph | `[a]: /b` alone in a paragraph | `cmark` | a paragraph whose entire accumulated content is one construct does not survive its own close |
| `(5@label) body` | `[^label]: body` | `cmark-gfm` | the same as the plain specimen, with the digit run the plain form does not carry |

The reference is whichever engine implements the isomorph's production, which is
not always cmark: pairing a GFM production against cmark would divide by an
engine that read the paired document as prose.

Because the two halves of a declaration pair are different lengths, they cannot
be sized to a byte target — that would hand one side more of the construct than
the other and produce a comparison of one document's size with another's. The
dialect half is `generated` to the byte target and the CommonMark half is
`counted` to whatever number of units its partner actually emitted. Every
generated name carries an index, so both sides bind the same number of
**distinct** identifiers; a repeated literal would leave one side with a
one-entry table against thousands.

`{n}` writes that index as it is. `{n:K}` writes it zero-padded to K digits, and
a case whose construct is **column-aligned** must use it. A grid table
establishes its columns from the positions of the `+` characters in its border
line and requires every row line to close its cells at exactly those columns, so
an index that gains a digit at ten and again at a hundred would silently stop the
construct being recognised part way down the document — the case would still
generate, and would measure a paragraph.

### The three numbers

Either kind of pair decomposes the ratio the same way:

- **Grammar**, this parser on the dialect spelling over this parser on the
  isomorph. One parser, one workload, two grammars — so a number above 1 is this
  grammar and nothing else, and it names the file to open.
- **Shape**, this parser over the isomorph's own reference on the isomorph,
  where both did the same job. It is what the parser costs on that shape before
  any dialect construct is involved, and no change to a dialect grammar will
  move it.
- Their product, which is a same-job ratio for a construct the reference does
  not implement, because the reference did the equivalent work on the isomorph.

The split is the point. A pair reading 1.0x on Grammar and 3x on Shape is not an
extension problem at all, however large the bound against cmark looked.

On a declaration pair the two Ir/B columns are each over their own document's
bytes, and those differ by construction — so Grammar is not their quotient. It
is the total over the total at an equal count of the construct, which is the
only denominator the pair holds fixed.

**Two of the three can be suppressed while the third stands.** Write them out:
`grammar = core/twinCore`, `shape = twinCore/twinCmark`, `sameJob =
core/twinCmark`. `twinCore` cancels out of the last one. So where the
*isomorph's* tree carries a field no reference builds — an ATX heading, where
this dialect derives an anchor and cmark does not — this parser spent something
on the isomorph half the reference never spent, and it lands in `twinCore`:
Grammar's denominator is inflated so it reads low, Shape's numerator is inflated
so it reads high, and Same-job is untouched. The twin declares the field under
`carries`, the driver prints a dash in those two columns rather than a number it
would have to caveat, and the report says which pairs were suppressed and why.
`pair-ldirective-dialect` is the one that needs this: nothing else in CommonMark
is a leaf block whose marker run becomes a field and whose rest of line becomes
inlines.

Neither half of a pair joins `mixed-commonmark` or `mixed-extended`. A pair is a
pair of isolation probes, written to mirror each other; a document written twice
would enter the concatenated aggregate twice and tilt it toward whichever
construct the pair isolates. Every other sample is in its aggregate.

### The pairing is checked, not claimed

`scripts/audit-corpus-reach.mjs` holds each kind of pair to what it asserts,
against the parser rather than against the manifest.

For a **substitution** pair: applying the declared substitution to the dialect
document must reproduce the isomorph byte for byte, and the two dumps must be
identical once the kind names are erased. A pair whose two documents parse to
different trees is two measurements presented as one, and the report would
attribute the difference in the trees to a grammar.

For a **declaration** pair, where there is no identical tree to compare:

- both sides must build the **same number** of the paired construct, each side
  counted by the kind it builds. The corpus generates them to an equal count,
  which is arithmetic; this is the parser agreeing that the bytes it was handed
  came out that way. For the anchor pair it is also what proves the reference
  side's definitions were *consumed* — a definition the parser declined to read
  as one stays a paragraph, and the count doubles.
- where the two sides build the **same kind**, an equal count proves nothing on
  its own (an item whose marker went unread is still a list item), so the pair
  must name either a `binding` or a `fallback`, and the audit refuses a pair
  that names neither.
- where a pair names a `binding`, every node of the kind on a declaring side
  must carry it, and a side that declares *out of band* must carry none at all.
  That asymmetry is the anchor pair: an explicit anchor binds on the block it
  sits on, a link reference definition binds into the reference map and leaves
  no node. `declares` takes one field name where both sides spell the binding
  the same way, and **one per side** where they do not — a callout binds its
  `variant` and a task item binds its `marker`, and naming only one of them
  would leave the other side's recognition unproven.
- where a pair names a `fallback`, that kind must be **absent from both
  documents**. It is the other way a same-kind pair can be held: a construct
  that degrades into a different kind when it stops being recognised — a simple
  table whose dash run goes unread is a paragraph, not a table — makes the count
  evidence after all, and what makes it evidence is that the fallback never
  appears. A task marker has no fallback in this sense, which is why that pair
  names a binding instead.
- where a pair names `demonstrates`, each side it lists must reach every
  declared grammar **state** named for it. This is the third way a same-kind
  pair can be held, and it reaches what a field cannot: two ordered lists build
  `List` and `ListItem` whatever their markers say, and the field that tells
  them apart — `variant=alpha(lowercased=true)` against `variant=decimal` —
  prints unquoted, so it is not a binding. Naming the state says the same thing
  in the vocabulary the manifest already declares, checked by the predicates the
  conformance corpus is held to.
- where a pair names `absent`, the sides it lists must build **none** of the
  kinds it names. It is `fallback` declared per side, for a pair whose two
  spellings degrade differently and so have no kind to name for both: a trailing
  table caption that went unread is a paragraph, while the list item it pairs
  with holds paragraphs whether or not it absorbed anything.
- where a pair names `alsoCounts`, those kinds must match too. A pair's claim
  can name more than one construct, and counting only the primary one leaves the
  rest unheld: the grid-cell pair counts `Paragraph` alongside the cell because
  *block parsing of the contents* is the second half of the criterion, and the
  specimen pair counts the **call** as well as the definition.

Each of those fails when broken, which was verified by mutating the corpus
rather than by reading the code.

### Which grammar STATES have a same-job number

Node kinds are the coarse question, and all 43 are either paired or written in a
syntax a reference reads the same way. The fine question is the 131 **states**
`specs/canonical-ast/manifest.json` declares — a table cell that spans rows, a
roman-numeral list, a metadata scalar that is a number. A corpus can build every
kind and still reach a state only inside a case with no reference to divide by,
and such a state is measured as a *bound*.

`node scripts/audit-corpus-reach.mjs --states` prints where each one stands, and
the summary line counts them. The predicates are the repository's own, in
`scripts/lib/canonical-states.mjs`, shared with the conformance-fixture checker
so the two cannot disagree about what a state is.

`corpus.json` carries a `stateFloor`, and the audit fails below it. It is a
ratchet rather than a target: a change that quietly stopped a case demonstrating
a state — an edited unit, a retired sample — would otherwise pass the audit while
the number shrank with nobody watching. Raise it in the commit that earns it.

**It stands at 129 of 131, and the other two cannot be closed.** They are
reachable only through a construct this corpus has *proved* unpairable — an
inline footnote's content, and a specimen definition with no label — so no case
can measure them against a reference, because no reference production of that
shape exists to write one against. Each is declared in `statesBoundByProof`
against the `unpairable` entry that binds it, and the audit checks that
declaration in both directions: the proof must exist, and the state must really
still be a bound. A state the corpus learns to measure loses its exemption in
the same change, or the count would understate itself with nobody noticing.

So the corpus is complete in the only sense that can be checked: every declared
grammar state either has a same-job ratio or has a written proof that it cannot.

### Constructs with no isomorph

A construct with no row in the pair table has no row for one of **two** reasons,
and reading them as one is what kept this list wrong for months. The reasons live
in `corpus.json`, not here and not in the report: `scripts/benchmark-stages.mjs`
quotes them out of the manifest when it writes the report, so a proof that falls
stops being published the moment it is withdrawn. A hard-coded list went stale
the instant one did — and ten fell in a single day, leaving this file asserting
that a grid table, a definition list, a callout and a directive could not pair
while the manifest already held the pairs for all four.

- **`unpairable`** is a *proof*: an argument off the two grammars that has been
  attacked and held. There are five, and three of them carry the caveat that
  nobody but their author has attacked them.
- **`openCandidates`** is a *candidate*: a pair that has not been built and has
  not been proved impossible. It is a bound today because nobody has settled it,
  which is a weaker sentence and is printed as one. **It is empty.** Every
  construct in the dialect is now either paired, or written in a syntax a
  reference reads the same way, or carries one of the five proofs.

What makes a proof survive is its **shape**. Nine of the ten that fell were
single-construct assertions — "no reference *table* has block cells", "CommonMark's
only *fence* opens a leaf" — which answer whether the reference has a
*construct* of this kind rather than a *production* of this shape. The one that
held (`inline footnote`) is an exhaustive enumeration over every inline
production of both grammars. Write the enumeration or do not write the proof.

Known reference productions that the fallen proofs did not expect, kept here so
the next proof has to get past them:

| The reference does have | which is why |
| --- | --- |
| promotion by what *follows* — GFM's delimiter row claims the paragraph line above it | the leading table caption pairs |
| a first line's inlines grouped with following indented blocks under one node — a loose list item | the definition list pairs |
| a per-line positional cut whose remainder is parsed as **blocks** — a list item's indent width | the grid cell pairs |
| a container category with a start condition, a continuation condition and block contents | the container directive pairs |
| a bracketed token at the start of a container's first line taken into a field — GFM's task marker | the callout pairs |
| a paragraph annihilated into a declaration — CommonMark's link reference definition | the anchor pair works at all |

**What is never a reason to refuse a pair** is that the two sides materialise
different numbers of nodes. That is reasoning off the implementations, which is
the one thing the criterion forbids, and the node counts are part of what the
ratio reports. The bare citation key was nearly left a bound on exactly that
ground — one `Link` over a `Text` against a `Cite` over a `Citation` with two
affix slots — and pairing it is what turned that asymmetry into a number
(Grammar 1.13x) instead of an excuse.

Two **substitution** pairs were tried and do not hold, which is a different
failure from having no isomorph at all:

| Tried | Why the bytes are not the same job |
| --- | --- |
| `==a====b==` ↔ `**a****b**` | our run splitter divides adjacent closers, CommonMark's does not |
| comment ↔ strong emphasis | strong parses its body, a comment keeps it literal |

A kind's own bookkeeping may differ where the manifest names it: `Formula`
records which spelling opened it in `mode`, which a code span has no equivalent
of. Those exceptions are declared per pair in `corpus.json`, and an exception
matching nothing in the dialect document fails the audit, so an allowance cannot
outlive the difference it was written for.

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

A case carrying one is reported as a **bound** — it is not ranked, and it is not
in any group's median — unless the corpus pairs it, because a pair is exactly
the thing that puts the same declaration in front of the reference. That is why
`pair-anchor-dialect` carries `anchor` and still gets a ratio.

A field on the **isomorph** half is a different case with a different cost. It
does not touch the same-job ratio, because `twinCore` cancels out of it; it
costs Grammar and Shape, which are suppressed for that pair. *The three numbers*
above writes the algebra out. `pair-ldirective-common` is the one twin that
carries a field, and it carries `anchor` because this dialect derives one on
every heading.

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

## The attribute grammar, against lexbor

Three of the isomorph pairs above work because the dialect construct is shaped
like a CommonMark one. Attributes are not: `{#lane .stage k="v"}` builds a map,
and no Markdown implementation builds a map, so there is nothing in cmark or
cmark-gfm to pair it with. The stage benchmark can only bound it, and the bound
it reports — 4.46x on `inline-span` — is mostly the inline parser around the
attributes rather than the attributes.

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
same proportion, so a toolchain roll moves the ratio too. **Two reports whose
toolchain tables differ are not comparable at all** — not their counts, not
their ratios — and a difference between them cannot be read as a code change.
What holds inside one report is that both engines met the same compiler, so the
ratio there is a fact about the two parsers rather than about the build.
