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
    markdown_core_chunk info;
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

typedef struct {
    markdown_core_chunk url;
    markdown_core_chunk title;
} markdown_core_link;

/* A link reference definition is a block node at the byte where its opening
 * bracket was written, in the container it was written in, and it stays there.
 * There are two kinds of reference definition and they differ in exactly one
 * thing: what the definition's body is. A link reference definition's body is a
 * resource -- a destination and an optional title -- so it is a leaf. A
 * footnote definition's body is flow content, so it is a container with block
 * children. Everything else is one rule for both: each carries the label
 * exactly as the source spells it, delimiters excluded, character escapes and
 * character references unresolved, whitespace uncollapsed, case unfolded; each
 * exists whether or not anything refers to it; each keeps the outcome of
 * matching off the node, in the reference map; and neither is ever moved,
 * reordered, renumbered, dropped, or given a back-reference by anything that
 * runs after the parse. This is mdast's model, adopted deliberately in
 * preference to cmark-gfm's, which erases a link reference definition into a
 * parser-private map and leaves no node behind.
 *
 * BOXED, and the reason is measured rather than stylistic: `markdown_core_link`
 * is 32 bytes and `markdown_core_code` is 40, which is the widest arm `node.as`
 * has. Three chunks are 48, so storing this inline would grow EVERY node in the
 * document by 8 bytes to carry a payload that appears once per definition. */
typedef struct {
    markdown_core_chunk label;
    markdown_core_chunk url;
    markdown_core_chunk title;
} markdown_core_definition;

enum markdown_core_node__internal_flags {
    MARKDOWN_CORE_NODE__OPEN = (1 << 0),
    MARKDOWN_CORE_NODE__LAST_LINE_BLANK = (1 << 1),
    MARKDOWN_CORE_NODE__LAST_LINE_CHECKED = (1 << 2),

    // The first bit an extension may claim. Extension flags are compile-time
    // constants owned by the extension that uses them; there is no runtime
    // registration and no allocator to run out of bits.
    MARKDOWN_CORE_NODE__EXTENSION_FIRST = (1 << 3),
};

typedef uint16_t markdown_core_node_internal_flags;

struct markdown_core_node {
    markdown_core_strbuf content;

    struct markdown_core_node *next;
    struct markdown_core_node *prev;
    struct markdown_core_node *parent;
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

    const markdown_core_syntax_extension *extension;

    union {
        markdown_core_chunk literal;
        markdown_core_list list;
        markdown_core_code code;
        markdown_core_heading heading;
        markdown_core_link link;
        markdown_core_definition *definition;
        int html_block_type;
        int cell_index; // For keeping track of TABLE_CELL table alignments
        void *opaque;
    } as;
};

static MARKDOWN_CORE_INLINE markdown_core_mem *markdown_core_node_mem(markdown_core_node *node) {
    return node->content.mem;
}
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
