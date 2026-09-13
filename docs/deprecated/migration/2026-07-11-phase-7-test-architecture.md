# Phase 7: freeze the repository-wide test architecture and consolidate CTest suites

Status: complete. The historical CTest migration and test-routing ownership
revision are implemented. The frozen contract is in
`docs/specs/test-architecture.md`; this document records the migration inventory,
key decisions, and validation results.

> Phase 18 moves the sole canonical AST conformance corpus to
> `specs/canonical-ast/`. It is runner-free product contract data, not a root
> `tests/` directory or shared harness. Package ownership of ordinary correctness
> fixtures, native runner discovery, and mutual exclusion of correctness,
> conformance, and benchmark remain unchanged.

> The Phase 17 audit showed that the historical flat `test:<lang>` entry points
> lacked real platform coverage. The first correction then overexpanded into a
> language → platform → suite task matrix. The final revision narrows root pnpm
> ownership to task family → execution platform; suite discovery/filtering stays
> with native runners. The CTest migration conclusions remain valid, and both
> routing implementation and Phase 7 are closed. Actual required-CI platform
> execution evidence belongs to Phase 19 acceptance and does not retroactively
> block the test-architecture phase.

## 1. Remove the Pro Git benchmark

- Remove the Makefile targets `progit`, `bench` (old implementation), `newbench`,
  `directive-bench`, `leakcheck`, `fuzztest`, and `operf`, together with
  `BENCHFILE`/`benchinput.md` and `$(ALLTESTS)` generation/dependency/cleanup rules.
- Remove `benchmarks.md` (Pro Git instructions and historical comparison tables),
  `packages/markdown-core/benchmarks/stats.py`, and `statistics.py`.
- Remove `.gitignore` entries for `progit/`, `benchinput.md`, `alltests.md`, and
  `afl_results/`; write AFL campaign output under build instead.
- `packaging_corpus_guard`/`benchmark_corpus_guard` (CTest) and
  `scripts/audit-test-topology.sh` continually assert that these paths are absent
  and not hidden by gitignore. Large-input coverage moves to
  `benchmark_large_document`, which repeats tracked sample blocks to roughly
  the historical 11MB corpus size. It is fully offline, deterministic, and
  redistributable.

## 2. C test inventory: old-to-new mapping

The old graph of 24 Python/ctypes/CLI-driven tests migrated completely to native
CTest suites: 57 correctness tests + 6 benchmarks, with no Python, network
access, or fallback skips.

| Old test (runner) | New test (label) | Notes |
| --- | --- | --- |
| `api_test` (native) | `api_engine` (api) | Retained, registration renamed |
| `facade_test` (native) | `facade_native` (facade) | Retained |
| `facade_cplusplus_test` (native) | `consumer_facade_cplusplus` (consumer) | Retained |
| `canonical_ast_cli` (ast_dump_tests.py) | `facade_dump_cli` (facade) | `dump_cli_runner` drives CLI `-t ast`; glob is the single source of the fixture list |
| `html_normalization` (doctest normalize.py) | Removed | Normalizer removed; roundtrip uses documented whitespace folding outside `<pre>` |
| `spectest_library`/`spectest_executable` (spec_tests.py) | `spec_commonmark` (spec) | In-process library calls through `spec_runner`, byte-exact |
| `smartpuncttest_executable` | `spec_smart_punctuation` (spec) | |
| `entity_library` (entity_tests.py) | `spec_entities` (spec) | `entity_runner` directly includes `#include "entities.inc"` |
| `roundtriptest_library` (roundtrip_tests.py) | `spec_roundtrip_commonmark` (spec) | commonmark→HTML roundtrip with whitespace-folded comparison |
| `extensions_executable` | `extensions_gfm` (extensions) | |
| `formula_github_compatibility_executable` | `extensions_formula_github` (extensions) | |
| `formula_option_gates_executable` (`-t xml`) | `extensions_formula_option_gates` (extensions) | spec_runner `--mode xml` |
| `formula_latex_compatibility_executable` | `extensions_formula_latex` (extensions) | |
| `formula_conflicts_executable` | `extensions_formula_conflicts` (extensions) | |
| `directive_extension_executable` | `extensions_directive` (extensions) | The former normalized comparison was already byte-exact in practice; now explicitly byte-exact |
| `directive_option_gates_executable` (`-t xml`) | `extensions_directive_option_gates` (extensions) | |
| `roundtrip_extensions_executable` | `extensions_roundtrip_gfm` (extensions) | |
| `option_table_prefer_style_attributes` | `extensions_option_table_style` (extensions) | |
| `option_full_info_string` | `extensions_option_full_info_string` (extensions) | Fixture contains NUL bytes; runner passes byte lengths |
| `regressiontest_executable` | `regression_commonmark` (regression) | |
| `pathological_tests_library` (21 cases, one process) | `pathological_<case>` ×21 (pathological) | Register each case separately with CTest TIMEOUT 30s; port regex assertions to repeated-segment matching |
| `directive_pathological_executable` (5 cases + 6 original scaling cases) | `pathological_directive_*` ×5, `pathological_complexity_*` ×8 | Complexity coverage expands to 6 directive scanner/attribute cases plus 2 reference-map cases, with 4 KiB → 128 MiB endpoint cost per byte, a 2.0× rejection threshold, and `RUN_SERIAL` |
| `inline_delimiter_stack_tests_executable` (4 checks) | `pathological_formula_*` ×4 | |
| None; fuzztest used /dev/urandom | `fuzz_smoke` (fuzz) | Deterministic fixed-seed xorshift generation + tracked corpora; parse/traverse/dump×2/free, asserting dump determinism |
| None | `packaging_corpus_guard` (packaging), `benchmark_corpus_guard` (benchmark) | Corpus/workspace policy guards |
| `make bench`/`newbench`/`directive-bench` (Make+Python stats) | `benchmark_*` ×5 (benchmark) | `bench_runner`: representative/large_document/deep_nesting/extensions/adversarial; 1 warmup + median of 5 repeats; assertions only on relative doubling ratios |

