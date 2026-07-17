#ifndef MARKDOWN_CORE_FACADE_H
#define MARKDOWN_CORE_FACADE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Thread safety and ownership contract
 * ====================================
 *
 * Initialization: none. The library holds no process-level mutable state —
 * no registry, no locks, no lazy initialization. Concurrent first calls from
 * any number of threads are safe by construction; no warmup, external lock,
 * or explicit init call is required.
 *
 * Distinct documents and sessions: parse, traversal, dump, and free of
 * *different* documents, and every operation on *different* sessions, may
 * run fully concurrently. A parse call or session shares no mutable state
 * with any other.
 *
 * A single session: calls on one session must be externally synchronized
 * (one writer at a time). Between mutating calls (edit, commit, free), the
 * session's document view, its nodes, and lookups are safe for concurrent
 * reads from any thread. The document view borrowed from a session is valid
 * until the next mutating call on that session. Changesets are caller-owned
 * plain data: they survive the session and are released with
 * markdown_core_changeset_free.
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
typedef struct markdown_core_session markdown_core_session;
typedef struct markdown_core_changeset markdown_core_changeset;

/** Session-assigned node identity: unique within a session, never reused,
 * stable across incremental commits while the node remains the same kind of
 * thing at the same place. 0 is never a valid id. */
typedef uint64_t markdown_core_node_id;

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

/*
 * Incremental sessions
 * ====================
 *
 * A session owns one Markdown text and its living AST. Apply edits (append
 * is an edit at end-of-text), then commit: the session reparses, reuses node
 * identity wherever the content is unchanged, and reports exactly what
 * changed. After any sequence of edits and commits the document is
 * semantically identical to a one-shot parse of the same final text.
 *
 * The stored text is the raw bytes exactly as edited; NUL and invalid UTF-8
 * are replaced with U+FFFD during parsing, per line, exactly as
 * markdown_core_document_parse does. A streamed append may therefore
 * complete a multi-byte character whose first bytes arrived earlier.
 *
 * Commits are transactional: on failure the session stays valid at its
 * previous revision and the commit may be retried (applied edits are
 * retained — the text advances, the tree does not).
 */

/** Opens an empty session at revision 0. `options == NULL` selects the
 * defaults; options are immutable for the session lifetime. */
MARKDOWN_CORE_API markdown_core_session *markdown_core_session_open(const markdown_core_parse_options *options,
                                                                    markdown_core_error **error);
MARKDOWN_CORE_API void markdown_core_session_free(markdown_core_session *session);

/** Replaces bytes [byte_start, byte_end) of the stored text with
 * `bytes[0..length)`. Append passes byte_start == byte_end ==
 * markdown_core_session_length. Edits only update the text; parsing happens
 * at commit. */
MARKDOWN_CORE_API bool markdown_core_session_edit(markdown_core_session *session, size_t byte_start, size_t byte_end,
                                                  const uint8_t *bytes, size_t length, markdown_core_error **error);

/** Reparses the pending text and advances the revision. When `changes` is
 * non-NULL it receives a caller-owned changeset (release with
 * markdown_core_changeset_free). */
MARKDOWN_CORE_API bool markdown_core_session_commit(markdown_core_session *session, markdown_core_changeset **changes,
                                                    markdown_core_error **error);

/** Borrowed view of the last committed document; valid until the next
 * mutating call on the session. */
MARKDOWN_CORE_API const markdown_core_document *markdown_core_session_document(const markdown_core_session *session);
MARKDOWN_CORE_API uint64_t markdown_core_session_revision(const markdown_core_session *session);

/** Per-session random salt; nodes from different sessions never share
 * identity even when ids collide numerically. */
MARKDOWN_CORE_API uint64_t markdown_core_session_lineage(const markdown_core_session *session);
MARKDOWN_CORE_API size_t markdown_core_session_length(const markdown_core_session *session);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_session_node_by_id(const markdown_core_session *session,
                                                                             markdown_core_node_id id);

