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
 * root holds the rosters of its definitions, and the text unit its scope queries count
 * columns in. */
struct markdown_core_document {
    markdown_core_node *root;
    markdown_core_text_unit unit;
};

/* The scopes of `node` in the published tree `root` parsed from `source`,
 * with columns in `unit`, in a new array markdown_core_scopes_free frees;
 * markdown_core_document_scope is this query over a document's tree and
 * unit, behind its public check that the source covers the node. False when
 * an allocation failed. */
bool markdown_core_tree_scope(const markdown_core_node *root, const markdown_core_node *node, const uint8_t *source,
                              size_t length, markdown_core_text_unit unit, markdown_core_scope **scopes, size_t *count);

/* ONE RELATION of a node: a node-valued field of the canonical AST, in the
 * canonical field order. Its nodes are the `count` nodes of `stem` from
 * `index` on (a table's row groups are runs of one stem), or, for a field
 * holding one node of its own, `node`. `group` names the list when the
 * canonical dump draws it as a group line (`Title`, `CitationPrefix`, a
 * table's row groups, a definition's term and bodies), and is NULL when its
 * nodes are drawn directly under the owner. A node's extent is relative to
 * the previous node of its stem, or to the owner's start. */
typedef struct markdown_core_relation {
    const char *group;
    const markdown_core_stem *stem;
    size_t index, count;
    const markdown_core_node *node;
    /* The canonical field the relation is, and which of its lists when it
     * is a list of lists (a definition's bodies). */
    markdown_core_field name;
    uint32_t list;
    /* Whether the relation is a field holding one node of its own (a
     * document's metadata, a table's caption, a directive's label, a
     * citation's note), rather than a list. */
    bool field;
} markdown_core_relation;

/* THE NODES OF A RELATION, one at a time, in order. */
typedef struct markdown_core_relation_walk {
    markdown_core_stem_walk stem;
    const markdown_core_node *node;
} markdown_core_relation_walk;

void markdown_core_relation_walk_begin(markdown_core_relation_walk *walk, const markdown_core_relation *relation);
/* The next node, or NULL once the walk has read them all. */
const markdown_core_node *markdown_core_relation_walk_next(markdown_core_relation_walk *walk);
/* Whether the walk has a node left to read. */
static inline bool markdown_core_relation_walk_more(const markdown_core_relation_walk *walk) {
    return walk->node || walk->stem.left;
}

/* The relations of one node, one at a time. This is the one place that knows
 * which fields each kind owns and in what order: publishing, scope queries
 * and the canonical dump all walk the tree through it. */
typedef struct markdown_core_relation_cursor {
    const markdown_core_node *owner;
    /* The owner kind's shape of relations (ast.c), read once. */
    uint8_t shape;
    int step;
    /* Where a relation that is a run of the owner's children starts next. */
    size_t next;
} markdown_core_relation_cursor;

void markdown_core_relations_begin(markdown_core_relation_cursor *cursor, const markdown_core_node *owner);
bool markdown_core_relations_next(markdown_core_relation_cursor *cursor, markdown_core_relation *relation);

/* A NODE'S RUNS IN ABSOLUTE OFFSETS (node.h, markdown_core_runs): the
 * content offset each run's decoded bytes start at, how many there are, and
 * the source range they were read from. A run whose range is as long as what
 * it decodes reads each content byte from one source byte; any other decodes
 * all of its content from all of its source, and a run that decodes nothing
 * reads none. Content and source both increase along the runs, and the source
 * between two runs is not the node's. */
typedef struct {
    uint32_t content, decoded, start, end;
} markdown_core_source_run;

typedef struct {
    markdown_core_source_run *runs;
    size_t count, capacity;
} markdown_core_source_runs;

/* The source window of the content range `place`: from where its first
 * byte is read to where its last is; an empty range is the empty window
 * where its offset is read from. */
markdown_core_place markdown_core_source_runs_window(const markdown_core_source_runs *table, markdown_core_place place);
/* The source the content range `place` is read from: its window, as
 * markdown_core_source_runs_window gives it, into `*window`, and the window
 * less the gaps between the runs of `table`, in source order, into `ranges`;
 * an empty window is one empty range. Writes at most `capacity` ranges and
 * returns how many there are. */
