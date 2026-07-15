# Phase 9 Renderer Removal

## Scope

Phase 9 removes every Markdown content renderer after Phase 8 moved parser
correctness assertions to the canonical AST dump. The native dump remains a
diagnostic tree representation implemented by the read-only facade and does
not call a content renderer.

## Removed implementation

- Deleted the HTML, XML, CommonMark, LaTeX, man, plaintext, and generic render
  source/header files from the C engine.
- Deleted `markdown_core_markdown_to_html`, every `markdown_core_render_*`
  declaration/definition, render-only option flags, custom-node enter/exit
  render payloads, and unused escaping helpers.
- Removed all syntax-extension render callback types, setters, stored callback
  fields, registrations, and extension implementations. The renderer-only
  `tagfilter` extension was removed entirely.
- Removed render-only examples, fixtures, wrappers, the renderer-backed man
  page generator, and all corresponding CMake, SwiftPM, Android, Make, fuzz,
  test, and package references.

## CLI and documentation

`markdown-core` is now an AST-only diagnostic CLI. It parses files or stdin and
writes the canonical file-tree dump directly. `-t`/`--to`, output width, unsafe
rendering, break rendering, and format-specific options are rejected. The man
page documents only parsing and AST dump behavior.

The package audit now fails if renderer APIs or deleted renderer files return,
if the CLI accepts `--to html`, or if CLI help advertises retired options.

## Public and packaged surface

- The installed `include/markdown_core.h` remains the read-only facade.
- The internal legacy engine header no longer declares renderer functions or
  extension render callbacks.
- The linker export allowlist remains facade-only. The C install and Android
  AAR symbol audits matched that allowlist byte-for-byte.
- SwiftPM and Android native source lists compile only parser, AST facade, and
  parsing extension sources.

## Validation

Validated on July 12, 2026:

- Release CMake clean configure/build: passed without renderer sources.
- CTest correctness preset: 53/53 tests passed.
- AddressSanitizer correctness preset: 53/53 tests passed.
- UndefinedBehaviorSanitizer correctness preset: 53/53 tests passed after the
  preset was corrected to link the UBSan runtime into C/C++ executables.
- SwiftPM `swift test`: 10 tests in 4 Swift Testing suites passed.
- C and CMake formatting checks and C lint build: passed.
- Test topology audit: passed.
- C install, SwiftPM source, npm package, Android AAR, and Android export
  audits: passed.
- Source audit found no renderer API declarations, definitions, callbacks,
  build inputs, wrappers, or operational documentation.

Phase 10 can therefore treat the parser and read-only facade as the complete C
runtime surface; no content renderer remains available as a fallback path.
