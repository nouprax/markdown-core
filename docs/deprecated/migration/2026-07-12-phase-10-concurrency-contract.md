# Phase 10 migration report: close C defects and freeze the concurrency contract

This report records Phase 10's thread model, portable-once design, registry
lifecycle decision, fixes, test reproduction, sanitizer/TSan results,
platform/package audit, and workaround removal. Defect details, including
reproduction, root cause, impact, regressions, and closure evidence, are in
`2026-07-12-phase-10-defect-ledger.md`.

## 1. Frozen thread model

The complete public contract appears in the header comment of
`packages/markdown-core/include/markdown_core.h`:

1. **Initialization:** the library initializes itself through a process-wide
   once inside `markdown_core_document_parse`. Concurrent first calls are safe,
   without warmup, an external lock, or explicit initialization.
2. **Registry lifetime:** after the first successful registration, the extension
   registry is immutable for the process lifetime. There is no teardown or
   reinitialization path.
3. **Different documents:** parse/traverse/dump/free can run fully concurrently;
   parse calls share no mutable state.
4. **The same document:** after parse returns, the document and its nodes are
   logically immutable through this API. Concurrent read-only traversal,
   accessor calls, and dumping are safe. `markdown_core_document_free` is the
   only mutation; callers must make it mutually exclusive with every other
   access and perform no access afterward. Node handles and string views borrow
   from their owning document and expire with it.
5. **Error/dump ownership:** errors and dump buffers returned through out
   parameters belong to the caller and are released with
   `markdown_core_error_free`/`markdown_core_dump_free`, both accepting NULL.
6. The contract is self-contained: bindings must not depend on undocumented
   conventions.

## 2. Portable-once design

`core/once.{h,c}` is internal and does not enter the public API:

- POSIX (macOS, Linux, Android, Emscripten): `pthread_once_t` + `pthread_once`.
- Windows (MSVC, MinGW-w64): `INIT_ONCE` + `InitOnceExecuteOnce`. A union carries
  the function pointer through the `PVOID` parameter, avoiding C99 restrictions
  on converting between function and object pointers.
- Both primitives guarantee exactly one callback execution and visibility of
  all its writes to every thread passing through once (happens-before).
  Node-type counters, node flags, and registry writes within the transaction
  are therefore visible to all parsing threads.
- The design meets the C99 baseline. CMake uses `find_package(Threads)` and
  links with `CMAKE_THREAD_LIBS_INIT`, a flag-only value that does not add an
  imported-target dependency to install(EXPORT).

`markdown_core_core_extensions_ensure_registered` wraps the **entire**
core-extension registration transaction in once. It does not merely lock
`markdown_core_register_node_flag`, and requires no consumer warmup.

## 3. Registry lifecycle decision

Decision: **freeze for the process lifetime and remove release paths**.

- Remove `markdown_core_release_plugins` from `registry.h`/`registry.c` and its
  CLI (`main.c`) call. That API is inherently disconnected from the once state
  (ledger MC10-02). The registry is a fixed descriptor set of a few KB created
  once per process; reclamation by the OS at process exit provides a lifecycle
  without races.
- `registry.h` states that registration occurs during initialization, before
  concurrent parsing. Registered extensions are immutable and live for the
  process lifetime. With no release/unregister operation, the API cannot
  represent "initialized flag true but registry already freed."
- Global registry pointers remain reachable, so LeakSanitizer does not classify
  them as leaks. The complete ASan suite passes.

## 4. Fixes and code changes

| Change | Files |
| --- | --- |
| Add portable once | `core/once.h`, `core/once.c`, added to core CMake, Package.swift, and Android JNI CMake source lists |
| Wrap registration in once | `extensions/core-extensions.c` |
| Remove release paths and freeze registry | `core/registry.{h,c}`, `core/main.c` |
| Make special/skip character tables parser-local (MC10-03) | `core/parser.h`, `core/inlines.{h,c}`, `core/blocks.c` |
| Publish thread contract | `include/markdown_core.h` |
| Add concurrency/lifecycle regressions | `packages/markdown-core/tests/runners/concurrency_runner.c`, `packages/markdown-core/tests/CMakeLists.txt` |
| Instrument the whole package for sanitizers and add Tsan build type | `packages/markdown-core/CMakeLists.txt`, `core/CMakeLists.txt` |
| Add tsan presets/targets/CI and the missing ubsan CI job | `CMakePresets.json`, `Makefile`, `.github/workflows/ci.yml` |
| Remove Swift warmup workaround | `packages/swift-markdown-core/Tests/MarkdownCoreTests/MarkdownCoreSuites.swift` |
| Synchronize frozen contract documentation | `docs/specs/test-architecture.md` |

