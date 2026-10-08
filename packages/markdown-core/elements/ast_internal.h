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
 * the previous node of its relation, or to the owner's start. */
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
/* The source ranges of `window` less the gaps between the runs of `table`,
 * in source order; an empty window is one empty range. Writes at most
 * `capacity` and returns how many there are. */
size_t markdown_core_source_runs_ranges(const markdown_core_source_runs *table, markdown_core_place window,
                                        markdown_core_place *ranges, size_t capacity);

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

/* Where a Footnote, Specimen, Reference or Heading was written, for the
 * definition tables. */
typedef struct {
    uint64_t start;
    const markdown_core_node *node;
} markdown_core_definition_entry;

typedef struct {
    markdown_core_definition_entry *values;
    size_t count, capacity;
} markdown_core_definition_table;

/* THE DEFINITION TABLES a parse fills, one per kind of node the document
 * finds by label. Every Heading enters its table; the ones whose text
 * declares a reference label are the labeled ones. */
typedef enum {
    MARKDOWN_CORE_TABLE_FOOTNOTES,
    MARKDOWN_CORE_TABLE_SPECIMENS,
    MARKDOWN_CORE_TABLE_REFERENCES,
    MARKDOWN_CORE_TABLE_HEADINGS,
    MARKDOWN_CORE_TABLE_COUNT
} markdown_core_definition_kind;

/* WHAT ONE PARSE PUBLISHES AS ITS NODES COMPLETE (docs/plans/2026-09-29-
 * incremental-parsing.md, 5.8, 5.9): each definition as it settles, at its
 * source start, and the runs of the inline root being completed, in
 * absolute offsets, read when a definition in its content first asks where
 * it was written or a node of its content first asks where its source lies.
 * For the reuse cursor it keeps the members a search climbs through, the old nodes each
 * child's range holds, and the nodes the parse made that settled as old
 * nodes, which go when the parse does. The document element holds it for
 * the parse. */
/* An old node a child's range holds, its source starting at `start`; `next` is the next one the child holds, plus one,
 * or 0. */
typedef struct markdown_core_candidate {
    const markdown_core_node *old;
    uint32_t start, next;
} markdown_core_candidate;

typedef struct markdown_core_publication {
    markdown_core_definition_table tables[MARKDOWN_CORE_TABLE_COUNT];
    markdown_core_source_runs runs;
    const markdown_core_inline_root *runs_root;
    /* The source ranges of the inline node being numbered. */
    markdown_core_place *ranges;
    size_t range_capacity;
    markdown_core_member **climb;
    size_t climb_capacity;
    markdown_core_candidate *candidates;
    size_t candidate_count, candidate_capacity;
    markdown_core_node **replaced;
    size_t replaced_count, replaced_capacity;
} markdown_core_publication;

/* COMPLETING the node `member` builds, which begins at `start`: each node it
 * holds that is not numbered yet, in canonical field order, is numbered. It
 * collects the old nodes it may continue (5.9), and takes its extent,
 * measured from the end of the node before it in its
 * relation or from where the relation is measured, and its runs when they
 * are not an inline root's; `observe` sees it; a node holding inline content
 * waits on the parser's list of inline roots. A node that waits on nothing
 * settles at once (markdown_core_settle_member), and one that waits keeps
 * its member, and its owner waits on it. A node that gains a node later
 * completes again and numbers only that one. A node that holds only a group
 * of its owner numbers nothing (MARKDOWN_CORE_NODE__GROUP): its owner numbers
 * the group. The inline root being completed (parser.h, `completing`) also
 * completes its own runs, from where it recorded its holder lies. False
 * when an allocation failed. */
bool markdown_core_complete_node(markdown_core_parser *parser, markdown_core_publication *publication,
                                 markdown_core_member *member, uint32_t start,
                                 void (*observe)(const markdown_core_element_instance *, markdown_core_parser *,
                                                 markdown_core_node *),
                                 const markdown_core_element_instance *observer);

/* THE DECLARATIONS OF A TAKEN SUBTREE (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.3, 5.7): `node`, which a parse took whole at `start`, and
 * every node under it are listed in the definition tables as if the parse
 * had made them, and `visit` sees each with the source offset where it was
 * written. False when an allocation failed. */
bool markdown_core_publication_take(markdown_core_publication *publication, const markdown_core_node *node,
                                    uint32_t start, void (*visit)(void *, const markdown_core_node *, uint32_t),
                                    void *context);

/* A NUMBERED NODE SETTLES once it waits on nothing (5.9), its kind and range
 * final: it decides the old node it continues, unless it decided already,
 * and takes that node's id or the next one, unless it took it already; when
 * it equals the old node it continues -- its kind, extent, runs and scalars, and every
 * relation holding the same nodes -- the old node takes its place in its
 * owner, or as the document; a definition enters its table; its member goes,
 * and an owner that waited only on it settles in turn. */
void markdown_core_settle_member(markdown_core_parser *parser, markdown_core_publication *publication,
                                 markdown_core_member *member);

/* PUBLISHING, the last step of the parse transaction: the document numbers
 * itself, the definition tables the parse filled are sealed into it, and it
 * settles. The parser's root is the result. False when an allocation
 * failed. */
bool markdown_core_publish_tree(markdown_core_parser *parser, markdown_core_publication *publication);

/* Releases what the publication holds, the nodes it keeps into `pool`. */
void markdown_core_publication_dispose(markdown_core_publication *publication, markdown_core_node_pool *pool);

#ifdef __cplusplus
}
#endif

#endif
