# Incremental canonical AST benchmark baseline

This is a migration/history document. It pins the four platform benchmark
suites on the tree **before** any incremental-canonical-AST milestone extends
the C-layer AST, as §14.7 of
[`../specs/incremental-canonical-ast.md`](../specs/incremental-canonical-ast.md)
requires and as the delivery plan's prerequisite
([`2026-08-01-incremental-canonical-ast-plan.md`](2026-08-01-incremental-canonical-ast-plan.md))
schedules before M2. M2.5 introduces concrete records and M3/M4 introduce
extents and `CanonicalText`; all three move allocation, traversal, and copy
counts, so a baseline recorded after any of them would already contain part of
the new engine, and M9's paired comparison would measure the new engine
against itself.

Nothing in this document is a new workload, harness, or assertion. It records
what the four existing commands produce on the pinned commit, the way
[`2026-07-11-phase-0-baseline.md`](2026-07-11-phase-0-baseline.md) records its
environment and figures, so that a later run on the same machine is comparable
rather than merely similar.

## Source identity

- Baseline commit: `157ec7fb86039134968d77244f1b9742b73da7ab`.
- Commit date: 2026-08-03.
- Commit subject: `Resync the delivery plan with three rounds of contract
  revision (#82)`.
- Branch state: local `main` equal to `origin/main`, working tree clean; every
  run below executed on exactly this tree.
- Delivered milestones at this commit: M0 (coverage gate) and M1
  (incremental-path coverage). No milestone that touches the C-layer AST has
  landed.

## Measurement environment

All four suites ran on one machine, sequentially, on 2026-08-03, with no other
benchmark or build running concurrently. Numbers without this table are not
comparable; a rerun for comparison must record the same fields.

| Component | Value |
| --- | --- |
| Host | macOS 26.6 (build 25G72), arm64 |
| Hardware | Apple M5 Max, 18 cores, 128 GiB RAM |
| Xcode | 26.6 (17F113) |
| CMake | 4.4.0 |
| C/C++ compiler | Apple clang 21.0.0 (`clang-2100.1.1.101`) |
| Swift | 6.3.3 (`swiftlang-6.3.3.1.3`), swift-driver 1.148.6, target `arm64-apple-macosx26.0` |
| Gradle | 9.6.1 (launcher JVM: Homebrew OpenJDK 26.0.2) |
| Gradle daemon JVM | Java 26, pinned by `gradle/gradle-daemon-jvm.properties` |
| Kotlin | 2.3.21 |
| Node.js | v26.5.0 (npm 11.17.0, pnpm 11.7.0) |
| Emscripten | 4.0.23, repository-managed at `.tools/emsdk/4.0.23` |
| Git | 2.55.0 |
| Python | 3.14.6 |
| GNU Make | 3.81 |

## How the numbers were produced

The four commands, exactly as `package.json` defines them at the baseline
commit:

```sh
npm run benchmark:c-host      # cmake --preset default && cmake --build --preset default --parallel && ctest --preset benchmark
npm run benchmark:swift-macos # CLANG_MODULE_CACHE_PATH=build/swift-module-cache swift run --disable-sandbox -c release MarkdownCoreBenchmarks
npm run benchmark:kotlin-jvm  # scripts/gradle.sh :packages:kotlin-markdown-core:kotlinBenchmark
npm run benchmark:es-node     # node packages/es-markdown-core/scripts/build.mjs && node packages/es-markdown-core/scripts/benchmark.mjs
```

Each suite was executed once; every per-case figure is the harness's own
median over its internal warmup/repeat counts, stated per platform below. The
phase-0 baseline, by contrast, recorded the median of three whole-suite runs
(`NUMRUNS=3`); a rerun that wants tighter suite-level statistics must say so
next to its numbers.

Two operational notes, neither of which changes what is measured:

- The CMake Release preset was configured and built once before
  `benchmark:c-host` ran, so the command's own configure/build steps were
  no-ops and the `ctest --preset benchmark` timing ran against a warm build
  directory. The benchmark preset itself is serial (`jobs: 1`, every workload
  `RUN_SERIAL`).
- `emcc` is not on this machine's default `PATH`. `benchmark:es-node` ran with
  the repository-managed toolchain prepended:
  `PATH=.tools/emsdk/4.0.23/upstream/emscripten:$PATH` — the same Emscripten
  version CI pins via `setup-emsdk`.

All timing assertions in these suites are relative-scaling ratios
(`bench_runner` fails a doubling step above 4.0×; the Kotlin and ES scaling
workloads fail above a normalized slowdown of 4.0×). The only absolute time
limits are CTest's hang-guard timeouts on `c-host` (600 s per workload, 60 s
for the corpus guard), four orders of magnitude above the observed medians; no
absolute performance threshold exists, so the figures below are trend records,
not gates.

