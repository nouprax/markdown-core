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
    MARKDOWN_CORE_KIND_TABLE_CELL,
    MARKDOWN_CORE_KIND_DIRECTIVE_LABEL,
    /* Appended, not inserted beside the other block kinds: this enum's ordinal
     * IS the wire kind every binding decodes, so a kind added in the middle
     * renumbers every kind after it. */
    MARKDOWN_CORE_KIND_REFERENCE_DEFINITION,
    MARKDOWN_CORE_KIND_LINK_REFERENCE,
    MARKDOWN_CORE_KIND_IMAGE_REFERENCE
} markdown_core_node_kind;

/** The form a reference was written in: `[t][l]`, `[l][]` and `[l]` all
 * resolve the same way and are three different spellings, so nothing else on
 * the node records which one the author wrote. A footnote reference has no
 * form: there is one footnote call syntax (Q3). */
#ifndef MARKDOWN_CORE_REFERENCE_FORM_TYPEDEF
#define MARKDOWN_CORE_REFERENCE_FORM_TYPEDEF
typedef enum markdown_core_reference_form {
    MARKDOWN_CORE_REFERENCE_FULL = 1,
    MARKDOWN_CORE_REFERENCE_COLLAPSED = 2,
    MARKDOWN_CORE_REFERENCE_SHORTCUT = 3
} markdown_core_reference_form;
#endif

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

/** THE ROLE A REGION'S BYTES PLAYED. Not what they look like -- what they DID.
 *
 * MARKER     the bytes that made the owner the kind of thing it is and are not
 *            its content: `> `, `- `, a fence, `[^label]:`, a link's brackets,
 *            an emphasis delimiter, an entity, a backslash escape.
 * CONTENT    the bytes that went into the owner's content, and so are the bytes
 *            its children are cut from.
 * DISCARDED  the bytes the parse read and kept nowhere -- indentation stripped
 *            from a continuation line, the trailing hashes of a closed ATX
 *            heading, a line ending nothing owns. They still have an owner.
 */
#ifndef MARKDOWN_CORE_REGION_ROLE_TYPEDEF
#define MARKDOWN_CORE_REGION_ROLE_TYPEDEF
typedef enum markdown_core_region_role {
    MARKDOWN_CORE_REGION_ROLE_MARKER = 0,
    MARKDOWN_CORE_REGION_ROLE_CONTENT = 1,
    MARKDOWN_CORE_REGION_ROLE_DISCARDED = 2
} markdown_core_region_role;
#endif

/** One region of the concrete view: a byte range of the normalized source with
 * exactly one owner and exactly one role. `start` and `length` index
 * `markdown_core_document_source`, never the caller's own buffer. */
typedef struct markdown_core_region {
    size_t start;
    size_t length;
    markdown_core_region_role role;
    const markdown_core_node *owner;
} markdown_core_region;

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

/**
 * ONE PARSE UNDER TWO TOTAL VIEWS, and the law that binds them.
 *
 * `markdown_core_document_semantic` is the tree: policy applied, and it MAY
 * omit bytes -- a fence's backticks are in no node's literal, and the closing
 * hashes of an ATX heading are in nothing at all.
 *
 * The concrete view -- `_source`, `_line_count`, `_line_start`, `_region_count`
 * and `_region_at` -- OMITS NOTHING, and that is not a promise about intent but
 * a checkable law:
 *
 *   EVERY BYTE of `markdown_core_document_source` lies in exactly one region,
 *   and every region has exactly one owner in the semantic tree.
 *
 * So the pair is complete: a consumer that wants meaning reads the tree, one
 * that wants provenance reads the regions, and no byte of the input is
 * reachable through neither. The source is the NORMALIZED source -- UTF-8
 * validated, NUL replaced, every line ending a single `\n` -- and not the bytes
 * the caller passed, because that is what the tree's positions and the regions
 * both index.
 *
 * Both views end with the document. A region's `owner` is a node of the same
 * document and is valid for exactly as long as it is.
 */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_document_semantic(const markdown_core_document *document);
/** The normalized source. Empty, never null, for a document that parsed no bytes. */
MARKDOWN_CORE_API markdown_core_string_view markdown_core_document_source(const markdown_core_document *document);
/** How many lines the normalized source has. */
MARKDOWN_CORE_API size_t markdown_core_document_line_count(const markdown_core_document *document);
/** Where line `line` begins in the source, counting lines from 1. */
MARKDOWN_CORE_API bool markdown_core_document_line_start(const markdown_core_document *document, size_t line,
                                                         size_t *offset);
/** How many regions the concrete view has. They are in source order. */
MARKDOWN_CORE_API size_t markdown_core_document_region_count(const markdown_core_document *document);
/** The region at `index`, counting from 0. */
MARKDOWN_CORE_API bool markdown_core_document_region_at(const markdown_core_document *document, size_t index,
                                                        markdown_core_region *region);
