#ifndef MARKDOWN_CORE_NODE_H
#define MARKDOWN_CORE_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>

#include "markdown-core.h"
#include "markdown-core-extension-api.h"
#include "buffer.h"
#include "chunk.h"

typedef struct {
    markdown_core_list_type list_type;
    int marker_offset;
    int padding;
    int start;
    markdown_core_delim_type delimiter;
    unsigned char bullet_char;
    bool tight;
    bool checked; // For task list extension
} markdown_core_list;

typedef struct {
    /* OPTIONAL, and the type says so (requirement 14). A fence with nothing
     * after it wrote no info string; `` ``` `` and an indented block are both
     * absent, and `js` is present. */
    markdown_core_optional_chunk info;
    markdown_core_chunk literal;
    uint8_t fence_length;
    uint8_t fence_offset;
    unsigned char fence_char;
    int8_t fenced;
    int8_t fence_closed;
} markdown_core_code;

typedef struct {
    int level;
    bool setext;
} markdown_core_heading;

/* THE RESOURCE a Link or Image reads its destination and title from (M2).
 *
 * It is COUNTED and SHARED. A link reference definition's resource is built
 * once, when the block phase reads the definition into the parser's map, and
 * every occurrence that resolves to that definition -- full, collapsed or
 * shortcut -- holds the same one; a direct link, a direct image and an
 * autolink each own one of their own. Sharing is what deletes D9 for good: a
 * definition with a long destination referenced many times costs one copy of
 * the destination however many times it is named, so nothing has to be
 * charged and no budget can make WHETHER A REFERENCE RESOLVES depend on how
 * many resolved before it. `holders` counts the map record and every node
 * reading through the resource; the last one out frees it, which is how the
 * tree outlives the parser that built the map.
 *
 * Identity is the pointer: `markdown_core_node_resource` hands it out, and a
 * consumer that materializes a destination once per distinct resource keys
 * on it. Nothing else about the pointer is stated. */
struct markdown_core_resource {
    /* REQUIRED (Q26). `[a]()`, `[a](<>)` and `[a]: <>` wrote a destination
     * and it was empty; there is no link whose author wrote no destination at
     * all, because a reference resolves to its definition's. */
    markdown_core_chunk url;
    /* OPTIONAL (requirement 14): `[a](/u)` wrote no title and `[a](/u "")`
     * wrote an empty one. */
    markdown_core_optional_chunk title;
    size_t holders;
};
#ifndef MARKDOWN_CORE_RESOURCE_TYPEDEF
#define MARKDOWN_CORE_RESOURCE_TYPEDEF
typedef struct markdown_core_resource markdown_core_resource;
#endif

typedef struct {
    /* NEVER NULL on a node the parser finished: the resource is attached in
     * the same step that makes the node a link, and an allocation that could
     * not attach one frees the node. */
    markdown_core_resource *resource;
} markdown_core_link;

/* THE ASSOCIATION a footnote definition or reference carries. TWO values, and
 * neither derives the other in either direction.
 *
 * `label` is the bytes between the delimiters exactly as written: escapes and
 * character references unresolved, whitespace uncollapsed, case unfolded.
 * `identifier` is the match key: full Unicode case fold, trim, collapse
 * internal whitespace -- and for a footnote it KEEPS its leading `^`, so that a
 * link definition and a footnote definition of the same name cannot collide in
 * a consumer's single map. That caret is a correction to mdast, which separates
 * the two namespaces only by node type and so cannot survive being flattened
 * onto a wire.
 *
 * NORMATIVE: `identifier` is compared with memcmp over its bytes. It is never
 * case mapped, never NFC/NFD normalized, never re-encoded, and never used as a
 * key in a language map whose `==` has an opinion about Unicode -- Swift's
 * `String ==` is canonical equivalence, which would collapse the NFC and NFD
 * spellings of `[cafe\u0301]` that this parser deliberately keeps apart.
 *
 * NEITHER derives the other. `raw -> key` needs the case-fold table; `key ->
 * raw` is impossible, because the fold is many-to-one and `[ss]` and
 * `[\u00df]` are two labels with one key. The producer computes the key at zero
 * marginal cost: it already builds one per occurrence for its own map. */
typedef struct {
    markdown_core_chunk label;
    markdown_core_chunk identifier;
} markdown_core_association;

