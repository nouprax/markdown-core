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
 * process-lifetime tables and constants. Every parser, element attachment,
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
 * Errors: every call that can fail returns a markdown_core_status and hands
 * its answer out through out-parameters, which it writes only when it returns
 * MARKDOWN_CORE_OK. A call that cannot fail returns its answer. Nothing a
 * failure reports is allocated, so an allocation failure is reported like any
 * other. Dump buffers are owned by the caller and released with
 * markdown_core_dump_free.
 *
 * Calls: every argument is the caller's to get right, and a call that would
 * read or write memory it does not own for a wrong one reports it instead.
 * A kind-specific accessor answers MARKDOWN_CORE_KIND_MISMATCH for a node (or
 * metadata value) of another kind, an `_at` accessor answers
 * MARKDOWN_CORE_OUT_OF_BOUNDS for an index at or past its count, and a scope
 * query answers MARKDOWN_CORE_OUT_OF_BOUNDS for a source too short for what it
 * reads. Pointer arguments are not checked for NULL, except that a source of
 * length 0 may be NULL and `*_free(NULL)` does nothing. A node belongs to the
 * document it is asked about; nothing checks that.
 *
 * No process-global lifecycle or shared mutable parser state exists: this
 * contract is complete, and bindings must not rely on undocumented
 * conventions.
 */

#if defined(_WIN32) && !defined(MARKDOWN_CORE_STATIC_DEFINE)
#if defined(MARKDOWN_CORE_ELEMENTS_EXPORTS)
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

/** What a call that can fail answers. MARKDOWN_CORE_OK is success; every other
 * value names the one reason there is no answer, and the value is the status
 * code of an MCB3 failure message (docs/architecture/wire-format.md).
 *
 * - ALLOCATION_FAILED: an allocation failed, or the input exceeds the
 *   capacity the library can represent.
 * - OUT_OF_BOUNDS: an index, position or source length names something that
 *   is not there.
 * - KIND_MISMATCH: the node or value is not of the kind the accessor reads.
 * - INSIDE_SCALAR: an offset falls inside a scalar, where none begins. */
typedef enum markdown_core_status {
    MARKDOWN_CORE_OK = 0,
    MARKDOWN_CORE_ALLOCATION_FAILED = 1,
    MARKDOWN_CORE_OUT_OF_BOUNDS = 2,
    MARKDOWN_CORE_KIND_MISMATCH = 3,
    MARKDOWN_CORE_INSIDE_SCALAR = 4,
} markdown_core_status;

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

/** How a document counts columns: UTF-8 bytes or UTF-16 code units. A
 * document counts every column a scope query returns or takes in the unit it
 * was parsed with. */
typedef enum markdown_core_text_unit {
    MARKDOWN_CORE_TEXT_UNIT_UTF8 = 1,
    MARKDOWN_CORE_TEXT_UNIT_UTF16 = 2
} markdown_core_text_unit;

/** Editor source coordinates: a line counted from 1, and a column counted
 * from 1 in the document's text unit. These are not string indices. */
typedef struct markdown_core_position {
    int32_t line;
    int32_t column;
} markdown_core_position;

/** A node's editor source coordinates, computed on request from its extent
 * and the source (markdown_core_document_scope). `start` is the position of
 * the node's first byte, where a line terminator is the column after its
 * line's last character. `end` is the line holding the byte just past the
 * node's last byte and the column count from that line's start to it, so a
 * node that ends right after a line terminator ends at `L:0` of the next
 * line, and a zero-byte document is `1:1..1:0`. */
typedef struct markdown_core_scope {
    markdown_core_position start;
    markdown_core_position end;
} markdown_core_scope;

/** WHERE A NODE IS, in bytes of the UTF-8 source. `lead` is the signed
 * distance from the end of the previous node in the same relation -- or from
 * the owner's start, for the first node of a relation -- to this node's
 * start, and `span` the length of its source range. */
#ifndef MARKDOWN_CORE_EXTENT_TYPEDEF
#define MARKDOWN_CORE_EXTENT_TYPEDEF
typedef struct markdown_core_extent {
    int32_t lead;
    uint32_t span;
} markdown_core_extent;
#endif

/** Metadata is a leaf node owned by Document.metadata.
 * Ten optional fields hold values; list items retain order and numbers their exact
 * spelling. Every returned handle and string borrows the document. A field
 * accessor answers NULL when the field is absent; an explicit null is a
 * present scalar. `_scalar` reads a scalar value, and `_item_count` and
 * `_item_at` a list value. */