/** The owner of region `index`, as the PATH of child indices from the semantic
 * root: `{}` is the root, `{0, 2}` is the third child of the first.
 *
 * A pointer names a node only while the handle is alive, and every binding
 * copies the tree into value types and frees the handle -- so a pointer is
 * exactly the locator that does not survive the copy, and this is the one that
 * does. It is an ordinal, which §5.8 rejected for the reference model, and the
 * difference is that this one names a node inside ONE immutable snapshot rather
 * than across edits: nothing filters or slices a parsed document.
 *
 * `capacity` is how many `int32_t` `path` can hold. `*length` receives the
 * depth, and the call fails without writing if the path is deeper than that --
 * ask again with more room. Traversal here is the RAW tree's, the same one the
 * regions were recorded against, not the canonical traversal
 * `markdown_core_node_get_first_child` performs. */
MARKDOWN_CORE_API bool markdown_core_document_region_owner_path(const markdown_core_document *document, size_t index,
                                                                int32_t *path, size_t capacity, size_t *length);
/** EVERY region's owner path, in one pass.
 *
 * `offsets` takes `markdown_core_document_region_count() + 1` entries and is
 * filled whenever it is large enough, even when `paths` is not: `offsets[i]` is
 * where region `i`'s path begins in `paths` and `offsets[i + 1]` is where it
 * ends, so `offsets[count]` is how many elements `paths` needs. Call once with
 * `paths_capacity` 0 to learn that number, then again with the buffer.
 *
 * This is not a convenience over the singular call, it is a different cost. A
 * node knows its parent but not its own index among its siblings, so each
 * singular call counts previous siblings from scratch; a pass in SOURCE ORDER
 * can start where the last one stopped. Measured on this repository's own
 * 674 KB design document, 52853 regions: the loop costs 96.8 ms against a
 * 30.8 ms parse, and both calls of this one together cost 1.13 ms. Bindings
 * copy every region, so this is the call they make. */
MARKDOWN_CORE_API bool markdown_core_document_region_owner_paths(const markdown_core_document *document, int32_t *paths,
                                                                 size_t paths_capacity, uint32_t *offsets,
                                                                 size_t offsets_capacity);

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
/** A directive's properties. There is no `mode`: an inline `Directive` is
 * always embedded and a `DirectiveBlock` always standalone, so the value was
 * implied by the kind and four surfaces had to keep a constant in step (Q29). */
MARKDOWN_CORE_API bool markdown_core_node_directive_properties(const markdown_core_node *node,
                                                               markdown_core_string_view *name, bool *has_attributes,
                                                               size_t *attribute_count);
MARKDOWN_CORE_API bool markdown_core_node_directive_attribute_at(const markdown_core_node *node, size_t index,
                                                                 markdown_core_string_view *name,
                                                                 markdown_core_string_view *value);
MARKDOWN_CORE_API bool markdown_core_node_link_properties(const markdown_core_node *node,
                                                          markdown_core_string_view *destination,
                                                          markdown_core_string_view *title);
MARKDOWN_CORE_API bool markdown_core_node_image_properties(const markdown_core_node *node,
                                                           markdown_core_string_view *source,
                                                           markdown_core_string_view *title);
/** The association a reference or a definition carries. Answers for
 * `ReferenceDefinition`, `LinkReference`, `ImageReference`,
 * `FootnoteDefinition` and `FootnoteReference`, and refuses every other kind.
 *
 * `label` is the bytes between the delimiters exactly as the source spells
 * them: character escapes and character references unresolved, whitespace
 * uncollapsed, case unfolded. `identifier` is the match key -- full Unicode
 * case fold, trimmed, internal whitespace collapsed -- and for the two
 * footnote kinds it KEEPS a leading `^`, so a footnote and a link definition
 * of one name cannot collide in a consumer's single map.
 *
 * NEITHER DERIVES THE OTHER. `label` to `identifier` needs the case-fold
 * table; `identifier` to `label` is impossible, because the fold is
 * many-to-one.
 *
 * NORMATIVE: `identifier` is compared with memcmp over its bytes. It is never
 * case mapped, never NFC/NFD normalized, never re-encoded, and never used as a
 * key in a language map whose equality has an opinion about Unicode -- Swift's
 * `String ==` is canonical equivalence, which would collapse two spellings
 * this parser deliberately keeps apart. */
MARKDOWN_CORE_API bool markdown_core_node_association(const markdown_core_node *node, markdown_core_string_view *label,
                                                      markdown_core_string_view *identifier);
/** A link reference definition's resource.
 *
 * `destination` is REQUIRED and is never absent -- a definition whose
 * destination could not be built is not emitted at all (Q7, Q26) -- while
 * `title` is absent when the source wrote none, and empty when the source
 * wrote an empty one. */
MARKDOWN_CORE_API bool markdown_core_node_definition_resource(const markdown_core_node *node,
                                                              markdown_core_string_view *destination,
                                                              markdown_core_string_view *title);
/** The form a `LinkReference` or `ImageReference` was written in. */
MARKDOWN_CORE_API bool markdown_core_node_reference_form(const markdown_core_node *node,
                                                         markdown_core_reference_form *form);

/** Allocates the canonical file-tree dump. Free it with markdown_core_dump_free. */
MARKDOWN_CORE_API bool markdown_core_document_dump(const markdown_core_document *document, uint8_t **output,
                                                   size_t *length, markdown_core_error **error);
MARKDOWN_CORE_API void markdown_core_dump_free(uint8_t *output);

#ifdef __cplusplus
}
#endif

#endif
