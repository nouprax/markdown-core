#ifndef MARKDOWN_CORE_FACADE_H
#define MARKDOWN_CORE_FACADE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Thread safety and ownership contract
 * ====================================
 *
 * Initialization: there is no process-level initialization, registry, cache,
 * teardown, or re-initialization path. The library contains only immutable
 * process-lifetime tables and constants. Every parser, extension attachment,
 * allocation, and failure flag belongs to one parse transaction. Concurrent
 * first calls from any number of threads therefore require no warmup,
 * external lock, or explicit init call.
 *
 * Language: there is exactly one. A parse recognizes the whole Markdown Core
 * dialect, every feature always on, and takes no options; nothing here
 * enables, disables, or configures a construct.
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
 * afterwards. Node handles and `markdown_core_string`s borrow from the owning document
 * and end with it.
 *
 * Errors: a markdown_core_error returned through an out-parameter is an
 * immutable, library-owned process-lifetime value. It requires no allocation,
 * including when it reports allocation failure. markdown_core_error_free is a
 * no-op release function (NULL is allowed). Dump buffers are owned by the caller
 * and released with markdown_core_dump_free (NULL is allowed).
 *
 * No process-global lifecycle or shared mutable parser state exists: this
 * contract is complete, and bindings must not rely on undocumented
 * conventions.
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

/** A read-only run of UTF-8 bytes that this library owns.
 *
 * IT BORROWS. `data` points into the parsed document and is valid exactly as
 * long as the `markdown_core_document` that produced it; copy the bytes before
 * freeing the document. It is not NUL-terminated and `length` is in BYTES.
 *
 * ~~`markdown_core_string_view`~~ until 3.0: the `_view` suffix carried the
 * borrowing, and the name is regular now beside `markdown_core_optional_i64`
 * and `markdown_core_optional_bool`, so this comment carries it instead. */
typedef struct markdown_core_string {
    const uint8_t *data;
    size_t length;
} markdown_core_string;

typedef struct markdown_core_position {
    int32_t line;
    int32_t column;
} markdown_core_position;

typedef struct markdown_core_scope {
    markdown_core_position start;
    markdown_core_position end;
} markdown_core_scope;

/** Metadata is a scoped value, not Markup. It receives no visitor callbacks.
 * Records and list items retain source order; number strings retain their exact
 * spelling. Every returned handle and string borrows the document. */
typedef struct markdown_core_metadata markdown_core_metadata;
typedef struct markdown_core_metadata_record markdown_core_metadata_record;
typedef enum markdown_core_metadata_value_kind {
    MARKDOWN_CORE_METADATA_SCALAR = 1,
    MARKDOWN_CORE_METADATA_LIST = 2
} markdown_core_metadata_value_kind;
typedef enum markdown_core_metadata_scalar_kind {
    MARKDOWN_CORE_METADATA_NULL = 0,
    MARKDOWN_CORE_METADATA_BOOL = 1,
    MARKDOWN_CORE_METADATA_NUMBER = 2,
    MARKDOWN_CORE_METADATA_TEXT = 3
} markdown_core_metadata_scalar_kind;
typedef struct markdown_core_metadata_scalar {
    markdown_core_metadata_scalar_kind kind;
    union {
        bool boolean;
        markdown_core_string string;
    } value;
} markdown_core_metadata_scalar;
typedef enum markdown_core_metadata_list_item_kind {
    MARKDOWN_CORE_METADATA_ITEM_NUMBER = 1,
    MARKDOWN_CORE_METADATA_ITEM_TEXT = 2
} markdown_core_metadata_list_item_kind;
typedef struct markdown_core_metadata_list_item {
    markdown_core_metadata_list_item_kind kind;
    markdown_core_string value;
} markdown_core_metadata_list_item;