typedef struct markdown_core_metadata_value markdown_core_metadata_value;
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

/** The document's Metadata node, or NULL when the source wrote none.
 * KIND_MISMATCH unless `node` is a Document. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_document_metadata(const markdown_core_node *node,
                                                                            const markdown_core_node **metadata);
/** A field of a Metadata node. KIND_MISMATCH unless `metadata` is a Metadata
 * node. */
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_name(const markdown_core_node *metadata,
                                                                   const markdown_core_metadata_value **value);
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_title(const markdown_core_node *metadata,
                                                                    const markdown_core_metadata_value **value);
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_subtitle(const markdown_core_node *metadata,
                                                                       const markdown_core_metadata_value **value);
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_time(const markdown_core_node *metadata,
                                                                   const markdown_core_metadata_value **value);
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_date(const markdown_core_node *metadata,
                                                                   const markdown_core_metadata_value **value);
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_authors(const markdown_core_node *metadata,
                                                                      const markdown_core_metadata_value **value);
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_keywords(const markdown_core_node *metadata,
                                                                       const markdown_core_metadata_value **value);
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_abstract(const markdown_core_node *metadata,
                                                                       const markdown_core_metadata_value **value);
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_state(const markdown_core_node *metadata,
                                                                    const markdown_core_metadata_value **value);
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_comment(const markdown_core_node *metadata,
                                                                      const markdown_core_metadata_value **value);
MARKDOWN_CORE_API markdown_core_metadata_value_kind
markdown_core_metadata_value_get_kind(const markdown_core_metadata_value *value);
/** KIND_MISMATCH unless `value` is a scalar. */
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_value_scalar(const markdown_core_metadata_value *value,
                                                                           markdown_core_metadata_scalar *scalar);
/** KIND_MISMATCH unless `value` is a list; `_item_at` answers OUT_OF_BOUNDS
 * for an index at or past the count. */
MARKDOWN_CORE_API markdown_core_status
markdown_core_metadata_value_item_count(const markdown_core_metadata_value *value, size_t *count);
MARKDOWN_CORE_API markdown_core_status markdown_core_metadata_value_item_at(const markdown_core_metadata_value *value,
                                                                            size_t index,
                                                                            markdown_core_metadata_list_item *item);

/* A node's kind. The value IS the wire kind every binding decodes, and it is
 * the kind's `ordinal` in the canonical AST contract: a new kind takes the next
 * ordinal, since a kind inserted in the middle renumbers every kind after it.
 *
 * `MARKDOWN_CORE_KIND_COMMENT` is the one kind valid in both block and inline
 * content. Its parent edge records which; the node stores no placement.
 * `markdown_core_node_literal` answers with the bytes between the delimiters. */
