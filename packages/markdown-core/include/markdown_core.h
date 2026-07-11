#ifndef MARKDOWN_CORE_FACADE_H
#define MARKDOWN_CORE_FACADE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Thread safety and ownership contract
 * ====================================
 *
 * Initialization: the library initializes itself inside
 * markdown_core_document_parse under a process-level once. Concurrent first
 * calls from any number of threads are safe; no warmup, external lock, or
 * explicit init call is required. The extension registry established by that
 * initialization is immutable for the remainder of the process; there is no
 * teardown or re-initialization path.
 *
 * Distinct documents: parse, traversal, dump, and free of *different*
 * documents may run fully concurrently. A parse call shares no mutable state
 * with other parse calls.
 *
 * A single document: after markdown_core_document_parse returns, the document
 * and its nodes are logically immutable through this API. Concurrent
 * read-only access (traversal, accessors, dump) to the same document from
 * multiple threads is safe. markdown_core_document_free is the only mutating
 * operation: the caller must ensure it happens after all other access to that
 * document has completed (external synchronization); no access is allowed
 * afterwards. Node handles and string views borrow from the owning document
 * and end with it.
 *
 * Errors: a markdown_core_error returned through an out-parameter is owned by
 * the caller of that call and is not shared with any other thread; release it
 * with markdown_core_error_free (NULL is allowed). Dump buffers are owned by
 * the caller and released with markdown_core_dump_free (NULL is allowed).
 *
 * No other process-global lifecycle exists: this contract is complete, and
 * bindings must not rely on undocumented conventions.
 */

#if defined(_WIN32) && !defined(MARKDOWN_CORE_STATIC_DEFINE)
#if defined(MARKDOWN_CORE_EXTENSIONS_EXPORTS)
#define MARKDOWN_CORE_API __declspec(dllexport)
#else
#define MARKDOWN_CORE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define MARKDOWN_CORE_API __attribute__((visibility("default")))
#else
#define MARKDOWN_CORE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct markdown_core_document markdown_core_document;
#ifndef MARKDOWN_CORE_NODE_TYPEDEF
#define MARKDOWN_CORE_NODE_TYPEDEF
typedef struct markdown_core_node markdown_core_node;
#endif
typedef struct markdown_core_error markdown_core_error;

typedef struct markdown_core_string_view {
    const uint8_t *data;
    size_t length;
} markdown_core_string_view;

typedef struct markdown_core_position {
    int32_t line;
    int32_t column;
} markdown_core_position;

typedef struct markdown_core_scope {
    markdown_core_position start;
    markdown_core_position end;
} markdown_core_scope;

typedef struct markdown_core_parse_options {
    bool smart_punctuation;
    bool footnotes;
    bool strip_html_comments;
    bool tables;
    bool strikethrough;
    bool autolinks;
    bool task_lists;
    bool formulas;
    bool dollar_formula_delimiters;
    bool latex_formula_delimiters;
    bool directives;
} markdown_core_parse_options;

typedef enum markdown_core_error_code {
    MARKDOWN_CORE_ERROR_NONE = 0,
    MARKDOWN_CORE_ERROR_INVALID_ARGUMENT = 1,
    MARKDOWN_CORE_ERROR_ALLOCATION_FAILED = 2,
    MARKDOWN_CORE_ERROR_INTERNAL = 3
} markdown_core_error_code;

typedef enum markdown_core_node_kind {
    MARKDOWN_CORE_KIND_NONE = 0,
    MARKDOWN_CORE_KIND_DOCUMENT,
    MARKDOWN_CORE_KIND_BLOCK_QUOTE,
    MARKDOWN_CORE_KIND_PARAGRAPH,
    MARKDOWN_CORE_KIND_HEADING,
    MARKDOWN_CORE_KIND_THEMATIC_BREAK,
    MARKDOWN_CORE_KIND_LIST,
    MARKDOWN_CORE_KIND_LIST_ITEM,
    MARKDOWN_CORE_KIND_CODE_BLOCK,
    MARKDOWN_CORE_KIND_HTML_BLOCK,
    MARKDOWN_CORE_KIND_FORMULA_BLOCK,
    MARKDOWN_CORE_KIND_TABLE,
    MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK,
    MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION,
    MARKDOWN_CORE_KIND_TEXT,
    MARKDOWN_CORE_KIND_SOFT_BREAK,
    MARKDOWN_CORE_KIND_LINE_BREAK,
    MARKDOWN_CORE_KIND_CODE,
    MARKDOWN_CORE_KIND_HTML,
    MARKDOWN_CORE_KIND_FORMULA,
    MARKDOWN_CORE_KIND_EMPHASIS,
    MARKDOWN_CORE_KIND_STRONG,
    MARKDOWN_CORE_KIND_STRIKETHROUGH,
    MARKDOWN_CORE_KIND_LINK,
    MARKDOWN_CORE_KIND_IMAGE,
    MARKDOWN_CORE_KIND_DIRECTIVE,
    MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE,
    MARKDOWN_CORE_KIND_TABLE_ROW,
    MARKDOWN_CORE_KIND_TABLE_CELL
} markdown_core_node_kind;

typedef enum markdown_core_list_flavor {
    MARKDOWN_CORE_LIST_FLAVOR_BULLET = 1,
    MARKDOWN_CORE_LIST_FLAVOR_ORDERED = 2
} markdown_core_list_flavor;

