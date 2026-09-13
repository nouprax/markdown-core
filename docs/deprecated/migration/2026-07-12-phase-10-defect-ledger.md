# Phase 10 C Defect Ledger

This ledger collects every C engine/facade defect confirmed by migration
reports, sanitizers, concurrency tests, and manual audits at the start of
Phase 10. Each entry records reproduction, root cause, impact, regression tests,
and closure evidence. Every fix is in C; no platform workaround is introduced.

Status: **4 defects, all closed**. No open C correctness, memory-safety,
thread-safety, or lifecycle defect blocks Phase 11.

---

## MC10-01 Unsynchronized core-extension registration during concurrent first parses

- **Source:** Phase 7 migration report §3.5, crashes during parallel Swift
  Testing execution.
- **Reproduction:** multiple threads in a fresh process simultaneously call
  `markdown_core_document_parse`. The unsynchronized `static int registered` in
  `markdown_core_core_extensions_ensure_registered` allows several threads to
  execute the registration transaction. Deterministic reproduction:
  `concurrency_runner --case first_parse`, releasing 8 threads from a barrier.
- **Root cause:** first registration performs three types of process-global
  mutation without synchronization: extension node-type counters
  (`MARKDOWN_CORE_NODE_LAST_BLOCK/_INLINE` through
  `markdown_core_syntax_extension_add_node`), node-flag allocation
  (`markdown_core_register_node_flag`, which calls `abort()` on duplicate
  registration), and registry-list appends.
- **Impact:** any multithreaded consumer's first parses can crash with
  `flag initialization error in markdown_core_register_node_flag`, register
  duplicate or inconsistent extensions, or allocate incorrect node types. All
  three binding platforms are exposed.
- **Fix:** add portable process-wide once in `core/once.{h,c}` using POSIX
  `pthread_once` and Windows `InitOnceExecuteOnce`, wrapping the **entire**
  registration transaction in `extensions/core-extensions.c`. No partial lock
  or consumer warmup requirement.
- **Regression:** `facade_concurrent_first_parse`, facade label, fresh process,
  no warmup, barrier-synchronized first parses, covering
  parse/attach/traverse/dump/free and byte-for-byte comparison to a
  single-threaded reference dump.
- **Closure evidence:** before the fix, the TSan build failed deterministically
  with `Subprocess aborted` and 4 flag-initialization aborts on stderr. After
  the fix, default/ASan/UBSan/TSan all pass. Swift Testing's global warmup is
  removed; parallel `swift test` with real concurrent first calls passes 10/10.

## MC10-02 `markdown_core_release_plugins` is disconnected from initialization state

- **Source:** Phase 10 specification audit, spec §Phase 10 task list.
- **Reproduction (code-path audit):** `markdown_core_release_plugins` frees and
  clears the registry but leaves registration's initialized flag true.
  Subsequent `markdown_core_document_parse` calls in the same process cannot
  find extensions in `attach_extension` and all fail with
  `required syntax extension is unavailable`. Overlap with a concurrent parse
  instead produces a registry-list free/read race and use-after-free.
- **Root cause:** once-only registration and repeatable release have independent
  lifecycles, allowing the invalid state "initialized flag true but registry
  already freed."
- **Impact:** the CLI teardown path (`main.c` previously called it before exit)
  and any future consumer calling the API as a library operation.
- **Fix:** freeze registry lifetime. After successful first registration,
  extension descriptors are immutable for the process lifetime for facade
  parsing. Remove `markdown_core_release_plugins` from `registry.h`/`registry.c`
  and its CLI caller. `registry.h` documents initialization-time registration
  with no release/unregister path. The OS reclaims storage at process exit;
  global pointers remain reachable, so LeakSanitizer does not report leaks.
- **Regression:** `regression_registry_lifecycle`, regression label, 2000
  parse/free cycles interleaved with failures. The final parse still attaches
  every extension and matches the first dump byte for byte.
- **Closure evidence:** the symbol is removed from source, headers, and callers,
  with no remaining repository-wide grep matches. The regression passes under
  default/ASan/UBSan/TSan. ASan reports no leaks in the CLI or any test on
  platforms with leak detection enabled.

## MC10-03 Per-parse process-global writes to `SPECIAL_CHARS`/`SKIP_CHARS`

- **Source:** newly confirmed in this phase's required audit of process-global
  mutable state on the facade parse path.
