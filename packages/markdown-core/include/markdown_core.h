#ifndef MARKDOWN_CORE_FACADE_H
#define MARKDOWN_CORE_FACADE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Version discovery
 * =================
 *
 * This installed facade intentionally exposes no compile-time version macro
 * or runtime version function. Discover the installed package version through
 * its markdown-core pkg-config or CMake package metadata.
 */

/*
 * Thread safety and ownership contract
 * ====================================
 *
 * Initialization: none. The library holds no process-level mutable state —
 * no registry, no locks, no lazy initialization. Concurrent first calls from
 * any number of threads are safe by construction; no warmup, external lock,
 * or explicit init call is required.
 *
 * Distinct chains: parse, traversal, dump, mutation, and free on documents
 * of *different* chains may run fully concurrently. No two chains share
 * mutable state.
 *
 * One chain: a mutation (append) is an exclusive operation on its
 * chain — all access to any handle on the chain must happen-before the
 * mutation or happen-after its return, and two mutations must be externally
 * serialized. Between mutations, concurrent read-only access (traversal,
 * accessors, dump) to the live head from any number of threads is safe.
 * markdown_core_document_free of one handle must be externally ordered
 * against other access to THAT handle; different handles of one chain may be
 * freed concurrently. Node handles and string views borrow from the owning
 * document and end with it.
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

/** Node identity, assigned once per series and never reused.
 *
 * Stable across appends while the node remains the same kind of thing at
 * the same place. 0 is never a valid id. */
typedef uint64_t markdown_core_node_id;

/** Bytes and a length.
 *
 * Every string this API HANDS OUT borrows from the document that owns it and
 * ends with that document — the ownership section above states that once,
 * for all of them. Every string HANDED IN is copied, so the caller keeps its
 * own. */
typedef struct markdown_core_string {
    const uint8_t *data;
    size_t length;
} markdown_core_string;

/** A one-based line/column source coordinate. */
typedef struct markdown_core_position {
    int32_t line;
    int32_t column;
} markdown_core_position;

/** An absolute source extent: first and last coordinate, both INCLUSIVE, and
 * covering the construct's own markers rather than its content alone.
 *
 * A heading's scope starts at its `#`, a fenced code block's ends on the
 * closing fence. */
typedef struct markdown_core_scope {
    markdown_core_position start;
    markdown_core_position end;
} markdown_core_scope;

/** The parser's feature switches.
 *
 * Every one of them defaults to ON, so a zeroed struct is not the default
 * configuration but its opposite: start from markdown_core_parse_options_init
 * and clear what you do not want. */
typedef struct markdown_core_parse_options {
    /** Straight quotes, dashes, and ellipses become their typographic forms. */
    bool smart_punctuation;
    /** `[^label]` references and the `[^label]:` definitions they resolve to,
     * both under this one switch. */
    bool footnotes;
    /** Pipe tables. */
    bool tables;
    /** `~struck~` and `~~struck~~`; the closer must match the opener's tilde
     * count. */
    bool strikethrough;
    /** Bare URLs and email addresses become links; `<https://x>` is
     * CommonMark's own and is not gated here. */
    bool autolinks;
    /** `[ ]` and `[x]` markers at the head of a list item. */
    bool task_lists;
    /** Formula spans and blocks: dollar and LaTeX delimiters, and `formula`
     * fenced blocks. */
    bool formulas;
    /** The directive grammar: `:name` inline, `::name` leaf and `:::name`
     * container at the head of a line, each with its `[label]` and
     * `{attributes}`. */
    bool directives;
    /** `[[reference]]`. */
    bool cross_links;
    /** `![[reference]]`. */
    bool embeds;
} markdown_core_parse_options;

/** Why a call failed.
 *
 * Never a verdict on the Markdown — every byte sequence is a valid document,
 * as the Diagnostics section below explains — so what is left is:
 *
 * - a rejected argument
 * - an allocation that did not succeed
 * - an engine invariant that did not hold
 */
typedef enum markdown_core_error_code {
    MARKDOWN_CORE_ERROR_NONE = 0,
    MARKDOWN_CORE_ERROR_INVALID_ARGUMENT = 1,
    MARKDOWN_CORE_ERROR_ALLOCATION_FAILED = 2,
    MARKDOWN_CORE_ERROR_INTERNAL = 3
} markdown_core_error_code;

