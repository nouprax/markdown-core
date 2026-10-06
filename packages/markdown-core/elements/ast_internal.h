#ifndef MARKDOWN_CORE_AST_INTERNAL_H
#define MARKDOWN_CORE_AST_INTERNAL_H

#include "../include/markdown_core.h"
#include <node.h>
#include <parser.h>

/* C LINKAGE, AND WINDOWS IS THE ONLY PLACE THIS SHOWS. The Itanium ABI does not
 * mangle a variable at global scope, so `MARKDOWN_CORE_ELEMENT_*` resolves on
 * Linux and macOS whether or not the declaration says `extern "C"`; MSVC mangles
 * every variable, and a C++ translation unit including this header without the
 * guard fails to link with LNK2019. */
#ifdef __cplusplus
extern "C" {
#endif

/* A PUBLISHED DOCUMENT: the tree, whose nodes hold ids and extents and whose
 * root holds the definition tables, and the text unit its scope queries count
 * columns in. */
struct markdown_core_document {
    markdown_core_node *root;
    markdown_core_text_unit unit;
};

/* PUBLISHING, the last step of the parse transaction: the one canonical walk
 * that gives every node its id, rewrites its parse-time place as its extent
 * and records the definition tables it finds on the way in the root,
 * continuing the tree the parser's revision names (parser.h): a fresh parse
 * numbers every node from 1 in walk order. The parser's root is the result.
 * It works in the parser's scratch. False, having changed neither tree's
 * structure, when an allocation failed. Nothing reads a place after this. */
bool markdown_core_publish_tree(markdown_core_parser *parser);

/* The scopes of `node` in the published tree `root` parsed from `source`,
 * with columns in `unit`, in a new array markdown_core_scopes_free frees;
 * markdown_core_document_scope is this query over a document's tree and
 * unit, behind its public check that the source covers the node. False when
 * an allocation failed. */
bool markdown_core_tree_scope(const markdown_core_node *root, const markdown_core_node *node, const uint8_t *source,
                              size_t length, markdown_core_text_unit unit, markdown_core_scope **scopes, size_t *count);

/* ONE RELATION of a node: a node-valued field of the canonical AST, in the
 * canonical field order. `first` is its first node and the rest follow by
 * `next` up to `end`, the node after its last (NULL at a chain's end; a
 * table's row groups share one chain). `group` names the list when the
 * canonical dump draws it as a group line (`Title`, `CitationPrefix`, a
 * table's row groups, a definition's term and bodies), and is NULL when its
 * nodes are drawn directly under the owner. A node's extent is relative to
 * the previous node of its relation, or to the owner's start. */
typedef struct markdown_core_relation {
    const char *group;
    const markdown_core_node *first;
    const markdown_core_node *end;
} markdown_core_relation;

/* How many nodes `relation` holds. */
size_t markdown_core_relation_count(const markdown_core_relation *relation);

/* The relations of one node, one at a time. This is the one place that knows
 * which fields each kind owns and in what order: publishing, scope queries
 * and the canonical dump all walk the tree through it. */
typedef struct markdown_core_relation_cursor {
    const markdown_core_node *owner;
    /* The owner kind's shape of relations (ast.c), read once. */
    uint8_t shape;
    int step;
    const markdown_core_node *next;
} markdown_core_relation_cursor;

void markdown_core_relations_begin(markdown_core_relation_cursor *cursor, const markdown_core_node *owner);
bool markdown_core_relations_next(markdown_core_relation_cursor *cursor, markdown_core_relation *relation);

/* A NODE'S RUNS IN ABSOLUTE OFFSETS (node.h, markdown_core_runs): the
 * content offset each run's bytes start at, how many there are, and the
 * source range they were read from. A copied run, whose source is as long as
 * its content, reads each content byte from one source byte; any other reads
 * all of its content from all of its source, and a run of length 0 reads
 * none. Content and source both increase along the runs, and the source
 * between two runs is not the node's. */
typedef struct {
    uint32_t content, length, start, end;
} markdown_core_source_run;

typedef struct {
    markdown_core_source_run *runs;
    size_t count, capacity;
} markdown_core_source_runs;

/* Reads the published `runs`, measured from `origin`, into `table`, whose
 * storage it reuses. False when it could not grow. */
bool markdown_core_source_runs_read(markdown_core_source_runs *table, const markdown_core_runs *runs, uint32_t origin);
/* The source window of the content range `place`: from where its first
 * byte is read to where its last is; an empty range is the empty window
 * where its offset is read from. */
markdown_core_place markdown_core_source_runs_window(const markdown_core_source_runs *table, markdown_core_place place);
/* The source ranges of `window` less the gaps between the runs of `table`,
 * in source order; an empty window is one empty range. Writes at most
 * `capacity` and returns how many there are. */
size_t markdown_core_source_runs_ranges(const markdown_core_source_runs *table, markdown_core_place window,
                                        markdown_core_place *ranges, size_t capacity);

/* THE CANONICAL WALK: every node of a published document's tree in canonical
 * walk order, each with its range, and the group lines of the canonical dump
 * between them, read from the extents. A node in an inline root's content has
 * its range in that content, and the walk holds the root's runs while it is
 * in it. An explicit stack of relation cursors, so its depth is the tree's
 * and never the C stack's. */
typedef struct markdown_core_walk_item {
    /* The node, or NULL for a group line. */
    const markdown_core_node *node;
    markdown_core_place place;
    const char *group;
    size_t count;
    /* The nesting level the line is drawn at (the root's is 0). */
    size_t level;
    /* Whether `place` is in the content of the inline root whose runs the
     * walk holds, rather than in the source. */
    bool content;
} markdown_core_walk_item;

typedef struct markdown_core_walk_frame {
    size_t level;
    markdown_core_relation_cursor cursor;
    markdown_core_relation relation;
    bool active, group_pending;
    /* Whether the relation in hand is in an inline root's content, and
     * whether the frame's node is that root. */
    bool content, root;
    const markdown_core_node *next;
    uint32_t owner_start, anchor;
} markdown_core_walk_frame;

typedef struct markdown_core_walk {
    const markdown_core_node *root;
    bool started, failed, at_group;
    markdown_core_walk_frame *frames;
    size_t count, capacity;
    /* The frame of the owner of the item returned last, counted from 1; 0 for
     * the root. */
    size_t owner;
    /* The runs of the inline root whose content the walk is in, those of
     * the block whose ranges are asked for, and the source ranges
     * markdown_core_walk_ranges answers with. */
    markdown_core_source_runs runs, own;
    markdown_core_place *ranges;
    size_t range_capacity;
} markdown_core_walk;

void markdown_core_walk_begin(markdown_core_walk *walk, const markdown_core_node *root);
/* The next item, or false at the end or when the walk could not allocate
 * (`failed`). */
bool markdown_core_walk_next(markdown_core_walk *walk, markdown_core_walk_item *item);
/* The source ranges of `item`, the node the walk returned last, in source
 * order: its window less the gaps between the runs that place it (ast.c). They live in the walk until its next call.
 * False, with the walk `failed`, when they could not be allocated. */
bool markdown_core_walk_ranges(markdown_core_walk *walk, const markdown_core_walk_item *item,
                               const markdown_core_place **ranges, size_t *count);
/* Whether another line follows the item the walk returned last at its level
 * under the same owner, which is how the canonical dump draws its branches. */
bool markdown_core_walk_has_next(const markdown_core_walk *walk);
void markdown_core_walk_end(markdown_core_walk *walk);

#ifdef __cplusplus
}
#endif

#endif
