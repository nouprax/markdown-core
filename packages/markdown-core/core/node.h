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
#include "map.h"

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

/* A link reference definition. `label` is the bytes between `[` and `]` as
 * written; the normalized form matching runs on belongs to the reference map,
 * not to the node. */
typedef struct {
    markdown_core_chunk label;
    markdown_core_chunk url;
    markdown_core_chunk title;
} markdown_core_definition;

/* A reference to a definition. It holds no destination: that is the
 * definition's, stated once there. */
typedef struct {
    markdown_core_chunk label;
    markdown_core_reference_type form;
} markdown_core_reference_link;

enum markdown_core_node__internal_flags {
    MARKDOWN_CORE_NODE__OPEN = (1 << 0),
    MARKDOWN_CORE_NODE__LAST_LINE_BLANK = (1 << 1),
    /* The memo a list's finalize keeps of "ends with a blank line": CHECKED
     * says the answer was taken, ENDS_BLANK is the answer. Both bits, so a
     * second ask answers as the first did — a container's answer is its
     * youngest chain's, not its own LAST_LINE_BLANK — which a one-shot parse
     * never needs (it asks once) and a stream does (it asks at every close
     * while the list is open, and keeps the memo on what has settled). */
    MARKDOWN_CORE_NODE__LAST_LINE_CHECKED = (1 << 2),
    MARKDOWN_CORE_NODE__ENDS_BLANK = (1 << 3),
    /* A list's tightness, kept up as the list grows: TIGHT_SCANNED on an
     * item says the item is settled (a sibling follows it), was weighed for
     * looseness, and weighed tight; LOOSE_AT says the list is loose at or
     * before this item. Both are permanent truths about settled items, so a
     * finalize weighs only the items after the last marked one — the same
     * answer as weighing them all, in the size of what grew. */
    MARKDOWN_CORE_NODE__TIGHT_SCANNED = (1 << 14),
    MARKDOWN_CORE_NODE__LOOSE_AT = (1 << 15),

    /* The block ended on the line being processed, having consumed its own
     * terminator, so its end position is that line rather than the one
     * before. `finalize` names the block types this is true of; an extension
     * container that closed at its fence sets the flag instead, because the
     * property is about how the block ended and not about what kind it is. */
    MARKDOWN_CORE_NODE__ENDS_ON_CURRENT_LINE = (1 << 6),

    // The node currently owns raw inline source that the core refine pipeline
    // must parse into its complete child list. This is parser lifecycle
    // state, not canonical AST shape: a node materialized inside another
    // owner's running inline parse intentionally lacks the bit.
    MARKDOWN_CORE_NODE__OWNS_INLINE_SOURCE = (1 << 4),

    // Extension-owned flags are compile-time constants in the range
    // (1 << 5)..(1 << 13); each owning extension defines its own bits (see
    // extensions/table.c). The engine holds no runtime flag registry.
    MARKDOWN_CORE_NODE__EXTENSION_FIRST = (1 << 5),
};

typedef uint16_t markdown_core_node_internal_flags;

struct markdown_core_concrete_records;

struct markdown_core_node {
    markdown_core_strbuf content;

    struct markdown_core_node *next;
    struct markdown_core_node *prev;
    struct markdown_core_node *parent;
    struct markdown_core_node *first_child;
    struct markdown_core_node *last_child;

    // Identity: `id` is unique within the owning series and stable across
    // appends while the node remains "the same thing" — every document build
    // mints ids for new subtrees, and the append diff (diff.c) hands the
    // receiver's ids over to the successor's paired nodes.
    // `last_changed_rev` is the document revision at which the node's own
    // fields, child list, or any descendant last changed. Both stay 0 for
    // raw parser trees that never pass through a document build.
    uint64_t id;
    uint64_t last_changed_rev;
    // A cheap order-sensitive fingerprint of this node's subtree: its
    // type, its literal bytes when it has any, and its children's hashes.
    //
    // It is what the streaming frontier pairs on (extensions/diff.c): when
    // a tick re-derives the tail of the tree from the held partial line,
    // the new tail's subtrees are stamped and paired against the retired
    // tail's by hash sweeps and a positional middle, so an unchanged node
    // keeps its id and revision. Meaningful on exactly the subtrees the
    // facade stamps for that — a spine block's run of children as it
    // publishes, a re-refined unit's children — and on nothing else: a
    // settled node's value is whatever it was when it last stood in a run,
    // and no reader asks it again.
    //
    // The pairing sweeps read it to decide WHICH nodes pair, and nothing
    // else: a paired node's changes are still found by comparing it field
    // by field and walking its children. So a collision, or a field the
    // hash does not cover, can only produce worse identity matching; it can
    // never let a changed node keep a stale revision. That is what makes
    // the bounded literal sample safe.
    uint64_t subtree_hash;

