# CTest is the single source of truth for the C test graph.  Suites are
# grouped by label:
#
#   api facade consumer spec extensions regression pathological fuzz packaging
#
# The default correctness preset excludes conformance. Performance measurement
# is not part of the test graph unless MARKDOWN_CORE_BENCHMARKS is explicitly
# enabled for a local benchmark build. Every suite is native (C/C++); the test
# tree has no scripting-language dependency and never touches the network.

set(MARKDOWN_CORE_TEST_FIXTURES_DIR "${MARKDOWN_CORE_TESTS_SOURCE_DIR}/fixtures")
set(MARKDOWN_CORE_CANONICAL_AST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../specs/canonical-ast")
set(MARKDOWN_CORE_CANONICAL_AST_MANIFEST "${MARKDOWN_CORE_CANONICAL_AST_DIR}/manifest.json")

if(NOT EXISTS "${MARKDOWN_CORE_CANONICAL_AST_MANIFEST}")
    message(FATAL_ERROR "Shared canonical AST manifest is missing: ${MARKDOWN_CORE_CANONICAL_AST_MANIFEST}")
endif()
file(READ "${MARKDOWN_CORE_CANONICAL_AST_MANIFEST}" MARKDOWN_CORE_CANONICAL_AST_JSON)
string(JSON MARKDOWN_CORE_CANONICAL_SCHEMA_VERSION GET "${MARKDOWN_CORE_CANONICAL_AST_JSON}" schemaVersion)
if(NOT MARKDOWN_CORE_CANONICAL_SCHEMA_VERSION EQUAL 1)
    message(FATAL_ERROR "Unsupported canonical AST manifest schema: ${MARKDOWN_CORE_CANONICAL_SCHEMA_VERSION}")
endif()
string(JSON MARKDOWN_CORE_CANONICAL_CASE_COUNT LENGTH "${MARKDOWN_CORE_CANONICAL_AST_JSON}" cases)
if(MARKDOWN_CORE_CANONICAL_CASE_COUNT LESS 1)
    message(FATAL_ERROR "Shared canonical AST manifest contains no cases")
endif()
math(EXPR MARKDOWN_CORE_CANONICAL_CASE_LAST "${MARKDOWN_CORE_CANONICAL_CASE_COUNT} - 1")
set(MARKDOWN_CORE_CANONICAL_NAMES "")
foreach(case_index RANGE 0 ${MARKDOWN_CORE_CANONICAL_CASE_LAST})
    string(
        JSON
        case_name
        GET
        "${MARKDOWN_CORE_CANONICAL_AST_JSON}"
        cases
        ${case_index}
        name)
    string(
        JSON
        case_input
        GET
        "${MARKDOWN_CORE_CANONICAL_AST_JSON}"
        cases
        ${case_index}
        input)
    string(
        JSON
        case_expected
        GET
        "${MARKDOWN_CORE_CANONICAL_AST_JSON}"
        cases
        ${case_index}
        expected)
    if(NOT case_input STREQUAL "${case_name}.md" OR NOT case_expected STREQUAL "${case_name}.ast")
        message(FATAL_ERROR "Canonical AST case ${case_name} has non-canonical paths")
    endif()
    # The manifest names no option: a case that still carried one would be
    # asking for a language the parser does not have.
    string(
        JSON
        case_options
        ERROR_VARIABLE
        case_options_error
        GET
        "${MARKDOWN_CORE_CANONICAL_AST_JSON}"
        cases
        ${case_index}
        parseOptions)
    if(NOT case_options_error)
        message(FATAL_ERROR "Canonical AST case ${case_name} names parse options; the dialect has none")
    endif()
    list(APPEND MARKDOWN_CORE_CANONICAL_NAMES "${case_name}")
endforeach()

if(NOT (MARKDOWN_CORE_SHARED OR MARKDOWN_CORE_STATIC))
    message(FATAL_ERROR "MARKDOWN_CORE_TESTS=ON requires MARKDOWN_CORE_SHARED" " or MARKDOWN_CORE_STATIC")
endif()

if(MARKDOWN_CORE_SHARED)
    set(MARKDOWN_CORE_TEST_LIBRARIES libmarkdown-core-extensions)
else()
    set(MARKDOWN_CORE_TEST_LIBRARIES libmarkdown-core-public-static)
endif()

# Shared native test support: fixture loading, byte comparison, deterministic
# diffs, PRNG, and timing, over the product library the consumers get. Every
# runner on it exercises exactly the exported contract: there is one language,
# one parse entry, and nothing in the test tree reaches past them.
set(MARKDOWN_CORE_TEST_SUPPORT_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/support" "${CMAKE_CURRENT_SOURCE_DIR}/../include" "${MARKDOWN_CORE_CORE_SOURCE_DIR}"
    "${MARKDOWN_CORE_CORE_BINARY_DIR}" "${MARKDOWN_CORE_EXTENSIONS_SOURCE_DIR}"
    "${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR}")
add_library(markdown-core-test-support STATIC support/test_support.c support/test_support.h)
target_include_directories(markdown-core-test-support PUBLIC ${MARKDOWN_CORE_TEST_SUPPORT_INCLUDES})
target_link_libraries(markdown-core-test-support PUBLIC ${MARKDOWN_CORE_TEST_LIBRARIES})

function(markdown_core_add_runner name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} markdown-core-test-support)
endfunction()