/** What a node is.
 *
 * MARKDOWN_CORE_KIND_NONE names no node: it is what a NULL node answers, and
 * nothing in a tree carries it. */
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
    MARKDOWN_CORE_KIND_TABLE_ROW,
    MARKDOWN_CORE_KIND_TABLE_CELL,
    MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK,
    MARKDOWN_CORE_KIND_DIRECTIVE_LABEL,
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
    MARKDOWN_CORE_KIND_CROSS_LINK,
    MARKDOWN_CORE_KIND_EMBED,
    /* Appended rather than grouped with the other block and inline kinds:
     * the wire format numbers this enum positionally and four bindings decode
     * that number, so inserting in the middle renumbers everything after it.
     * Block-ness is carried by the node type, not by position here. */
    MARKDOWN_CORE_KIND_REFERENCE_DEFINITION,
    MARKDOWN_CORE_KIND_LINK_REFERENCE,
    MARKDOWN_CORE_KIND_IMAGE_REFERENCE
} markdown_core_node_kind;

typedef enum markdown_core_list_flavor {
    MARKDOWN_CORE_LIST_FLAVOR_BULLET = 1,
    MARKDOWN_CORE_LIST_FLAVOR_ORDERED = 2
} markdown_core_list_flavor;

/** How a reference was written.
 *
 * The three resolve identically and differ only in source form, which the
 * tree keeps because it is what was written. */
typedef enum markdown_core_reference_form {
    /** `[text][label]`: caption and label both written out. */
    MARKDOWN_CORE_REFERENCE_FULL = 0,
    /** `[label][]`: the label doubles as the caption. */
    MARKDOWN_CORE_REFERENCE_COLLAPSED = 1,
    /** `[label]`: no second bracket pair at all. */
    MARKDOWN_CORE_REFERENCE_SHORTCUT = 2
} markdown_core_reference_form;

/** Whether a construct is embedded in surrounding inline content or stands
 * alone.
 *
 * Placement is not containment and cannot be read off the parent: a
 * standalone formula is still written inside a paragraph. */
typedef enum markdown_core_placement_mode {
    MARKDOWN_CORE_PLACEMENT_EMBEDDED = 1,
    MARKDOWN_CORE_PLACEMENT_STANDALONE = 2
} markdown_core_placement_mode;

/** A table column's alignment, as the colons in the delimiter row declare it.
 *
 * - `:---` left
 * - `:--:` center
 * - `---:` right
 *
 * NONE is a column that declared nothing, not a fourth alignment. */
typedef enum markdown_core_table_alignment {
    MARKDOWN_CORE_TABLE_ALIGNMENT_NONE = 0,
    MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT = 1,
    MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER = 2,
    MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT = 3
} markdown_core_table_alignment;

/** An int64 that may be absent; `value` is meaningful only when `has_value`
 * is set.
 *
 * No sentinel would do in its place: an ordered list may legitimately start
 * at 0, and `0` has to stay distinguishable from "this list has no start
 * number". */
typedef struct markdown_core_optional_i64 {
    bool has_value;
    int64_t value;
} markdown_core_optional_i64;

/** A bool that may be absent, which is three states and not two.
 *
 * A list item's checkbox is:
 *
 * - ABSENT when the item is not a task item at all
 * - false when the box is empty
 * - true when it is ticked
 */
typedef struct markdown_core_optional_bool {
    bool has_value;
    bool value;
} markdown_core_optional_bool;

/** Initializes every field to the frozen Markdown Core defaults. */
MARKDOWN_CORE_API void markdown_core_parse_options_init(markdown_core_parse_options *options);

/** NULL is allowed. Legal on a superseded handle at any time, from any
 * thread — freeing touches only the document's own state and the chain's
 * refcount, never the live head (the Mutation section states the
 * supersession rule; the TREE is what expires there). Costs O(1) beyond the
 * handle's own teardown; releasing a chain's last handle reclaims what the
 * chain itself owns. */
MARKDOWN_CORE_API void markdown_core_document_free(markdown_core_document *document);
/** The root of the document's tree: kind DOCUMENT, scoped to the whole text.
 *
 * NULL for a NULL document, and NULL once the document has been superseded:
 * the chain keeps one tree, and it belongs to the head. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_document_root(const markdown_core_document *document);

/** MARKDOWN_CORE_ERROR_NONE for a NULL error, so the code can be read before
 * the pointer is checked. */
