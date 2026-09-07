#ifndef MARKDOWN_CORE_H
#define MARKDOWN_CORE_H

#include <stddef.h>
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
    MARKDOWN_CORE_NODE_CALLOUT = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0002,
    MARKDOWN_CORE_NODE_LIST = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0003,
    MARKDOWN_CORE_NODE_LIST_ITEM = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0004,
    MARKDOWN_CORE_NODE_CODE_BLOCK = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0005,
    MARKDOWN_CORE_NODE_HTML_BLOCK = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0006,
    MARKDOWN_CORE_NODE_PARAGRAPH = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0007,
    MARKDOWN_CORE_NODE_HEADING = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0008,
    MARKDOWN_CORE_NODE_THEMATIC_BREAK = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0009,
    /* A footnote definition (M4): a block container while it is parsed, and
     * a document-owned `Footnote` value once the document finalizes, when
     * every one leaves the tree for the root's own footnote chain. */
    MARKDOWN_CORE_NODE_FOOTNOTE = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x000a,
    /* 0x000b through 0x000f are taken by the extension block types in
     * extensions/markdown-core-extensions.h, numbered when 0x000a was the last
     * core block. A new core block therefore starts at 0x0010 rather than at
     * the next free value. The internal type value is NOT the wire ordinal --
     * markdown_core_node_kind numbers the facade's kinds -- so the gap costs
     * nothing, but the natural assumption is that the next value is free and
     * it is not. (A link reference definition took 0x0010 until M2 resolved
     * every reference into the `Link` or `Image` it names and the definition
     * went back into the parser's map, where the inherited grammar keeps it.)
     *
     * A block comment: an HTML block that opened with `<!--` and whose end
     * line held only whitespace after the first `-->`. One public kind,
     * `MARKDOWN_CORE_KIND_COMMENT`, stands for this and for the inline type
     * below; the two internal types record which content the node sits in,
     * which is what containment checks and the inline parser ask. */
    MARKDOWN_CORE_NODE_COMMENT_BLOCK = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0010,

    /* A specimen definition: a document-owned citation value (P9b). */
    MARKDOWN_CORE_NODE_SPECIMEN = MARKDOWN_CORE_NODE_TYPE_BLOCK | 0x0011,

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
    /* A citation cluster (M4): the inline `Cite` kind, whose items are a
     * chain of CITATION nodes it owns beside its children, which it never
     * has. */
    MARKDOWN_CORE_NODE_CITE = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000a,
    /* 0x000b through 0x000e are the extension INLINE types; the core values
     * continue at 0x000f. The block class had no such value -- its
     * extensions run to 0x000f -- which is why its next core type starts at
     * 0x0010 and this one does not. The two classes are numbered
     * independently; the class bits are what separate them.
     *
     * An inline HTML comment token: `<!-- ... -->`, `<!-->` or `<!--->`. */
    MARKDOWN_CORE_NODE_COMMENT = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000f,
    /* One item of a `Cite` (M4): a scoped value, never a child of anything,
     * owning a prefix chain and a suffix chain of inline nodes beside its
     * referent. Inline-classed because it lives in inline content. */
    MARKDOWN_CORE_NODE_CITATION = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0010,
    MARKDOWN_CORE_NODE_CROSS_LINK = MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0011,
} markdown_core_node_type;

typedef enum { MARKDOWN_CORE_NO_LIST, MARKDOWN_CORE_BULLET_LIST, MARKDOWN_CORE_ORDERED_LIST } markdown_core_list_type;

typedef enum { MARKDOWN_CORE_NO_DELIM, MARKDOWN_CORE_PERIOD_DELIM, MARKDOWN_CORE_PAREN_DELIM } markdown_core_delim_type;

#ifndef MARKDOWN_CORE_NODE_TYPEDEF
#define MARKDOWN_CORE_NODE_TYPEDEF
typedef struct markdown_core_node markdown_core_node;
#endif
typedef struct markdown_core_parser markdown_core_parser;
typedef struct markdown_core_iter markdown_core_iter;
typedef struct markdown_core_extension markdown_core_extension;

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
typedef void (*markdown_core_free_func)(markdown_core_mem *mem, void *user_data);

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
void markdown_core_llist_free_full(markdown_core_mem *mem, markdown_core_llist *head,
                                   markdown_core_free_func free_func);

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
MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_new_with_mem(markdown_core_node_type type,
                                                                         markdown_core_mem *mem);