## `c-host`

`ctest --preset benchmark`: 8 of 8 tests passed (the seven timed workloads
plus `benchmark_corpus_guard`, the corpus/workspace policy guard, which is not
a timed workload). Harness: `tests/runners/bench_runner.c`; every case is
median of 5 repeats after 1 warmup parse, one-shot
`markdown_core_document_parse` per repeat.

### `binding_baseline`

The shared cross-runtime unit document (a 79-byte section repeated 2,000
times), also reported with peak RSS for cross-runtime comparison. The other
three runtimes parse this same 158,000-byte input in their `large_document`
workload.

```text
benchmark case=binding_baseline bytes=158000 repeats=5 warmup=1 median_ms=3.907
baseline runtime=c boundary=native_parse workload=representative_large workload_version=1 bytes=158000 warmup=1 repeats=5 median_ns=3907000 peak_rss_kib=9024
```

### `representative` — the 26 tracked samples

Each sample repeated 200 times, as in the phase-0 baseline.

| Case | Bytes | Median ms |
| --- | ---: | ---: |
| `block-bq-flat.md` | 49200 | 0.654 |
| `block-bq-nested.md` | 71000 | 0.797 |
| `block-code.md` | 14600 | 0.085 |
| `block-fences.md` | 14400 | 0.119 |
| `block-heading.md` | 24600 | 0.483 |
| `block-hr.md` | 14800 | 0.227 |
| `block-html.md` | 56600 | 0.447 |
| `block-lheading.md` | 22600 | 0.272 |
| `block-list-flat.md` | 105000 | 2.844 |
| `block-list-nested.md` | 78200 | 2.304 |
| `block-ref-flat.md` | 94800 | 1.547 |
| `block-ref-nested.md` | 51400 | 1.889 |
| `directive.md` | 223600 | 4.500 |
| `inline-autolink.md` | 111400 | 0.975 |
| `inline-backticks.md` | 12800 | 0.153 |
| `inline-em-flat.md` | 31000 | 0.908 |
| `inline-em-nested.md` | 31600 | 0.708 |
| `inline-em-worst.md` | 34600 | 0.569 |
| `inline-entity.md` | 65600 | 0.776 |
| `inline-escape.md` | 37000 | 1.072 |
| `inline-html.md` | 103200 | 1.554 |
| `inline-links-flat.md` | 146000 | 1.488 |
| `inline-links-nested.md` | 62600 | 1.768 |
| `inline-newlines.md` | 28000 | 0.618 |
| `lorem1.md` | 757800 | 2.755 |
| `rawtabs.md` | 59000 | 0.431 |

### Scaling workloads

Each generator measured at doubling scales; every step is its own raw number.

| Case | Bytes | Median ms |
| --- | ---: | ---: |
| `large_document@128` | 1476224 | 22.978 |
| `large_document@256` | 2952448 | 46.046 |
| `large_document@512` | 5904896 | 93.246 |
| `deep_nesting@8192` | 16385 | 0.464 |
| `deep_nesting@16384` | 32769 | 0.977 |
| `deep_nesting@32768` | 65537 | 1.999 |
| `extensions@100` | 111800 | 2.175 |
| `extensions@200` | 223600 | 4.486 |
| `extensions@400` | 447200 | 8.982 |
| `large_table@10000` | 1050092 | 23.435 |
| `large_table@20000` | 2100092 | 50.213 |
| `large_table@40000` | 4200092 | 103.247 |
| `adversarial_links@16384` | 81920 | 4.596 |
| `adversarial_links@32768` | 163840 | 9.374 |
| `adversarial_links@65536` | 327680 | 18.400 |
| `adversarial_emphasis@16384` | 65536 | 2.279 |
| `adversarial_emphasis@32768` | 131072 | 4.751 |
| `adversarial_emphasis@65536` | 262144 | 9.542 |

The generators, for later re-derivation: `large_document` is the concatenation
of all 26 samples repeated at the given scale (the harness comment sizes the
×512 step "roughly" to the retired 11 MB Pro Git corpus; the generated input
measures 5,904,896 bytes); `deep_nesting` is `"> "` repeated at the given
depth plus one leaf byte; `extensions` is `directive.md` repeated;
`large_table` is an 8-column GFM table with the given row count;
`adversarial_links` is `"[a](b"` repeated; `adversarial_emphasis` is `"*a_ "`
repeated.

## `swift-macos`