size_t markdown_core_source_runs_read(const markdown_core_source_runs *table, markdown_core_place place,
                                      markdown_core_place *window, markdown_core_place *ranges, size_t capacity);

/* THE CANONICAL WALK: every node of a published document's tree in canonical
 * walk order, each with the source window it lies in, and the group lines of
 * the canonical dump between them, read from the runs. An explicit stack of relation cursors, so its depth is
 * the tree's and never the C stack's. */
typedef struct markdown_core_walk_item {
    /* The node, or NULL for a group line. */
    const markdown_core_node *node;
    markdown_core_place place;
    const char *group;
    size_t count;
    /* The nesting level the line is drawn at (the root's is 0). */
    size_t level;
} markdown_core_walk_item;

typedef struct markdown_core_walk_frame {
    size_t level;
    markdown_core_relation_cursor cursor;
    markdown_core_relation relation;
    bool active, group_pending;
    markdown_core_relation_walk nodes;
    /* Where the frame's node begins in the source, and where the source of
     * the node before the next ends. */
    uint32_t owner_start, anchor;
} markdown_core_walk_frame;

typedef struct markdown_core_walk {
    const markdown_core_node *root;
    /* The offset the root's extent is measured from. */
    uint32_t anchor;
    bool started, failed, at_group;
    markdown_core_walk_frame *frames;
    size_t count, capacity;
    /* The frame of the owner of the item returned last, counted from 1; 0 for
     * the root. */
    size_t owner;
    /* The source ranges markdown_core_walk_ranges answers with. */
    markdown_core_place *ranges;
    size_t range_capacity;
} markdown_core_walk;

void markdown_core_walk_begin(markdown_core_walk *walk, const markdown_core_node *root);
/* Begins at `root`, a node of a tree whose extent is measured from
 * `anchor`. */
void markdown_core_walk_begin_at(markdown_core_walk *walk, const markdown_core_node *root, uint32_t anchor);
/* Begins under `owner`, which starts at `start` in the source, at the first
 * node it holds, keeping the walk's buffers. False, with the walk `failed`,
 * when they could not grow. */
bool markdown_core_walk_begin_under(markdown_core_walk *walk, const markdown_core_node *owner, uint32_t start);
/* The next item, or false at the end or when the walk could not allocate
 * (`failed`). */
bool markdown_core_walk_next(markdown_core_walk *walk, markdown_core_walk_item *item);
/* The source ranges of `item`, the node the walk returned last, in source
 * order: its runs.
 * They live in the walk until its next call. False, with the walk `failed`,
 * when they could not be allocated. */
bool markdown_core_walk_ranges(markdown_core_walk *walk, const markdown_core_walk_item *item,
                               const markdown_core_place **ranges, size_t *count);
/* Whether another line follows the item the walk returned last at its level
 * under the same owner, which is how the canonical dump draws its branches. */
bool markdown_core_walk_has_next(const markdown_core_walk *walk);
void markdown_core_walk_end(markdown_core_walk *walk);

/* ONE OLD NODE of an inline root's old content (5.9), at its place in the
 * canonical walk: the image of its anchor, the place of the last node it
 * holds, where its kind's list holds it (UINT32_MAX when none of its bytes
 * is left), and whether a node continues it or it is kept under a node the
 * parse took whole. */
typedef struct markdown_core_match_node {
    const markdown_core_node *node;
    uint32_t image, last, at;
    uint8_t kind, state;
} markdown_core_match_node;

/* THE ROOT'S MATCH: the old root's content, read whole when a node of the
 * content first asks for it, in canonical walk order; the old nodes by
 * address, for the nodes the parse takes whole; and, for each kind read, a
 * segment of `keys` holding its old nodes in the order of their anchors'
 * images, each with its place in the walk, over which a Fenwick tree in the
 * same segment of `open` counts those no node continues. */