All runners reuse native test support in `packages/markdown-core/tests/support/`:
fixture loading, spec parsing, in-process conversion, UTF-8 byte comparisons,
deterministic diffs, segment matching, PRNG, and a monotonic clock. There is no
duplicated subprocess/ctypes glue.

## 3. Key decisions

1. **Whitespace folding in roundtrip comparisons.** The old Python normalizer
   performed HTML-level normalization only to absorb whitespace differences
   from the commonmark writer in roundtrip suites: flattened softbreaks in
   setext headings and code-span padding, affecting 5 cases in spec.txt. The
   native implementation uses documented deterministic canonicalization,
   folding whitespace runs outside `<pre>` on both sides before comparison.
   Every non-roundtrip suite is byte-exact. The old directive suite nominally
   normalized output but was already byte-exact in practice and now requires it.
2. **`-e footnotes` semantics.** As in the CLI, it maps to the `OPT_FOOTNOTES`
   option bit rather than attaching an extension. `OPT_DIRECTIVE` implicitly
   attaches the directive extension, reproducing CLI `attach_option_extensions`.
3. **Porting pathological regex assertions.** The old assertions were anchored
   regexes made of repeated literals. They migrate to `ts_match_segments`, which
   matches consecutive repeated literal segments. `backticks` uses a character
   class scan; exact directive expectations use complete string comparison.
   None of the assertions is weakened.
4. **Real Swift suites.** `pnpm test:swift` changes from `swift build` to
   `swift test`, tools-version rises to 6.0, and a Swift Testing target,
   `MarkdownCoreTests` in `packages/swift-markdown-core/Tests/`, adds 10 tests in
   `api`/`errors`/`unicode`/`ownership` suites that directly test the C facade
   through the SwiftPM module. `swift build` remains a separate
   `pnpm build:swift` entry point.
5. **A facade defect found for Phase 10 to fix.** Concurrent first calls to
   `markdown_core_document_parse` race in `markdown_core_register_node_flag`.
   The entire initial core-extension registration transaction is not thread-safe:
   extension node-type/flag allocation and registry mutation are process-global
   mutable state. Parallel Swift Testing exposes this as a crash. Tests
   temporarily use a thread-safe one-time warmup. The new C defect-closure
   Phase 10 must fix registry initialization and lifecycle, add native
   concurrency/TSan regressions without warmup, and remove the workaround before
   Phase 11 Swift binding work starts.

## 4. Validation

- `ctest --preset correctness`: 57/57 passed (labels: api 1, facade 2, consumer 1,
  spec 4, extensions 10, regression 1, pathological 36, fuzz 1, packaging 1).