Harness: `Benchmarks/MarkdownCoreBenchmarks/main.swift`; warmup 3, repeats 10,
`ContinuousClock`, peak RSS from `getrusage`. The reported value is
`durations[5]`, the upper-middle of the 10 sorted samples — an even repeat
count has no exact median. Raw output, verbatim:

```text
benchmark runtime=swift boundary=native_parse_and_value_copy workload=large_document workload_version=1 bytes=158000 warmup=3 repeats=10 median_ns=5588750 peak_rss_kib=26288
benchmark runtime=swift boundary=native_parse_and_value_copy workload=deep_nesting workload_version=1 bytes=261 warmup=3 repeats=10 median_ns=33916 peak_rss_kib=26432
benchmark runtime=swift boundary=native_session_scope_materialization workload=deep_scope_materialization workload_version=1 bytes=8197 depth=4096 warmup=3 repeats=10 median_ns=64250 peak_rss_kib=28352
benchmark runtime=swift boundary=session_stream_and_delta_decode workload=streamed_document workload_version=1 bytes=39500 commits=500 warmup=3 repeats=10 median_ns=7431791 peak_rss_kib=28880
benchmark runtime=swift boundary=session_stream_and_delta_decode workload=deep_path_edit workload_version=1 bytes=10002 commits=1 warmup=3 repeats=10 median_ns=1169042 peak_rss_kib=29792
```

## `kotlin-jvm`

Harness: `src/jvmBenchmark/.../Benchmark.kt`; plain workloads are median of 5
after 1 warmup; the two `deep_*` scaling workloads warm up twice and take the
median of 3 samples, each sample averaged over at least 100 ms of iterations,
and assert normalized slowdown ≤ 4.0 between depth 1,024 and 8,192. Raw
output, verbatim:

```text
benchmark runtime=kotlin boundary=jni_parse_and_value_copy workload=large_document workload_version=1 bytes=158000 warmup=1 repeats=5 median_ns=7739209 heap_used_kib=22442 heap_committed_kib=196608 rss_kib=145936
benchmark runtime=kotlin boundary=jni_parse_and_value_copy workload=deep_nesting workload_version=1 bytes=261 warmup=1 repeats=5 median_ns=66416 heap_used_kib=25377 heap_committed_kib=196608 rss_kib=147760
benchmark runtime=kotlin boundary=jni_parse_and_value_copy workload=deep_one_shot_parse workload_version=1 bytes=16389 depth=8192 warmup=2 repeats=3 median_ns=1730200 scale_from_depth=1024 normalized_slowdown=1.5591020343407092 heap_used_kib=35005 heap_committed_kib=589824 rss_kib=582848
benchmark runtime=kotlin boundary=jni_session_first_commit_and_delta_decode workload=deep_first_commit workload_version=1 bytes=16389 depth=8192 warmup=2 repeats=3 median_ns=1189653 scale_from_depth=1024 normalized_slowdown=1.4662020195301224 heap_used_kib=118372 heap_committed_kib=589824 rss_kib=584480
benchmark runtime=kotlin boundary=jni_session_scope_materialization workload=deep_scope_materialization workload_version=1 bytes=8197 depth=4096 warmup=1 repeats=5 median_ns=396792 heap_used_kib=133553 heap_committed_kib=589824 rss_kib=584528
benchmark runtime=kotlin boundary=jni_session_stream_and_delta_decode workload=session_stream_flat workload_version=1 bytes=30800 commits=800 warmup=1 repeats=5 median_ns=2847166 heap_used_kib=164186 heap_committed_kib=589824 rss_kib=584912
```

## `es-node`

Harness: `packages/es-markdown-core/scripts/benchmark.mjs` against the wasm
build; median of 5 after 1 warmup; `many_sibling_relink` is the JS-side relink
complexity gate (median of 3 samples, each averaged over at least 100 ms,
normalized slowdown ≤ 4.0 between widths 1,024 and 8,192) and prints under the
`complexity` prefix. Raw output, verbatim:

```text
benchmark runtime=es boundary=wasm_parse_and_value_copy workload=large_document workload_version=1 bytes=158000 warmup=1 repeats=5 median_ns=10407000 peak_rss_kib=176832 rss_kib=168240
benchmark runtime=es boundary=wasm_parse_and_value_copy workload=deep_nesting workload_version=1 bytes=261 warmup=1 repeats=5 median_ns=64916 peak_rss_kib=176832 rss_kib=168448
benchmark runtime=es boundary=wasm_session_scope_materialization workload=deep_scope_materialization workload_version=1 bytes=8197 depth=4096 warmup=1 repeats=5 median_ns=277459 peak_rss_kib=176832 rss_kib=173152
benchmark runtime=es boundary=wasm_session_stream_and_delta_decode workload=streamed_document workload_version=1 bytes=39500 commits=500 warmup=1 repeats=5 median_ns=15648917 peak_rss_kib=186368 rss_kib=186368
benchmark runtime=es boundary=wasm_session_stream_and_delta_decode workload=fan_out_narrow_edit workload_version=1 bytes=30000 commits=1 warmup=1 repeats=5 median_ns=34110 peak_rss_kib=192096 rss_kib=192096
complexity runtime=es boundary=js_bubbled_parent_relink workload=many_sibling_relink width=8192 scale_from_width=1024 median_ns=44075 normalized_slowdown=1.2144029683324877 checksum=9216
```