/* A link reference definition is not a node (M2). The block phase reads it off
 * the front of the paragraph that held it into the parser's map, which owns
 * its resource once, and every reference that resolves to it is the `Link` or
 * `Image` it names, sharing that resource. This is the inherited grammar's
 * model: a definition exists to be referred to, an unreferenced one produces
 * nothing, and the first definition of a label in source order wins. A footnote
 * definition stays a node, because its body is flow content. */

enum markdown_core_node__internal_flags {
    MARKDOWN_CORE_NODE__OPEN = (1 << 0),
    MARKDOWN_CORE_NODE__LAST_LINE_BLANK = (1 << 1),
    MARKDOWN_CORE_NODE__LAST_LINE_CHECKED = (1 << 2),
    MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK = (1 << 3),
    // An HTML block whose own end condition matched on the line being
    // processed. `finalize` reads it to end the block on that line rather
    // than on the line before, and to know that a `-->` line really closed a
    // type-2 block rather than the input or a container running out.
    MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION = (1 << 4),

    // The first bit an extension may claim. Extension flags are compile-time
    // constants owned by the extension that uses them; there is no runtime
    // registration and no allocator to run out of bits.
    MARKDOWN_CORE_NODE__EXTENSION_FIRST = (1 << 5),
};

typedef uint16_t markdown_core_node_internal_flags;

struct markdown_core_node {
    markdown_core_strbuf content;

    struct markdown_core_node *next;
    struct markdown_core_node *prev;
    struct markdown_core_node *parent;
    /* Intrusive list of content children. */
    struct markdown_core_node *first_child;
    struct markdown_core_node *last_child;

    void *user_data;
    markdown_core_free_func user_data_free_func;

    int start_line;
    int start_column;
    int end_line;
    int end_column;
    int internal_offset;
    /* This block's run in parser->line_marks -- the content-to-source map.
     * `content_mark_count == 0` means the block took no lines, which is the
     * state of every node that is not a block that accumulates content. */
    int content_mark;
    int content_mark_count;
    uint16_t type;
    markdown_core_node_internal_flags flags;

    const markdown_core_extension *extension;
    /* Per-node data an extension owns, allocated by its opaque_alloc_func and
     * freed by its opaque_free_func. It lives beside the type-specific arm,
     * never in it: it belongs to the node and its extension, not to the type,
     * so a type change reinitializes the arm and leaves it in place. */
    void *opaque;

    union {
        markdown_core_chunk literal;
        markdown_core_list list;
        markdown_core_code code;
        markdown_core_heading heading;
        markdown_core_link link;
        markdown_core_association association;
        int html_block_type;
        int cell_index; // For keeping track of TABLE_CELL table alignments
    } as;
};

static MARKDOWN_CORE_INLINE markdown_core_mem *markdown_core_node_mem(markdown_core_node *node) {
    return node->content.mem;
}

/* Takes ownership of `url` and `title` and answers a resource with one holder,
 * or NULL having taken nothing -- the caller still owns both chunks and frees
 * them. */
markdown_core_resource *markdown_core_resource_new(markdown_core_mem *mem, markdown_core_chunk url,
                                                   markdown_core_optional_chunk title);
void markdown_core_resource_retain(markdown_core_resource *resource);
/* Drops one holder and frees the resource with the last. NULL is a no-op. */
void markdown_core_resource_release(markdown_core_mem *mem, markdown_core_resource *resource);
MARKDOWN_CORE_EXPORT int markdown_core_node_check(markdown_core_node *node, FILE *out);

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_TYPE_BLOCK_P(markdown_core_node_type node_type) {
    return (node_type & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_BLOCK;
}

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_BLOCK_P(markdown_core_node *node) {
    return node != NULL && MARKDOWN_CORE_NODE_TYPE_BLOCK_P((markdown_core_node_type)node->type);
}

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_TYPE_INLINE_P(markdown_core_node_type node_type) {
    return (node_type & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_INLINE;
}

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_INLINE_P(markdown_core_node *node) {
    return node != NULL && MARKDOWN_CORE_NODE_TYPE_INLINE_P((markdown_core_node_type)node->type);
}

MARKDOWN_CORE_EXPORT bool markdown_core_node_can_contain_type(markdown_core_node *node,
                                                              markdown_core_node_type child_type);

#ifdef __cplusplus
}
#endif

#endif
