# Phase 2 Microsoft-specific Extension Removal

This is a migration/history document. MS-specific identifiers appear only to
record the deleted surface and the audit that prevents it from returning.

## Removed surface

Commit `2ccca08` removed:

- the accordion, annotation, and citation extension implementations, headers,
  node kinds, accessors, registration, and build inputs;
- all `CMARK_OPT_MS_COPILOT_*` options and CLI flags;
- `CMARK_OPT_MS_FORMULA_DELIMITERS`, its parser branches, and the
  single-backslash compatibility scanners;
- corresponding tests, fixtures, scripts, generated scanner declarations, and
  fuzz configuration.

The retained LaTeX formula fixture explicitly verifies that single-backslash
lookalikes are ordinary Markdown escapes. It is a negative compatibility
regression test, not an implementation of the removed syntax.

## Completion audit correction

A retrospective case-insensitive audit found that the generated C API man page
had not been refreshed after the source removal. It still documented the four
deleted MS option families even though the public header and binaries no longer
exported them.

The correction:

- regenerated `man/man3/markdown-core.3` from the current public C header;
- fixed `man/make_man_page.py` to locate the selected build library, call the
  three-argument parse API correctly, and release parsed documents;
- made package audit regenerate the man page and compare its body with the
  tracked page;
- added case-insensitive legacy identifier scans across repository source, the
  installed C prefix, and the Android AAR.

The audit covers the removed option/extension identifier families without
matching this historical document itself.

## Validation

```sh
pnpm run verify

cmake --build /tmp/markdown-core-phase3-asan --parallel 4
ASAN_OPTIONS='halt_on_error=1:abort_on_error=1:detect_leaks=0' \
    ctest --test-dir /tmp/markdown-core-phase3-asan \
        --output-on-failure --parallel 4

cmake --build /tmp/markdown-core-phase3-ubsan --parallel 4
ctest --test-dir /tmp/markdown-core-phase3-ubsan \
    --output-on-failure --parallel 4
```

Results:

- root `verify`: passed, including 21 of 21 release C tests;
- Gradle Tooling API model load: passed;
- source, install, generated man page, Android AAR, and package allowlist
  audits: passed;
- AddressSanitizer static build: 17 of 17 tests passed;
- UndefinedBehaviorSanitizer shared/static build: 21 of 21 tests passed.

No MS-specific parser surface remains in source, build metadata, CLI, public
headers, tests, generated scanners, installed documentation, symbols, or
packaged Android artifacts.