typedef enum markdown_core_node_kind {
    /* BEGIN GENERATED by scripts/tooling/generate-node-kinds.mjs; edit the node-kind schemas instead. */
    MARKDOWN_CORE_KIND_NONE = 0,
    MARKDOWN_CORE_KIND_DOCUMENT = 1,
    MARKDOWN_CORE_KIND_CALLOUT = 2,
    MARKDOWN_CORE_KIND_PARAGRAPH = 3,
    MARKDOWN_CORE_KIND_HEADING = 4,
    MARKDOWN_CORE_KIND_THEMATIC_BREAK = 5,
    MARKDOWN_CORE_KIND_LIST = 6,
    MARKDOWN_CORE_KIND_LIST_ITEM = 7,
    MARKDOWN_CORE_KIND_CODE_BLOCK = 8,
    MARKDOWN_CORE_KIND_HTML_BLOCK = 9,
    MARKDOWN_CORE_KIND_FORMULA_BLOCK = 10,
    MARKDOWN_CORE_KIND_TABLE = 11,
    MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK = 12,
    MARKDOWN_CORE_KIND_TEXT = 13,
    MARKDOWN_CORE_KIND_SOFT_BREAK = 14,
    MARKDOWN_CORE_KIND_LINE_BREAK = 15,
    MARKDOWN_CORE_KIND_CODE = 16,
    MARKDOWN_CORE_KIND_HTML = 17,
    MARKDOWN_CORE_KIND_FORMULA = 18,
    MARKDOWN_CORE_KIND_EMPHASIS = 19,
    MARKDOWN_CORE_KIND_STRONG = 20,
    MARKDOWN_CORE_KIND_STRIKETHROUGH = 21,
    MARKDOWN_CORE_KIND_LINK = 22,
    MARKDOWN_CORE_KIND_EMBEDDED = 23,
    MARKDOWN_CORE_KIND_DIRECTIVE = 24,
    MARKDOWN_CORE_KIND_CITE = 25,
    MARKDOWN_CORE_KIND_TABLE_ROW = 26,
    MARKDOWN_CORE_KIND_TABLE_CELL = 27,
    MARKDOWN_CORE_KIND_DIRECTIVE_LABEL = 28,
    MARKDOWN_CORE_KIND_COMMENT = 29,
    MARKDOWN_CORE_KIND_CROSS_LINK = 30,
    MARKDOWN_CORE_KIND_MARK = 31,
    MARKDOWN_CORE_KIND_CROSS_EMBEDDED = 32,
    MARKDOWN_CORE_KIND_INSERTION = 33,
    MARKDOWN_CORE_KIND_SPAN = 34,
    MARKDOWN_CORE_KIND_SUPERSCRIPT = 35,
    MARKDOWN_CORE_KIND_SUBSCRIPT = 36,
    MARKDOWN_CORE_KIND_DEFINITION_LIST = 37,
    MARKDOWN_CORE_KIND_DEFINITION = 38,
    MARKDOWN_CORE_KIND_TABLE_CAPTION = 39,
    MARKDOWN_CORE_KIND_CITATION = 40,
    MARKDOWN_CORE_KIND_FOOTNOTE = 41,
    MARKDOWN_CORE_KIND_SPECIMEN = 42,
    MARKDOWN_CORE_KIND_METADATA = 43,
    MARKDOWN_CORE_KIND_REFERENCE = 44,
    /* END GENERATED */
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

typedef enum markdown_core_placement {
    MARKDOWN_CORE_PLACEMENT_EMBEDDED = 1,
    MARKDOWN_CORE_PLACEMENT_STANDALONE = 2
} markdown_core_placement;

/** Authored horizontal content alignment; NONE means no explicit alignment. */
typedef enum markdown_core_flow {
    MARKDOWN_CORE_FLOW_NONE = 0,
    MARKDOWN_CORE_FLOW_LEFT = 1,
    MARKDOWN_CORE_FLOW_CENTER = 2,
    MARKDOWN_CORE_FLOW_RIGHT = 3
} markdown_core_flow;

typedef struct markdown_core_table_column {
    markdown_core_flow flow;
    markdown_core_optional_double relative;
} markdown_core_table_column;

typedef struct markdown_core_optional_i64 {
    bool has_value;
    int64_t value;
} markdown_core_optional_i64;

/** A node-independent size value. Width is required; height is optional.
 * Every present component is in 1..2147483647. */
typedef struct markdown_core_dimensions {
    int32_t width;
    markdown_core_optional_i64 height;
} markdown_core_dimensions;

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
 * The bytes are read as they are: nothing validates or repairs them, and a
 * malformed sequence is parsed like any other input. There are no options:
 * every feature of the dialect is recognized on every call. The document
 * counts columns in UTF-8 bytes.
 * `*document` receives the document, which owns every node and every
 * `markdown_core_string` handed out of it. ALLOCATION_FAILED when an
 * allocation fails or `length` exceeds the 1 GiB a document can hold.
 */
MARKDOWN_CORE_API markdown_core_status markdown_core_document_parse(const uint8_t *source, size_t length,
                                                                    markdown_core_document **document);
/** Parses as markdown_core_document_parse does, into a document that counts
 * columns in `unit`. */
MARKDOWN_CORE_API markdown_core_status markdown_core_document_parse_in(const uint8_t *source, size_t length,
                                                                       markdown_core_text_unit unit,
                                                                       markdown_core_document **document);
MARKDOWN_CORE_API void markdown_core_document_free(markdown_core_document *document);
MARKDOWN_CORE_API markdown_core_text_unit markdown_core_document_unit(const markdown_core_document *document);

/** SESSIONS. A session holds a text, counted in the unit it was made with,
 * and the document parsed from it, and changes both with each edit: the new
 * document continues the previous one, so a node that continues an old node
 * keeps its identifier, and one whose value is unchanged is the old node.
 * The document a session returns borrows from the session until its next
 * edit or its release.
 *
 * `markdown_core_session_new` parses `size` bytes of `source` as
 * markdown_core_document_parse does, into a session whose offsets and
 * columns count in `unit`. */
typedef struct markdown_core_session markdown_core_session;

/** One replacement: the text in [start, end), offsets in the session's unit
 * into the text before the batch, becomes the `size` bytes of `text`. */
typedef struct markdown_core_text_edit {
    size_t start;
    size_t end;
    const uint8_t *text;
    size_t size;
} markdown_core_text_edit;

MARKDOWN_CORE_API markdown_core_status markdown_core_session_new(const uint8_t *source, size_t size,
                                                                 markdown_core_text_unit unit,
                                                                 markdown_core_session **session);
/** Applies `count` disjoint edits, listed in any order, to the text and parses
 * it once; `*document` receives the new document. Two edits at one offset
 * apply in the order listed, and each text is stored as given. OUT_OF_BOUNDS
 * when an edit's start is after its end, its end is past the text or two
 * edits overlap; INSIDE_SCALAR when an offset falls inside a scalar: at a
 * continuation byte in UTF-8, or between the two units of one scalar in
 * UTF-16; ALLOCATION_FAILED when an allocation fails or the text would
 * exceed the 1 GiB a document can hold. */
MARKDOWN_CORE_API markdown_core_status markdown_core_session_edit(markdown_core_session *session,
                                                                  const markdown_core_text_edit *edits, size_t count,
                                                                  const markdown_core_document **document);
/** Appends `size` bytes of `text`: the edit at the end of the text. */
MARKDOWN_CORE_API markdown_core_status markdown_core_session_append(markdown_core_session *session, const uint8_t *text,
                                                                    size_t size,
                                                                    const markdown_core_document **document);
MARKDOWN_CORE_API const markdown_core_document *markdown_core_session_document(const markdown_core_session *session);
MARKDOWN_CORE_API markdown_core_text_unit markdown_core_session_unit(const markdown_core_session *session);
/** The size of the session's text in bytes, and a copy of it into `bytes`,
 * which holds that many. */
MARKDOWN_CORE_API size_t markdown_core_session_text_size(const markdown_core_session *session);
MARKDOWN_CORE_API void markdown_core_session_text(const markdown_core_session *session, uint8_t *bytes);
MARKDOWN_CORE_API void markdown_core_session_free(markdown_core_session *session);

/** Return the immutable semantic root owned by `document`.
 *
 * The returned node and every string read from it borrow from `document` and
 * end when the document is freed. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_document_root(const markdown_core_document *document);

/** The node's identifier: unique within its document, and numbered from 1 in
 * canonical walk order by a parse. In a session a node that continues a node
 * of the previous document keeps its identifier, and a node that continues
 * none takes one the session has never issued. Every identifier is below
 * 2^53. */
MARKDOWN_CORE_API uint64_t markdown_core_node_id(const markdown_core_node *node);
/** The node's extent (markdown_core_extent). */
MARKDOWN_CORE_API markdown_core_extent markdown_core_node_extent(const markdown_core_node *node);

/** SCOPE QUERIES. Each takes the source the document was parsed from and
 * computes absolute positions from the extents in one walk of the document,
 * with columns in the document's text unit. Each answers ALLOCATION_FAILED
 * when it cannot allocate.
 *
 * `markdown_core_document_scope` computes the scope of `node`, a node of the
 * document. OUT_OF_BOUNDS when `length` ends before the node does. */
MARKDOWN_CORE_API markdown_core_status markdown_core_document_scope(const markdown_core_document *document,
                                                                    const markdown_core_node *node,
                                                                    const uint8_t *source, size_t length,
                                                                    markdown_core_scope *scope);
/** `*node` receives the last node in canonical walk order whose source range
 * holds the byte at `position`, or NULL when no node holds it or the position
 * names no byte of the source. OUT_OF_BOUNDS when the line or the column is
 * below 1. */
MARKDOWN_CORE_API markdown_core_status markdown_core_document_node_at(const markdown_core_document *document,
                                                                      markdown_core_position position,
                                                                      const uint8_t *source, size_t length,
                                                                      const markdown_core_node **node);
MARKDOWN_CORE_API markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node);
/** The kind's name in the canonical AST. OUT_OF_BOUNDS for a value that is
 * not a markdown_core_node_kind. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_kind_name(markdown_core_node_kind kind, const char **name);

/** A directive's `label` is a separate node-valued field and is not part of
 * its child sequence. For a `DirectiveBlock`, these functions traverse only
 * block `content`; an inline `Directive` has no children. Read its label with
 * `markdown_core_node_directive_label`. DefinitionList exposes its Definition
 * members here. Definition has no generic children: its term and ordered body
 * collections are read through the definition accessors below. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_get_first_child(const markdown_core_node *node);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_get_next_sibling(const markdown_core_node *node);
MARKDOWN_CORE_API size_t markdown_core_node_child_count(const markdown_core_node *node);

/** KIND ACCESSORS. Each reads the fields of the kinds it names and answers
 * KIND_MISMATCH for a node of any other kind. */
