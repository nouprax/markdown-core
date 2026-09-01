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
 * afterwards. Node handles and `markdown_core_string`s borrow from the owning document
 * and end with it.
 *
 * Shared immutable state: a document may share reference-counted immutable
 * state (cached inline lists, frozen content buffers) with the session that
 * produced it and with other documents. That sharing is internal and its
 * counts are atomic, so freeing a document requires no synchronization
 * against the session or against other documents -- only the per-document
 * rule above. Every accessor in this header is a non-mutating read, so the
 * read-only promises hold even for the nodes documents share. A session's
 * own entry points remain single-caller: two threads must not drive one
 * session concurrently.
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

/** Initializes every field to the frozen Markdown Core defaults. */
MARKDOWN_CORE_API void markdown_core_parse_options_init(markdown_core_parse_options *options);

/**
 * Parses exactly `length` UTF-8 bytes. `options == NULL` selects the defaults.
 * The returned document owns every node and every `markdown_core_string`
 * handed out of it. On failure,
 * NULL is returned and `*error` is set when `error` is non-NULL.
 */
MARKDOWN_CORE_API markdown_core_document *markdown_core_document_parse(
    const uint8_t *source,
    size_t length,
    const markdown_core_parse_options *options,
    markdown_core_error **error
);
MARKDOWN_CORE_API void markdown_core_document_free(markdown_core_document *document);

/**
 * THE STREAM (docs/STREAMING.md §4 D5): a session, `feed`, and the document's
 * one view -- the same `semantic` every document
 * publishes. `feed` returns THE DOCUMENT AFTER THOSE BYTES: a value the
 * caller owns outright, frees with `markdown_core_document_free`, and keeps
 * -- it stays readable after every later feed and after the session itself
 * is gone. There is no ask and no snapshot handle; the return value is the
 * only answer there is.
 *
 * What a mid-stream document is: the projection of the parse as it stands.
 * A trailing line whose ending has not arrived is not yet in it, and an open
 * construct is projected as it stands (a list still open has not settled
 * its tightness).
 *
 * `finish` ends the stream: the pending line is processed, every construct
 * closes, and the SEALED document comes back -- byte-identical to what
 * `markdown_core_document_parse` returns for the same
 * bytes. It also ends the session's parse: `feed` and `finish` after it
 * report MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, and only
 * `markdown_core_session_free` remains to take the shell back.
 */
typedef struct markdown_core_session markdown_core_session;

/** Opens a session. `options == NULL` selects the defaults, exactly as
 * `markdown_core_document_parse` reads them. */
MARKDOWN_CORE_API markdown_core_session *markdown_core_session_new(
    const markdown_core_parse_options *options,
    markdown_core_error **error
);
/** Feeds exactly `length` UTF-8 bytes and returns the document after them.
 * `length == 0` is a legal feed: the document as it stands. */
MARKDOWN_CORE_API markdown_core_document *markdown_core_session_feed(
    markdown_core_session *session,
    const uint8_t *chunk,
    size_t length,
    markdown_core_error **error
);
/** WHAT A WIRE PAYLOAD IS RELATIVE TO (#162). Every payload the wire
 * writes -- a feed's, a seal's, `markdown_core_document_wire`'s -- leads
 * with one frame byte. FULL: the whole tree follows, as the layout under
 * `markdown_core_document_wire` states it. DELTA: what follows names the
 * tree by its differences from THE PREVIOUS PAYLOAD THIS SESSION WROTE,
 * so a binding that decoded that payload reuses the values it already
 * built for everything that did not move. A caller REQUESTS a frame; the
 * session answers DELTA only when it asked for one AND a previous payload
 * exists, and FULL otherwise -- so the first read of a stream, and every
 * read after a caller that lost its previous value asks for FULL, are
 * whole. The request is the binding's whole protocol: ask DELTA while the
 * previous value is in hand, FULL when it is not. */
typedef enum markdown_core_wire_frame {
    MARKDOWN_CORE_WIRE_FULL = 0,
    MARKDOWN_CORE_WIRE_DELTA = 1
} markdown_core_wire_frame;