/*
 * Footnote queries
 * ----------------
 *
 * The tree is source-faithful: definitions stay at their source position
 * whether referenced or not, and references always carry the label exactly
 * as written. Numbering, first-use order, resolution state, and
 * back-reference ordinals are answered from a session-maintained index.
 * Labels match case-folded with collapsed whitespace, and the earliest
 * definition of a label in document order wins. When a commit changes only
 * an answer below (an ordinal shift, a resolution flip), the affected nodes
 * are reported `changed` with a revision bump and identical dump content.
 */

/** Answers for one footnote node.
 * For a FootnoteReference: `definition` is the winning definition's id (0
 * while unresolved), `number` its 1-based first-use ordinal (0 while
 * unresolved), `reference_ordinal` the reference's 1-based position among
 * the label's references in document order, and `reference_count` how many
 * references share the label.
 * For a FootnoteDefinition: `definition` is the id of the label's winning
 * definition (its own id unless an earlier definition shadows it),
 * `number`/`reference_count` describe the label (0 when unreferenced), and
 * `reference_ordinal` is 0. */
typedef struct markdown_core_footnote_info {
    markdown_core_node_id definition;
    uint64_t number;
    uint64_t reference_ordinal;
    uint64_t reference_count;
} markdown_core_footnote_info;

/** Fills `info` for the footnote node with the given id at the current
 * revision. Returns false (with `info` zeroed) when the id does not name a
 * footnote reference or definition of this session. */
MARKDOWN_CORE_API bool markdown_core_session_footnote_info(const markdown_core_session *session,
                                                           markdown_core_node_id id, markdown_core_footnote_info *info);

/** Borrows the ids of the referenced (winning) definitions in first-use
 * order — the order a renderer lists them in. Valid until the next mutating
 * call on the session. */
MARKDOWN_CORE_API size_t markdown_core_session_footnotes(const markdown_core_session *session,
                                                         const markdown_core_node_id **ids);

/** Borrows the ids of the references that resolve to `definition`, in
 * document order — the renderer's back-reference targets. Empty unless
 * `definition` is a referenced winning definition. Valid until the next
 * mutating call on the session. */
MARKDOWN_CORE_API size_t markdown_core_session_footnote_references(const markdown_core_session *session,
                                                                   markdown_core_node_id definition,
                                                                   const markdown_core_node_id **ids);

/** Identity accessors. `id` is 0 only for a NULL node; `revision` is the
 * commit revision at which the node's own fields, child list, or any
 * descendant last changed — two nodes of one session with equal (id,
 * revision) have identical content. A pure positional shift never changes a
 * node's revision. */
MARKDOWN_CORE_API markdown_core_node_id markdown_core_node_get_id(const markdown_core_node *node);
MARKDOWN_CORE_API uint64_t markdown_core_node_get_revision(const markdown_core_node *node);

/** Canonical parent: NULL for the root; a directive-label child's parent is
 * its owning directive (label wrappers are never exposed). */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_get_parent(const markdown_core_node *node);

/** Changeset accessors. The four arrays are disjoint: `added` and `removed`
 * list nodes that appeared/disappeared, `changed` lists nodes whose own
 * fields or direct child list changed, and `bubbled` lists ancestors whose
 * revision advanced only because a descendant changed. Ids of removed nodes
 * are retired and never reused. */
MARKDOWN_CORE_API void markdown_core_changeset_revisions(const markdown_core_changeset *changes, uint64_t *before,
                                                         uint64_t *after);
MARKDOWN_CORE_API size_t markdown_core_changeset_added(const markdown_core_changeset *changes,
                                                       const markdown_core_node_id **ids);
MARKDOWN_CORE_API size_t markdown_core_changeset_removed(const markdown_core_changeset *changes,
                                                         const markdown_core_node_id **ids);
MARKDOWN_CORE_API size_t markdown_core_changeset_changed(const markdown_core_changeset *changes,
                                                         const markdown_core_node_id **ids);
MARKDOWN_CORE_API size_t markdown_core_changeset_bubbled(const markdown_core_changeset *changes,
                                                         const markdown_core_node_id **ids);
MARKDOWN_CORE_API void markdown_core_changeset_free(markdown_core_changeset *changes);

#ifdef __cplusplus
}
#endif

#endif