Single-threaded parsing behavior is unchanged: every existing
spec/extensions/regression/pathological/fuzz golden matches byte for byte before
and after the fixes.

## 5. Tests and reproduction

Three CTest cases are added within the frozen label taxonomy, with no new label:

| Test | Label | Coverage |
| --- | --- | --- |
| `facade_concurrent_first_parse` | `facade` | A fresh process releases 8 threads from a barrier into their **first** parses, without warmup. Covers parse, extension attachment, traversal, dump, and free. Every dump matches a single-threaded reference computed after joining the threads, byte for byte |
| `facade_concurrent_stress` | `facade` | After initialization, runs 8 threads × 200 rounds × 6 inputs × 3 ParseOptions variants (default/minimal/split), deliberately mixing conflicting extension sets to protect parser-local character tables. Repeated parses within each thread must also agree |
| `regression_registry_lifecycle` | `regression` | Interleaves 2000 parse/free cycles with failure paths (NULL source). The final parse still attaches every extension and matches the first dump |

The runner is native C using POSIX pthreads or Win32 threads and its own
barrier, with no scripting language, network, or warmup. Platforms without TSan
run the same tests through the default preset rather than silently skipping.

**Defect-sensitivity checks** (temporary reversions, all restored):

- Temporarily reverting once to the old `static int registered` makes
  `facade_concurrent_first_parse` fail deterministically in a TSan build with
  `flag initialization error in markdown_core_register_node_flag` and abort.
- Temporarily reverting character tables to process-global storage makes
  `facade_concurrent_stress` report multiple TSan `data race` findings and
  functional differences in thread dumps.

## 6. Validation matrix (local macOS arm64, Xcode clang)

| Validation | Result |
| --- | --- |
| `ctest --preset correctness` (Release, shared) | 56/56 passed |
| `ctest --preset correctness-asan` (static) | 56/56 passed |
| `ctest --preset correctness-ubsan` (static, whole-package instrumentation) | 56/56 passed |
| `ctest --preset correctness-tsan` (static, new) | 56/56 passed |
| `swift test` (parallel, no warmup, real concurrent first calls) | 4 suites / 10 tests passed |
| C/C++ consumer (`consumer_facade_cplusplus`) | Passed in every preset above |
| Packaging guard (`packaging_corpus_guard`) | Passed |
| `scripts/audit-test-topology.sh` | All checks passed |
| `pnpm format:c:check` / `format:cmake:check` / `lint:c` / `check:contracts` | Passed |

CI adds `ubsan` and `tsan` jobs on ubuntu-latest/clang alongside the existing
default (shared/static × clang/gcc × ubuntu/macos/windows), ASan, and Swift jobs.
Windows has no TSan and runs the same concurrency regressions in the default
matrix.

## 7. Platform/package audit

- All four build routes compiling the C engine are synchronized with `once.c`:
  core CMake, SwiftPM (`Package.swift`), Android JNI
  (`android/src/main/cpp/CMakeLists.txt`), and nmake/appveyor, which delegates to
  CMake and has no separate source list.
- Public exports do not expand. `once.h`/`registry.h` remain internal;
  `exports/markdown_core.map` is unchanged because
  `markdown_core_release_plugins` was never exported. The only installed public
  header remains `include/markdown_core.h`.
- All callers of changed internal signatures such as
  `markdown_core_parse_inlines` migrate in the same commit, without compatibility
  shims or deprecated aliases.

## 8. Workaround removal

- Swift Testing's global facade warmup (`facadeWarmedUp` global `let` and the
  `#expect` before every parse) is removed. Parallel Swift tests now exercise
  real first calls directly.
- No other test-side serialization or binding-level lock exists in C. All three
  `concurrency_runner` cases run natively without warmup.
- No workaround is carried into Swift, Kotlin, or ES production bindings.
  Kotlin/ES bindings do not yet exist at this phase; Swift has only a test
  target, which has been cleaned up.

## 9. Acceptance

- [x] Concurrent first and subsequent facade parses need no warmup or external lock ✅ (first_parse/stress tests)
- [x] Registry initialization/release has no races or disconnected state ✅ (release path removed + lifecycle regression)
- [x] Native concurrency regressions, TSan where supported, ASan, UBSan, Release, shared/static, consumer, and package checks all pass ✅ (§6)
- [x] Every confirmed C defect known at the start of the phase is closed ✅ (ledger 4/4)
- [x] Swift test warmup is removed ✅ (§8)
- [x] Platform bindings can depend solely on the public C contract ✅ (§1 contract in the public header)