MARKDOWN_CORE_EXPORT markdown_core_node *markdown_core_node_new_with_ext(markdown_core_node_type type,
                                                                         const markdown_core_extension *extension);

MARKDOWN_CORE_EXPORT markdown_core_node *
markdown_core_node_new_with_mem_and_ext(markdown_core_node_type type, markdown_core_mem *mem,
                                        const markdown_core_extension *extension);

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

/** Returns the first content child of 'node', or NULL if 'node' has no
 * children. Node-valued typed fields are not part of this sibling list.
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
 * whether the node is being entered or exited. The iterator first descends to
 * the first child, then advances through next siblings, and returns to the
 * parent when no sibling remains. Node-valued fields are independent roots
 * and are never discovered by this traversal. Returning to the parent uses
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
 * Iterators will never return `EXIT` events for leaf nodes, which are nodes
 * of type:
 *
 * * MARKDOWN_CORE_NODE_HTML_BLOCK
 * * MARKDOWN_CORE_NODE_THEMATIC_BREAK
 * * MARKDOWN_CORE_NODE_CODE_BLOCK
 * * MARKDOWN_CORE_NODE_TEXT
 * * MARKDOWN_CORE_NODE_SOFT_BREAK
 * * MARKDOWN_CORE_NODE_LINE_BREAK
 * * MARKDOWN_CORE_NODE_CODE
 * * MARKDOWN_CORE_NODE_HTML
 * * MARKDOWN_CORE_NODE_COMMENT
 * * MARKDOWN_CORE_NODE_COMMENT_BLOCK
 *
 * Nodes must only be modified after an `EXIT` event, or an `ENTER` event for
 * leaf nodes.
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

/** Returns the current event type.
 */
MARKDOWN_CORE_EXPORT
markdown_core_event_type markdown_core_iter_get_event_type(markdown_core_iter *iter);

/** Returns the root node.
 */
MARKDOWN_CORE_EXPORT
markdown_core_node *markdown_core_iter_get_root(markdown_core_iter *iter);

/** Resets the iterator so that the current node is 'current' and
 * the event type is 'event_type'.  The new current node must be a
 * descendant of the root node or the root node itself.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_iter_reset(markdown_core_iter *iter, markdown_core_node *current,
                              markdown_core_event_type event_type);

/**
 * ## Accessors
 */

/** Returns the user data of 'node'.
 */
MARKDOWN_CORE_EXPORT void *markdown_core_node_get_user_data(markdown_core_node *node);

/** Sets arbitrary user data for 'node'.  Returns 1 on success,
 * 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_user_data(markdown_core_node *node, void *user_data);

/** Set free function for user data */
MARKDOWN_CORE_EXPORT
int markdown_core_node_set_user_data_free_func(markdown_core_node *node, markdown_core_free_func free_func);

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

/** Returns the source-order item index of 'node'. */
MARKDOWN_CORE_EXPORT int markdown_core_node_get_list_item_index(markdown_core_node *node);

/** Sets item index of 'node'. Returns 1 on success, 0 on failure.
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_list_item_index(markdown_core_node *node, int idx);

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

/** Sets code blocks fencing details
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_set_fenced(markdown_core_node *node, int fenced, int length, int offset,
                                                       char character);

/** Returns code blocks fencing details
 */
MARKDOWN_CORE_EXPORT int markdown_core_node_get_fenced(markdown_core_node *node, int *length, int *offset,
                                                       char *character);

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
 */

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

/** Track multiline inline source positions while parsing. */
#define MARKDOWN_CORE_OPT_SOURCEPOS (1 << 1)

/**
 * ### Options affecting parsing
 */

/** Legacy option (no effect).
 */
#define MARKDOWN_CORE_OPT_NORMALIZE (1 << 8)

/** Be liberal in interpreting inline HTML tags.
 */
#define MARKDOWN_CORE_OPT_LIBERAL_HTML_TAG (1 << 12)

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