MARKDOWN_CORE_API const markdown_core_metadata *markdown_core_node_document_metadata(const markdown_core_node *node);
MARKDOWN_CORE_API markdown_core_scope markdown_core_metadata_scope(const markdown_core_metadata *metadata);
MARKDOWN_CORE_API size_t markdown_core_metadata_record_count(const markdown_core_metadata *metadata);
MARKDOWN_CORE_API const markdown_core_metadata_record *
markdown_core_metadata_record_at(const markdown_core_metadata *metadata, size_t index);
MARKDOWN_CORE_API markdown_core_scope markdown_core_metadata_record_scope(const markdown_core_metadata_record *record);
MARKDOWN_CORE_API markdown_core_string markdown_core_metadata_record_name(const markdown_core_metadata_record *record);
MARKDOWN_CORE_API markdown_core_metadata_value_kind
markdown_core_metadata_record_kind(const markdown_core_metadata_record *record);
MARKDOWN_CORE_API bool markdown_core_metadata_record_scalar(const markdown_core_metadata_record *record,
                                                            markdown_core_metadata_scalar *value);
MARKDOWN_CORE_API size_t markdown_core_metadata_record_item_count(const markdown_core_metadata_record *record);
MARKDOWN_CORE_API bool markdown_core_metadata_record_item_at(const markdown_core_metadata_record *record, size_t index,
                                                             markdown_core_metadata_list_item *value);

typedef enum markdown_core_error_code {
    MARKDOWN_CORE_ERROR_NONE = 0,
    MARKDOWN_CORE_ERROR_INVALID_ARGUMENT = 1,
    MARKDOWN_CORE_ERROR_ALLOCATION_FAILED = 2,
    MARKDOWN_CORE_ERROR_INTERNAL = 3
} markdown_core_error_code;

typedef enum markdown_core_node_kind {
    MARKDOWN_CORE_KIND_NONE = 0,
    MARKDOWN_CORE_KIND_DOCUMENT,
    MARKDOWN_CORE_KIND_CALLOUT,
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
    MARKDOWN_CORE_KIND_CITE,
    MARKDOWN_CORE_KIND_TABLE_ROW,
    MARKDOWN_CORE_KIND_TABLE_CELL,
    MARKDOWN_CORE_KIND_DIRECTIVE_LABEL,
    /* Appended, not inserted beside the other block kinds: this enum's ordinal
     * IS the wire kind every binding decodes, so a kind added in the middle
     * renumbers every kind after it. (While 3.0.0 is unreleased a landing item
     * may still renumber: M2 removed the three reference kinds that stood
     * here, and this one moved down.)
     *
     * A comment of the dialect: the one kind that is valid in both block and
     * inline content. Its parent edge records which; the node stores no
     * placement. `markdown_core_node_literal` answers with the bytes between
     * the delimiters. */
    MARKDOWN_CORE_KIND_COMMENT,
    MARKDOWN_CORE_KIND_CROSS_LINK
} markdown_core_node_kind;

typedef enum markdown_core_list_flavor {
    MARKDOWN_CORE_LIST_FLAVOR_BULLET = 1,
    MARKDOWN_CORE_LIST_FLAVOR_ORDERED = 2
} markdown_core_list_flavor;

typedef enum markdown_core_ordered_list_variant_kind {
    MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL = 1,
    MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA = 2,
    MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN = 3,
    MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT = 4
} markdown_core_ordered_list_variant_kind;

typedef struct markdown_core_ordered_list_variant {
    markdown_core_ordered_list_variant_kind kind;
    bool lowercased;
} markdown_core_ordered_list_variant;

typedef enum markdown_core_ordered_list_delimiter_kind {
    MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD = 1,
    MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS = 2,
    MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT = 3
} markdown_core_ordered_list_delimiter_kind;

typedef struct markdown_core_ordered_list_delimiter {
    markdown_core_ordered_list_delimiter_kind kind;
    bool closed;
} markdown_core_ordered_list_delimiter;

/** A positive authored width share, or absent when no width was authored. */
typedef struct markdown_core_optional_double {
    bool has_value;
    double value;
} markdown_core_optional_double;

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

typedef struct markdown_core_table_column {
    markdown_core_table_alignment alignment;
    markdown_core_optional_double relative;
} markdown_core_table_column;

typedef struct markdown_core_optional_i64 {
    bool has_value;
    int64_t value;
} markdown_core_optional_i64;

typedef struct markdown_core_optional_bool {
    bool has_value;
    bool value;
} markdown_core_optional_bool;