/** A `Heading`. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_heading_level(const markdown_core_node *node, int32_t *level);
/** A `List`. `variant` and `delimiter` have meaning only for an ordered list,
 * when `start.has_value` is true. Bullet lists have no ordered-marker
 * properties. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_list_properties(
    const markdown_core_node *node, markdown_core_list_flavor *flavor, markdown_core_optional_i64 *start,
    markdown_core_ordered_list_variant *variant, markdown_core_ordered_list_delimiter *delimiter, bool *tight);
/** A `ListItem`. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_list_item_marker(const markdown_core_node *node,
                                                                           markdown_core_optional_string *marker);
/** A `CodeBlock`. `info` and `language` are OPTIONAL: a fence with nothing
 * but whitespace after it wrote no info string, and an indented block has no
 * fence to write one on. `language` is the info string's first word and is
 * present exactly when `info` is. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_code_block_properties(const markdown_core_node *node,
                                                                                markdown_core_optional_string *info,
                                                                                markdown_core_optional_string *language,
                                                                                markdown_core_string *literal,
                                                                                bool *fenced, bool *closed);
/** The literal of a `Text`, `Code`, `HTML`, `HTMLBlock`, or `Comment` node.
 * A comment's literal excludes its delimiters and keeps every byte between
 * them, line endings and indentation included. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_literal(const markdown_core_node *node,
                                                                  markdown_core_string *literal);
/** A `Formula` or `FormulaBlock`. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_formula_properties(const markdown_core_node *node,
                                                                             markdown_core_placement *mode,
                                                                             markdown_core_string *literal);
/** A `Table`. The node's children are its rows, in head/content/foot order.
 * These counts partition that single owned chain; row membership is a table
 * fact. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_table_properties(const markdown_core_node *node,
                                                                           size_t *column_count, size_t *head_count,
                                                                           size_t *content_count, size_t *foot_count);
/** A `Table`'s column; OUT_OF_BOUNDS for an index at or past the column
 * count. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_table_column_at(const markdown_core_node *node, size_t index,
                                                                          markdown_core_table_column *column);
/** A `Table`'s independently owned caption field, or NULL when it has none. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_table_caption(const markdown_core_node *node,
                                                                        const markdown_core_node **caption);
/** A `TableCell`. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_table_cell_spans(const markdown_core_node *node,
                                                                           int64_t *rowspan, int64_t *colspan);
/** A `Directive` or `DirectiveBlock`. A directive's name is absent only for a
 * nameless DirectiveBlock. There is no `mode`: an inline `Directive` is
 * always embedded and a `DirectiveBlock` always standalone, so the value was
 * implied by the kind and four surfaces had to keep a constant in step (Q29). */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_directive_properties(const markdown_core_node *node,
                                                                               markdown_core_optional_string *name);
