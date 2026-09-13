# Paired PR benchmark

The benchmark answers one bounded question: how the exact PR head compares with
the PR event's exact base commit on one runner, for one unchanged input and one
measurement driver. It does not combine a head timing with a historical timing
from a different machine. The ordinary correctness CI remains independent.

## Build and process ownership

The read-only `pull_request` job checks out both SHAs with persisted credentials
disabled. The orchestrator exports each revision into a new temporary source
directory. Base is configured and built first, its public DSO is staged, and its
source/build directory is removed before head starts. Every build has its own
CMake cache and no compiler launcher; both use the same compiler and Release
`-O3 -DNDEBUG` configuration. Target-specific settings remain those of each
revision, with complete CMake caches and compile commands retained as provenance.
No build overlaps timing and no compiler/cache artifact is reused between lanes.

The C measurement driver is compiled once. Each observation starts a new process
which loads exactly one staged DSO through the public API. Input loading, dynamic
linking, canonical dumping and JSON serialization are outside the timer. A fresh
process prevents the other revision's allocator state and peak RSS from leaking
into an observation. The process exits before the next one starts. Both DSOs and
the driver are removed after measurement, including failure cleanup.

Linux observations are pinned to the first CPU allowed by the job's cpuset;
macOS local reproduction records affinity as unavailable. The pipeline does not
flush operating-system caches or require root. Its target is steady-state parsing,
so every process performs five unmeasured warmups before nine timed parse/free
pairs. Cold-start performance is a separate experiment.

## Input and samples

The 158,000-byte representative input remains the existing repeated heading,
paragraph, emphasis, link and Unicode workload. It is generated once and supplied
unchanged to both libraries. Its SHA-256, byte count, common driver source hash,
common executable hash and library hashes are recorded. Full canonical output
must agree before timing starts; a grammar/API incompatibility produces no
performance comparison. A changed input is identified by its hash, not a manually
maintained version number.

One block runs **base, head, head, base**. Twelve sequential blocks give 48 fresh
processes and 432 timed parse/free pairs. Both revisions appear equally often
early and late; geometric pairing cancels a linear trend in log time within a
block. All samples are retained. There is no outlier deletion, best-of selection,
or repeated rerun until a desired number appears.

The driver reads a monotonic clock before parse, after parse, and after free.
It stores parse and free durations separately; total is their sum for the same
iteration. Peak RSS is process-wide and includes the input and driver, so it is
not a measurement of live AST bytes alone. The report uses the median process
peak for each lane. Shared-library byte sizes are recorded separately.

## Estimation and limits

For each phase, every process contributes its median. Within an ABBA block, the
two paired head/base ratios are combined geometrically. The reported percentage
is the geometric mean across blocks; displayed base/head medians summarize raw
samples and need not divide to exactly that paired estimate.

A deterministic 10,000-resample percentile bootstrap resamples **whole ABBA
blocks**, preserving process grouping and within-block order. Its 95% interval
describes uncertainty among this job's observations. It is not a probability that
the code is faster, nor an estimate of variation across runner models. A range
crossing zero leaves the direction unresolved in this run. Sustained interference,
thermal effects or autocorrelation spanning multiple blocks can still bias the
result; CPU, affinity, OS image, compiler and before/after load averages are
retained to investigate that. A hosted runner is not a controlled performance lab.

Timing remains informational. Invalid output, failed builds, mismatched canonical
results or missing samples fail measurement; a slow timing does not fail ordinary
correctness CI. Algorithmic complexity and allocation bounds retain their native
deterministic tests. This one workload cannot establish overall parser optimality.

## Reporting and trust

The entire paired artifact is PR-controlled diagnostic data. Neither side becomes
a reusable trusted baseline. The measurement job has read-only permissions.
The separate `workflow_run` reporter checks out the immutable
`github.workflow_sha` of its default-branch workflow, never the PR revision, and
never builds or executes downloaded content. It validates artifact size, exact
schema, bounded raw arrays, balanced ordering, common driver identity, run id and
attempt, and both current PR SHAs. Derived statistics are recomputed by trusted
code; artifact strings are not executed or copied into Markdown. It checks both
SHAs again immediately before posting so an advanced base is not silently relabeled.

The paired artifact and build provenance remain on the workflow run. The producer
also writes the report to the job summary. On the PR that introduces this schema,
the old default-branch reporter will ignore the new artifact; the job summary is
available immediately, and automatic comments switch after the workflow change
merges. The old historical-baseline producer and its fallback builder are removed.

## Local reproduction

Both source directories must contain the requested Git objects; they can be the
same repository. The driver and orchestration code come from the current checkout.
The output directory contains raw comparison JSON, a rendered report, build logs,
CMake caches and compile commands. `--blocks 2` is available for a smoke check;
use the default twelve blocks for a reported experiment.

```sh
node scripts/pr-benchmark.mjs \
    --base-source . --base-sha FULL_BASE_SHA \
    --head-source . --head-sha FULL_HEAD_SHA \
    --output build/paired-benchmark
node --test scripts/tests/pr-benchmark.test.mjs
```