/** An optional string, and the ONLY way this library reports one.
 *
 * `has_value == false` means the source did not write this. `has_value ==
 * true` with a zero-length `value` means the source wrote it and it was
 * empty. The two are different facts and nothing here folds one into the
 * other -- an accessor that answers with this type cannot be handed a plain
 * `markdown_core_string`, which is what makes the distinction survive.
 *
 * `value.data` is NOT the presence flag. A caller that tests it instead of
 * `has_value` has re-invented the convention this type replaced. */
typedef struct markdown_core_optional_string {
    bool has_value;
    markdown_core_string value;
} markdown_core_optional_string;

/**
 * Parses exactly `length` bytes as UTF-8 in the one Markdown Core dialect.
 * Valid UTF-8 is a caller precondition; Markdown Core does not validate or
 * repair malformed input. There are no options: every feature of the dialect
 * is recognized on every call.
 * The returned document owns every node and every `markdown_core_string`
 * handed out of it. On failure,
 * NULL is returned and `*error` is set when `error` is non-NULL.
 */
MARKDOWN_CORE_API markdown_core_document *markdown_core_document_parse(const uint8_t *source, size_t length,
                                                                       markdown_core_error **error);
MARKDOWN_CORE_API void markdown_core_document_free(markdown_core_document *document);

/** Return the immutable semantic root owned by `document`.
 *
 * Every node carries a `scope`: line-and-column boundaries reported by the
 * cmark-family parser. The returned node and every string read from it borrow
 * from `document` and end when the document is freed. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_document_root(const markdown_core_document *document);
/** A parse failure. There is NO document, and there is no scope: an input the
 * parser could not turn into a document has no extent to point at. The value
 * is immutable and library-owned; `markdown_core_error_free` is a no-op. */
MARKDOWN_CORE_API markdown_core_error_code markdown_core_error_get_code(const markdown_core_error *error);
MARKDOWN_CORE_API markdown_core_string markdown_core_error_get_message(const markdown_core_error *error);
MARKDOWN_CORE_API void markdown_core_error_free(markdown_core_error *error);

MARKDOWN_CORE_API markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node);
MARKDOWN_CORE_API const char *markdown_core_node_kind_name(markdown_core_node_kind kind);
MARKDOWN_CORE_API markdown_core_scope markdown_core_node_scope(const markdown_core_node *node);

/** A directive's `label` is a separate node-valued field and is not part of
 * its child sequence. For a `DirectiveBlock`, these functions traverse only
 * block `content`; an inline `Directive` has no children. Read its label with
 * `markdown_core_node_directive_label`. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_get_first_child(const markdown_core_node *node);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_get_next_sibling(const markdown_core_node *node);
MARKDOWN_CORE_API size_t markdown_core_node_child_count(const markdown_core_node *node);

MARKDOWN_CORE_API bool markdown_core_node_heading_level(const markdown_core_node *node, int32_t *level);
MARKDOWN_CORE_API bool markdown_core_node_list_properties(const markdown_core_node *node,
                                                          markdown_core_list_flavor *flavor,
                                                          markdown_core_optional_i64 *start,
                                                          markdown_core_ordered_list_variant *variant,
                                                          markdown_core_ordered_list_delimiter *delimiter, bool *tight);
MARKDOWN_CORE_API bool markdown_core_node_list_item_marker(const markdown_core_node *node,
                                                           markdown_core_optional_string *marker);
/** `info` and `language` are OPTIONAL: a fence with nothing but whitespace
 * after it wrote no info string, and an indented block has no fence to write
 * one on. `language` is the info string's first word and is present exactly
 * when `info` is. */
MARKDOWN_CORE_API bool markdown_core_node_code_block_properties(const markdown_core_node *node,
                                                                markdown_core_optional_string *info,
                                                                markdown_core_optional_string *language,
                                                                markdown_core_string *literal, bool *fenced,
                                                                bool *closed);
/** The literal of a `Text`, `Code`, `HTML`, `HTMLBlock`, or `Comment` node.
 * A comment's literal excludes its delimiters and keeps every byte between
 * them, line endings and indentation included. */