MARKDOWN_CORE_API markdown_core_error_code markdown_core_error_get_code(const markdown_core_error *error);
/** The engine's account of the failure.
 *
 * It borrows from the error rather than from a document and ends with
 * markdown_core_error_free; a NULL error gives an empty view. */
MARKDOWN_CORE_API markdown_core_string markdown_core_error_get_message(const markdown_core_error *error);
MARKDOWN_CORE_API void markdown_core_error_free(markdown_core_error *error);

MARKDOWN_CORE_API markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node);
/** The kind's canonical name, the spelling the dump prints: "BlockQuote",
 * "HTMLBlock", "FootnoteDefinition".
 *
 * A static string belonging to the library and not to any document — it
 * outlives every document and is never freed. "None" for a value outside the
 * enum. */
MARKDOWN_CORE_API const char *markdown_core_node_kind_name(markdown_core_node_kind kind);
/** The node's extent, stored on the node: the read is O(1) and walks no
 * ancestors.
 *
 * All zeros for a NULL node. */
MARKDOWN_CORE_API markdown_core_scope markdown_core_node_scope(const markdown_core_node *node);

/** Canonical traversal follows the refined AST's direct semantic edges.
 *
 * Children come in source order, so first child then next sibling reads a
 * node's content in the order it was written. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_get_first_child(const markdown_core_node *node);
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_get_next_sibling(const markdown_core_node *node);
/** Counts by walking the child list rather than reading a stored number, so a
 * caller that wants the count and the children should get both from one walk.
 *
 * Zero for a NULL node. */
MARKDOWN_CORE_API size_t markdown_core_node_child_count(const markdown_core_node *node);

/*
 * Node accessors
 * ==============
 *
 * Each of the accessors below answers for one kind, or for the small family of
 * kinds that shares the field. Each returns false when the node is NULL, when
 * it is not one of those kinds, or when an out-parameter is NULL — and every
 * out-parameter is then left untouched, so asking a node for a field it does
 * not have costs nothing and needs no kind test in front of it. The one that
 * answers with a node rather than a bool, markdown_core_node_directive_label,
 * returns NULL in those same cases. What the comments here add is what the
 * out-parameters hold when the answer is true.
 */

/** `*level` is 1 through 6. */
MARKDOWN_CORE_API bool markdown_core_node_heading_level(const markdown_core_node *node, int32_t *level);
/** `*start` carries a value only for an ordered list, the only flavor with a
 * number to start from, and `*tight` is whether the list renders without
 * paragraph spacing between its items. */
MARKDOWN_CORE_API bool markdown_core_node_list_properties(
    const markdown_core_node *node,
    markdown_core_list_flavor *flavor,
    markdown_core_optional_i64 *start,
    bool *tight
);
/** True for any list item, task-list or not: a plain item is answered as a
 * checkbox that is absent, not as a failure. */
MARKDOWN_CORE_API bool markdown_core_node_list_item_checked(
    const markdown_core_node *node,
    markdown_core_optional_bool *checked
);
/** `*info` is the whole info string after the opening fence and `*language`
 * its first word — a view INTO `*info`, not a copy of it.
 *
 * Both have NULL `data` when there is no info string to read them from.
 * `*fenced` separates a fenced block from an indented one, and `*closed` is
 * whether a fenced block ever met its closing fence: an unclosed fence runs to
 * the end of the document, and an indented block is closed by definition. */
MARKDOWN_CORE_API bool markdown_core_node_code_block_properties(
    const markdown_core_node *node,
    markdown_core_string *info,
    markdown_core_string *language,
    markdown_core_string *literal,
    bool *fenced,
    bool *closed
);
/** The literal bytes of a Text, Code, HTML, or HTMLBlock node, and false for
 * every other kind.
 *
 * CodeBlock, Formula, and FormulaBlock hold text of their own but answer
 * through markdown_core_node_code_block_properties and
 * markdown_core_node_formula_properties. */
MARKDOWN_CORE_API bool markdown_core_node_literal(const markdown_core_node *node, markdown_core_string *literal);
/** For HTMLBlock and HTML nodes: true when the literal is one complete
 * comment.
 *
 * After surrounding whitespace it opens with `<!--` and its first `-->` is
 * the terminal bytes. Comment-prefixed html with a same-line tail is not a
 * comment. Derived purely from the literal, so consumers on platforms without
 * an html parser can skip comment material by this bit. */