- **Reproduction:** `process_inlines`, called on every
  `markdown_core_parser_finish`, adds then removes characters in process-global
  `SPECIAL_CHARS[256]`/`SKIP_CHARS[256]` tables according to attached inline
  extensions. Concurrent parses with different extension sets, such as enabling
  versus disabling strikethrough, interfere. The special/emphasis-skip state of
  `~`/`$`/`:` changes during another parser's inline scanning and emphasis
  flanking checks, producing incorrect ASTs and TSan data races. Deterministic
  reproduction: `concurrency_runner --case stress` with mixed parse options.
- **Root cause:** per-parse mutable state is incorrectly process-global. The
  initialization boundary from MC10-01 cannot cover writes required on every
  parse for its option set.
- **Impact:** all concurrent parses **after** initialization, broader than
  MC10-01: silent AST errors in emphasis boundaries and extension inline
  construction, plus TSan data races.
- **Fix:** make tables parser-local. Add `special_chars[256]`/`skip_chars[256]`
  to `parser.h`; `markdown_core_parser_reset` initializes them from immutable
  base tables. `markdown_core_inlines_add/remove_special_character` and
  `markdown_core_manage_extensions_special_characters` modify only parser
  tables. `subject` borrows their pointers; reference parsing without a parser
  borrows const base tables. No process-global table is written during parsing;
  `SMART_PUNCT_CHARS` and base tables are declared `const`.
- **Regression:** `facade_concurrent_stress`, facade label, 8 threads × 200
  rounds × 6 inputs × 3 option variants, including skip-character-sensitive
  `*a~b*c~` and `*a$b*c$`. All dumps match reference bytes exactly.
- **Closure evidence:** simulating the old process-global tables produces
  multiple `WARNING: ThreadSanitizer: data race` reports and functional dump
  differences (`thread 0 reported a violation`). Restoring parser-local tables
  passes default/ASan/UBSan/TSan with no drift in single-threaded goldens across
  all 56 correctness tests, including spec/extensions/regression.

## MC10-04 UBSan instrumentation gap (validation infrastructure)

- **Source:** sanitizer configuration audit in this phase.
- **Reproduction:** the `Ubsan` build type adds `-fsanitize=undefined` only in
  the directory scope of `core/CMakeLists.txt`; extensions and tests are not
  instrumented. `correctness-ubsan` therefore never actually checked UB in
  extensions/ast.c, the six extension implementations, or all test runners.
- **Root cause:** sanitizer flags are set in the wrong directory scope.
- **Impact:** UBSan validation does not apply to more than half the C code.
- **Fix:** move sanitizer build-type compile flags to
  `packages/markdown-core/CMakeLists.txt` so core, extensions, and tests are all
  instrumented. At the same level add `Tsan`, `tsan`/`correctness-tsan` presets,
  `make tsan-test`, and CI `ubsan`/`tsan` jobs; CI previously lacked a ubsan job.
- **Regression:** `ctest --preset correctness-ubsan` / `correctness-tsan`
  themselves, now covering all 56 correctness tests and all C source.
- **Closure evidence:** fully instrumented UBSan and TSan each pass 56/56.

---

## Audit-scope notes (not defects)

Other process-global state on the facade parse path:

| State | Finding |
| --- | --- |
| `MARKDOWN_CORE_NODE_LAST_BLOCK/_INLINE`, node-flag counters, registry list | Written only within once, then immutable for the process lifetime; once establishes happens-before |
| `MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR`, `syntax_extension.c:_mem` | Never written after static initialization |
| `inlines.c` base tables, `SMART_PUNCT_CHARS`, scanners/entities/houdini tables | `const`/read-only |
| Static arena in `arena.c` | Diagnostic allocator used only by CLI `main.c`, outside facade parsing, which always uses the default allocator |
| `node.c:enable_safety_checks` | Startup switch in the legacy engine API, outside the public facade; must be set before concurrency starts. The header contract excludes undocumented conventions |

Facade failure-path review: all four parse failures (invalid argument, parser
allocation failure, document allocation failure, missing root) correctly release
acquired resources without touching global state. `set_error` leaves
`*error == NULL` if allocating the error itself fails; callers detect failure
through document == NULL and do not dereference the error. There are no leaks or
double frees. Each concurrency/lifecycle case continually checks determinism
through two byte-compared dumps.