MARKDOWN_CORE_API bool markdown_core_node_literal(const markdown_core_node *node, markdown_core_string *literal);
MARKDOWN_CORE_API bool markdown_core_node_formula_properties(const markdown_core_node *node,
                                                             markdown_core_placement_mode *mode,
                                                             markdown_core_string *literal);
/** The node's children are its rows, in head/content/foot order. These counts
 * partition that single owned chain; row membership is a table fact. */
MARKDOWN_CORE_API bool markdown_core_node_table_properties(const markdown_core_node *node, size_t *column_count,
                                                           size_t *head_count, size_t *content_count,
                                                           size_t *foot_count);
MARKDOWN_CORE_API bool markdown_core_node_table_column_at(const markdown_core_node *node, size_t index,
                                                          markdown_core_table_column *column);
MARKDOWN_CORE_API bool markdown_core_node_table_cell_spans(const markdown_core_node *node, int64_t *rowspan,
                                                           int64_t *colspan);
/** A directive's properties. There is no `mode`: an inline `Directive` is
 * always embedded and a `DirectiveBlock` always standalone, so the value was
 * implied by the kind and four surfaces had to keep a constant in step (Q29). */
MARKDOWN_CORE_API bool markdown_core_node_directive_properties(const markdown_core_node *node,
                                                               markdown_core_string *name);
/** Universal fields. Classes and records retain source order and duplicates.
 * An out-of-range index returns false; absent attributes have zero counts. */
MARKDOWN_CORE_API markdown_core_optional_string markdown_core_node_anchor(const markdown_core_node *node);
MARKDOWN_CORE_API size_t markdown_core_node_attribute_class_count(const markdown_core_node *node);
MARKDOWN_CORE_API bool markdown_core_node_attribute_class_at(const markdown_core_node *node, size_t index,
                                                             markdown_core_string *value);
MARKDOWN_CORE_API size_t markdown_core_node_attribute_record_count(const markdown_core_node *node);
MARKDOWN_CORE_API bool markdown_core_node_attribute_record_at(const markdown_core_node *node, size_t index,
                                                              markdown_core_string *name, markdown_core_string *value);
/** Image dimensions are absent until O9 produces them. */
MARKDOWN_CORE_API bool markdown_core_node_image_dimensions(const markdown_core_node *node,
                                                           markdown_core_optional_i64 *width,
                                                           markdown_core_optional_i64 *height);
/** The directive's optional `DirectiveLabel` field. The returned node is not
 * a directive child; its own children are the label's inline content. NULL
 * means either no label or a non-directive input. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_directive_label(const markdown_core_node *node);
/** A `Callout`'s metadata (M3). Every `>` container is a callout: `variant`
 * is the authored type as written, absent when the container has no metadata
 * line, and `collapsed` is its fold marker, absent when no `+` or `-` was
 * authored, false for `+` (the callout opens expanded) and true for `-`.
 * Until the callouts module's metadata rule lands with `O8`, every callout
 * answers an absent variant and an absent marker. */
MARKDOWN_CORE_API bool markdown_core_node_callout_properties(const markdown_core_node *node,
                                                             markdown_core_optional_string *variant,
                                                             markdown_core_optional_bool *collapsed);
/** The first node of a `Callout`'s `title`: a node-valued field whose inline
 * nodes follow by `markdown_core_node_get_next_sibling` and are never callout
 * children. A present title holds at least one node, so NULL means no title,
 * or a non-callout input. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_callout_title(const markdown_core_node *node);
/** The tagged `Destination` value of a `Link`, `Image`, or `CrossLink`: a value, not
 * a node, so it has no scope and no children, and a branch's fields exist
 * only in that branch. `MARKDOWN_CORE_DESTINATION_URL` fills `url` and zeroes
 * `path` and `anchor`; `MARKDOWN_CORE_DESTINATION_CROSS`, the workspace
 * address a cross link produces, fills `path` and `anchor`
 * and zeroes `url`. Every `Link` and `Image` answers the `url` branch.
 *
 * A destination is REQUIRED (Q26, requirement 14): `[a]()` and `[a](<>)`
 * wrote one and wrote nothing in it, so `url` is the empty string, and a
 * reference occurrence answers the destination its definition stated (M2).
 * `url` holds the complete semantic destination the inherited grammar
 * produced -- the bytes between angle brackets or the bare destination, with
 * backslash escapes and character references decoded and no percent-encoding,
 * normalization, or resolution. */