    // The concrete marker records of this node's own ownership region
    // (concrete_records.h), lazily allocated by the block phase and owned by
    // the node for its whole life — freed with it. NULL for the many nodes
    // whose region owns no marker bytes. Invisible to the canonical dump and
    // to markdown_core_ast_projection_changed by construction: concrete
    // spelling is not canonical content, so it must never decide the append
    // diff's pairing sweeps (diff.c:167/173) or its changed classification.
    struct markdown_core_concrete_records *concrete;

    // The inline token records of this node's inline sequence, in content
    // buffer coordinates (concrete_records.h). A separate vector from
    // `concrete` because it belongs to the inline ownership domain, not to
    // the node's marker lines. NULL except on the inline-owning region
    // nodes of 11.1.
    struct markdown_core_inline_concrete_records *inline_concrete;

    /* THE LABELS THIS UNIT'S INLINE PARSE ASKED THE DEFINITION TABLES ABOUT
     * — hits and misses alike, as hashes of the normalized label, threaded
     * through the parser's probe index (map.h) — so that when a definition
     * arrives later, the units whose answer it changes are exactly the ones
     * that asked, found in the size of that set. NULL for the many units
     * that never asked. Owned by the node; unthreaded and freed with it. */
    struct markdown_core_probes *probes;

    int start_line;
    int start_column;
    int end_line;
    int end_column;
    int internal_offset;
    uint16_t type;
    markdown_core_node_internal_flags flags;

    markdown_core_extension *extension;

    /* Named so a snapshot of it can be taken and put back: a close writes
     * into it (a list's tightness, a code block's literal), and the record
     * that undoes a close keeps the value it had. */
    union markdown_core_node_payload {
        markdown_core_chunk literal;
        markdown_core_list list;
        markdown_core_code code;
        markdown_core_heading heading;
        markdown_core_link link;
        markdown_core_definition definition;
        markdown_core_reference_link reference;
        int html_block_type;
        int cell_index; // For keeping track of TABLE_CELL table alignments
        void *opaque;
    } as;
};

static MARKDOWN_CORE_INLINE markdown_core_mem *markdown_core_node_mem(markdown_core_node *node) {
    return node->content.mem;
}
MARKDOWN_CORE_EXPORT int markdown_core_node_check(markdown_core_node *node, FILE *out);

/** Mixes one scalar into a running subtree hash. */
uint64_t markdown_core_hash_mix(uint64_t h, uint64_t value);

/** Mixes a byte range into a running subtree hash: its LENGTH plus a bounded
 * sample of both ends, never every byte — see the note on `subtree_hash`. */
uint64_t markdown_core_hash_bytes(uint64_t h, const uint8_t *data, size_t length);

/** Stamps `node->subtree_hash` from its type, its literal, and the hashes its
 * children already carry — so a subtree is stamped children first, which
 * is what markdown_core_node_stamp_tree does. `stamp_own` is the fold over
 * the node's own fields alone. */
void markdown_core_node_stamp(markdown_core_node *node);
uint64_t markdown_core_node_stamp_own(const markdown_core_node *node);

/** Stamps every node of `root`'s subtree, each as the walk leaves it. */
void markdown_core_node_stamp_tree(markdown_core_node *root);

/** Makes every chunk-valued field of ONE node its own: a literal, a label,
 * a destination or a title that borrows another buffer's bytes is copied
 * out, and one already owned is left alone. The union's chunk fields are
 * exactly the ones markdown_core_node_free releases, so this and that free
 * are the two readers of one list. Returns false when a copy could not be
 * allocated; whatever was copied stays copied and correct. */
bool markdown_core_node_own_chunks(markdown_core_node *node);

/*
 * Parser-internal mutation primitives. Callers must already have proved
 * allocator, containment, and non-ancestry invariants from grammar state.
 * They exist so a delimiter reduction does not repeat an O(depth) defensive
 * ancestor walk for every node it creates or moves.
 */
void markdown_core_node_set_type_unchecked(markdown_core_node *node, markdown_core_node_type type);
void markdown_core_node_insert_before_unchecked(markdown_core_node *node, markdown_core_node *sibling);
void markdown_core_node_insert_after_unchecked(markdown_core_node *node, markdown_core_node *sibling);
void markdown_core_node_append_child_unchecked(markdown_core_node *node, markdown_core_node *child);

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

MARKDOWN_CORE_EXPORT bool markdown_core_node_can_contain_type(
    markdown_core_node *node,
    markdown_core_node_type child_type
);

/** True when `node` directly owns inline source and its parsed children.
 * These nodes are the units of the per-block postprocess pipeline. */
MARKDOWN_CORE_EXPORT bool markdown_core_node_owns_inlines(markdown_core_node *node);

#ifdef __cplusplus
}
#endif

#endif
