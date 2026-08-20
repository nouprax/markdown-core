# Phase 6 Read-Only C AST Facade

## Outcome

Phase 6 adds the stable, read-only C boundary at
`packages/markdown-core/include/markdown_core.h`. It parses the
frozen `ParseOptions`, owns the native document, exposes canonical kinds and
borrowed UTF-8 views, traverses the complete canonical tree, and releases every
owned result through a matching free API.

The native diagnostic dump is implemented directly over that facade. It does
not call the HTML, XML, CommonMark, LaTeX, man, plaintext, or generic render
framework. `markdown-core -t ast` selects the canonical defaults and emits the
same deterministic file tree for command-line diagnostics.

## Existing-interface audit

The pre-Phase 6 C surface offered mutable native nodes, iterators, content
renderers, and CLI renderer formats. `markdown_core_render_xml` could expose a
tree-shaped representation, but it belongs to the renderer framework and does
not implement the frozen canonical schema. No independent, renderer-free AST
dump interface existed, so Phase 6 introduced one instead of repurposing XML.

## Facade contract

The stable header exposes only:

- explicit default initialization for the eleven typed parse options;
- parse, root, and document-free operations;
- canonical node kinds, scope, child count, first-child, and next-sibling
  traversal;
- read-only typed accessors for every behavior-bearing core and extension
  field;
- `{data, length}` UTF-8 views whose lifetime is bounded by the document;
- explicit error code, UTF-8 diagnostic, optional scope, and error free;
- canonical dump allocation and its matching free operation.

It exposes no node constructors, setters, append/insert/replace/unlink
operations, renderer callbacks, renderer options, or native implementation
layout. SwiftPM, Android Prefab, and CMake install now publish only this stable
header; their old mutation/render headers remain build-internal while the
legacy engine is still needed by Phases 7 and 8.

The Android shared library also uses an explicit ELF version script containing
only the facade functions. Native parser, mutation, renderer, extension setter,
scanner, and registration symbols remain local even though those sources are
still linked internally until the later deletion phases.

## Native normalization

The facade preserves parser behavior while adapting native details to the
Phase 5 contract:

- footnote-reference ids come from their linked definitions because native
  postprocessing replaces the reference literal with a display index;
- native table alignment bytes map to the canonical alignment enum;
- `TableRow` and `TableCell` remain traversable scoped `Markup` nodes exposed
  through typed table ownership edges;
- directive-label wrapper nodes are hidden, with label presence, label count,
  label children, and block content exposed separately;
- code `language` is a borrowed slice of the first non-whitespace info token,
  and absent/empty info maps to `null`;
- indented code is `fenced=false, closed=true`, while an unclosed fence remains
  visible as `closed=false`;
- every native line and column integer, including `0:0`, is copied unchanged.

## Dump completeness

The dump prints canonical connectors, child counts, scopes, fixed
field order, JSON string escaping, null/empty/default values, and one final LF.
The reviewed `completeness.md/.ast` pair adds the previously important
contrasts: loose versus tight lists, checked false versus null/true, absent code
info, indented and unclosed fenced code, absent versus empty link titles,
absent versus explicit-empty directive attributes and labels, and unaligned
tables.

The contract checker now verifies exact field-name order for every record kind
and requires representative null/empty/default/true/false tokens across the
C-owned goldens. Goldens are still never rewritten by tests.

## Tests and ownership

`facade_test` is a C consumer of only the stable header. It covers defaults,
typed accessors, scopes, explicit errors, null-safe free APIs, extension option
gates, repeated parse/dump/free ownership, and byte-for-byte comparison of all
C-owned fixtures. `facade_cplusplus_test` independently compiles and consumes
the same header as C++. `canonical_ast_cli` feeds every C-owned Markdown fixture
through `markdown-core -t ast` and compares stdout with the reviewed golden.

## Validation

Phase 6 was validated with:

```sh
cmake -S . -B build-phase6-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-phase6-release -j2
ctest --test-dir build-phase6-release --output-on-failure

cmake -S . -B build-phase6-asan -DCMAKE_BUILD_TYPE=Asan \
  -DMARKDOWN_CORE_SHARED=OFF -DMARKDOWN_CORE_STATIC=ON
cmake --build build-phase6-asan -j2
ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
  ctest --test-dir build-phase6-asan -R 'facade|canonical_ast_cli' --output-on-failure

cmake -S . -B build-phase6-ubsan -DCMAKE_BUILD_TYPE=Ubsan \
  -DMARKDOWN_CORE_SHARED=OFF -DMARKDOWN_CORE_STATIC=ON \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined
cmake --build build-phase6-ubsan -j2
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-phase6-ubsan -R 'facade|canonical_ast_cli' --output-on-failure

swift build
pnpm run test:contracts
pnpm run verify
git diff --check
```

LeakSanitizer is unavailable in the AppleClang runtime on the validation host,
so ASan ran with leak detection disabled; address errors still abort the test
run. The explicit ownership suite exercises every facade allocation/free path.