MARKDOWN_CORE_API bool markdown_core_node_html_comment(const markdown_core_node *node, bool *comment);
/** `*literal` is the formula source between the delimiters.
 *
 * `*mode` is which delimiters those were and not where the node sits: an
 * inline formula written `$$...$$` reports standalone, while a formula block
 * is standalone always. */
MARKDOWN_CORE_API bool markdown_core_node_formula_properties(
    const markdown_core_node *node,
    markdown_core_placement_mode *mode,
    markdown_core_string *literal
);
/** The number of columns the delimiter row declares.
 *
 * Every row of the table has exactly that many cells: a short row is
 * completed with empty ones and a long row is cut. */
MARKDOWN_CORE_API bool markdown_core_node_table_column_count(const markdown_core_node *node, size_t *count);
/** False when `index` is not less than the column count. */
MARKDOWN_CORE_API bool markdown_core_node_table_alignment_at(
    const markdown_core_node *node,
    size_t index,
    markdown_core_table_alignment *alignment
);
/** Exactly one row of a table is its header, and it is the first. */
MARKDOWN_CORE_API bool markdown_core_node_table_row_is_header(const markdown_core_node *node, bool *is_header);
/** A directive's placement and name, and whether it was written with an
 * attribute container at all.
 *
 * `has_attributes` is what separates `:a` from `:a{}`; the entries themselves
 * come from the two accessors below. */
MARKDOWN_CORE_API bool markdown_core_node_directive_properties(
    const markdown_core_node *node,
    markdown_core_placement_mode *mode,
    markdown_core_string *name,
    bool *has_attributes
);
/**
 * A directive's attributes, as the string-to-string map the grammar defines.
 *
 * One entry per name, in source order, with the merge rules already applied:
 *
 * - a later `k=` replaces an earlier one
 * - `.a .b` accumulates into one `class`
 * - `#a #b` keeps the last
 *
 * There is no duplicate name to represent, which is why a map is the whole of
 * it and no serialization sits in between.
 */
MARKDOWN_CORE_API bool markdown_core_node_directive_attribute_count(const markdown_core_node *node, size_t *count);
/** False when `index` is not less than the count. */
MARKDOWN_CORE_API bool markdown_core_node_directive_attribute_at(
    const markdown_core_node *node,
    size_t index,
    markdown_core_string *key,
    markdown_core_string *value
);
/** The directive's label node, or NULL when the directive declares no label.
 *
 * That is not an empty `[]`, which is a label node whose content is empty. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_directive_label(const markdown_core_node *node);
/** A link reference definition's label as written, and the destination and
 * title it defines. */
MARKDOWN_CORE_API bool markdown_core_node_reference_definition_properties(
    const markdown_core_node *node,
    markdown_core_string *label,
    markdown_core_string *destination,
    markdown_core_string *title
);

/** A reference's label as written and the form it was written in.
 *
 * It has no destination: that belongs to the definition the label resolves
 * to. */
MARKDOWN_CORE_API bool markdown_core_node_reference_properties(
    const markdown_core_node *node,
    markdown_core_string *label,
    markdown_core_reference_form *form
);

/** The link's destination and title.
 *
 * NULL `data` means the author wrote nothing; non-NULL `data` of zero length
 * means they wrote it empty. A title has both cases — `[a](/u)` gives NULL,
 * `[a](/u "")` gives the empty string — while a destination has only the
 * second, because an inline link always writes its `(…)`: `[a]()` and
 * `[a](<>)` both give the empty string and never NULL. The link's caption is
 * its child content, not a field here. */
MARKDOWN_CORE_API bool markdown_core_node_link_properties(
    const markdown_core_node *node,
    markdown_core_string *destination,
    markdown_core_string *title
);
/** The image's source and title, on a link's terms.
 *
 * Its alt text is the node's inline children, parsed like any other content,
 * not a string here. */
MARKDOWN_CORE_API bool markdown_core_node_image_properties(
    const markdown_core_node *node,
    markdown_core_string *source,
    markdown_core_string *title
);
/** The label between `[^` and `]`, delimiters excluded and exactly as written.
 *
 * Matching a reference to its definition case-folds, trims, and collapses
 * inner whitespace, the bytes here are not normalized. Answers for both
 * kinds. */
