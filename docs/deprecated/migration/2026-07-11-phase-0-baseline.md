# Phase 0 C Engine Baseline

This is a migration/history document. Legacy names appear only to identify the
imported source surface that later phases must remove or rename.

## Source identity

- Target repository: `https://github.com/nouprax/markdown-core.git`.
- Source repository: `https://github.com/DongyuZhao/cmark-gfm`.
- Baseline commit: `711032b2a16cf25c3df75033833eba086b17ca6a`.
- Commit date: 2026-07-05.
- Commit subject: `[Feature] Support detect fenced code status`.
- Imported baseline version: `0.29.0.gfm.13`.
- Import method: fetch the exact commit with `--no-tags`, then restore only the
  paths present in that tree while preserving this repository's specification.
- Release lineage: no upstream tags were imported; the target tag namespace was
  empty on 2026-07-11 and the first planned Markdown Core release is `1.0.0`.

The parser, fixtures, and expected outputs were not intentionally changed in
Phase 0.

## Validation environment

| Component | Value |
| --- | --- |
| Host | macOS 26.5.2, arm64 |
| CMake | 4.3.3 |
| C/C++ compiler | Apple clang 21.0.0 (`clang-2100.1.1.101`) |
| Python | 3.14.6 |
| Make | GNU Make 3.81 |
| Git | 2.53.0.vfs.0.7 |

CMake 4 requires `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` for this inherited
baseline. Phase 1 must modernize the project policy instead of relying on this
command-line compatibility setting.

## Build and test baseline

### Release build and tests

```sh
cmake -S . -B build-phase0 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-phase0 --parallel 2
ctest --test-dir build-phase0 --output-on-failure
```

Result on 2026-07-11: all targets built and 28 of 28 tests passed in 20.48
seconds. The test set includes the C/C++ API test, CommonMark and extension
specs, pathological inputs, round trips, option gates, and regressions.

### Make frontend and CLI

```sh
make -s cmake_build BUILDDIR=build-phase0
build-phase0/src/cmark-gfm --version
```

Result: the Make frontend completed an incremental build and the CLI reported
the imported version `0.29.0.gfm.13`.

### AddressSanitizer

```sh
cmake -S . -B build-phase0-asan \
    -DCMAKE_BUILD_TYPE=Asan \
    -DCMARK_SHARED=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-phase0-asan --parallel 2
ASAN_OPTIONS='halt_on_error=1:abort_on_error=1:detect_leaks=0' \
    ctest --test-dir build-phase0-asan --output-on-failure --parallel 4
```

Result: 24 of 24 tests passed with no AddressSanitizer report. Disabling the
shared target removes four library-backed tests, so this count is expected.
The API executable reported 608 passed checks and the executable CommonMark
spec test reported 670 passed examples. The inherited target disables leak
detection; this result is not evidence of leak freedom.

### UndefinedBehaviorSanitizer

```sh
cmake -S . -B build-phase0-ubsan \
    -DCMAKE_BUILD_TYPE=Ubsan \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-phase0-ubsan --parallel 2
ctest --test-dir build-phase0-ubsan --output-on-failure --parallel 4
```

Result: 28 of 28 tests passed with no UndefinedBehaviorSanitizer report. The
pathological library test completed in 18 seconds.

### Known baseline warnings

- CMake warns that compatibility below 3.10 will be removed.
- CMake warns that the inherited `FindPythonInterp` module is removed under
  policy `CMP0148`.
- Shared extension targets pass `cmark-gfm` as a preprocessor definition,
  which Apple clang reports as an invalid C99 macro name.
- Apple clang reports inherited uses of deprecated `sprintf` in API tests.

These warnings predate the Markdown Core migration and are inputs to Phase 1;
they are not accepted as the final warnings-as-errors baseline.

## Benchmark baseline

The offline benchmark command uses all 26 tracked samples. Each timed input is
the sample repeated 200 times; the table records the median of three runs.

```sh
make -s newbench BUILDDIR=build-phase0 NUMRUNS=3
```

| Sample | Median seconds |
| --- | ---: |
| `block-bq-flat.md` | 0.31 |
| `block-bq-nested.md` | 0.30 |
| `block-code.md` | 0.29 |
| `block-fences.md` | 0.29 |
| `block-heading.md` | 0.29 |
| `block-hr.md` | 0.29 |
| `block-html.md` | 0.29 |
| `block-lheading.md` | 0.29 |
| `block-list-flat.md` | 0.29 |
| `block-list-nested.md` | 0.29 |
| `block-ref-flat.md` | 0.29 |
| `block-ref-nested.md` | 0.29 |
| `directive.md` | 0.29 |
| `inline-autolink.md` | 0.29 |
| `inline-backticks.md` | 0.28 |
| `inline-em-flat.md` | 0.28 |
| `inline-em-nested.md` | 0.29 |
| `inline-em-worst.md` | 0.30 |
| `inline-entity.md` | 0.34 |
| `inline-escape.md` | 0.30 |
| `inline-html.md` | 0.29 |
| `inline-links-flat.md` | 0.30 |
| `inline-links-nested.md` | 0.30 |
| `inline-newlines.md` | 0.29 |
| `lorem1.md` | 0.29 |
| `rawtabs.md` | 0.28 |

The inherited `make bench` target is not part of the offline baseline because
it clones `progit/progit` and generates `bench/benchinput.md`. It is historical
inventory only and must not be executed during Phase 7. Phase 7 removes the
runtime network target before running the replacement benchmark. Pro Git's
CC BY-NC-SA 3.0 content is not accepted as a vendored test corpus; the
replacement uses project-authored synthetic input, existing license-compatible
samples, or a commercially redistributable pinned corpus. Any external corpus
must be licensed, attributed, manifested, hashed, and package-excluded; no Git
checkout, download archive, loose generated input, hidden ignore entry, or
cache may remain.