typedef enum markdown_core_placement_mode {
    MARKDOWN_CORE_PLACEMENT_EMBEDDED = 1,
    MARKDOWN_CORE_PLACEMENT_STANDALONE = 2
} markdown_core_placement_mode;

typedef enum markdown_core_table_alignment {
    MARKDOWN_CORE_TABLE_ALIGNMENT_NONE = 0,
    MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT = 1,
    MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER = 2,
    MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT = 3
} markdown_core_table_alignment;

typedef struct markdown_core_optional_i64 {
    bool has_value;
    int64_t value;
} markdown_core_optional_i64;

typedef struct markdown_core_optional_bool {
    bool has_value;
    bool value;
} markdown_core_optional_bool;

/** Initializes every field to the frozen Markdown Core defaults. */
MARKDOWN_CORE_API void markdown_core_parse_options_init(markdown_core_parse_options *options);

/**
 * Parses exactly `length` UTF-8 bytes. `options == NULL` selects the defaults.
 * The returned document owns all nodes and borrowed string views. On failure,
 * NULL is returned and `*error` is set when `error` is non-NULL.
 */
MARKDOWN_CORE_API markdown_core_document *markdown_core_document_parse(const uint8_t *source, size_t length,
                                                                       const markdown_core_parse_options *options,
                                                                       markdown_core_error **error);
MARKDOWN_CORE_API void markdown_core_document_free(markdown_core_document *document);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_document_root(const markdown_core_document *document);

MARKDOWN_CORE_API markdown_core_error_code markdown_core_error_get_code(const markdown_core_error *error);
MARKDOWN_CORE_API markdown_core_string_view markdown_core_error_get_message(const markdown_core_error *error);
MARKDOWN_CORE_API bool markdown_core_error_get_scope(const markdown_core_error *error, markdown_core_scope *scope);
MARKDOWN_CORE_API void markdown_core_error_free(markdown_core_error *error);

MARKDOWN_CORE_API markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node);
MARKDOWN_CORE_API const char *markdown_core_node_kind_name(markdown_core_node_kind kind);
MARKDOWN_CORE_API markdown_core_scope markdown_core_node_scope(const markdown_core_node *node);

/** Canonical traversal hides directive-label wrapper nodes. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_get_first_child(const markdown_core_node *node);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_get_next_sibling(const markdown_core_node *node);
MARKDOWN_CORE_API size_t markdown_core_node_child_count(const markdown_core_node *node);

MARKDOWN_CORE_API bool markdown_core_node_heading_level(const markdown_core_node *node, int32_t *level);
MARKDOWN_CORE_API bool markdown_core_node_list_properties(const markdown_core_node *node,
                                                          markdown_core_list_flavor *flavor,
                                                          markdown_core_optional_i64 *start, bool *tight);
MARKDOWN_CORE_API bool markdown_core_node_list_item_checked(const markdown_core_node *node,
                                                            markdown_core_optional_bool *checked);
MARKDOWN_CORE_API bool markdown_core_node_code_block_properties(const markdown_core_node *node,
                                                                markdown_core_string_view *info,
                                                                markdown_core_string_view *language,
                                                                markdown_core_string_view *literal, bool *fenced,
                                                                bool *closed);
MARKDOWN_CORE_API bool markdown_core_node_literal(const markdown_core_node *node, markdown_core_string_view *literal);
MARKDOWN_CORE_API bool markdown_core_node_formula_properties(const markdown_core_node *node,
                                                             markdown_core_placement_mode *mode,
                                                             markdown_core_string_view *literal);
MARKDOWN_CORE_API bool markdown_core_node_table_column_count(const markdown_core_node *node, size_t *count);
MARKDOWN_CORE_API bool markdown_core_node_table_alignment_at(const markdown_core_node *node, size_t index,
                                                             markdown_core_table_alignment *alignment);
MARKDOWN_CORE_API bool markdown_core_node_table_row_is_header(const markdown_core_node *node, bool *is_header);
MARKDOWN_CORE_API bool markdown_core_node_directive_properties(const markdown_core_node *node,
                                                               markdown_core_placement_mode *mode,
                                                               markdown_core_string_view *name,
                                                               markdown_core_string_view *attributes, bool *has_label,
                                                               size_t *label_count);
MARKDOWN_CORE_API const markdown_core_node *
markdown_core_node_directive_first_label_child(const markdown_core_node *node);
MARKDOWN_CORE_API const markdown_core_node *
markdown_core_node_directive_first_content_child(const markdown_core_node *node);
MARKDOWN_CORE_API bool markdown_core_node_link_properties(const markdown_core_node *node,
                                                          markdown_core_string_view *destination,
                                                          markdown_core_string_view *title);
MARKDOWN_CORE_API bool markdown_core_node_image_properties(const markdown_core_node *node,
                                                           markdown_core_string_view *source,
                                                           markdown_core_string_view *title);
MARKDOWN_CORE_API bool markdown_core_node_footnote_id(const markdown_core_node *node, markdown_core_string_view *id);

/** Allocates the canonical file-tree dump. Free it with markdown_core_dump_free. */
MARKDOWN_CORE_API bool markdown_core_document_dump(const markdown_core_document *document, uint8_t **output,
                                                   size_t *length, markdown_core_error **error);
MARKDOWN_CORE_API void markdown_core_dump_free(uint8_t *output);

#ifdef __cplusplus
}
#endif

#endif