MARKDOWN_CORE_API bool markdown_core_node_footnote_id(const markdown_core_node *node, markdown_core_string *id);
/** The reference between `[[` and `]]`, exactly the bytes that were written:
 * nothing is trimmed, resolved, or normalized. */
MARKDOWN_CORE_API bool markdown_core_node_cross_link_reference(
    const markdown_core_node *node,
    markdown_core_string *reference
);
/** The reference between `![[` and `]]`, on a cross-link's terms. */
MARKDOWN_CORE_API bool markdown_core_node_embed_reference(
    const markdown_core_node *node,
    markdown_core_string *reference
);

/*
 * Diagnostics
 * ===========
 *
 * What an editor underlines. There is exactly one thing to underline, and
 * that is not a placeholder for a longer list to come.
 *
 * Markdown has no parse errors: every byte sequence is a valid document, and
 * the constructs an author most often gets "wrong" are all defined outcomes
 * of the standard semantics rather than failures. An unclosed fence runs to
 * the end of the document; a link reference with no definition is the text
 * the author typed; a short table row is autocompleted. Reporting any of
 * those would be reporting Markdown itself.
 *
 * A directive's attribute block is the exception, because it is this
 * repository's own grammar rather than Markdown's: `{...}` that does not
 * parse is not a construct with a defined fallback meaning, it is a mistake,
 * and the parse degrades the braces to literal text where the author plainly
 * wanted attributes. That degradation is invisible in the tree — the node
 * simply has no attributes — so without a diagnostic an editor cannot tell
 * "no attributes were written" from "the attributes were rejected".
 */

/** Why a diagnostic was raised. */
typedef enum markdown_core_diagnostic_code {
    /** A directive's `{...}` attribute block did not parse, so its braces
     * stayed literal text.
     *
     * The scope covers the block, brace to brace. */
    MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_ATTRIBUTES = 1
} markdown_core_diagnostic_code;

/** One thing to underline: what, and where.
 *
 * No message. A message is a string an editor shows a human, which means it
 * is localized, and localizing it here would put one English sentence in the
 * library and four copies of the decision to ignore it in the bindings. The
 * code says what happened; the wording belongs to whoever is speaking to the
 * user. */
typedef struct markdown_core_diagnostic {
    markdown_core_diagnostic_code code;
    markdown_core_scope scope;
} markdown_core_diagnostic;

/** Every diagnostic of this document, in source order.
 *
 * Returns the count and points `*diagnostics` at the document's own array,
 * which borrows from the document and ends with it — nothing to free. A clean
 * document reports zero and leaves `*diagnostics` NULL. */
MARKDOWN_CORE_API size_t markdown_core_document_diagnostics(
    const markdown_core_document *document,
    const markdown_core_diagnostic **diagnostics
);

/** Allocates the canonical file-tree dump.
 *
 * Free it with markdown_core_dump_free. A failed dump produces no buffer, so
 * there is nothing to release but the error. */
MARKDOWN_CORE_API bool markdown_core_document_dump(
    const markdown_core_document *document,
    uint8_t **output,
    size_t *length,
    markdown_core_error **error
);
MARKDOWN_CORE_API void markdown_core_dump_free(uint8_t *output);

