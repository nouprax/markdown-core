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

/** A PARSE PRODUCES AN ORDERED LIST OF DIAGNOSTICS, and one law governs it:
 *
 *   RECORDING THE LIST CHANGES NOTHING THE PARSE BUILDS.
 *
 * For every input the semantic tree and the concrete records are byte-identical
 * whether or not diagnostics were recorded. THERE IS NO PARTIAL LIST: an
 * allocation the list cannot make abandons the parse, so either a document
 * carries every diagnostic its input earned or there is no document. The
 * converse is equally normative: A PARSE FAILURE IS NOT A DIAGNOSTIC --
 * `markdown_core_error` means there is no document at all, and it carries no
 * scope.
 *
 * WHAT EARNS A DIAGNOSTIC is a fact about the two views rather than a list of
 * syntax rules: A DIAGNOSTIC EXISTS EXACTLY WHERE THE TWO TOTAL VIEWS CANNOT
 * SAY WHAT HAPPENED. Neither view omits a byte, so what is missing is never a
 * byte -- it is why a byte that looks like a construct is not one. So an
 * unclosed fence is NOT diagnosed (`closed` on the node already says it) and a
 * duplicate definition is not (both are nodes, and first-in-document-order is
 * derivable), while a directive whose attribute list was rejected is: that
 * directive and one written with no braces at all are the same tree.
 */
#ifndef MARKDOWN_CORE_DIAGNOSTIC_TYPEDEFS
#define MARKDOWN_CORE_DIAGNOSTIC_TYPEDEFS
typedef enum markdown_core_diagnostic_severity {
    /** The author wrote something the engine did not read the way they meant,
     * and the bytes stand as prose. */
    MARKDOWN_CORE_DIAGNOSTIC_WARNING = 1,
    /** The author NAMED something that does not exist. */
    MARKDOWN_CORE_DIAGNOSTIC_ERROR = 2
} markdown_core_diagnostic_severity;

/** Why a diagnostic was recorded. The ordinal is the wire value every binding
 * decodes, so a code is appended and never inserted. */
typedef enum markdown_core_diagnostic_code {
    /** A directive stands and the `[` after its name did not become a label. */
    MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_LABEL_REJECTED = 1,
    /** A directive stands and the `{` after it did not become an attribute
     * list, so `attributes` is null -- which is also what a directive with no
     * braces at all reports. */
    MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_ATTRIBUTES_REJECTED = 2,
    /** A `::name`/`:::name` line did not open a directive block at all: the
     * block form has no partial fallback, so the whole line is a paragraph. */
    MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_REJECTED = 3,
    /** A container directive was closed by the end of the input rather than by
     * a fence. */
    MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_UNCLOSED = 4,
    /** A delimiter row was found and the header row above it has a different
     * number of columns, so the paragraph is not a table. */
    MARKDOWN_CORE_DIAGNOSTIC_TABLE_REJECTED = 5,
    /** `[text][label]` or `[label][]` naming a label the document does not
     * define. The shortcut form `[label]` is deliberately never reported: it is
     * indistinguishable from ordinary bracketed prose. */
    MARKDOWN_CORE_DIAGNOSTIC_REFERENCE_UNDEFINED = 6,
    /** `[^label]` naming a footnote the document does not define. */
    MARKDOWN_CORE_DIAGNOSTIC_FOOTNOTE_UNDEFINED = 7,
    /** A label the ENGINE refused as too long. The author's label is well
     * formed, which is why this is not "no such definition". */
    MARKDOWN_CORE_DIAGNOSTIC_LABEL_TOO_LONG = 8
} markdown_core_diagnostic_code;
#endif

/** One diagnostic. `scope` is a place in `markdown_core_document_source` and is
 * resolvable without a node handle, which is what the concrete view is for.
 * `message` borrows from the document and ends with it; it is UTF-8, one line,
 * and never empty. */