typedef enum markdown_core_destination_kind {
    MARKDOWN_CORE_DESTINATION_URL = 1,
    MARKDOWN_CORE_DESTINATION_CROSS = 2
} markdown_core_destination_kind;

typedef struct markdown_core_destination {
    markdown_core_destination_kind kind;
    markdown_core_string url;
    markdown_core_string path;
    markdown_core_optional_string anchor;
} markdown_core_destination;

/** Answers for `Link` and `Image` and refuses every other kind. */
MARKDOWN_CORE_API bool markdown_core_node_destination(const markdown_core_node *node,
                                                      markdown_core_destination *destination);
/** The OPTIONAL title of a `Link` or `Image`: `[a](/u)` wrote no title and
 * `[a](/u "")` wrote an empty one, and the two stay different. Refuses every
 * other kind. */
/** Raw authored cross-link fields. Returns false for other kinds or null outputs. */
MARKDOWN_CORE_API bool markdown_core_node_cross_link_properties(const markdown_core_node *node, bool *embedded,
                                                                markdown_core_optional_string *label);

MARKDOWN_CORE_API bool markdown_core_node_title(const markdown_core_node *node, markdown_core_optional_string *title);

/** The resource a `Link` or `Image` reads its destination and title from, as
 * an opaque identity (M2). Two nodes answer the same pointer exactly when they
 * share one resource: every occurrence that resolved through one link
 * reference definition does -- `[t][l]`, `[l][]` and `[l]` alike -- and a
 * direct link, a direct image and an autolink never do. NULL for every other
 * kind.
 *
 * The sharing is what bounds a document: one definition with a long
 * destination referenced many times stores that destination once, however
 * many occurrences name it. A consumer that materializes a destination once
 * per distinct resource keys on this pointer. Nothing else about it is
 * stated, and it is valid only while the document is. */
#ifndef MARKDOWN_CORE_RESOURCE_TYPEDEF
#define MARKDOWN_CORE_RESOURCE_TYPEDEF
typedef struct markdown_core_resource markdown_core_resource;
#endif
MARKDOWN_CORE_API const markdown_core_resource *markdown_core_node_resource(const markdown_core_node *node);
/** The scoped values of the citation model (M4). A `Citation` is one item of
 * a `Cite` and a `Footnote` is one element of `Document.footnotes`. Each is
 * written, so it has a scope, and each owns Markup, but neither is a `Markup`
 * kind: a value is reached only through its owner's accessor below, never as
 * a child, and it has no `markdown_core_node_kind`. The handle types are
 * never defined, so nothing can pass one where a node is expected. Both are
 * valid only while the document is. */
typedef struct markdown_core_citation markdown_core_citation;
typedef struct markdown_core_footnote markdown_core_footnote;
typedef struct markdown_core_specimen markdown_core_specimen;

/** How a bibliographic citation is to be rendered (M4): `[@key]` is normal,
 * `@key` in running text names the author in text, and `-@key` suppresses
 * the author. First produced by the citations module with `P7`. */
typedef enum markdown_core_bib_mode {
    MARKDOWN_CORE_BIB_MODE_NORMAL = 1,
    MARKDOWN_CORE_BIB_MODE_AUTHOR_IN_TEXT = 2,
    MARKDOWN_CORE_BIB_MODE_SUPPRESS_AUTHOR = 3
} markdown_core_bib_mode;

typedef enum markdown_core_referent_kind {
    MARKDOWN_CORE_REFERENT_BIB = 1,
    MARKDOWN_CORE_REFERENT_FOOTNOTE = 2,
    MARKDOWN_CORE_REFERENT_SPECIMEN = 3
} markdown_core_referent_kind;

