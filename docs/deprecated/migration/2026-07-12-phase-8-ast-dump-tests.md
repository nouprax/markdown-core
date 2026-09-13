# Phase 8: migrate all parser tests to AST dumps

Status: complete. All retained parser/spec/API/fuzz tests use canonical AST
dumps and typed accessors exclusively; none calls a renderer.

> Phase 18 moves only cross-product canonical conformance goldens to the root
> shared manifest. This phase's C-owned CommonMark/extension/regression
> correctness fixtures and AST-dump assertions remain in the C package and do
> not enter binding correctness discovery.

## 1. Classification of render-dependent assertions (from the Phase 7 inventory)

| Category | Disposition |
| --- | --- |
| Spec/extension fixtures (12 files, 801 cases) | Migrate all expected blocks from HTML/XML to canonical AST dumps |
| `spec_roundtrip_commonmark`, `extensions_roundtrip_gfm` | Remove: commonmark-writer roundtrip has renderer-only value. `spec_runner` double dumps for each case and `fuzz_smoke` now cover AST determinism |
| `extensions_option_table_style`, `extensions_option_full_info_string` | Retain as AST suites: alignment and the full info string are AST semantics and always dumped; remove the roundtrip transport |
| `spec_entities` (HTML substrings) | Assert Text literals after facade parsing. Remove all HTML-escaping exceptions (quot/nvlt/nvgt); AST literals are the raw UTF-8 expansions |
| `pathological_*` (30 HTML regex assertions) | Facade structural assertions: node-kind counts, concatenated Text literals equal to original input, and typed property probes. Iterative traversal avoids recursion at depth 50000 |
| `pathological_complexity_*` (HTML conversion timings) | Time facade parsing; retain relative-ratio assertions |
| `api_engine` (61 renderer references) | See §2 |
| `fuzz_smoke` | Already parse/traverse/dump/free in Phase 7; unchanged |
| libFuzzer campaign harnesses (`markdown-core-fuzz.c`, `fuzz_quadratic*.c`) | Replace render calls with full-tree iterator traversal and accessor access |
| `facade_dump_cli` | Already uses `-t ast`; unchanged |
| `tagfilter` example in spec.txt | Remove: tagfilter only changes `<` in HTML output, has no AST semantics, and must not masquerade as an AST test |

## 2. `api_engine` migration details

Remove renderer-only suites: `render_html`, `render_xml`, `render_man`,
`render_latex`, `render_commonmark`, `render_plaintext`, `test_safe`
(OPT_SAFE/UNSAFE are renderer policy only), and `custom_nodes`
(CUSTOM_INLINE/BLOCK on_enter/on_exit are renderer payloads).

Replace with AST/accessor assertions:

- `accessors`: replace whole-document rendered-HTML checks with getter
  read-back assertions for each setter.
- `iterator_delete`/`create_tree`: replace HTML checks with tree structure and
  literal assertions.
- `utf8`/`numeric_entities`/`parser`: replace `test_md_to_html` with
  `test_md_paragraph_text`, comparing concatenated Text literals in one
  paragraph as raw bytes without HTML escaping.
- `line_endings`: assert list/code-block structure. Remove the
  `OPT_HARDBREAKS`/`OPT_NOBREAKS` assertions because they affect only rendered
  output. CRLF remains SoftBreak in the AST; retain that parser assertion.
- `source_pos`, `source_pos_inlines`, `ref_source_pos`, `autolink_source_pos`:
  replace sourcepos XML assertions with facade parse + canonical dump byte
  comparisons. Every node emits `scope=`, covering the same coordinate
  semantics. Add `test_facade_dump`.
- `directive_extension_accessors`: remove commonmark/XML/HTML transport
  assertions and replace normalized JSON assertions with direct accessor
  comparisons.
- `test_pathological_regressions`: time parse+free without rendering.
- C++ consumer (`cplusplus.cpp`): replace markdown_to_html assertions with node
  type and literal checks.

Result: all 650 assertions passed (649 before migration, essentially unchanged
in count; new structural assertions offset removed renderer assertions).

## 3. Fixture migration and manual review

Generation: `spec_runner --rewrite`, a new explicit maintenance mode, regenerates
expected output for each suite's `ParseOptions` combination. Markdown blocks and
tags remain byte-for-byte unchanged.

Review evidence to avoid self-validation:

1. **Witness:** Phase 8 changed no parser/facade source. In the same worktree,
   all 57/57 byte-exact HTML suites passed at the end of Phase 7, as recorded in
   its report. Dumps therefore encode the same ASTs validated by CommonMark/GFM
   HTML comparisons.
2. **Structural checks:** scripts compared every fixture's example count, tags,
   and Markdown bytes before and after migration. All agree except the one
   removed tagfilter example in spec.txt.
3. **Semantic spot checks:** inspect dumps by kind: table alignments/header
   rows, footnote IDs, formula mode standalone/embedded, directive attributes
   JSON and label=null/label=N, full info strings including NUL→U+FFFD, smart
   punctuation, empty leading Text in autolinks, title=null/"" distinctions,
   and scope coordinates matching original XML sourcepos.
4. **Anomaly scan:** all 801 enabled examples have expected blocks beginning
   with `Document scope=`, with no remaining HTML/XML.
5. The former `<IGNORE>` autolink-crash regression now has a real dump assertion.

Option mapping: legacy option-gate suites that attached an extension with its
option disabled correspond to false fields in facade `ParseOptions`.
`-e footnotes` maps to the `footnotes` field. tagfilter is excluded from
`ParseOptions` because it has no AST semantics.

## 4. Test infrastructure changes

- `test_support` removes all render paths: HTML/XML/commonmark conversion and
  roundtrip whitespace-folding canonicalization. New facade helpers are
  `ts_ast_options_none`/`ts_ast_enable` (fixture tags to typed options),
  `ts_ast_parse`, and iterative
  `ts_ast_walk`/`ts_ast_count_kinds`/`ts_ast_concat_text`.
- `spec_runner` supports AST mode only, asserts deterministic double dumps for
  every case, and provides `--rewrite` as described above.
- CTest suite count changes from 57 to 55 after removing two roundtrip suites;
  labels are unchanged.
- No canonicalization layer remains in the comparison policy; contract §6 is
  updated.

## 5. Validation

- `ctest --preset correctness`: 55/55; `ctest --preset benchmark`: 6/6.
- ASan correctness: 55/55; `swift test`: 4 suites/10 tests; the complete
  `pnpm verify` chain passed at the end of Phase 8.
- Repository-wide test-tree renderer audit: `grep render_|markdown_to_html`
  finds no calls under `packages/markdown-core/tests/` or `fuzz/`, excluding
  comments. Phase 15 removed the former root `tests/` directory.
- Renderers and CLI `-t html|xml|man|latex|commonmark|plaintext` still exist for
  Phase 9 to remove; no test depends on them after Phase 8.