/** A `Definition`. Definition collections preserve the term/body boundary.
 * Body cursors are borrowed collection roots, not Markup nodes. All pointers
 * live with Document. */
typedef struct markdown_core_definition_body markdown_core_definition_body;
MARKDOWN_CORE_API markdown_core_status markdown_core_node_definition_compact(const markdown_core_node *node,
                                                                             bool *compact);
MARKDOWN_CORE_API markdown_core_status markdown_core_node_definition_term(const markdown_core_node *node,
                                                                          const markdown_core_node **term);
MARKDOWN_CORE_API markdown_core_status
markdown_core_node_definition_bodies(const markdown_core_node *node, const markdown_core_definition_body **bodies);
MARKDOWN_CORE_API const markdown_core_definition_body *
markdown_core_definition_body_next(const markdown_core_definition_body *body);
MARKDOWN_CORE_API const markdown_core_node *
markdown_core_definition_body_content(const markdown_core_definition_body *body);
/** Universal fields. Classes and records retain source order and duplicates;
 * absent attributes have zero counts. An `_at` accessor answers OUT_OF_BOUNDS
 * for an index at or past its count. */
MARKDOWN_CORE_API markdown_core_optional_string markdown_core_node_anchor(const markdown_core_node *node);
MARKDOWN_CORE_API size_t markdown_core_node_attribute_class_count(const markdown_core_node *node);
MARKDOWN_CORE_API markdown_core_status markdown_core_node_attribute_class_at(const markdown_core_node *node,
                                                                             size_t index, markdown_core_string *value);