/** The tagged `CitationReferent` value (M4): a value, not a node, so it has
 * no scope, and a branch's fields exist only in that branch. `BIB` fills
 * `key` and `mode` and zeroes `id`; `FOOTNOTE` and `SPECIMEN` fill `id`, the definition id
 * the item names, and zeroes `key` and `mode`. Every referent is the
 * `FOOTNOTE` branch until `P7`. */
typedef struct markdown_core_referent {
    markdown_core_referent_kind kind;
    markdown_core_string key;
    markdown_core_bib_mode mode;
    markdown_core_string id;
} markdown_core_referent;

/** The first item of a `Cite`, or NULL for a non-cite input; a cite holds at
 * least one item, and the items follow by `markdown_core_citation_next` in
 * source order. */
MARKDOWN_CORE_API const markdown_core_citation *markdown_core_node_cite_citations(const markdown_core_node *node);
MARKDOWN_CORE_API const markdown_core_citation *markdown_core_citation_next(const markdown_core_citation *citation);
MARKDOWN_CORE_API markdown_core_scope markdown_core_citation_scope(const markdown_core_citation *citation);
MARKDOWN_CORE_API bool markdown_core_citation_referent(const markdown_core_citation *citation,
                                                       markdown_core_referent *referent);
/** The first node of an item's `prefix` or `suffix`, the inline nodes
 * following by `markdown_core_node_get_next_sibling`, or NULL when the affix
 * is empty; every affix is empty until `P7`. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_citation_prefix(const markdown_core_citation *citation);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_citation_suffix(const markdown_core_citation *citation);

/** The first element of `Document.footnotes`, or NULL when the document has
 * none or the node is not the document root. Footnotes follow by
 * `markdown_core_footnote_next` in ascending scope order: every winning or
 * unreferenced definition, wherever it was written, and none of them is a
 * child of any node. */
MARKDOWN_CORE_API const markdown_core_footnote *markdown_core_node_document_footnotes(const markdown_core_node *node);
MARKDOWN_CORE_API const markdown_core_footnote *markdown_core_footnote_next(const markdown_core_footnote *footnote);
MARKDOWN_CORE_API markdown_core_scope markdown_core_footnote_scope(const markdown_core_footnote *footnote);
/** The id: the definition's label under the reference-label normalization --
 * full Unicode case fold, trimmed, internal whitespace collapsed -- WITHOUT
 * the caret, exactly the `id` of every `footnote` referent that names it.
 *
 * NORMATIVE: an id is compared with memcmp over its bytes. It is never case
 * mapped, never NFC/NFD normalized, never re-encoded, and never used as a key
 * in a language map whose equality has an opinion about Unicode. */
MARKDOWN_CORE_API bool markdown_core_footnote_id(const markdown_core_footnote *footnote, markdown_core_string *id);
/** The first node of the footnote's block content, the rest following by
 * `markdown_core_node_get_next_sibling`, or NULL when the content is empty. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_footnote_content(const markdown_core_footnote *footnote);

/** Specimens are document-owned scoped citation definitions, visited after
 * footnotes and never counted as content children. The syntax first lands in
 * P9b. An anonymous definition has no id, and an absent start means no
 * explicit counter reset. Display numbers are not stored in the AST. */
MARKDOWN_CORE_API const markdown_core_specimen *markdown_core_node_document_specimens(const markdown_core_node *node);
MARKDOWN_CORE_API const markdown_core_specimen *markdown_core_specimen_next(const markdown_core_specimen *specimen);
MARKDOWN_CORE_API markdown_core_scope markdown_core_specimen_scope(const markdown_core_specimen *specimen);
MARKDOWN_CORE_API bool markdown_core_specimen_properties(const markdown_core_specimen *specimen,
                                                         markdown_core_optional_string *id,
                                                         markdown_core_optional_i64 *start);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_specimen_content(const markdown_core_specimen *specimen);

/** Allocates the canonical file-tree dump. Free it with markdown_core_dump_free. */
MARKDOWN_CORE_API bool markdown_core_document_dump(const markdown_core_document *document, uint8_t **output,
                                                   size_t *length, markdown_core_error **error);
MARKDOWN_CORE_API void markdown_core_dump_free(uint8_t *output);

#ifdef __cplusplus
}
#endif

#endif
