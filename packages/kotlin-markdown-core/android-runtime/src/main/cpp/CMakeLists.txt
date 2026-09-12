cmake_minimum_required(VERSION 3.22.1)
project(markdown_core_kotlin_android LANGUAGES C)

set(MARKDOWN_CORE_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../.."
    CACHE PATH "Path to the markdown-core repository root")
get_filename_component(MARKDOWN_CORE_ROOT "${MARKDOWN_CORE_ROOT}" ABSOLUTE)
set(MARKDOWN_CORE_CORE_DIR "${MARKDOWN_CORE_ROOT}/packages/markdown-core/core")
set(MARKDOWN_CORE_ELEMENTS_DIR "${MARKDOWN_CORE_ROOT}/packages/markdown-core/elements")

add_library(
    markdown_core_kotlin SHARED
    "${MARKDOWN_CORE_CORE_DIR}/markdown_core.c"
    "${MARKDOWN_CORE_CORE_DIR}/node.c"
    "${MARKDOWN_CORE_CORE_DIR}/iterator.c"
    "${MARKDOWN_CORE_CORE_DIR}/blocks.c"
    "${MARKDOWN_CORE_CORE_DIR}/inlines.c"
    "${MARKDOWN_CORE_CORE_DIR}/utf8.c"
    "${MARKDOWN_CORE_CORE_DIR}/buffer.c"
    "${MARKDOWN_CORE_CORE_DIR}/references.c"
    "${MARKDOWN_CORE_CORE_DIR}/map.c"
    "${MARKDOWN_CORE_CORE_DIR}/houdini_html_u.c"
    "${MARKDOWN_CORE_CORE_DIR}/markdown_core_ctype.c"
    "${MARKDOWN_CORE_CORE_DIR}/linked_list.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/code.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/code_block.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/document.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/emphasis.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/html.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/html_block.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/line_break.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/paragraph.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/text.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/thematic_break.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/autolink_scanners.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/code_block_scanners.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/comment_scanners.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/footnote_scanners.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/formula_scanners.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/heading_scanners.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/html_scanners.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/link_scanners.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/table_scanners.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/text_scanners.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/ast.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/attributes.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/autolink.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/block_identifier.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/callout.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/citation.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/comment.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/core-elements.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/cross_link.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/definition_list.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/directive.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/footnote.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/formula.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/heading.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/insertion.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/link.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/list.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/mark.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/media.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/properties.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/span.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/specimen.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/strikethrough.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/subscript.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/superscript.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/table.c"
    "${MARKDOWN_CORE_ELEMENTS_DIR}/tasklist.c"
    "${MARKDOWN_CORE_ROOT}/packages/kotlin-markdown-core/src/native/markdown_core_kotlin_jni_payload.c"
    "${MARKDOWN_CORE_ROOT}/packages/kotlin-markdown-core/src/native/markdown_core_kotlin_jni.c")

target_include_directories(
    markdown_core_kotlin
    PRIVATE "${MARKDOWN_CORE_CORE_DIR}"
            "${MARKDOWN_CORE_ELEMENTS_DIR}"
            "${MARKDOWN_CORE_ROOT}/packages/markdown-core/include"
            "${MARKDOWN_CORE_ROOT}/packages/kotlin-markdown-core/src/native"
            "${MARKDOWN_CORE_ROOT}/packages/markdown-core/core/include")
target_compile_definitions(markdown_core_kotlin PRIVATE MARKDOWN_CORE_EXPORTS)
target_link_options(
    markdown_core_kotlin PRIVATE
    "-Wl,--version-script=${MARKDOWN_CORE_ROOT}/packages/kotlin-markdown-core/src/native/markdown_core_kotlin.map")
target_compile_options(
    markdown_core_kotlin
    PRIVATE $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:GNU>>:-Wall>
            $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:GNU>>:-Wextra>
            $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:GNU>>:-Wno-unused-parameter>
            $<$<OR:$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:GNU>>:-pedantic>)
set_target_properties(
    markdown_core_kotlin
    PROPERTIES C_STANDARD 99
               C_STANDARD_REQUIRED YES
               C_EXTENSIONS NO
               C_VISIBILITY_PRESET hidden
               POSITION_INDEPENDENT_CODE ON
               OUTPUT_NAME "markdown_core_kotlin")