/** Feeds exactly `length` UTF-8 bytes and answers the WIRE for the current
 * document state directly -- the same tree `markdown_core_session_feed`
 * followed by `markdown_core_document_wire` would produce, without building
 * the owned document in between. For a bridge whose document never escapes
 * the delivering call, that intermediate is an allocation and a free per
 * feed for a document nothing reads; here the wire is written during
 * the synchronous call. `request` asks for a FULL or a DELTA frame
 * (`markdown_core_wire_frame` above): the delta is against the last
 * payload THIS entry or `markdown_core_session_finish_wire` wrote
 * successfully -- `markdown_core_session_feed`'s owned documents and
 * `markdown_core_session_advance` are not payloads and move nothing -- and
 * a request the session cannot honor is answered FULL. A payload that
 * fails to build leaves the previous one standing, so a caller may keep
 * or drop its previous value after a failure and stay in step either way.
 * `prefix` reserves zeroed envelope room ahead of the payload, in the one
 * allocation, exactly as `markdown_core_document_wire` does; release the
 * buffer with `markdown_core_wire_free`. C consumers that read the
 * document through accessors keep using `markdown_core_session_feed`. */
MARKDOWN_CORE_API bool markdown_core_session_feed_wire(
    markdown_core_session *session,
    const uint8_t *chunk,
    size_t length,
    size_t prefix,
    markdown_core_wire_frame request,
    uint8_t **output,
    size_t *output_length,
    markdown_core_error **error
);
/** Ends the stream and returns the sealed document. */
MARKDOWN_CORE_API markdown_core_document *markdown_core_session_finish(
    markdown_core_session *session,
    markdown_core_error **error
);
/** Ends the stream and answers the sealed document's WIRE directly: the
 * same tree `markdown_core_session_finish` seals, in the frame `request`
 * asks for, against the last payload the session wrote. Like `_finish` it
 * ends the session's parse whatever it answers: `feed` and both `finish`
 * entries report MARKDOWN_CORE_ERROR_INVALID_ARGUMENT after it, and only
 * `markdown_core_session_free` remains. */
MARKDOWN_CORE_API bool markdown_core_session_finish_wire(
    markdown_core_session *session,
    size_t prefix,
    markdown_core_wire_frame request,
    uint8_t **output,
    size_t *output_length,
    markdown_core_error **error
);
/** Feeds exactly `length` UTF-8 bytes and answers NOTHING: no projection is
 * taken and no document is built. The one legitimate caller is a read the
 * caller's own contract DISCARDS -- the bindings' `Document(markdown)`
 * constructor, whose initial feed's read is thrown away undecoded -- and for
 * that lifecycle a full `markdown_core_session_feed` derives, copies and
 * frees a document nothing looks at. Returns false with `*error` set exactly
 * where `_feed` would have failed; a later `_feed` or `_finish` answers as if
 * the same bytes had arrived through it, which is feed/seal partition
 * invariance doing the work. */
MARKDOWN_CORE_API bool markdown_core_session_advance(
    markdown_core_session *session,
    const uint8_t *chunk,
    size_t length,
    markdown_core_error **error
);
MARKDOWN_CORE_API void markdown_core_session_free(markdown_core_session *session);

/**
 * THE PARSE, AND WHAT ITS COORDINATES ARE COUNTED AGAINST.
 *
 * `markdown_core_document_semantic` is the tree. Every node carries a `scope`,
 * and a scope exists for one purpose: SO A CONSUMER CAN MAP AN ELEMENT BACK TO
 * THE SOURCE IT CAME FROM.
 *
 * A SCOPE IS A PAIR OF BOUNDARIES, NOT A BYTE RANGE. Owner ruling, 2026-08-24:
 * a scope's line and column do not stand for any source subrange and no
 * subrange can be taken with them; what they are for is telling an editor which
 * line-and-column range an element occupies. So a line of L bytes carries
 * boundaries 1 through L+1, and an end at column 0 of line N says the element
 * stopped where line N-1 ended.
 *
 * They are counted against the NORMALIZED source -- UTF-8 as fed, every NUL
 * replaced by the three bytes of U+FFFD, every line ending a single `\n` and
 * every line having one -- and NOT against the buffer you passed. The library
 * does not hand that text back; a caller whose input can differ from it (a
 * NUL, a CRLF, a missing final newline) applies the same normalization to its
 * own copy before resolving a scope against it.
 *
 * All of it ends with the document.
 */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_document_semantic(const markdown_core_document *document);