typedef struct markdown_core_match {
    const markdown_core_node *old;
    uint32_t old_start;
    bool read;
    markdown_core_walk walk;
    markdown_core_match_node *nodes;
    /* The nodes read whose last held node is not read yet, with their
     * levels, while the content is read; then the kinds' segments, each key
     * an image above a place in the walk, ordered with `scratch`. */
    uint64_t *keys, *scratch;
    uint32_t *open;
    uint32_t count, capacity;
    /* The old nodes by address: an open-addressed table of places in the
     * walk, counted from 1, `slot_count` a power of two. */
    uint32_t *slots;
    uint32_t slot_count, slot_capacity;
    uint64_t kinds;
    uint32_t first[64], total[64];
} markdown_core_match;

/* WHAT ONE PARSE PUBLISHES AS ITS NODES COMPLETE (docs/plans/2026-09-29-
 * incremental-parsing.md, 5.8, 5.9): the runs of the inline root being
 * completed, in absolute offsets, read when a node of its content first asks
 * where its source lies, and the root's match. For the reuse cursor it keeps
 * the frames a decision climbs through, and the nodes the parse made that
 * settled as old nodes, which go when the parse does. The document element
 * holds it for the parse. */

typedef struct markdown_core_publication {
    /* The runs of the inline root `runs_node` being completed, read as
     * places (completing_runs in ast.c). */
    markdown_core_source_runs runs;
    const markdown_core_node *runs_node;
    /* The source ranges of the inline node being numbered. */
    markdown_core_place *ranges;
    size_t range_capacity;
    markdown_core_frame **climb;
    size_t climb_capacity;
    markdown_core_match match;
    markdown_core_node **replaced;
    size_t replaced_count, replaced_capacity;
    /* A splice's search for the node it replaces: the nodes it reached, and
     * those it has yet to look into. */
    struct markdown_core_splice_step *steps;
    size_t step_capacity;
    size_t *pending;
    size_t pending_capacity;
} markdown_core_publication;

/* `frame`, whose kind and range are final, decides what old node it
 * continues (5.9), the frames above it that have not decided first. False
 * when an allocation failed. */
bool markdown_core_decide_frame(markdown_core_parser *parser, markdown_core_publication *publication,
                                markdown_core_frame *frame);
/* COMPLETING the node `frame` builds, which begins at `start`: it decides
 * what old node it continues (5.9), the children it has not numbered yet
 * are numbered, each completing first, and become its stem with those it
 * numbered before, and each field not numbered yet, in canonical field
 * order, is numbered. Each takes its extent, measured from the end of the
 * node before it in its relation or from where the relation is measured, and
 * its runs; `observe` sees it. Each settles at once
 * (markdown_core_settle_frame). A group (MARKDOWN_CORE_NODE__GROUP) holds a
 * relation of its owner, whose nodes it numbers from where its owner
 * begins, and its owner numbers the group. The content of the inline root being completed
 * (parser.h, `completing`) is numbered by its holder's draft
 * (markdown_core_complete_draft); a root read again in place also publishes
 * its own runs, from where it recorded its holder lies, and `observe` sees
 * it. False when an allocation failed. */
bool markdown_core_complete_node(markdown_core_parser *parser, markdown_core_publication *publication,
                                 markdown_core_frame *frame, uint32_t start,
                                 void (*observe)(const markdown_core_element_instance *, markdown_core_parser *,
                                                 markdown_core_node *),
                                 const markdown_core_element_instance *observer);

/* `frame` TAKES ANOTHER CHILD: the children it holds before it are
 * numbered in order, as far as each has completed, as complete_node numbers
 * them, and join its numbered children (node.h, markdown_core_built); their
 * frames go. False when an allocation failed. */
bool markdown_core_number_children(markdown_core_parser *parser, markdown_core_publication *publication,
                                   markdown_core_frame *frame,
                                   void (*observe)(const markdown_core_element_instance *, markdown_core_parser *,
                                                   markdown_core_node *),
                                   const markdown_core_element_instance *observer);

/* AN INLINE ROOT'S PARSE BEGINS: the root decides what old node it
 * continues (5.9), and its match reads that node's content, in the holder's
 * place, from the start. False when an allocation failed. */
bool markdown_core_open_root(markdown_core_parser *parser, markdown_core_publication *publication,
                             markdown_core_inline_root *root);