/*
 * Mutation
 * ========
 *
 * A document is the live head of a CHAIN, and the chain grows one way:
 * `append` adds bytes at the end. There is no whole-text edit — replacing
 * the text describes a DIFFERENT document, and the way to say so is
 * markdown_core_document_new, which opens a new chain with a new series.
 *
 * An append advances the chain and SUPERSEDES its receiver: the successor
 * is the new head, and from that moment the receiver is a handle to its own
 * identity and nothing else. It still answers markdown_core_document_free
 * (at any time, from any thread) and the three scalars that describe WHICH
 * document it was — revision, series, length — and its TREE is gone from
 * it: root answers NULL, dump and diagnostics answer as they would for no
 * document at all. A node pointer taken while it was the head is invalid
 * too, and that one is undefined behaviour rather than a checked error,
 * because a bare node pointer cannot carry the check.
 *
 * The tree goes because the chain keeps ONE of them. What a superseded
 * handle could have shown is the text it described plus everything appended
 * since, which is not the document it names; answering nothing is the only
 * honest thing left, and it is why decoded values — not native handles —
 * are what a consumer keeps. Same chain, same series,
 * revision strictly +1 on the chain's
 * own counter — and appending to a superseded handle is a deterministic
 * error, so history is linear and there is no forking. The result has the
 * same tree, dump, and identity rules as a fresh parse of the concatenated
 * text; the difference is the preserved series. Any byte split is legal —
 * mid-UTF-8, mid-CRLF, mid-line.
 *
 * What changed is asked of the new tree itself: a node's id names the same
 * thing across the append, and its revision says when its projection last
 * changed — (id, revision) is the whole update protocol.
 *
 * The text is the raw bytes exactly as given. UTF-8 is ASSUMED AND NEVER
 * VALIDATED: nothing is scanned, nothing is replaced, and nothing is rejected.
 * NUL is replaced with U+FFFD during parsing because CommonMark requires it of
 * canonical text, and nothing else is.
 *
 * A failed append POISONS THE CHAIN — "the chain is done": every further
 * mutation fails deterministically, only free remains, the caller holds
 * every byte it ever sent, and recovery is a new chain. A rejected argument
 * (NULL data with nonzero length, a stale receiver) fails the call, never
 * the chain.
 */

/**
 * `Document(markdown, options)` — the one entry point; opens a chain.
 *
 * `options == NULL` selects the defaults and they are fixed for the chain's
 * whole life: a mutation takes text and not options, and different options
 * are a new chain with a new series. The returned document owns all nodes
 * and borrowed string views. On failure, NULL is returned and `*error` is
 * set when `error` is non-NULL.
 */
MARKDOWN_CORE_API markdown_core_document *markdown_core_document_new(
    markdown_core_string markdown,
    const markdown_core_parse_options *options,
    markdown_core_error **error
);

/** `document.append(chunk)` — the trailing mutation: add bytes to the end of
 * the chain's head and get back the document all bytes so far describe,
 * which SUPERSEDES the receiver.
 *
 * Any byte split is legal — mid-UTF-8, mid-CRLF, mid-line — and settled
 * content never moves: a node the append did not reach keeps its id, its
 * revision, and its positions. The caller owns the returned document; the
 * receiver keeps free at any time and its own revision, series and length,
 * and stops answering for a tree (see the Mutation section). On a stale or
 * poisoned receiver
 * this is a deterministic error and nothing changes; a failure past the
 * guards poisons the chain (see the Mutation section). */
MARKDOWN_CORE_API markdown_core_document *markdown_core_document_append(
    markdown_core_document *document,
    markdown_core_string chunk,
    markdown_core_error **error
);

/** This document's place in its chain: strictly its predecessor's plus one
 * (append is the only mutation there is); a fresh parse is zero.
 *
 * One counter per chain suffices because history is linear — a superseded
 * handle cannot mutate, so no two successors of one document ever exist to
 * need telling apart. */
MARKDOWN_CORE_API uint64_t markdown_core_document_revision(const markdown_core_document *document);

/** Per-series random salt; nodes from different parses never share identity
 * even when ids collide numerically.
 *
 * A SERIES is one chain's identity space: every document on the chain
 * shares the salt, ids restart at 1 for each new chain, and that is what
 * makes the salt load-bearing rather than decorative. */
MARKDOWN_CORE_API uint64_t markdown_core_document_series(const markdown_core_document *document);
/** The length of the document's text in bytes, not in characters. */
MARKDOWN_CORE_API size_t markdown_core_document_length(const markdown_core_document *document);

/** Identity accessors.
 *
 * `id` is 0 only for a NULL node; `revision` is the document revision at
 * which the node's own fields, child list, or any descendant last changed —
 * two nodes of one series with equal (id, revision) have identical content. A
 * pure positional shift never changes a node's revision. */
MARKDOWN_CORE_API markdown_core_node_id markdown_core_node_get_id(const markdown_core_node *node);
MARKDOWN_CORE_API uint64_t markdown_core_node_get_revision(const markdown_core_node *node);

/** Canonical parent, or NULL for the root. */
MARKDOWN_CORE_API const markdown_core_node *markdown_core_node_get_parent(const markdown_core_node *node);

#ifdef __cplusplus
}
#endif

#endif