markdown_core_add_runner(spec_runner runners/spec_runner.c)
markdown_core_add_runner(pathological_runner runners/pathological_runner.c)

# oom_runner injects an allocator into the internal implementation of the
# public facade transaction, so it links the static engine directly.
if(MARKDOWN_CORE_STATIC)
    add_executable(oom_runner runners/oom_runner.c)
    target_include_directories(
        oom_runner PRIVATE "${MARKDOWN_CORE_CORE_SOURCE_DIR}" "${MARKDOWN_CORE_CORE_BINARY_DIR}"
                           "${MARKDOWN_CORE_EXTENSIONS_SOURCE_DIR}" "${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR}")
    target_link_libraries(oom_runner libmarkdown-core-public-static)
endif()
markdown_core_add_runner(entity_runner runners/entity_runner.c)
markdown_core_add_runner(dump_cli_runner runners/dump_cli_runner.c)
markdown_core_add_runner(fuzz_smoke_runner runners/fuzz_smoke_runner.c)
markdown_core_add_runner(concurrency_runner runners/concurrency_runner.c)
markdown_core_add_runner(stress_runner runners/stress_runner.c)
add_executable(corpus_guard runners/corpus_guard.c)

target_include_directories(fuzz_smoke_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_include_directories(concurrency_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_include_directories(stress_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
if(NOT WIN32)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(concurrency_runner Threads::Threads)
endif()

# Facade tests exercise the read-only markdown_core C API directly.
add_executable(facade_test facade.c)
target_include_directories(facade_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(facade_test ${MARKDOWN_CORE_TEST_LIBRARIES})

add_executable(facade_cplusplus_test facade_cplusplus.cpp)
target_include_directories(facade_cplusplus_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(facade_cplusplus_test ${MARKDOWN_CORE_TEST_LIBRARIES})

set(MARKDOWN_CORE_ALL_TESTS "")

function(markdown_core_add_test name label timeout)
    add_test(NAME ${name} COMMAND ${ARGN})
    set_tests_properties(${name} PROPERTIES LABELS "${label}" TIMEOUT ${timeout})
    list(APPEND MARKDOWN_CORE_ALL_TESTS ${name})
    set(MARKDOWN_CORE_ALL_TESTS
        "${MARKDOWN_CORE_ALL_TESTS}"
        PARENT_SCOPE)
endfunction()

# api: legacy engine API harness (native C/C++).
markdown_core_add_test(api_engine api 120 api_test)

# conformance: public facade/schema shape and every manifest-owned canonical dump.
markdown_core_add_test(
    facade_native
    conformance
    120
    facade_test
    --fixtures
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}"
    ${MARKDOWN_CORE_CANONICAL_NAMES})
markdown_core_add_test(consumer_facade_cplusplus consumer 120 facade_cplusplus_test)

# The installed CLI parses the one dialect and refuses every switch: a former
# `--profile` must not silently parse the dialect, and must not parse at all.
markdown_core_add_test(
    regression_cli_refuses_switches
    regression
    120
    $<TARGET_FILE:markdown-core>
    --profile
    commonmark
    "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/profile-order.md")
set_tests_properties(regression_cli_refuses_switches PROPERTIES PASS_REGULAR_EXPRESSION "takes no options"
                                                                FAIL_REGULAR_EXPRESSION "Document scope=")

# Facade concurrency contract: the first_parse case releases every thread into
# its first parse in a fresh process; stress covers concurrent instance-local
# parsing over the input matrix.
markdown_core_add_test(facade_concurrent_first_parse facade 240 concurrency_runner --case first_parse)
markdown_core_add_test(facade_concurrent_stress facade 600 concurrency_runner --case stress)

# Repeated instance construction/destruction and facade failures must not affect
# any later parse.
markdown_core_add_test(regression_instance_lifecycle regression 240 concurrency_runner --case lifecycle)

# Large/deep/repeated-input correctness checks are distinct from timed
# benchmark workloads over related input shapes.
foreach(stress_case large_document deep_nesting repeated_release)
    markdown_core_add_test(pathological_stress_${stress_case} pathological 600 stress_runner --case ${stress_case})
endforeach()

markdown_core_add_test(
    facade_dump_cli
    conformance
    120
    dump_cli_runner
    --program
    $<TARGET_FILE:markdown-core>
    --fixtures
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}"
    ${MARKDOWN_CORE_CANONICAL_NAMES})

# spec: the product-owned GFM 0.29 golden (base examples plus tagged extension
# sections), punctuation stored as written, and entities. Current CommonMark
# authority is the separately pinned cmark parity gate, not this historical
# golden. The runner also asserts dump determinism per example.
markdown_core_add_test(spec_gfm_golden spec 240 spec_runner --spec "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/spec.txt")
markdown_core_add_test(spec_punctuation spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/punctuation.txt")
markdown_core_add_test(spec_entities spec 240 entity_runner)

# regression: fixed bug corpus.
markdown_core_add_test(regression_commonmark regression 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/regression.txt")

# regression: every injected allocation failure terminates the parse and is
# reported to the facade consumer; no lossless fallback may return a document.
if(MARKDOWN_CORE_STATIC)
    markdown_core_add_test(regression_strict_oom regression 240 oom_runner --case strict_oom)
endif()

# extensions: the product's reviewed requirements for the GFM extensions,
# formulas, and directives. Every suite parses the one dialect, exactly as a
# consumer would; a fence tag classifies an example for the oracle corpora and
# selects nothing.
markdown_core_add_test(extensions_gfm extensions 240 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions.txt")
markdown_core_add_test(extensions_formula_github extensions 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-formula-github.txt")
markdown_core_add_test(extensions_formula_latex extensions 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-formula-latex.txt")
markdown_core_add_test(extensions_formula_conflicts extensions 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-formula-conflicts.txt")
markdown_core_add_test(extensions_directive extensions 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-directive.txt")
# extensions: attaching one extension must not change another's block opener.
markdown_core_add_test(extensions_conflicts extensions 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-conflicts.txt")

# spec: the examples of each dialect module whose behavior has landed, byte for
# byte in the dump grammar of today; one fixture per module.
markdown_core_add_test(spec_dialect_properties spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-properties.txt")
markdown_core_add_test(spec_dialect_marks spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-marks.txt")
markdown_core_add_test(spec_dialect_insertion spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-insertion.txt")
markdown_core_add_test(spec_dialect_bracketed_spans spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-bracketed-spans.txt")
markdown_core_add_test(spec_dialect_superscript_and_subscript spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-superscript-and-subscript.txt")
markdown_core_add_test(spec_dialect_strikethrough spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-strikethrough.txt")
markdown_core_add_test(spec_dialect_cross_links spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-cross-links.txt")
markdown_core_add_test(spec_dialect_comments spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-comments.txt")
markdown_core_add_test(spec_dialect_links_and_images spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-links-and-images.txt")
markdown_core_add_test(spec_dialect_footnotes spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-footnotes.txt")
markdown_core_add_test(spec_dialect_callouts spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-callouts.txt")
markdown_core_add_test(spec_dialect_tables spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-tables.txt")
markdown_core_add_test(spec_dialect_directives spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-directives.txt")
markdown_core_add_test(spec_dialect_anchors spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-anchors.txt")
markdown_core_add_test(spec_dialect_attributes spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-attributes.txt")
markdown_core_add_test(spec_dialect_lists spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-lists.txt")
markdown_core_add_test(spec_dialect_task_lists spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-task-lists.txt")
markdown_core_add_test(spec_dialect_block_identifiers spec 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/dialect-block-identifiers.txt")
# pathological: adversarial inputs, one CTest test per case so timeouts and
# failures stay addressable.  The audit script cross-checks this list against
# `pathological_runner --list`.
set(MARKDOWN_CORE_PATHOLOGICAL_CASES
    nested_strong_emph
    many_emph_closers
    many_emph_openers
    many_link_closers
    many_link_openers
    mismatched_openers_closers
    openers_closers_multiple_of_3
    link_openers_emph_closers
    pattern_bracket_paren
    pattern_image_link
    hard_link_emph
    nested_brackets
    cross_reference_kinds
    nested_block_quotes
    deeply_nested_lists
    empty_lines_in_deeply_nested_lists
    empty_lines_in_deep_list_blockquote
    emphasis_in_deep_blockquote
    nul_in_input
    backticks
    unclosed_links_a
    unclosed_links_b
    task_marker_runs
    unclosed_comment
    comment_percent_runs
    comment_unclosed_body
    comment_nested_quote_candidates
    comment_nested_item_blank_runs
    tables
    reference_collisions
    reference_expansion_bound
    directive_unclosed_labels
    directive_unclosed_attributes
    directive_colon_pairs
    directive_long_label
    directive_long_attributes
    formula_long_dollar
    formula_long_backslash
    formula_dollar_backtick_openers
    formula_backslash_openers
    formula_backslash_closers
    formula_backslash_pairs
    formula_backslash_nested)
foreach(case IN LISTS MARKDOWN_CORE_PATHOLOGICAL_CASES)
    markdown_core_add_test(pathological_${case} pathological 30 pathological_runner --case ${case})
endforeach()

# fuzz: deterministic smoke over fixed corpora and seeded generated inputs.
# Long-running campaigns stay in the explicit AFL/libFuzzer maintenance
# targets and reuse the corpus under core/afl_test_cases.
markdown_core_add_test(
    fuzz_smoke
    fuzz
    240
    fuzz_smoke_runner
    --corpus
    "${MARKDOWN_CORE_TESTS_SOURCE_DIR}/core/afl_test_cases/test.md"
    --corpus
    "${MARKDOWN_CORE_TESTS_SOURCE_DIR}/core/afl_test_cases/properties.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/blocks.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/comments.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/completeness.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/formulas.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/inlines.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/media-dimensions.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/obsidian-integration.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/cross-links.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/block-identifiers.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/callouts.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/inline-footnotes.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/marks.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/insertion.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/task-markers.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/scopes.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/structure.md"
    --generated
    256)

# packaging: corpus/workspace policy guard.
markdown_core_add_test(packaging_corpus_guard packaging 60 corpus_guard --root "${CMAKE_SOURCE_DIR}")

# Performance measurement is a developer tool, not a correctness oracle. It is
# absent from normal and sanitizer builds so CI artifacts cannot accidentally
# promote hosted-runner timing into a merge gate. The explicit benchmark preset
# enables this block and reports measurements without wall-clock assertions.
if(MARKDOWN_CORE_BENCHMARKS)
    set(MARKDOWN_CORE_BENCH_SAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/samples")
    # The public flavour on purpose: the PR benchmark times the product library
    # and reports the size of the shared library this target therefore builds.
    markdown_core_add_runner(bench_runner runners/bench_runner.c)
    target_include_directories(bench_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
    markdown_core_add_test(benchmark_corpus_guard benchmark 60 corpus_guard --root "${CMAKE_SOURCE_DIR}")
    set(MARKDOWN_CORE_BENCH_WORKLOADS binding_baseline representative large_document deep_nesting extensions
                                      adversarial)
    foreach(workload IN LISTS MARKDOWN_CORE_BENCH_WORKLOADS)
        markdown_core_add_test(
            benchmark_${workload}
            benchmark
            600
            bench_runner
            --workload
            ${workload}
            --samples
            "${MARKDOWN_CORE_BENCH_SAMPLES_DIR}")
        set_tests_properties(benchmark_${workload} PROPERTIES RUN_SERIAL TRUE)
    endforeach()
endif()

if(WIN32 AND MARKDOWN_CORE_SHARED)
    file(TO_NATIVE_PATH ${MARKDOWN_CORE_CORE_BINARY_DIR} WIN_SRC_DLL_DIR)
    file(TO_NATIVE_PATH ${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR} WIN_EXTENSIONS_DLL_DIR)
    set(NEWPATH "${WIN_SRC_DLL_DIR};${WIN_EXTENSIONS_DLL_DIR};$ENV{PATH}")
    string(REPLACE ";" "\\;" NEWPATH "${NEWPATH}")
    set_tests_properties(${MARKDOWN_CORE_ALL_TESTS} PROPERTIES ENVIRONMENT "PATH=${NEWPATH}")
endif()