/** A parse failure. There is NO document, and there is no scope: an input the
 * parser could not turn into a document has no extent to point at. */
MARKDOWN_CORE_API markdown_core_error_code markdown_core_error_get_code(const markdown_core_error *error);
MARKDOWN_CORE_API markdown_core_string markdown_core_error_get_message(const markdown_core_error *error);
MARKDOWN_CORE_API void markdown_core_error_free(markdown_core_error *error);

MARKDOWN_CORE_API markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node);
MARKDOWN_CORE_API const char *markdown_core_node_kind_name(markdown_core_node_kind kind);
MARKDOWN_CORE_API markdown_core_scope markdown_core_node_scope(const markdown_core_node *node);

/** A node's children are read through a BY-VALUE cursor. Sibling order is
 * the PARENT's fact (D9): asking a node what follows it was the one question
 * a node shared between two reads of one stream could not answer, so the
 * cursor carries the parent and the position, every step is O(1), and no
 * allocation or free is involved. `child` is NULL once the children are
 * exhausted (and for a NULL or childless `node`). Iterate:
 *
 *     markdown_core_children cur = markdown_core_node_children(node);
 *     for (; cur.child; cur = markdown_core_children_next(cur)) { ... }
 */
typedef struct {
    const markdown_core_node *parent;
    const markdown_core_node *child;
    size_t index;
} markdown_core_children;

MARKDOWN_CORE_API markdown_core_children markdown_core_node_children(const markdown_core_node *node);
MARKDOWN_CORE_API markdown_core_children markdown_core_children_next(markdown_core_children cursor);
MARKDOWN_CORE_API size_t markdown_core_node_child_count(const markdown_core_node *node);

MARKDOWN_CORE_API bool markdown_core_node_heading_level(const markdown_core_node *node, int32_t *level);
MARKDOWN_CORE_API bool markdown_core_node_list_properties(
    const markdown_core_node *node,
    markdown_core_list_flavor *flavor,
    markdown_core_optional_i64 *start,
    bool *tight
);
MARKDOWN_CORE_API bool markdown_core_node_list_item_checked(
    const markdown_core_node *node,
    markdown_core_optional_bool *checked
);
/** `info` and `language` are OPTIONAL: a fence with nothing but whitespace
 * after it wrote no info string, and an indented block has no fence to write
 * one on. `language` is the info string's first word and is present exactly
 * when `info` is. */
MARKDOWN_CORE_API bool markdown_core_node_code_block_properties(
    const markdown_core_node *node,
    markdown_core_optional_string *info,
    markdown_core_optional_string *language,
    markdown_core_string *literal,
    bool *fenced,
    bool *closed
);
MARKDOWN_CORE_API bool markdown_core_node_literal(const markdown_core_node *node, markdown_core_string *literal);
MARKDOWN_CORE_API bool markdown_core_node_formula_properties(
    const markdown_core_node *node,
    markdown_core_placement_mode *mode,
    markdown_core_string *literal
);
MARKDOWN_CORE_API bool markdown_core_node_table_column_count(const markdown_core_node *node, size_t *count);
MARKDOWN_CORE_API bool markdown_core_node_table_alignment_at(
    const markdown_core_node *node,
    size_t index,
    markdown_core_table_alignment *alignment
);
MARKDOWN_CORE_API bool markdown_core_node_table_row_is_header(const markdown_core_node *node, bool *is_header);
/** A directive's properties. There is no `mode`: an inline `Directive` is
 * always embedded and a `DirectiveBlock` always standalone, so the value was
 * implied by the kind and four surfaces had to keep a constant in step (Q29). */
