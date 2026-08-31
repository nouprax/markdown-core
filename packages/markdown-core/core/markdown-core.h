#ifndef MARKDOWN_CORE_H
#define MARKDOWN_CORE_H

#include <stdio.h>
#include <stdint.h>
#include "markdown-core-export.h"
#include "markdown-core-version.h"

#ifdef __cplusplus
extern "C" {
#endif

/** # NAME
 *
 * **markdown-core** - CommonMark parsing and AST inspection
 */

/** ## Node Structure
 */

#define MARKDOWN_CORE_NODE_TYPE_PRESENT (0x8000)
#define MARKDOWN_CORE_NODE_TYPE_BLOCK (MARKDOWN_CORE_NODE_TYPE_PRESENT | 0x0000)
#define MARKDOWN_CORE_NODE_TYPE_INLINE (MARKDOWN_CORE_NODE_TYPE_PRESENT | 0x4000)
#define MARKDOWN_CORE_NODE_TYPE_MASK (0xc000)
#define MARKDOWN_CORE_NODE_VALUE_MASK (0x3fff)

typedef enum {
    /* Error status */
    MARKDOWN_CORE_NODE_NONE = 0x0000,

    /* Block */
    MARKDOWN_CORE_NODE_DOCUMENT = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0001,
    MARKDOWN_CORE_NODE_BLOCK_QUOTE = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0002,
    MARKDOWN_CORE_NODE_LIST = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0003,
    MARKDOWN_CORE_NODE_LIST_ITEM = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0004,
    MARKDOWN_CORE_NODE_CODE_BLOCK = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0005,
    MARKDOWN_CORE_NODE_HTML_BLOCK = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0006,
    MARKDOWN_CORE_NODE_PARAGRAPH = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0007,
    MARKDOWN_CORE_NODE_HEADING = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0008,
    MARKDOWN_CORE_NODE_THEMATIC_BREAK = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0009,
    MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000a,
    /* 0x000b through 0x000f are taken by the extension block types in
     * extensions/markdown-core-extensions.h, numbered when 0x000a was the last
     * core block. A new core block therefore starts at 0x0010 rather than at
     * the next free value. The internal type value is NOT the wire ordinal --
     * markdown_core_node_kind numbers the facade's kinds -- so the gap costs
     * nothing, but the natural assumption is that the next value is free and
     * it is not. */
    MARKDOWN_CORE_NODE_REFERENCE_DEFINITION = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0010,

    /* Inline */
    MARKDOWN_CORE_NODE_TEXT = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0001,
    MARKDOWN_CORE_NODE_SOFT_BREAK = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0002,
    MARKDOWN_CORE_NODE_LINE_BREAK = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0003,
    MARKDOWN_CORE_NODE_CODE = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0004,
    MARKDOWN_CORE_NODE_HTML = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0005,
    MARKDOWN_CORE_NODE_EMPHASIS = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0006,
    MARKDOWN_CORE_NODE_STRONG = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0007,
    MARKDOWN_CORE_NODE_LINK = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0008,
    MARKDOWN_CORE_NODE_IMAGE = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0009,
    MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000a,
    /* 0x000b through 0x000e are the extension INLINE types; 0x000f is the next
     * free value in that class and these two take it. The block class had no
     * such value -- its extensions run to 0x000f -- which is why
     * MARKDOWN_CORE_NODE_REFERENCE_DEFINITION starts at 0x0010 and these do
     * not. The two classes are numbered independently; the class bits are what
     * separate them. */
    MARKDOWN_CORE_NODE_LINK_REFERENCE = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000f,
    MARKDOWN_CORE_NODE_IMAGE_REFERENCE = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0010,
} markdown_core_node_type;

/* The form a reference was WRITTEN in, which is not derivable from anything
 * else on the node: `[t][l]`, `[l][]` and `[l]` all resolve the same way and
 * are three different spellings. A footnote reference deliberately has no form
 * -- there is one footnote call syntax, so a field would have one value (Q3). */
#ifndef MARKDOWN_CORE_REFERENCE_FORM_TYPEDEF
#define MARKDOWN_CORE_REFERENCE_FORM_TYPEDEF
typedef enum markdown_core_reference_form {
    MARKDOWN_CORE_REFERENCE_FULL = 1,
    MARKDOWN_CORE_REFERENCE_COLLAPSED = 2,
    MARKDOWN_CORE_REFERENCE_SHORTCUT = 3
} markdown_core_reference_form;
#endif

typedef enum { MARKDOWN_CORE_NO_LIST, MARKDOWN_CORE_BULLET_LIST, MARKDOWN_CORE_ORDERED_LIST } markdown_core_list_type;

typedef enum { MARKDOWN_CORE_NO_DELIM, MARKDOWN_CORE_PERIOD_DELIM, MARKDOWN_CORE_PAREN_DELIM } markdown_core_delim_type;

#ifndef MARKDOWN_CORE_NODE_TYPEDEF
#define MARKDOWN_CORE_NODE_TYPEDEF
typedef struct markdown_core_node markdown_core_node;
#endif
typedef struct markdown_core_parser markdown_core_parser;
typedef struct markdown_core_iter markdown_core_iter;
typedef struct markdown_core_syntax_extension markdown_core_syntax_extension;

/**
 * ## Custom memory allocator support
 */

/** Defines the memory allocation functions to be used by Markdown Core
 * when parsing and allocating a document tree
 */
typedef struct markdown_core_mem {
    void *(*calloc)(size_t, size_t);
    void *(*realloc)(void *, size_t);
    void (*free)(void *);
} markdown_core_mem;

/** The default memory allocator; uses the system's calloc,
 * realloc and free.
 */
MARKDOWN_CORE_EXPORT
markdown_core_mem *markdown_core_get_default_mem_allocator(void);

/** Callback for freeing user data with a 'markdown_core_mem' context.
 */
typedef void (*markdown_core_free_func)(markdown_core_mem *mem, void *data);

/*
 * ## Basic data structures
 *
 * To keep dependencies to the strict minimum, libmarkdown_core implements
 * its own versions of "classic" data structures.
 */

/**
 * ### Linked list
 */

/** A generic singly linked list.
 */
typedef struct _markdown_core_llist {
    struct _markdown_core_llist *next;
    void *data;
} markdown_core_llist;

/** Free the list starting with 'head', calling 'free_func' with the
 *  data pointer of each of its elements
 */
MARKDOWN_CORE_EXPORT
void markdown_core_llist_free_full(
    markdown_core_mem *mem,
    markdown_core_llist *head,
    markdown_core_free_func free_func
);

/** Free the list starting with 'head'
 */
MARKDOWN_CORE_EXPORT
void markdown_core_llist_free(markdown_core_mem *mem, markdown_core_llist *head);

/**
 * ## Creating and Destroying Nodes
 */

/** Creates a new node of type 'type'.  Note that the node may have
 * other required properties, which it is the caller's responsibility
 * to assign.
 */
MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_new(markdown_core_node_type type);

/** Same as `markdown_core_node_new`, but explicitly listing the memory
 * allocator used to allocate the node.  Note:  be sure to use the same
 * allocator for every node in a tree, or bad things can happen.
 */
MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_new_with_mem(
    markdown_core_node_type type,
    markdown_core_mem *mem
);

MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_new_with_ext(
    markdown_core_node_type type,
    const markdown_core_syntax_extension *extension
);

MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_new_with_mem_and_ext(
    markdown_core_node_type type,
    markdown_core_mem *mem,
    const markdown_core_syntax_extension *extension
);

/** Frees the memory allocated for a node and any children.
 */
MARKDOWN_CORE_EXPORT void markdown_core_node_free(markdown_core_node *node);

/**
 * ## Tree Traversal
 */

/** Returns the next node in the sequence after 'node', or NULL if
 * there is none.
 */
MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_next(markdown_core_node *node);

/** Returns the previous node in the sequence after 'node', or NULL if
 * there is none.
 */
MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_previous(markdown_core_node *node);

/** Returns the parent of 'node', or NULL if there is none.
 */
MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_parent(markdown_core_node *node);

/** Returns the first child of 'node', or NULL if 'node' has no children.
 */
MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_first_child(markdown_core_node *node);

/** Returns the last child of 'node', or NULL if 'node' has no children.
 */
MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_last_child(markdown_core_node *node);

/**
 * ## Iterator
 *
 * An iterator will walk through a tree of nodes, starting from a root
 * node, returning one node at a time, together with information about
 * whether the node is being entered or exited.  The iterator will
 * first descend to a child node, if there is one.  When there is no
 * child, the iterator will go to the next sibling.  When there is no
 * next sibling, the iterator will return to the parent (but with
 * a 'markdown_core_event_type' of `MARKDOWN_CORE_EVENT_EXIT`).  The iterator will
 * return `MARKDOWN_CORE_EVENT_DONE` when it reaches the root node again.
 * An iterator might be used to inspect or transform an AST in some systematic
 * way, for example, turning all level-3 headings into regular paragraphs.
 *
 *     void
 *     usage_example(markdown_core_node *root) {
 *         markdown_core_event_type ev_type;
 *         markdown_core_iter *iter = markdown_core_iter_new(root);
 *
 *         while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
 *             markdown_core_node *cur = markdown_core_iter_get_node(iter);
 *             // Do something with `cur` and `ev_type`
 *         }
 *
 *         markdown_core_iter_free(iter);
 *     }
 *
 * The event contract is total: every node in the subtree yields exactly one
 * `ENTER` and exactly one `EXIT`, whatever its type -- a childless node's
 * `EXIT` follows its `ENTER` directly.
 *
 * Nodes must only be modified after an `EXIT` event; the only node that may
 * be freed is the one whose `EXIT` is current.  (See `markdown_core_iter_new`
 * below for the full contract and its history.)
 */

typedef enum {
    MARKDOWN_CORE_EVENT_NONE,
    MARKDOWN_CORE_EVENT_DONE,
    MARKDOWN_CORE_EVENT_ENTER,
    MARKDOWN_CORE_EVENT_EXIT
} markdown_core_event_type;

/** Creates a new iterator starting at 'root'.  The current node and event
 * type are undefined until 'markdown_core_iter_next' is called for the first time.
 * The memory allocated for the iterator should be released using
 * 'markdown_core_iter_free' when it is no longer needed.
 *
 * THE EVENT CONTRACT IS TOTAL: every node in the subtree yields exactly one
 * `ENTER` and exactly one `EXIT`, in that order, with its descendants' events
 * between them.  Until Step 5 an internal `S_is_leaf` list of eight node types
 * suppressed the `EXIT` of a node that "cannot have children" -- which was a
 * list, not a property, so a `FOOTNOTE_REFERENCE` with no children got an
 * `EXIT` and a `TEXT` with no children did not, and every walk had to know
 * which.
 *
 * THE MUTATION RULE NAMES A NODE, NOT AN EVENT: while walking, the only node
 * that may be freed is the one whose `EXIT` is current.  That is exactly the
 * moment at which the iterator's lookahead names something outside the node's
 * own subtree, and it is the only such moment.  Freeing at `ENTER` used to be
 * legal for the eight suppressed types and is not legal for anything now; use
 * `markdown_core_iter_reset(iter, node, MARKDOWN_CORE_EVENT_EXIT)` to bring a
 * node back under the rule after mutating around it.
 */
MARKDOWN_CORE_EXPORT
markdown_core_iter *markdown_core_iter_new(markdown_core_node *root);

/** Frees the memory allocated for an iterator.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_iter_free(markdown_core_iter *iter);

/** Advances to the next node and returns the event type (`MARKDOWN_CORE_EVENT_ENTER`,
 * `MARKDOWN_CORE_EVENT_EXIT` or `MARKDOWN_CORE_EVENT_DONE`).
 */
MARKDOWN_CORE_EXPORT
markdown_core_event_type markdown_core_iter_next(markdown_core_iter *iter);

/** Returns the current node.
 */
MARKDOWN_CORE_EXPORT
markdown_core_node *markdown_core_iter_get_node(markdown_core_iter *iter);

/** Resets the iterator so that the current node is 'current' and
 * the event type is 'event_type'.  The new current node must be a
 * descendant of the root node or the root node itself.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_iter_reset(
    markdown_core_iter *iter,
    markdown_core_node *current,
    markdown_core_event_type event_type
);

/**
 * ## Accessors
 */

/** Returns the type of 'node', or `MARKDOWN_CORE_NODE_NONE` on error.
 */
MARKDOWN_CORE_EXPORT markdown_core_node_type markdown_core_node_get_type(markdown_core_node *node);

/** Like 'markdown_core_node_get_type', but returns a string representation
    of the type, or `"<unknown>"`.
 */
MARKDOWN_CORE_EXPORT
const char *markdown_core_node_get_type_string(markdown_core_node *node);

/** Returns the string contents of 'node', or an empty
    string if none is set.  Returns NULL if called on a
    node that does not have string content.

    MATERIALIZES a NUL-terminated private copy on first call, WRITING the
    node (#153): an inline list can be shared by several derived documents,
    so this call needs the caller to order access across every document
    that can reach the node -- the cross-thread freedom the facade contract
    grants applies to the facade's own accessors, which read views and
    mutate nothing.
 */
MARKDOWN_CORE_EXPORT const char *markdown_core_node_get_literal(markdown_core_node *node);

/** Sets the string contents of 'node'.  Returns 1 on success,
 * 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_literal(markdown_core_node *node, const char *content);

/** Returns the heading level of 'node', or 0 if 'node' is not a heading.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_get_heading_level(markdown_core_node *node);

/** Sets the heading level of 'node', returning 1 on success and 0 on error.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_heading_level(markdown_core_node *node, int level);

/** Returns the list type of 'node', or `MARKDOWN_CORE_NO_LIST` if 'node'
 * is not a list.
 */
MARKDOWN_CORE_EXPORT markdown_core_list_type markdown_core_node_get_list_type(markdown_core_node *node);

/** Sets the list type of 'node', returning 1 on success and 0 on error.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_list_type(markdown_core_node *node, markdown_core_list_type type);

/** Returns the list delimiter type of 'node', or `MARKDOWN_CORE_NO_DELIM` if 'node'
 * is not a list.
 */
MARKDOWN_CORE_EXPORT markdown_core_delim_type markdown_core_node_get_list_delim(markdown_core_node *node);

/** Sets the list delimiter type of 'node', returning 1 on success and 0
 * on error.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_list_delim(markdown_core_node *node, markdown_core_delim_type delim);

/** Returns starting number of 'node', if it is an ordered list, otherwise 0.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_get_list_start(markdown_core_node *node);

/** Sets starting number of 'node', if it is an ordered list. Returns 1
 * on success, 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_list_start(markdown_core_node *node, int start);

/** Returns 1 if 'node' is a tight list, 0 otherwise.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_get_list_tight(markdown_core_node *node);

/** Sets the "tightness" of a list.  Returns 1 on success, 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_list_tight(markdown_core_node *node, int tight);

/** Returns the info string from a fenced code block.
 */
MARKDOWN_CORE_EXPORT const char *markdown_core_node_get_fence_info(markdown_core_node *node);

/** Sets the info string in a fenced code block, returning 1 on
 * success and 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_fence_info(markdown_core_node *node, const char *info);

/** Returns 1 if a fenced code block has a closing fence, 0 otherwise.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_get_fence_closed(markdown_core_node *node);

/** Returns the URL of a link or image 'node', or an empty string
    if no URL is set.  Returns NULL if called on a node that is
    not a link or image.
 */
MARKDOWN_CORE_EXPORT const char *markdown_core_node_get_url(markdown_core_node *node);

/** Sets the URL of a link or image 'node'. Returns 1 on success,
 * 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_url(markdown_core_node *node, const char *url);

/** Returns the title of a link or image 'node', or an empty
    string if no title is set.  Returns NULL if called on a node
    that is not a link or image.
 */
MARKDOWN_CORE_EXPORT const char *markdown_core_node_get_title(markdown_core_node *node);

/** Sets the title of a link or image 'node'. Returns 1 on success,
 * 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_title(markdown_core_node *node, const char *title);

/** Returns the line on which 'node' begins.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_get_start_line(markdown_core_node *node);

/** Returns the column at which 'node' begins.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_get_start_column(markdown_core_node *node);

/** Returns the line on which 'node' ends.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_get_end_line(markdown_core_node *node);

/** Returns the column at which 'node' ends.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_get_end_column(markdown_core_node *node);

/**
 * ## Tree Manipulation
 */

/** Unlinks a 'node', removing it from the tree, but not freeing its
 * memory.  (Use 'markdown_core_node_free' for that.)
 */
MARKDOWN_CORE_EXPORT void markdown_core_node_unlink(markdown_core_node *node);

/** Inserts 'sibling' before 'node'.  Returns 1 on success, 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_insert_before(markdown_core_node *node, markdown_core_node *sibling);

/** Inserts 'sibling' after 'node'. Returns 1 on success, 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_insert_after(markdown_core_node *node, markdown_core_node *sibling);

/** Replaces 'oldnode' with 'newnode' and unlinks 'oldnode' (but does
 * not free its memory).
 * Returns 1 on success, 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_replace(markdown_core_node *oldnode, markdown_core_node *newnode);

/** Adds 'child' to the beginning of the children of 'node'.
 * Returns 1 on success, 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_prepend_child(markdown_core_node *node, markdown_core_node *child);

/** Adds 'child' to the end of the children of 'node'.
 * Returns 1 on success, 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_append_child(markdown_core_node *node, markdown_core_node *child);

/** Consolidates adjacent text nodes.
 */
/** Merges adjacent text nodes.  Returns 0 when merged text could not be
 *  materialized because an allocation failed; the tree stays valid. */
MARKDOWN_CORE_EXPORT int markdown_core_consolidate_text_nodes(markdown_core_node *root);

/**
 * ## Parsing
 *
 * Simple interface:
 *
 *     markdown_core_node *document = markdown_core_parse_document("Hello *world*", 13,
 *                                                 MARKDOWN_CORE_OPT_DEFAULT);
 *
 * Streaming interface:
 *
 *     markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT);
 *     FILE *fp = fopen("myfile.md", "rb");
 *     while ((bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
 *     	   markdown_core_parser_feed(parser, buffer, bytes);
 *     	   if (bytes < sizeof(buffer)) {
 *     	       break;
 *     	   }
 *     }
 *     document = markdown_core_parser_finish(parser);
 *     markdown_core_parser_free(parser);
 */

/** Creates a new parser object.
 */
MARKDOWN_CORE_EXPORT
markdown_core_parser *markdown_core_parser_new(int options);

/** Creates a new parser object with the given memory allocator
 */
MARKDOWN_CORE_EXPORT
markdown_core_parser *markdown_core_parser_new_with_mem(int options, markdown_core_mem *mem);

/** Frees memory allocated for a parser object.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_parser_free(markdown_core_parser *parser);

/** Feeds a string of length 'len' to 'parser'.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_parser_feed(markdown_core_parser *parser, const char *buffer, size_t len);

/** Finish parsing and return a pointer to a tree of nodes.
 */
MARKDOWN_CORE_EXPORT
markdown_core_node *markdown_core_parser_finish(markdown_core_parser *parser);

/** Parse a CommonMark document in 'buffer' of length 'len'.
 * Returns a pointer to a tree of nodes.  The memory allocated for
 * the node tree should be released using 'markdown_core_node_free'
 * when it is no longer needed.
 */
MARKDOWN_CORE_EXPORT
markdown_core_node *markdown_core_parse_document(const char *buffer, size_t len, int options);

/**
 * ## Options
 */

/** Default options.
 */
#define MARKDOWN_CORE_OPT_DEFAULT 0

/* Bits (1 << 1), (1 << 8) and (1 << 12) were MARKDOWN_CORE_OPT_SOURCEPOS,
 * MARKDOWN_CORE_OPT_NORMALIZE and MARKDOWN_CORE_OPT_LIBERAL_HTML_TAG.
 * The first two changed no behavior - position tracking is unconditional
 * in the 3.0 scope model, and NORMALIZE was inherited as a no-op. The
 * liberal-tag scan was unreachable from the frozen 3.0 facade and its
 * scanner rescanned to end of line for every '<' (quadratic); it is
 * deleted rather than memoized. All three bits stay unassigned.

 * ### Options affecting parsing
 */

/** Validate UTF-8 in the input before parsing, replacing illegal
 * sequences with the replacement character U+FFFD.
 */
#define MARKDOWN_CORE_OPT_VALIDATE_UTF8 (1 << 9)

/** Convert straight quotes to curly, --- to em dashes, -- to en dashes.
 */
#define MARKDOWN_CORE_OPT_SMART (1 << 10)

/** Strip HTML comment nodes from the parsed AST.
 */
#define MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS (1 << 25)

/** Parse footnotes.
 */
#define MARKDOWN_CORE_OPT_FOOTNOTES (1 << 13)

/** Only parse strikethroughs if surrounded by exactly 2 tildes.
 * Gives some compatibility with redcarpet.
 */
#define MARKDOWN_CORE_OPT_STRIKETHROUGH_DOUBLE_TILDE (1 << 14)

/**
 * ## Version information
 */

/** The library version as integer for runtime checks. Also available as
 * macro MARKDOWN_CORE_VERSION for compile time checks.
 *
 * * Bits 16-23 contain the major version.
 * * Bits 8-15 contain the minor version.
 * * Bits 0-7 contain the patchlevel.
 *
 * In hexadecimal format, the number 0x010203 represents version 1.2.3.
 */
MARKDOWN_CORE_EXPORT
int markdown_core_version(void);

/** The library version string for runtime checks. Also available as
 * macro MARKDOWN_CORE_VERSION_STRING for compile time checks.
 */
MARKDOWN_CORE_EXPORT
const char *markdown_core_version_string(void);

/** # AUTHORS
 *
 * John MacFarlane, Vicent Marti,  Kārlis Gaņģis, Nick Wellnhofer.
 */

typedef int32_t bufsize_t;

#ifdef __cplusplus
}
#endif

#endif