/* The root's parse took `draft`'s old node whole: the match reads on to it,
 * and the draft continues it. False when an allocation failed. */
bool markdown_core_take_old(markdown_core_parser *parser, markdown_core_publication *publication,
                            markdown_core_draft *draft);
/* `draft`, which continued an old node, goes: no node continues that one. */
void markdown_core_forget_draft(markdown_core_publication *publication, markdown_core_draft *draft);

/* COMPLETING `draft`, a node of the content of the inline root being
 * completed, the nodes it holds complete (5.8): each node it holds, in
 * canonical field order, is numbered: it continues the earliest old node of
 * the root's match of its kind that no node continues yet and whose anchor's
 * image its source window holds (5.9), and it takes its extent,
 * measured from the end of the node before it in its relation or from where
 * the relation is measured, the source its range was read from as its runs,
 * and the id of the old node it continues or the next one, and the old node
 * takes its place when it equals it; `observe` sees it. A group's owner
 * numbers the group. The holder numbers the root's content, whose orders the
 * root's frame collects. False when an allocation failed. */
bool markdown_core_complete_draft(markdown_core_parser *parser, markdown_core_publication *publication,
                                  markdown_core_draft *draft,
                                  void (*observe)(const markdown_core_element_instance *, markdown_core_parser *,
                                                  markdown_core_node *),
                                  const markdown_core_element_instance *observer);

/* THE INLINE STAGE (5.7): each root the block stage listed is parsed in
 * place, in the order listed, against the content of the old root it reads
 * again, and settles; the old content the stage did not take leaves the
 * registry. False, with the parse failed, when an allocation failed. */
bool markdown_core_parse_listed(markdown_core_parser *parser, markdown_core_publication *publication);

/* AN INLINE ROOT IS PARSED AGAIN IN PLACE (5.7): `root`, found where it
 * begins by its order, holds content whose lookups are answered otherwise
 * now. It reads that content again from its runs, as the inline stage does:
 * in place when the new tree owns it, and else in a copy that takes its
 * order and its definitions. The facts its content gave leave the registry,
 * and the old content leaves once the parse has taken what it takes of it;
 * a root read in place continues the old node it continued, and a copy
 * continues `root` when `root` continues itself. Its node takes `root`'s
 * place as it settles (markdown_core_publication_splice). False, with the
 * parse failed, when an allocation failed. */
bool markdown_core_parse_again(markdown_core_parser *parser, markdown_core_publication *publication,
                               const markdown_core_node *root);

/* A NODE OF A TAKEN SUBTREE TAKES ITS REPLACEMENT'S PLACE (5.7, 5.11): the
 * document's path down to `old` is found by the order `node` took from it,
 * and `node` takes `old`'s place: each node on the path that the old tree
 * shares is copied, with its facts and its order, and one that only the new
 * tree holds changes in place. Takes the reference to `node`. False when an allocation failed. */
bool markdown_core_publication_splice(markdown_core_parser *parser, markdown_core_publication *publication,
                                      const markdown_core_node *old, markdown_core_node *node);

/* A NUMBERED BLOCK SETTLES (5.9), its kind and range final: it decides the
 * old node it continues, unless it decided already, and takes that node's id
 * or the next one, unless it took it already; when it equals the old node it
 * continues -- its kind, extent, runs and scalars, and every relation holding
 * the same nodes -- the old node takes its place in its owner's field, as
 * the frame's node, or as the document; a field root's frame goes, and a
 * child's as its owner numbers it. */
void markdown_core_settle_frame(markdown_core_parser *parser, markdown_core_publication *publication,
                                markdown_core_frame *frame);

/* PUBLISHING, the last step of the parse transaction: the document numbers
 * itself, holds the registry's rosters as they are now, and it settles. The parser's root is the result. False when an
 * allocation failed. */
bool markdown_core_publish_tree(markdown_core_parser *parser, markdown_core_publication *publication);

/* Releases what the publication holds, the nodes it keeps into `pool`. */
void markdown_core_publication_dispose(markdown_core_publication *publication, markdown_core_node_pool *pool);

#ifdef __cplusplus
}
#endif

#endif
