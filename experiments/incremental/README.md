# Incremental parsing experiments

Baseline: `5eca3bc1598b0313a0ac6a9e69f078a309feb3e1` (2026-09-13).
This is a diagnostic experiment, **not a production incremental parser**.
`results.json` preserves the original baseline run. The current probe also runs
against the radix-index performance fixes; its lookup-branch metric supersedes
the baseline insertion-slot metric. Save new runs to a different output file.

From the repository root, with a C compiler, CMake, and Python 3:

```sh
cmake --preset benchmark
cmake --build --preset benchmark --parallel 8
python3 experiments/incremental/run.py --output build/incremental-results.json
```

The runner compiles `probe.c` against the existing static product library and
loads the existing shared product library. It uses no downloaded dependencies.
The recorded run is [results.json](results.json). `--mode review`,
`--mode witnesses`, and `--mode incremental` run individual experiments.
`--build /path/to/build` selects another **Release, shared + static** build.
Do not feed sanitizer timings into the recorded Release comparison.

## What is measured

- **Baseline review:** depth × width inputs revisit the previous attachment
  regression; sparse attributes expose auxiliary-space cost; generated labels
  replay the baseline hash collision workload. Timings use the public C
  parse + free API, one warmup and seven samples, reported as medians.
- **Native allocation and work:** a separate internal parse injects a counting
  allocator and a postprocessor through the existing test setup mechanism.
  Allocation counts include one extra contiguous registry attachment for the
  recorder. They are not counts for the uninstrumented public facade.
  `peak_bytes` is peak **live requested capacity**, excluding allocator headers,
  transient realloc old+new residency, input storage, Python, and bindings. It
  is not RSS. Each measured parse must end with zero live requested bytes.
- **Collision replay:** the generator retains the baseline FNV/finalizer hash
  and selects labels sharing bucket zero at the baseline's preallocated
  capacity. The original run proved `n(n+1)/2` insertion slot visits. The current
  probe inserts those same keys into the replacement radix tree, verifies each
  lookup and counts its branch visits, bounded by `n * (9 * key_bytes + 1)`.
  Candidate generation is excluded from parse timings. These metrics have
  different units; compare parse/free timings and structural bounds separately.
- **Semantic witnesses:** sixteen append pairs make already parsed content
  change. The runner asserts that an old non-Document record changes; dumps
  retain scopes, fields, definitions, and owned field roots. The witnesses
  disprove unconditional prefix freezing. They do not implement a dependency
  graph or establish that every possible dependency is enumerated.
- **Incremental source/island experiment:** `ParagraphSession` retains native
  documents for completed independent prose islands. Append reparses its active
  island and any newly closed islands. The editor experiment uses a Fenwick
  index to locate byte offsets in O(log paragraphs), replaces only that island,
  and updates its length. A variable-width replacement does not copy the source
  suffix. Both routes use the selected full parser for every island.

## Deliberate limits

The island experiment admits **unindented ASCII letter/space prose and LF
only**. Its admission check establishes a semantic reason that empty lines close
independent paragraphs. It rejects other syntax; it is not a fast path proposed
for production. Production must support the entire current dialect through
state/dependency-based reuse, not an allowlist or a different streaming grammar.

The editor benchmark replaces letters inside existing paragraphs, including
length-changing replacements. It does not insert/delete islands, change line
counts, or implement arbitrary editor edits. Fenwick's fixed leaf count is an
experimental simplification; the design calls for a balanced dynamic source
tree. Choosing the next edit position belongs to the workload generator and
is outside timing; resolving the supplied byte offset is inside timing.

Timing includes Python orchestration, native parsing, source updates and
release of replaced native documents. Streaming compares updates with an
interleaved full-reparse control; editor initialization is excluded from both
update series. Island source/AST retention increases resident memory, which the
update benchmark does not measure. It retains one current revision, not a
persistent history. It omits cross-language projection, delta serialization,
UI rendering, snapshot publication, and old-snapshot retention/reclamation.
Do not present its speedup as a product end-to-end speedup.

For correctness, the runner compares all canonical records, including scopes,
with a fresh full parse. Only tree-drawing glyphs are normalized. Island
coordinates are translated by known LF line bases **for this restricted
experiment**; production must not reconstruct ranges from public Scope.
Oracle parsing, flattening, and dumping are outside timing. The small stream
matrix checks all 1,392 prefixes; the small editor case checks all 128 edits.
Larger measurements check their final snapshots only. This is not a full-dialect
incremental conformance claim, an OOM sweep, or a random-edit campaign.

The long-paragraph control is intentional: it reparses exactly the same number
of bytes as full parsing and may run slower because of bookkeeping. It proves
that an active-block implementation alone does not solve long streaming text.

## Interpretation

Use deterministic parsed-byte and slot-visit counts to reason about complexity.
Sequence p50/p95 timings are descriptive distributions from one local run;
they are neither cross-device latency guarantees nor merge gates. Re-run on
an otherwise idle machine before comparing timings between changes. The
repository's existing benchmarks time parsing alone and exclude destruction;
those numbers have a different boundary from this experiment.

See the [review](../../docs/reviews/2026-09-13-incremental-readiness.md),
[design](../../docs/architecture/incremental-parsing.md), and
[implementation tasklist](../../docs/plans/2026-09-13-incremental-parsing.md).