MARKDOWN_CORE_API size_t markdown_core_node_attribute_record_count(const markdown_core_node *node);
MARKDOWN_CORE_API markdown_core_status markdown_core_node_attribute_record_at(const markdown_core_node *node,
                                                                              size_t index, markdown_core_string *name,
                                                                              markdown_core_string *value);
/** A node's attributes as one immutable value, borrowed for the document
 * lifetime: what the node accessors above read. Each node's are the ones
 * written on it; a reference occurrence's are its own, and the `Reference`
 * it names has the ones the definition states. */
typedef struct markdown_core_attribute_value markdown_core_attribute_value;
MARKDOWN_CORE_API const markdown_core_attribute_value *markdown_core_node_attributes(const markdown_core_node *node);
MARKDOWN_CORE_API markdown_core_optional_string
markdown_core_attribute_value_anchor(const markdown_core_attribute_value *attributes);
MARKDOWN_CORE_API size_t markdown_core_attribute_value_class_count(const markdown_core_attribute_value *attributes);
MARKDOWN_CORE_API markdown_core_status markdown_core_attribute_value_class_at(
    const markdown_core_attribute_value *attributes, size_t index, markdown_core_string *value);
MARKDOWN_CORE_API size_t markdown_core_attribute_value_record_count(const markdown_core_attribute_value *attributes);
MARKDOWN_CORE_API markdown_core_status
markdown_core_attribute_value_record_at(const markdown_core_attribute_value *attributes, size_t index,
                                        markdown_core_string *name, markdown_core_string *value);
/** The borrowed dimensions of an `Embedded` or `CrossEmbedded`, valid for the
 * document lifetime, or NULL when none were authored. Dimension values have
 * no node identity. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_dimensions(const markdown_core_node *node,
                                                                     const markdown_core_dimensions **dimensions);
/** A `Directive`'s or `DirectiveBlock`'s optional `DirectiveLabel` field. The
 * returned node is not a directive child; its own children are the label's
 * inline content. NULL means no label. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_directive_label(const markdown_core_node *node,
                                                                          const markdown_core_node **label);
/** A `Callout`'s metadata (M3). Every `>` container is a callout: `variant`
 * is the authored type as written, absent when the container has no metadata
 * line, and `collapsed` is its fold marker, absent when no `+` or `-` was
 * authored, false for `+` (the callout opens expanded) and true for `-`.
 */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_callout_properties(const markdown_core_node *node,
                                                                             markdown_core_optional_string *variant,
                                                                             markdown_core_optional_bool *collapsed);
/** The first node of a `Callout`'s `title`: a node-valued field whose inline
 * nodes follow by `markdown_core_node_get_next_sibling` and are never callout
 * children. A present title holds at least one node, so NULL means no title. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_callout_title(const markdown_core_node *node,
                                                                        const markdown_core_node **title);
/** The tagged `Destination` value of a `Link`, `Embedded`, `Reference`,
 * `CrossLink`, or `CrossEmbedded`: a value, not a node, so it has no scope
 * and no children, and a branch's fields exist only in that branch; the
 * fields a branch does not use are zero. `MARKDOWN_CORE_DESTINATION_URL`
 * fills `url`: a direct `Link` or `Embedded` and every `Reference` answer
 * it. `MARKDOWN_CORE_DESTINATION_CROSS`, the workspace address a cross link
 * produces, fills `path` and `anchor`. `MARKDOWN_CORE_DESTINATION_REFERENCE`
 * fills `label`: a reference occurrence -- `[t][l]`, `[l][]` or `[l]` --
 * names the definition it resolves to by its normalized label, which
 * `markdown_core_document_reference_for` finds.
 *
 * A destination is REQUIRED (Q26, requirement 14): `[a]()` and `[a](<>)`
 * wrote one and wrote nothing in it, so `url` is the empty string. `url`
 * holds the complete semantic destination the inherited grammar produced --
 * the bytes between angle brackets or the bare destination, with backslash
 * escapes and character references decoded and no percent-encoding,
 * normalization, or resolution. */