MARKDOWN_CORE_API bool markdown_core_node_directive_properties(
    const markdown_core_node *node,
    markdown_core_string *name,
    bool *has_attributes,
    size_t *attribute_count
);
MARKDOWN_CORE_API bool markdown_core_node_directive_attribute_at(
    const markdown_core_node *node,
    size_t index,
    markdown_core_string *name,
    markdown_core_string *value
);
/** A destination is REQUIRED and a title is OPTIONAL (Q26, requirement 14).
 * `[a]()` and `[a](<>)` wrote a destination and wrote nothing in it, so they
 * answer with the empty string; there is no inline link whose author wrote no
 * destination, because the shortcut and collapsed forms are `LinkReference`
 * and carry none. `[a](/u)` wrote no title; `[a](/u "")` wrote an empty one. */
MARKDOWN_CORE_API bool markdown_core_node_link_properties(
    const markdown_core_node *node,
    markdown_core_string *destination,
    markdown_core_optional_string *title
);
MARKDOWN_CORE_API bool markdown_core_node_image_properties(
    const markdown_core_node *node,
    markdown_core_string *source,
    markdown_core_optional_string *title
);
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
MARKDOWN_CORE_API bool markdown_core_node_association(
    const markdown_core_node *node,
    markdown_core_string *label,
    markdown_core_string *identifier
);
/** A link reference definition's resource.
 *
 * `destination` is REQUIRED and is never absent -- a definition whose
 * destination could not be built is not emitted at all (Q7, Q26) -- while
 * `title` is absent when the source wrote none, and empty when the source
 * wrote an empty one. */
MARKDOWN_CORE_API bool markdown_core_node_definition_resource(
    const markdown_core_node *node,
    markdown_core_string *destination,
    markdown_core_optional_string *title
);
/** The form a `LinkReference` or `ImageReference` was written in. */
MARKDOWN_CORE_API bool markdown_core_node_reference_form(
    const markdown_core_node *node,
    markdown_core_reference_form *form
);

/** A NODE'S IDENTITY (docs/STREAMING.md §4 D4): the name a consumer tracks an
 * element by across a stream's feeds. `block` is the owning block's
 * document-unique mint; `ordinal` is the node's pre-order ordinal among that
 * block's inline descendants, and 0 for the block itself. The pair is unique
 * within one document and never reused within a parse; it is NOT stable across
 * documents or across two parses of different byte streams. The block is the
 * minimal update unit, so `block` alone names the region an incremental
 * consumer re-renders. */
typedef struct markdown_core_identity {
    uint32_t block;
    uint32_t ordinal;
} markdown_core_identity;

/** THE NODE'S IDENTIFIER: the value that carries its identity (D4's own
 * words -- the identity is the concept, the identifier is the value), answered
 * whole from the node alone like `markdown_core_node_scope`, by value,
 * `{0, 0}` for NULL. A block's is `(its mint, 0)`; an inline's is
 * `(owning block's mint, its ordinal)`, the owner stamped by the same pass
 * that assigns the ordinal, so no caller ever composes the pair itself. */
MARKDOWN_CORE_API markdown_core_identity markdown_core_node_identifier(const markdown_core_node *node);

/** THE DEFINITION EDGE (docs/STREAMING.md §4 D4): the identity of the
 * definition a reference resolved to. Answers for `LinkReference`,
 * `ImageReference` and `FootnoteReference`, and refuses every other kind.
 *
 * The target is a definition BLOCK, so its ordinal is 0 by construction. The
 * winner for a repeated label is the definition that opens first in document
 * order -- block mints are monotone in parse order, so it is also the one
 * with the smallest identity -- while every later definition of the same
 * label stays in the tree where it was written. The edge never means
 * "unresolved": a well-formed reference that resolves to nothing is prose, so
 * a reference node exists only because resolution succeeded. */
MARKDOWN_CORE_API bool markdown_core_node_reference_definition(
    const markdown_core_node *node,
    markdown_core_identity *definition
);

/** Allocates the canonical file-tree dump. Free it with markdown_core_dump_free. */
MARKDOWN_CORE_API bool markdown_core_document_dump(
    const markdown_core_document *document,
    uint8_t **output,
    size_t *length,
    markdown_core_error **error
);
MARKDOWN_CORE_API void markdown_core_dump_free(uint8_t *output);