- `ctest --preset benchmark`: 6/6 passed (guard + 5 workloads, serial).
- `swift test`: 4 suites / 10 tests passed.
- `scripts/audit-test-topology.sh`: all checks passed: no .py runners, Python
  CMake dependencies, corpus residue, or runtime network; pnpm routing exactly
  matches the contract; every label is nonempty; no disabled tests;
  correctness/benchmark are disjoint; CMake registrations match runner `--list`;
  Swift suites are nonempty.
- `make test`/`make bench` delegate to correctness/benchmark presets.
  ASan/UBSan reuse the graph through `asan`/`ubsan` presets. CI (`ci.yml`) uses
  presets throughout, and a separate `benchmark.yml` schedules benchmarks
  weekly and on manual dispatch.

## 5. Routing contract revision

The revised Phase 7 tasks replace flat routing and the subsequent suite-task
matrix with these boundaries:

- Root exposes only three peer task families: `test:<platform>`,
  `conformance:<platform>`, and `benchmark:<platform>`. Platform names include
  the language. Language aggregation, suite suffixes, `:full` aliases, root
  suite matrices, and generic family routers are prohibited.
- Platform leaves cover the C host/compiler matrix, Swift macOS/iOS Simulator,
  Kotlin JVM/Android host/Android emulator/macOS ARM64/Linux x64, and ES
  Node/browser.
- Suite/case listing and filtering belong only to CTest, Swift Testing/
  `xcodebuild`, Gradle/instrumentation, and the ES package runner. pnpm and CI
  do not duplicate suite lists.
- `test` runs traditional correctness only. `conformance` is a peer
  contract/spec/schema target family, not implicitly included by `test`.
  `benchmark` owns timing, throughput, scaling, and performance baselines only.
- There is no public `stress` task. Large/deep/repeated inputs register as
  `robustness` cases under correctness and as separate timed workloads under
  benchmark, with distinct purposes and acceptance evidence.

The topology audit must reject suite-level pnpm tasks and check platform entry
points, named native targets, and CI destinations. The generic router from the
first revision has been removed, and required CI maps every declared platform,
so both routing implementation tasks and Phase 7 are closed. Actual remote
execution on each platform, failure rather than silent skipping when a
destination is missing, and passing Linux x64/repository-managed Android
emulator evidence belong to Phase 19 acceptance. Linux x64 has no local
container/emulation substitute entry point.

Local revision evidence: C correctness 57/57 and conformance 2/2 passed, as did
separate large-document/deep-nesting benchmarks. Swift macOS and iOS Simulator
each passed 5 `MarkdownCoreTests` correctness tests and 2
`MarkdownCoreConformanceTests`. Paired Kotlin JVM, Android host, and macOS ARM64
correctness/conformance tasks passed; JUnit reports show `AstTest` only in the
conformance target. ES Node passed 9 correctness tests, 2 conformance tests,
browser correctness, and two benchmark workloads. The topology audit passed.
The Gradle DSL pins Pixel 10 Pro XL/64-bit Google APIs Managed Devices; root
entry points separately and sequentially invoke
`markdownCoreApi36Page4kAndroidDeviceTest` and
`markdownCoreApi36Page16kAndroidDeviceTest`, provisioning API 36 4 KB/16 KB system
images. Each emulator passed 10 correctness and 2 conformance tests, totaling
20 correctness and 4 conformance executions; subsequent runs can reuse
configuration/device caches. Entry points read neither existing Android Studio
AVDs nor serials; required CI uses the same routes. Linux x64 is not emulated
locally and is accepted by the Phase 19 required-CI `ubuntu-latest` platform job.
GMD automatically stops running instances and restores a clean snapshot.
`clean:kotlin-android-emulator` is an explicit, on-demand maintenance task for
managed AVD caches, not part of correctness/conformance lifecycles.

## 6. Closure re-audit

This review separately assessed architecture implementation and remote CI
execution:

- Closed: the execution-platform routing revision; mutually exclusive native
  correctness/conformance/benchmark targets; separation of stress-shaped inputs
  into correctness robustness cases and benchmark workloads; CI
  platform/destination mappings; and Phase 7 overall.
- Transferred to Phase 19: passing required-CI correctness/conformance evidence
  for Kotlin Linux x64 and repository-managed Android emulators, plus
  fail-not-skip behavior when an execution destination is missing. Passing local
  Android GMD runs remain implementation evidence.