typedef enum markdown_core_destination_kind {
    MARKDOWN_CORE_DESTINATION_URL = 1,
    MARKDOWN_CORE_DESTINATION_CROSS = 2,
    MARKDOWN_CORE_DESTINATION_REFERENCE = 3
} markdown_core_destination_kind;

typedef struct markdown_core_destination {
    markdown_core_destination_kind kind;
    markdown_core_string url;
    markdown_core_string path;
    markdown_core_optional_string anchor;
    markdown_core_string label;
} markdown_core_destination;

MARKDOWN_CORE_API markdown_core_status markdown_core_node_destination(const markdown_core_node *node,
                                                                      markdown_core_destination *destination);
/** The raw label of a `CrossLink` or remaining raw prefix of a
 * `CrossEmbedded` after a valid dimension suffix. Absent if no separator was
 * authored. A size-only CrossEmbedded label is present and empty. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_cross_label(const markdown_core_node *node,
                                                                      markdown_core_optional_string *label);

/** The OPTIONAL title of a `Link`, `Embedded` or `Reference`: `[a](/u)`
 * wrote no title and `[a](/u "")` wrote an empty one. A reference
 * occurrence writes none; the `Reference` it names states its own. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_title(const markdown_core_node *node,
                                                                markdown_core_optional_string *title);

/** Citation items and document definitions are Markup nodes reached through
 * their typed owning relations. The accessors below read kind-specific fields. */

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

/** The tagged `CitationReferent` value (M4): a value, not a node, and a
 * branch's fields exist only in that branch. `BIB` fills `key` and `mode`.
 * `FOOTNOTE` names a definition by `label`, or owns an inline note: then
 * `note` is its `Footnote` and `label` is empty. `SPECIMEN` fills `label`.
 * The fields a branch does not use are zero. */
typedef struct markdown_core_referent {
    markdown_core_referent_kind kind;
    markdown_core_string key;
    markdown_core_bib_mode mode;
    markdown_core_string label;
    const markdown_core_node *note;
} markdown_core_referent;

/** The first item of a `Cite`; a cite holds at least one item, and the items
 * follow by `markdown_core_node_get_next_sibling` in source order. */
MARKDOWN_CORE_API markdown_core_status markdown_core_node_cite_citations(const markdown_core_node *node,
                                                                         const markdown_core_node **citations);
/** A `Citation`'s referent. */
MARKDOWN_CORE_API markdown_core_status markdown_core_citation_referent(const markdown_core_node *citation,
                                                                       markdown_core_referent *referent);
/** The first node of an item's `prefix` or `suffix`, the inline nodes
 * following by `markdown_core_node_get_next_sibling`, or NULL when the affix
 * is empty. */
MARKDOWN_CORE_API markdown_core_status markdown_core_citation_prefix(const markdown_core_node *citation,
                                                                     const markdown_core_node **prefix);
MARKDOWN_CORE_API markdown_core_status markdown_core_citation_suffix(const markdown_core_node *citation,
                                                                     const markdown_core_node **suffix);

/** THE DOCUMENT'S DEFINITION TABLES. Every `Footnote` -- a definition or an
 * inline note -- and every `Specimen` stays in the tree where it was written;
 * the document lists them in source order. `_at` answers OUT_OF_BOUNDS for an
 * index at or past the count. `_for` returns the first one whose label equals
 * `label` byte for byte, or NULL. */
MARKDOWN_CORE_API size_t markdown_core_document_footnote_count(const markdown_core_document *document);
MARKDOWN_CORE_API markdown_core_status markdown_core_document_footnote_at(const markdown_core_document *document,
                                                                          size_t index,
                                                                          const markdown_core_node **footnote);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_document_footnote_for(const markdown_core_document *document,
                                                                                markdown_core_string label);