/** THE WIRE: the document serialized into ONE buffer, so a
 * binding whose boundary is expensive to cross -- JNI, WebAssembly -- crosses
 * it once per read instead of once per field. The dump above is the canonical
 * TEXT of a document; this is its canonical BYTES, and the two change together
 * or not at all.
 *
 * Layout, little-endian throughout, no padding. The payload leads with one
 * u8 FRAME byte (`markdown_core_wire_frame`): 0 (FULL) and the root node
 * follows; 1 (DELTA) and one SPINE op follows, described after the node.
 * This entry always writes FULL. A `string` is an i32 byte
 * length followed by that many UTF-8 bytes, or length -1 for an absent
 * optional string -- `null` and `""` stay two answers on the wire (requirement
 * 14). A `node` is:
 *
 *   u8 kind (the markdown_core_node_kind ordinal)
 *   u32 identity.block, u32 identity.ordinal
 *   i32 x4 scope (start line, start column, end line, end column)
 *   kind-specific fields, then children where the kind has them:
 *     Heading: i32 level
 *     List: i32 flavor, i64 start, u8 start-present, u8 tight
 *     ListItem: u8 checked (0, 1, or 255 for null)
 *     CodeBlock: string? info, string? language, string literal, u8 fenced,
 *       u8 closed
 *     HTMLBlock, Text, Code, HTML: string literal
 *     Formula: i32 mode, string literal;  FormulaBlock: string literal
 *     Table: i32 column count, u8 alignment each, children
 *     Directive, DirectiveBlock: string name, u8 has-attributes, i32 count,
 *       (string name, string value) each, children
 *     FootnoteDefinition: string label, string identifier, children
 *     ReferenceDefinition: string label, string identifier,
 *       string destination, string? title
 *     FootnoteReference: string label, u32 x2 definition identity
 *     LinkReference, ImageReference: string label, i32 form,
 *       u32 x2 definition identity, children
 *     Link, Image: string destination/source, string? title, children
 *     TableRow: u8 is-header, children
 *   children: i32 count, then each child node
 *
 * A DELTA frame (#162) is written by the session entries against the
 * previous payload the same session wrote, and is a tree of OPS. An op
 * leads with a u8 tag: a tag below 0xFE is a node's kind byte and the rest
 * of that node follows (NODE: a subtree written whole); 0xFE is SPINE: u8
 * kind, identity, scope and the kind's own fields as above, then i32 op
 * count and that many ops IN PLACE OF the children -- the node's fields are
 * rewritten and its child list is rebuilt from the ops; 0xFF is SAME: i32
 * n, standing for the next n children of the PREVIOUS payload's node at
 * the same position, unchanged. Positions pair the two payloads' child
 * lists index by index: a SPINE op at position i names the previous node's
 * child i, and SAME runs are counted in the same positions. Only block
 * containers whose children are blocks are ever SPINE -- document, block
 * quote, list, list item, table, directive block, footnote definition --
 * and a SPINE's kind and identity always equal the previous child's. The
 * root of a DELTA frame is one SPINE op for the document. A node the
 * engine retained across the two derivations (docs/STREAMING.md F27) is
 * exactly what SAME names, so a delta costs the open spine and the
 * changed blocks, never the document.
 *
 * The root node ends the payload. Free the buffer with
 * `markdown_core_wire_free`. The layout changes only with the version this
 * library ships, which is why the buffer carries no version of its own --
 * a transport that can skew (a prebuilt native library beside newer binding
 * code) wraps this payload in its own versioned envelope. `prefix` is that
 * envelope's room: the buffer's first `prefix` bytes are reserved, zeroed,
 * for the caller to write, and `*length` counts them -- so the wrap costs no
 * second allocation and no copy of the payload. Pass 0 for the bare payload. */
MARKDOWN_CORE_API bool markdown_core_document_wire(
    const markdown_core_document *document,
    size_t prefix,
    uint8_t **output,
    size_t *length,
    markdown_core_error **error
);
MARKDOWN_CORE_API void markdown_core_wire_free(uint8_t *output);

#ifdef __cplusplus
}
#endif

#endif
