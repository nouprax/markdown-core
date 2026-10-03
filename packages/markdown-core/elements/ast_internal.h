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

/* WHERE A FOOTNOTE OR SPECIMEN WAS WRITTEN: the definitions of each kind, as
 * they complete, with their starts, from which publishing builds the
 * document's lookup tables. */
typedef struct {
    uint64_t start;
    const markdown_core_node *node;
} markdown_core_definition_entry;

typedef struct {
    markdown_core_definition_entry *values;
    size_t count, capacity;
} markdown_core_definition_registry;

typedef struct {
    markdown_core_definition_registry footnotes, specimens;
} markdown_core_lookup_registry;

void markdown_core_lookup_registry_dispose(markdown_core_lookup_registry *registry);

/* A NODE IS PUBLISHED WHEN IT COMPLETES (docs/plans/2026-09-29-incremental-
 * parsing.md, 4.1, 4.3, 5.8). `node` holds its place, and `owner` holds it,
 * as a child or a field, or is NULL for the document's root. Unless `node`
 * only holds a group of its owner's relation (a callout's title, a
 * definition's term and bodies, a citation's affixes), which is not a node
 * of the document, it takes the next id after the revision's last, the
 * nodes of its relations take their extents (markdown_core_measure_relation),
 * and a footnote or specimen joins `registry`; the root's extent is
 * measured from 0. Returns whether `node` is a node of the document. An
 * allocation failure fails the parse. */
bool markdown_core_publish_node(markdown_core_parser *parser, markdown_core_lookup_registry *registry,
                                markdown_core_node *node, const markdown_core_node *owner);

/* MEASURING a relation: every node of the relations of `owner` that `part`
 * holds -- `part` itself, when it is a field of `owner`; its children, when
 * it is `owner` or the holder of one of `owner`'s groups -- takes its extent
 * in place of its place: the distance from the end of the node before it in
 * the relation, or from `start`, where `owner` starts, for the first, and its
 * length (node.h). */
void markdown_core_measure_relation(const markdown_core_node *owner, uint32_t start, const markdown_core_node *part);

/* PUBLISHING THE DOCUMENT, the last step of the parse transaction: every node
 * already holds its id and its extent, and the lookup tables are built from
 * `registry`. A parse that continues a tree (the parser's revision, parser.h)
 * matches its nodes to the old ones (5.9): a matched node takes its old
 * node's id, and the old tree is shared where the two are equal. The
 * parser's root is the result. It works in the parser's scratch. False,
 * having changed neither tree's structure, when an allocation failed. */
bool markdown_core_publish_tree(markdown_core_parser *parser, markdown_core_lookup_registry *registry);

/* The scope of `node` in the published tree `root` parsed from `source`,
 * with columns in `unit`; markdown_core_document_scope is this query over a
 * document's tree and unit, behind its public check that the source covers
 * the node. False when an allocation failed. */
bool markdown_core_tree_scope(const markdown_core_node *root, const markdown_core_node *node, const uint8_t *source,
                              size_t length, markdown_core_text_unit unit, markdown_core_scope *scope);

/* ONE RELATION of a node: a node-valued field of the canonical AST, in the
 * canonical field order. Its nodes are the one node `field` holds, or the
 * children [start, end) of `holder`, which is NULL when there are none (a
 * table's row groups are ranges of the table's children); a relation of one
 * field node runs from 0 to 1. `group` names the list when the canonical
 * dump draws it as a group line (`Title`, `CitationPrefix`, a table's row
 * groups, a definition's term and bodies), and is NULL when its nodes are
 * drawn directly under the owner. A node's extent is relative to the end of
 * the previous node of its relation, or to the owner's start. */
typedef struct markdown_core_relation {
    const char *group;
    markdown_core_node **field;
    markdown_core_node *holder;
    size_t start, end;
} markdown_core_relation;

/* How many nodes `relation` holds. */
static inline size_t markdown_core_relation_count(const markdown_core_relation *relation) {
    return relation->end - relation->start;
}

/* The node of `relation` at `at`, in [start, end). */
static inline markdown_core_node *markdown_core_relation_node(const markdown_core_relation *relation, size_t at) {
    return relation->field ? *relation->field : markdown_core_children_at(relation->holder->children, at);
}

/* The relations of one node, one at a time. This is the one place that knows
 * which fields each kind owns and in what order: publishing, scope queries
 * and the canonical dump all walk the tree through it. */
typedef struct markdown_core_relation_cursor {
    const markdown_core_node *owner;
    /* The owner kind's shape of relations (ast.c), read once. */
    uint8_t shape;
    int step;
    /* The next of the owner's children a later relation starts at. */
    size_t at;
} markdown_core_relation_cursor;

void markdown_core_relations_begin(markdown_core_relation_cursor *cursor, const markdown_core_node *owner);
bool markdown_core_relations_next(markdown_core_relation_cursor *cursor, markdown_core_relation *relation);

/* THE CANONICAL WALK: every node of a tree in canonical walk order, each with
 * its absolute source range, and the group lines of the canonical dump
 * between them, read from a published tree's extents. An explicit stack of
 * relation cursors, so its depth is the tree's and never the C stack's. */
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
    /* The relation in hand: `start` is its next node. */
    markdown_core_relation relation;
    bool active, group_pending;
    uint32_t owner_start, anchor;
} markdown_core_walk_frame;

typedef struct markdown_core_walk {
    const markdown_core_node *root;
    /* The absolute offset the root's extent is relative to: 0 for a
     * document's root. */
    uint32_t anchor;
    bool started, failed, at_group;
    markdown_core_walk_frame *frames;
    size_t count, capacity;
    /* The frame of the owner of the item returned last, counted from 1; 0 for
     * the root. */
    size_t owner;
} markdown_core_walk;

void markdown_core_walk_begin(markdown_core_walk *walk, const markdown_core_node *root);
/* The next item, or false at the end or when a frame could not be allocated
 * (`failed`). */
bool markdown_core_walk_next(markdown_core_walk *walk, markdown_core_walk_item *item);
/* Whether another line follows the item the walk returned last at its level
 * under the same owner, which is how the canonical dump draws its branches. */
bool markdown_core_walk_has_next(const markdown_core_walk *walk);
void markdown_core_walk_end(markdown_core_walk *walk);

#ifdef __cplusplus
}
#endif

#endif