MARKDOWN_CORE_API size_t markdown_core_document_specimen_count(const markdown_core_document *document);
MARKDOWN_CORE_API markdown_core_status markdown_core_document_specimen_at(const markdown_core_document *document,
                                                                          size_t index,
                                                                          const markdown_core_node **specimen);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_document_specimen_for(const markdown_core_document *document,
                                                                                markdown_core_string label);
/** A `Footnote`'s label: a definition's label under the reference-label
 * normalization -- full Unicode case fold, trimmed, internal whitespace
 * collapsed -- WITHOUT the caret, exactly the `label` of every `footnote`
 * referent that names it; absent for an inline note.
 *
 * NORMATIVE: a label is compared with memcmp over its bytes. It is never case
 * mapped, never NFC/NFD normalized, never re-encoded, and never used as a key
 * in a language map whose equality has an opinion about Unicode. */
MARKDOWN_CORE_API markdown_core_status markdown_core_footnote_label(const markdown_core_node *footnote,
                                                                    markdown_core_optional_string *label);
/** The first node of a `Footnote`'s block content, the rest following by
 * `markdown_core_node_get_next_sibling`, or NULL when the content is empty. */
MARKDOWN_CORE_API markdown_core_status markdown_core_footnote_content(const markdown_core_node *footnote,
                                                                      const markdown_core_node **content);

/** THE DOCUMENT'S REFERENCES. Every `Reference` stays in the tree where it
 * was written; the document lists them in source order. `_at` answers
 * OUT_OF_BOUNDS for an index at or past the count. `_for` returns the node a
 * reference occurrence naming `label`, a normalized label, resolves to,
 * byte for byte, or NULL: the first `Reference` in source order whose label
 * equals it, or, when none does, the first `Heading` in source order whose
 * text declares it. The label table lists each label that resolves, in
 * byte order, with that node. */
MARKDOWN_CORE_API size_t markdown_core_document_reference_count(const markdown_core_document *document);
MARKDOWN_CORE_API markdown_core_status markdown_core_document_reference_at(const markdown_core_document *document,
                                                                           size_t index,
                                                                           const markdown_core_node **reference);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_document_reference_for(const markdown_core_document *document,
                                                                                 markdown_core_string label);
MARKDOWN_CORE_API size_t markdown_core_document_reference_label_count(const markdown_core_document *document);
MARKDOWN_CORE_API markdown_core_status markdown_core_document_reference_label_at(const markdown_core_document *document,
                                                                                 size_t index,
                                                                                 markdown_core_string *label,
                                                                                 const markdown_core_node **target);
/** A `Reference`'s label, under the reference-label normalization -- full
 * Unicode case fold, trimmed, internal whitespace collapsed -- exactly the
 * `label` of every `reference` destination that names it. Its destination
 * and title are read with `markdown_core_node_destination` and
 * `markdown_core_node_title`; its attributes are its own. Compared as the
 * `Footnote` label is. */
MARKDOWN_CORE_API markdown_core_status markdown_core_reference_label(const markdown_core_node *reference,
                                                                     markdown_core_string *label);

/** A `Specimen` is a block where it was written. An anonymous definition has
 * no label, and an absent start means no explicit counter reset. Display
 * numbers are not stored in the AST. */
MARKDOWN_CORE_API markdown_core_status markdown_core_specimen_properties(const markdown_core_node *specimen,
                                                                         markdown_core_optional_string *label,
                                                                         markdown_core_optional_i64 *start);
MARKDOWN_CORE_API markdown_core_status markdown_core_specimen_content(const markdown_core_node *specimen,
                                                                      const markdown_core_node **content);

/** Allocates the canonical file-tree dump of `node`, a node of `document`,
 * with scopes computed from `source`, the source the document was parsed
 * from, always in UTF-8 columns. `*output` and `*length` receive the dump,
 * which markdown_core_dump_free releases. ALLOCATION_FAILED when an
 * allocation fails; OUT_OF_BOUNDS when `source_length` ends before the node
 * does. */
MARKDOWN_CORE_API markdown_core_status markdown_core_document_dump(const markdown_core_document *document,
                                                                   const markdown_core_node *node,
                                                                   const uint8_t *source, size_t source_length,
                                                                   uint8_t **output, size_t *length);
MARKDOWN_CORE_API void markdown_core_dump_free(uint8_t *output);

#ifdef __cplusplus
}
#endif

#endif