typedef struct markdown_core_diagnostic {
    markdown_core_diagnostic_severity severity;
    markdown_core_diagnostic_code code;
    markdown_core_scope scope;
    markdown_core_string_view message;
} markdown_core_diagnostic;

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
 * `markdown_core_string_view`, which is what makes the distinction survive.
 *
 * `value.data` is NOT the presence flag. A caller that tests it instead of
 * `has_value` has re-invented the convention this type replaced. */
typedef struct markdown_core_optional_string_view {
    bool has_value;
    markdown_core_string_view value;
} markdown_core_optional_string_view;

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
 * every line having one -- and NOT against the buffer you passed, which is why
 * `markdown_core_document_source` publishes it: a caller whose input contained
 * a NUL has a buffer whose columns no longer agree with ours.
 * `_line_count` and `_line_start` are that source's line index.
 *
 * All of it ends with the document.
 */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_document_semantic(const markdown_core_document *document);
/** The normalized source: the text every scope's coordinates are counted
 * against. Empty, never null, for a document that parsed no bytes. */
MARKDOWN_CORE_API markdown_core_string_view markdown_core_document_source(const markdown_core_document *document);
/** How many lines the normalized source has. */
MARKDOWN_CORE_API size_t markdown_core_document_line_count(const markdown_core_document *document);
/** Where line `line` begins in the source, counting lines from 1. */
MARKDOWN_CORE_API bool markdown_core_document_line_start(const markdown_core_document *document, size_t line,
                                                         size_t *offset);
/** How many diagnostics the parse recorded. They are in the order they were
 * recorded, which is source order for everything the block phase reports and
 * block-then-inline order otherwise. */
MARKDOWN_CORE_API size_t markdown_core_document_diagnostic_count(const markdown_core_document *document);
/** The diagnostic at `index`, counting from 0. */
MARKDOWN_CORE_API bool markdown_core_document_diagnostic_at(const markdown_core_document *document, size_t index,
                                                            markdown_core_diagnostic *diagnostic);
/** The stable spelling of a code, for a consumer that would otherwise keep its
 * own table. NULL for a value no version of this library defines. */
MARKDOWN_CORE_API const char *markdown_core_diagnostic_code_name(markdown_core_diagnostic_code code);

/** A parse failure. There is NO document, and there is no scope: an input the
 * parser could not turn into a document has no extent to point at, and a
 * failure the author could act on would have been a diagnostic instead. */
MARKDOWN_CORE_API markdown_core_error_code markdown_core_error_get_code(const markdown_core_error *error);
MARKDOWN_CORE_API markdown_core_string_view markdown_core_error_get_message(const markdown_core_error *error);
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
/** `info` and `language` are OPTIONAL: a fence with nothing but whitespace
 * after it wrote no info string, and an indented block has no fence to write
 * one on. `language` is the info string's first word and is present exactly
 * when `info` is. */
MARKDOWN_CORE_API bool markdown_core_node_code_block_properties(const markdown_core_node *node,
                                                                markdown_core_optional_string_view *info,
                                                                markdown_core_optional_string_view *language,
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
/** A destination is REQUIRED and a title is OPTIONAL (Q26, requirement 14).
 * `[a]()` and `[a](<>)` wrote a destination and wrote nothing in it, so they
 * answer with the empty string; there is no inline link whose author wrote no
 * destination, because the shortcut and collapsed forms are `LinkReference`
 * and carry none. `[a](/u)` wrote no title; `[a](/u "")` wrote an empty one. */
MARKDOWN_CORE_API bool markdown_core_node_link_properties(const markdown_core_node *node,
                                                          markdown_core_string_view *destination,
                                                          markdown_core_optional_string_view *title);
MARKDOWN_CORE_API bool markdown_core_node_image_properties(const markdown_core_node *node,
                                                           markdown_core_string_view *source,
                                                           markdown_core_optional_string_view *title);
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
                                                              markdown_core_optional_string_view *title);
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