## Microsoft-specific removal inventory

Phase 2 must remove this surface rather than merely disable it.

### Extension implementations and registration

- `extensions/ms_copilot_accordion.c` and `.h`.
- `extensions/ms_copilot_annotation.c` and `.h`.
- `extensions/ms_copilot_citation.c` and `.h`.
- Source lists in `extensions/CMakeLists.txt`, `Package.swift`, and
  `android/src/main/cpp/CMakeLists.txt`.
- Includes and registration in `extensions/core-extensions.c`.
- Public declarations in `extensions/cmark-gfm-core-extensions.h` and its
  Android Prefab copy.
- Node kinds for accordion, accordion header/content, annotation, and citation.
- Citation and annotation getters/setters and every syntax extension callback.

### Options, CLI, formula branches, and scanners

- `CMARK_OPT_MS_COPILOT_ACCORDION`.
- `CMARK_OPT_MS_COPILOT_ANNOTATION`.
- `CMARK_OPT_MS_COPILOT_CITATION`.
- `CMARK_OPT_MS_FORMULA_DELIMITERS`.
- CLI flags `--ms-copilot-accordion`, `--ms-copilot-annotation`,
  `--ms-copilot-citation`, and `--ms-formula-delimiters` in `src/main.c`.
- Option declarations in `src/cmark-gfm.h`, the Android Prefab header, and the
  generated man page.
- MS block/inline delimiter branches and constants in `extensions/formula.c`.
- Single-backslash scanners in `extensions/ext_scanners.re`, generated
  `extensions/ext_scanners.c`, and declarations/macros in
  `extensions/ext_scanners.h`.

### Tests and fixtures

- `test/extensions-ms-copilot-accordion.txt`.
- `test/extensions-ms-copilot-annotation.txt`.
- `test/extensions-ms-copilot-citation.txt`.
- `test/extensions-ms-copilot-option-gates.txt`.
- `test/extensions-formula-ms.txt`.
- `test/ms_copilot_accordion_depth_limit.py`.
- `test/ms_copilot_annotation_render_tests.py`.
- MS cases in `test/inline_delimiter_stack_tests.py`.
- All corresponding targets and command arguments in `test/CMakeLists.txt`.

The post-removal audit must be case-insensitive and include source, generated
scanners, headers, package copies, build metadata, tests, docs, CLI help, and
exported symbols.

## Renderer and render-dependent test inventory

Phase 6 must first introduce an independent AST dump. Phase 7 then unifies all
C test execution under complete CTest suites, Phase 8 migrates parser tests,
and only Phase 9 may remove the renderer surface below.

### Renderer implementation and public functions

- Framework: `src/render.c` and `src/render.h`.
- HTML: `src/html.c` and `src/html.h`.
- XML: `src/xml.c`.
- CommonMark: `src/commonmark.c`.
- LaTeX: `src/latex.c`.
- man: `src/man.c`.
- plaintext: `src/plaintext.c`.
- Convenience API: `cmark_markdown_to_html` in `src/cmark.c`.
- Public `cmark_render_*` and `cmark_render_*_with_mem` declarations in
  `src/cmark-gfm.h` and its Android Prefab copy.
- Source lists in `src/CMakeLists.txt`, `Package.swift`, and Android CMake.

The current CLI defaults to HTML and supports `html`, `xml`, `man`,
`commonmark`, `plaintext`, and `latex` through `-t`/`--to`. Its output routing
is implemented in `src/main.c`.

There is no renderer-independent native AST dump in this baseline. CLI XML
output directly calls `cmark_render_xml_with_mem` and therefore cannot satisfy
the Phase 6 dump contract.

### Extension renderer callbacks

Renderer callbacks and registrations exist in:

- `extensions/table.c`;
- `extensions/strikethrough.c`;
- `extensions/tasklist.c`;
- `extensions/formula.c`;
- `extensions/directive.c`;
- all three MS-specific extension implementations listed above.

The generic callback types and setters are part of the inherited syntax
extension API and must be removed after test migration.

### Tests, fuzzers, wrappers, and generated documentation

- `api_test/main.c` mixes parser, mutation, and all renderer assertions;
  `api_test/cplusplus.cpp` calls the HTML convenience API.
- `test/spec_tests.py` compares CommonMark, smart punctuation, extension,
  option, and regression fixtures against renderer output.
- `test/cmark.py` exposes HTML and CommonMark renderers to Python tests.
- `test/roundtrip_tests.py`, `test/roundtrip.sh`, and `test/roundtrip.bat`
  depend on CommonMark rendering.
- `test/pathological_tests.py`, `test/entity_tests.py`, and renderer-oriented
  targets in `test/CMakeLists.txt` require reassessment and AST migration.
- Formula, directive, and MS option-gate tests currently use XML output.
- `test/cmark-fuzz.c`, `fuzz/fuzz_quadratic.c`, and
  `fuzz/fuzz_quadratic_brackets.c` call renderers after parsing.
- `test/normalize.py` and expected HTML/XML content in fixture files become
  obsolete where they serve only renderer comparison.
- `man/make_man_page.py` calls the man renderer; `man/man3/cmark-gfm.3`
  publishes renderer APIs.
- `wrappers/wrapper.py`, `.rb`, `.rkt`, and `wrapper_ext.py` expose renderer
  entry points.

Phase 8 must classify every affected assertion as parser behavior to migrate
or renderer-only behavior to delete. It must not regenerate expected output
without human review.
