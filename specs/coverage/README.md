# Coverage policy

`policy.json` is the single source of truth for the repository's coverage gate.
Thresholds, exemptions, measured-file floors, and the record of unpinned
surface live here and nowhere else, so no platform producer can hold a second
opinion about whether it passed.

The contract this data serves is frozen in
[`docs/specs/test-architecture.md`](../../docs/specs/test-architecture.md).

## What this gate is for

It exists to prove that the incremental-canonical-AST rewrite does not change
`source -> canonical AST` output. That single purpose decides everything about
its shape, and two things follow that a conventional coverage gate gets the
other way round.

**Only suites that assert parse output count.** The C producer runs `spec`,
`extensions`, `regression`, `conformance`, `equivalence`, and `pathological`.
A covered branch is one whose behaviour a golden assertion would catch
changing; an uncovered branch is behaviour the rewrite can alter with nothing
failing. Suites that execute the parser without asserting its output — `api`,
`facade`, `consumer`, `fuzz`, `packaging` — are excluded, because they raise
the number and protect nothing.

**`unpinned` is a map of unprotected behaviour, not a backlog of unwritten
tests.** It is not a statement that the current numbers are acceptable, and it
must never be used as one.

## The gate

Every measured source file is 100% covered on lines, functions, and branches.
Four rules make that enforceable on a codebase that is not there yet:

1. a measured file with **no `unpinned` entry must be at 100%** — that is the
   gate every file added from now on meets by construction;
2. an **`unpinned` entry is an upper bound** on that file's unpinned counts, so
   the unprotected surface may only shrink;
3. an **`exempt` entry removes a file from measurement** and is legitimate only
   when no source input can reach the file at all; and
4. a platform that **cannot produce a metric declares it** in
   `unsupportedMetrics` with a reason.

Rule 2 is a bound rather than an equality because the same source compiles
differently across the operating systems the required jobs run on: demanding an
exact match would turn an unrelated platform difference into a coverage
failure. Entries that have gone loose are reported on every run and rewritten
by `--update-ledger`.

Two things rule 3 is **not** a licence for:

- **Generated code is not exemptible.** Nobody reviews it, so its behaviour
  needs pinning more than hand-written code, not less. An earlier revision of
  this policy exempted the re2c scanners and the Unicode case-fold table on
  "it's generated" grounds; `core/scanners.c` alone turned out to be a third of
  the entire unpinned surface.
- **Code no input can reach is not an `unpinned` entry either.** It is a defect
  or dead code, and it needs a decision. Recording it here would promise a test
  that cannot be written.

## Anti-laundering rules

A coverage gate's real failure mode is not a low number, it is a high number
that measures nothing. Four checks close that:

- **`minimumMeasuredFiles`** — a floor on how many files the report covers. A
  file that vanishes from a report (an instrumenter that silently skips classes
  it cannot read, a filter that stops matching) would otherwise read as an
  improvement.
- **unresolvable paths fail the run** — a report naming a file the gate cannot
  place in the repository is refused rather than dropped.
- **a required metric with no counters at all fails** — an instrumenter that
  quietly stops emitting branch data would otherwise pass every per-file rule
  while proving nothing.
- **a `-summary-only` llvm-cov report is refused** — it charges every branch of
  a function to the file that function starts in, so a generated table included
  into a function body lands on its host while reporting 0/0 itself. That is
  not less detail, it is wrong in the flattering direction.

## Shrinking the unpinned surface

Only one kind of work does it: **adding corpus inputs whose canonical dump is
frozen.** Every input added to `packages/markdown-core/tests/fixtures/` or
`specs/canonical-ast/` pins whatever behaviour it reaches, permanently, and
independently of which implementation produces it.

Two kinds of work look like progress and are not:

- a test that executes the parser without asserting its output raises the
  number while protecting nothing; and
- a test written against today's internals is deleted by the rewrite along with
  the internals.

Both leave the gate reporting protection it does not have, which is worse than
leaving the number where it is. Priority follows the measurement rather than
the milestone order — the largest unprotected surfaces, the schedule, and the
rewrite's acceptance mechanism are in
[`docs/migration/2026-08-01-incremental-canonical-ast-plan.md`](../../docs/migration/2026-08-01-incremental-canonical-ast-plan.md).

## Running a platform

```sh
pnpm coverage:swift-macos
pnpm coverage:kotlin-jvm
pnpm coverage:es-node
```

Each producer builds an instrumented tree, runs its suites, and hands the
toolchain-native report to `scripts/check-coverage.mjs`, which normalizes it
and applies this policy.

These platforms run their full suites rather than a pinning selection.

There is no `c-host` platform. It ratcheted line and branch counts per C
file, and those files are exactly what a Rust engine replaces, so the ledger
could not outlive the rewrite it existed to protect. What protects the engine
across that rewrite is the `source -> AST` corpus — spec, pathological,
entity, extension-order, equivalence, fuzz, and both parity oracles — none of
which mentions a C symbol.

## Recording an improvement

```sh
sh scripts/coverage-swift-macos.sh --update-ledger
```

This rewrites `policy.json` with the exact current numbers and drops the entry
for every file that reached 100%. Review the diff: it is the burn-down record.
`.prettierignore` excludes `policy.json` because this command writes it —
reformatting it by hand would put every update in permanent conflict with the
gate that produces it.