## §14.7 category coverage

§14.7 names four benchmark classes to pin: **representative**,
**large-document**, **extension**, and **adversarial**. Every workload above,
classified:

### `c-host`

| Workload | §14.7 class |
| --- | --- |
| `representative` (26 samples) | representative |
| `binding_baseline` | representative (the shared cross-runtime unit document) |
| `large_document` (1.5–5.9 MB) | large-document |
| `large_table` (1.1–4.2 MB) | large-document, over an extension construct (GFM table) |
| `extensions` (directive.md ×100/200/400) | extension |
| `adversarial_links`, `adversarial_emphasis` | adversarial |
| `deep_nesting` (depth 8,192–32,768) | adversarial (pathological structure depth, not a document class) |

All four classes are covered on `c-host`.

### `swift-macos`, `kotlin-jvm`, `es-node`

| Workload | Platforms | §14.7 class |
| --- | --- | --- |
| `large_document` (158 KB unit document) | all three | representative — despite the name; see the gap note below |
| `deep_nesting` (depth 128) | all three | none of the four: structure-depth stress at depth 128, far below the 8,192–32,768 range that makes `c-host`'s `deep_nesting` adversarial |
| `deep_one_shot_parse` (depth 1,024→8,192) | kotlin | adversarial (structure depth): a one-shot parse of the same `"> "`-nesting construct, at the same depth, as `c-host`'s `deep_nesting@8192` |
| `deep_first_commit` | kotlin | none of the four: session-boundary cost (first commit + delta decode), over the same adversarial-depth input |
| `deep_scope_materialization` (depth 4,096) | all three | none of the four: session-boundary cost (scope materialization) |
| `streamed_document` / `session_stream_flat` | swift, es / kotlin | none of the four: session streaming cost |
| `deep_path_edit` / `fan_out_narrow_edit` | swift / es | none of the four: localized-commit cost |
| `many_sibling_relink` | es | none of the four: JS-side relink complexity gate, no parser involved |

The session-boundary workloads are not one of §14.7's four parser-benchmark
classes, but they are exactly the "localized commit time" and boundary-cost
measurements the same section requires the pinned workloads to report later,
so they are pinned here with everything else.

### Gaps, stated as found

Per the prerequisite's own rule, no workload was created or modified to fill
these; they are recorded as-is:

- **Extension: empty on all three binding platforms.** No Swift, Kotlin, or
  ES workload parses an extension construct. Extension coverage exists only on
  `c-host`: the `extensions` workload, `large_table`'s table construct, and
  the `directive.md` and `inline-autolink.md` samples inside the
  `representative` corpus.
- **Adversarial: what exists today splits into two families, and the bindings
  have at most one of them.** The pathological-inline family (`[a](b`,
  `*a_ `) runs only on `c-host`. The structure-depth family — the `"> "`
  nesting that makes `c-host`'s `deep_nesting` adversarial at depths
  8,192–32,768 — exists on exactly one binding: Kotlin's scaling pair parses
  depth 8,192, equal to `c-host`'s smallest adversarial step. Swift and ES
  reach depth 128 only.
- **Large-document: MB-scale inputs exist only on `c-host`.** The binding
  workloads named `large_document` parse the 158,000-byte shared unit
  document — the name overstates the size by ~37× relative to `c-host`'s
  largest step.
- **Representative: one document on the bindings.** `c-host` runs the
  26-sample corpus; each binding runs a single synthetic unit document. It is
  the same input across all four runtimes, which is what makes the
  cross-runtime comparison in `binding_baseline` meaningful, but it is one
  document, not a corpus.

## What invalidates this baseline

- Any benchmark run recorded after M2.5, M3, or M4 land contains part of the
  new engine; M9's paired comparison must compare against this document, not
  against a rerun on a later tree.
- These figures are machine-specific. A comparison run must use the same
  machine and record the same environment table; §14.7's comparisons are
  paired same-trace runs, never cross-machine.
- If a later commit changes any workload generator, scale, repeat count, or
  harness, the affected rows stop being comparable and the change must be
  recorded next to the new numbers rather than silently absorbed.
