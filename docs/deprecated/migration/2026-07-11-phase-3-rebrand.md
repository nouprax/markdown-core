# Phase 3 Repository Rebrand

This is a migration/history document. Legacy names appear only to record the
removed surface, its replacement, and the audit exceptions required by the
repository specification.

## Inventory and naming policy

The initial case-insensitive inventory found 136 tracked text files containing
`cmark` and 25 tracked paths whose names contained `cmark` or `cmark-gfm`. The
inventory covered source and generated C files, public and private symbols,
types, macros, include guards, CMake/Make targets, library names, CLI names,
tests, wrappers, CI, man pages, SwiftPM metadata, Android Prefab metadata, and
package-content audits.

The migration used one naming family without compatibility aliases:

| Removed family | Replacement |
| --- | --- |
| `cmark_*` / `cmark_gfm_*` | `markdown_core_*` |
| `CMARK_*` / `CMARK_GFM_*` | `MARKDOWN_CORE_*` |
| `cmark-gfm` product, target, CLI, and library names | `markdown-core` |
| C implementation filenames | `markdown_core.c`, `markdown_core_ctype.*` |
| Public headers | `markdown-core*.h` |
| pkg-config file | `markdown-core.pc` |
| SwiftPM product/module | `MarkdownCore` |
| Android Prefab package/module | `markdown_core` |

The independent version line is `1.0.0`; inherited `.gfm.*` version suffixes
and SONAMEs were removed.

## Build and package changes

- CMake options, source-directory variables, install exports, generated export
  headers, libraries, the CLI, fuzz targets, and test commands use the new
  names.
- Make, NMake, CI, AppVeyor, test helpers, language wrappers, scanner inputs,
  fixtures, and man-page generation reference only the new symbols and paths.
- The root Swift manifest is named `swift-markdown-core` and exposes the
  `MarkdownCore` product/module.
- Android metadata uses the `com.nouprax` group, the Markdown Core namespace,
  the `markdown_core` Prefab module, and renamed packaged headers and library.
- Package audits validate the renamed C install tree, SwiftPM manifest, npm
  allowlist, and Android AAR entries.

## Validation

The phase was verified on macOS arm64 with Apple clang 21.0.0:

```sh
cmake -S . -B /tmp/markdown-core-phase3-cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DMARKDOWN_CORE_WARNINGS_AS_ERRORS=ON
cmake --build /tmp/markdown-core-phase3-cmake --parallel 4
ctest --test-dir /tmp/markdown-core-phase3-cmake \
    --output-on-failure --parallel 4

swift build
scripts/check_android_prefab_support.sh
pnpm run format:check
pnpm run lint
pnpm run audit:packages
```

The release build passed with warnings treated as errors, all 21 CTest tests
passed, SwiftPM built successfully, and the Android Prefab and package-content
audits passed.

Sanitizer validation used:

```sh
cmake -S . -B /tmp/markdown-core-phase3-asan \
    -DCMAKE_BUILD_TYPE=Asan \
    -DMARKDOWN_CORE_SHARED=OFF \
    -DMARKDOWN_CORE_WARNINGS_AS_ERRORS=ON
cmake --build /tmp/markdown-core-phase3-asan --parallel 4
ASAN_OPTIONS='halt_on_error=1:abort_on_error=1:detect_leaks=0' \
    ctest --test-dir /tmp/markdown-core-phase3-asan \
        --output-on-failure --parallel 4

cmake -S . -B /tmp/markdown-core-phase3-ubsan \
    -DCMAKE_BUILD_TYPE=Ubsan \
    -DMARKDOWN_CORE_WARNINGS_AS_ERRORS=ON
cmake --build /tmp/markdown-core-phase3-ubsan --parallel 4
ctest --test-dir /tmp/markdown-core-phase3-ubsan \
    --output-on-failure --parallel 4
```

The ABI and artifact audit installed into an isolated prefix and checked:

- no legacy path in the install tree;
- no legacy exported symbol in either shared library;
- no legacy string in installed files or CLI help;
- expected exported symbols such as `markdown_core_parse_document`,
  `markdown_core_node_first_child`, and `markdown_core_version`;
- CLI identity `markdown-core 1.0.0`;
- no unexpected entry in the npm dry-run package, C install tree, SwiftPM
  source allowlist, or Android AAR.

## Allowed remaining references

The post-migration source audit permits the old project names only in:

- `LICENSE` and `COPYING` attribution/license text;
- `README.md` and `UPSTREAM.md` source and rewrite statements;
- `docs/specs/2026-07-11-repo-setup.md` migration requirements;
- files under `docs/migration/`, including the imported upstream changelog and
  historical project-naming essay;
- Git history.

No implementation source, operational documentation, build metadata, test
name, installed artifact, CLI text, or exported symbol uses the old names.
