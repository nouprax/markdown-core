# CTest is the single source of truth for the C test graph.  Suites are
# grouped by label:
#
#   api facade consumer spec equivalence extensions regression pathological
#   complexity fuzz packaging benchmark
#
# The default correctness preset excludes conformance and benchmark; sanitizer
# correctness presets also exclude wall-clock complexity gates.  The benchmark
# preset selects only its label.  Every suite is native (C/C++); the test tree
# has no scripting-language dependency and never touches the network.

set(MARKDOWN_CORE_TEST_FIXTURES_DIR "${MARKDOWN_CORE_TESTS_SOURCE_DIR}/fixtures")
set(MARKDOWN_CORE_CANONICAL_AST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../specs/canonical-ast")
set(MARKDOWN_CORE_CANONICAL_AST_MANIFEST "${MARKDOWN_CORE_CANONICAL_AST_DIR}/manifest.json")
set(MARKDOWN_CORE_BENCH_SAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/samples")

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
set(MARKDOWN_CORE_CANONICAL_FACADE_ARGUMENTS "")
set(MARKDOWN_CORE_CANONICAL_OPTION_NAMES
    smartPunctuation
    footnotes
    stripHTMLComments
    tables
    strikethrough
    autolinks
    taskLists
    formulas
    dollarFormulaDelimiters
    latexFormulaDelimiters
    directives)
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
    set(case_option_mask "")
    foreach(option_name IN LISTS MARKDOWN_CORE_CANONICAL_OPTION_NAMES)
        string(
            JSON
            option_enabled
            GET
            "${MARKDOWN_CORE_CANONICAL_AST_JSON}"
            cases
            ${case_index}
            parseOptions
            ${option_name})
        if(option_enabled)
            string(APPEND case_option_mask "1")
        else()
            string(APPEND case_option_mask "0")
        endif()
    endforeach()
    list(APPEND MARKDOWN_CORE_CANONICAL_NAMES "${case_name}")
    list(APPEND MARKDOWN_CORE_CANONICAL_FACADE_ARGUMENTS "${case_name}" "${case_option_mask}")
endforeach()

if(NOT (MARKDOWN_CORE_SHARED OR MARKDOWN_CORE_STATIC))
    message(FATAL_ERROR "MARKDOWN_CORE_TESTS=ON requires MARKDOWN_CORE_SHARED" " or MARKDOWN_CORE_STATIC")
endif()

if(MARKDOWN_CORE_SHARED)
    set(MARKDOWN_CORE_TEST_LIBRARIES libmarkdown-core-extensions)
else()
    set(MARKDOWN_CORE_TEST_LIBRARIES libmarkdown-core-extensions_static libmarkdown-core_static)
endif()

# Shared native test support: fixture loading, in-process conversion, byte
# comparison, deterministic diffs, PRNG, timing, and the session replay
# harness (shadow text, delta mirror, per-commit equivalence verification).
add_library(markdown-core-test-support STATIC support/test_support.c support/test_support.h support/session_replay.c
                                              support/session_replay.h)
target_include_directories(
    markdown-core-test-support
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support" "${CMAKE_CURRENT_SOURCE_DIR}/../include"
           "${MARKDOWN_CORE_CORE_SOURCE_DIR}" "${MARKDOWN_CORE_CORE_BINARY_DIR}"
           "${MARKDOWN_CORE_EXTENSIONS_SOURCE_DIR}" "${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR}")
target_link_libraries(markdown-core-test-support PUBLIC ${MARKDOWN_CORE_TEST_LIBRARIES})

function(markdown_core_add_runner name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} markdown-core-test-support)
endfunction()

markdown_core_add_runner(spec_runner runners/spec_runner.c)
markdown_core_add_runner(equivalence_runner runners/equivalence_runner.c)
markdown_core_add_runner(pathological_runner runners/pathological_runner.c)
markdown_core_add_runner(complexity_runner runners/complexity_runner.c)

# fallback_runner drives core-private map symbols directly, so it links the
# static engine instead of the shared facade and skips test-support.
if(MARKDOWN_CORE_STATIC)
    add_executable(fallback_runner runners/fallback_runner.c)
    target_include_directories(
        fallback_runner PRIVATE "${MARKDOWN_CORE_CORE_SOURCE_DIR}" "${MARKDOWN_CORE_CORE_BINARY_DIR}"
                                "${MARKDOWN_CORE_EXTENSIONS_SOURCE_DIR}" "${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR}")
    target_link_libraries(fallback_runner libmarkdown-core-extensions_static libmarkdown-core_static)
endif()
markdown_core_add_runner(entity_runner runners/entity_runner.c)
markdown_core_add_runner(dump_cli_runner runners/dump_cli_runner.c)
markdown_core_add_runner(fuzz_smoke_runner runners/fuzz_smoke_runner.c)
markdown_core_add_runner(bench_runner runners/bench_runner.c)
markdown_core_add_runner(concurrency_runner runners/concurrency_runner.c)
markdown_core_add_runner(stress_runner runners/stress_runner.c)
add_executable(corpus_guard runners/corpus_guard.c)

target_include_directories(fuzz_smoke_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_include_directories(bench_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
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
    ${MARKDOWN_CORE_CANONICAL_FACADE_ARGUMENTS})
markdown_core_add_test(consumer_facade_cplusplus consumer 120 facade_cplusplus_test)

# facade concurrency contract: the first_parse case must exercise library
# initialization in a fresh process, so it never shares a process with a
# prior parse; stress covers steady-state concurrent parsing with
# disagreeing option sets.
markdown_core_add_test(facade_concurrent_first_parse facade 240 concurrency_runner --case first_parse)
markdown_core_add_test(facade_concurrent_stress facade 600 concurrency_runner --case stress)

# facade multi-session contract: isolated sessions streaming concurrently
# with per-byte commits must never interfere, and one session's document
# must support concurrent read-only access between mutating calls.
markdown_core_add_test(facade_concurrent_sessions facade 600 concurrency_runner --case sessions)

# regression: registry lifecycle stays immutable across parse/free cycles
# and facade failure paths.
markdown_core_add_test(regression_registry_lifecycle regression 240 concurrency_runner --case lifecycle)

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

# spec: CommonMark spec, smart punctuation, entities.  Expected results are
# canonical AST dumps; the runner also asserts dump determinism per example.
markdown_core_add_test(spec_commonmark spec 240 spec_runner --spec "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/spec.txt")
markdown_core_add_test(
    spec_smart_punctuation
    spec
    120
    spec_runner
    --spec
    "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/smart_punct.txt"
    --option
    smart)
markdown_core_add_test(spec_entities spec 240 entity_runner)

# regression: fixed bug corpus.
markdown_core_add_test(regression_commonmark regression 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/regression.txt")

# regression: allocation-failure and probe-limit fallbacks of the shared
# byte-key index keep hash-path duplicate semantics; probe exhaustion grows
# the table once before falling back.
if(MARKDOWN_CORE_STATIC)
    set(MARKDOWN_CORE_FALLBACK_CASES
        reference_sorted_fallback
        reference_map_v2
        directive_sorted_fallback
        key_index_probe_growth
        map_prepare_oom
        constructor_oom
        oom_sweep
        session_oom_sweep
        restart_locality_counters)
    foreach(case IN LISTS MARKDOWN_CORE_FALLBACK_CASES)
        markdown_core_add_test(regression_fallback_${case} regression 240 fallback_runner --case ${case})
    endforeach()
endif()

# equivalence: incrementally edited sessions must dump byte-identically to a
# one-shot parse of the same final text after every commit, and the delta
# stream must account for every observable node change (mirror check).
markdown_core_add_test(
    equivalence_canonical
    equivalence
    240
    equivalence_runner
    --case
    canonical
    --fixtures
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}"
    ${MARKDOWN_CORE_CANONICAL_FACADE_ARGUMENTS})
markdown_core_add_test(
    equivalence_spec
    equivalence
    600
    equivalence_runner
    --case
    spec
    --spec
    "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/spec.txt")
markdown_core_add_test(
    equivalence_random_edits
    equivalence
    600
    equivalence_runner
    --case
    random_edits
    --spec
    "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/spec.txt")
markdown_core_add_test(equivalence_link_ref_edits equivalence 120 equivalence_runner --case link_ref_edits)
markdown_core_add_test(equivalence_footnote_edits equivalence 120 equivalence_runner --case footnote_edits)
markdown_core_add_test(equivalence_boundary_edits equivalence 120 equivalence_runner --case boundary_edits)

# extensions: GFM extensions, formulas, directives, and option gates.
markdown_core_add_test(
    extensions_gfm
    extensions
    240
    spec_runner
    --spec
    "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions.txt"
    --option
    table
    --option
    strikethrough
    --option
    autolink
    --option
    tasklist
    --option
    footnotes)
markdown_core_add_test(
    extensions_formula_github
    extensions
    120
    spec_runner
    --spec
    "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-formula-github.txt"
    --option
    formula
    --option
    dollar-formula-delimiters)
markdown_core_add_test(
    extensions_formula_option_gates
    extensions
    120
    spec_runner
    --spec
    "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-formula-option-gates.txt"
    --option
    formula)
markdown_core_add_test(
    extensions_formula_latex
    extensions
    120
    spec_runner
    --spec
    "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-formula-latex.txt"
    --option
    formula
    --option
    dollar-formula-delimiters
    --option
    latex-formula-delimiters)
markdown_core_add_test(
    extensions_formula_conflicts
    extensions
    120
    spec_runner
    --spec
    "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-formula-conflicts.txt"
    --option
    formula
    --option
    dollar-formula-delimiters
    --option
    latex-formula-delimiters)
markdown_core_add_test(
    extensions_directive
    extensions
    120
    spec_runner
    --spec
    "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-directive.txt"
    --option
    directive)
markdown_core_add_test(extensions_directive_option_gates extensions 120 spec_runner --spec
                       "${MARKDOWN_CORE_TEST_FIXTURES_DIR}/extensions-directive-option-gates.txt")
# pathological: adversarial inputs, one CTest test per case so timeouts and
# failures stay addressable.  The audit script cross-checks this list against
# `pathological_runner --list` and `complexity_runner --list`.
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
    nested_block_quotes
    deeply_nested_lists
    nul_in_input
    backticks
    unclosed_links_a
    unclosed_links_b
    unclosed_comment
    tables
    reference_collisions
    directive_unclosed_labels
    directive_unclosed_attributes
    directive_colon_pairs
    directive_long_label
    directive_long_attributes
    formula_long_dollar
    formula_long_backslash
    formula_dollar_backtick_openers
    formula_backslash_openers)
foreach(case IN LISTS MARKDOWN_CORE_PATHOLOGICAL_CASES)
    markdown_core_add_test(pathological_${case} pathological 30 pathological_runner --case ${case})
endforeach()

set(MARKDOWN_CORE_COMPLEXITY_CASES
    valid_long_quoted_value
    valid_consecutive_backslashes
    unclosed_long_quoted_value
    unclosed_backslash_value
    many_unique_attributes
    many_duplicate_attributes
    many_unique_references
    many_duplicate_references
    session_stream_flat
    session_edit_storm
    session_ref_retarget
    session_footnote_shift
    session_head_defs
    session_footnote_defs
    session_quote_suffix)
foreach(case IN LISTS MARKDOWN_CORE_COMPLEXITY_CASES)
    markdown_core_add_test(pathological_complexity_${case} "pathological;complexity" 120 complexity_runner --case
                           ${case})
    set_tests_properties(pathological_complexity_${case} PROPERTIES RUN_SERIAL TRUE)
endforeach()

# fuzz: deterministic smoke over fixed corpora and seeded generated inputs.
# Long-running campaigns stay in the explicit AFL/libFuzzer maintenance
# targets and reuse the corpus under core/afl_test_cases; the session
# edit-script campaign (fuzz_session_edits) shares the script format the
# seeded smoke below drives deterministically.
markdown_core_add_test(
    fuzz_smoke
    fuzz
    240
    fuzz_smoke_runner
    --corpus
    "${MARKDOWN_CORE_TESTS_SOURCE_DIR}/core/afl_test_cases/test.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/blocks.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/completeness.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/formulas.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/inlines.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/scopes.md"
    --corpus
    "${MARKDOWN_CORE_CANONICAL_AST_DIR}/structure.md"
    --generated
    256)
markdown_core_add_test(
    fuzz_script_smoke
    fuzz
    240
    fuzz_smoke_runner
    --generated
    0
    --script-generated
    512)

# packaging: corpus/workspace policy guard, also run before benchmarks.
markdown_core_add_test(packaging_corpus_guard packaging 60 corpus_guard --root "${CMAKE_SOURCE_DIR}")
markdown_core_add_test(benchmark_corpus_guard benchmark 60 corpus_guard --root "${CMAKE_SOURCE_DIR}")

# benchmark: independently scheduled performance workloads; serial, offline,
# deterministic inputs, relative-scaling assertions only.
set(MARKDOWN_CORE_BENCH_WORKLOADS binding_baseline representative large_document deep_nesting extensions adversarial)
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

if(WIN32 AND MARKDOWN_CORE_SHARED)
    file(TO_NATIVE_PATH ${MARKDOWN_CORE_CORE_BINARY_DIR} WIN_SRC_DLL_DIR)
    file(TO_NATIVE_PATH ${MARKDOWN_CORE_EXTENSIONS_BINARY_DIR} WIN_EXTENSIONS_DLL_DIR)
    set(NEWPATH "${WIN_SRC_DLL_DIR};${WIN_EXTENSIONS_DLL_DIR};$ENV{PATH}")
    string(REPLACE ";" "\\;" NEWPATH "${NEWPATH}")
    set_tests_properties(${MARKDOWN_CORE_ALL_TESTS} PROPERTIES ENVIRONMENT "PATH=${NEWPATH}")
endif()
