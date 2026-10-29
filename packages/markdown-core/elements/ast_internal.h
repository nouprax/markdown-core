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
 * that gives every node its id, makes its extent of its place and records
 * the definition tables it finds on the way in the root; a session's parse
 * publishes its tree in place (ast.c, PUBLISHING IN PLACE), and a fresh
 * parse numbers every node from 1 in walk order. The parser's root is the
 * result. It works in the parser's scratch. False when an allocation
 * failed. */
bool markdown_core_publish_tree(markdown_core_parser *parser);
/* PUBLISHING A CANDIDATE (ast.c) for `root`, an inline root a snapshot
 * published, itself or, a callout's title or a definition's term, as its
 * holder that starts at `holder_start`: the candidate takes `root`'s id, and
 * ids, extents and runs continuing `root`'s subtree. False when an
 * allocation failed. */
bool markdown_core_publish_candidate(markdown_core_parser *parser, markdown_core_node *candidate,
                                     markdown_core_node *root, uint32_t holder_start);
/* A DOCUMENT REOPENED FOR AN EDIT (ast.c): the flushed `root` keeps its
 * children before `child` and lets the others go, its metadata with them
 * when `child` is 0, each an old node the next publish matches against,
 * and lists the definitions that stay as committed. False, with nothing
 * changed, when storage could not be had. */
bool markdown_core_publish_reopen(markdown_core_parser *parser, markdown_core_node *root, uint32_t child);
/* A heading that stays in a reopened document listed as committed, `key`
 * its place among those that stay. False when storage could not be had. */
bool markdown_core_publish_keep_heading(markdown_core_parser *parser, const markdown_core_node *node, uint32_t key);

/* THE LINES OF A SOURCE: the offset each begins at. A line ends after LF,
 * after CR, or after CRLF, which is one terminator. */
typedef struct {
    size_t *starts;
    size_t count;
} markdown_core_source_lines;

/* Reads the line starts of `length` bytes of `source`: false when the table
 * could not be allocated. */
bool markdown_core_source_lines_read(markdown_core_source_lines *lines, const uint8_t *source, size_t length);
/* A byte range as editor coordinates in `unit`. */
markdown_core_scope markdown_core_source_scope(const markdown_core_source_lines *lines, const uint8_t *source,
                                               markdown_core_place place, markdown_core_text_unit unit);
/* The byte `position` names, or the place between bytes a CURSOR at it
 * stands, in `*offset` (ast.c, THE BYTE A POSITION NAMES). */
bool markdown_core_position_offset(const markdown_core_source_lines *lines, const uint8_t *source, size_t length,
                                   markdown_core_position position, markdown_core_text_unit unit, bool cursor,
                                   size_t *offset);
/* The same on one line, which runs from `at` to `end`, the start of the
 * line after it or the source's end, for the column `column`. */
bool markdown_core_column_offset(const uint8_t *source, size_t at, size_t end, int32_t column,
                                 markdown_core_text_unit unit, bool cursor, size_t *offset);
/* The nodes that hold the byte at `offset`, root first (ast.c, THE NODES
 * THAT HOLD A BYTE). */
bool markdown_core_tree_holders(const markdown_core_node *root, size_t offset, const markdown_core_node ***nodes,
                                size_t *count, const markdown_core_node **last);

/* Parses `sink`'s text, the text before a cursor, as a fresh parse whose
 * parser reports what it leaves open to `sink`, with the labels `whole`
 * defines declared (session.c). */
struct markdown_core_context_sink;
markdown_core_status markdown_core_parse_before_cursor(struct markdown_core_context_sink *sink,
                                                       markdown_core_text_unit unit,
                                                       const markdown_core_document *whole,
                                                       markdown_core_document **document);

/* A session's context state (context.c, THE SESSION'S CONTEXT): a new
 * empty context, or NULL when the allocation failed; the context at
 * `position` of `document`, parsed from `length` bytes of `source`, which
 * replaces what `context` holds and leaves it as it was on a failure; and
 * the empty context `context` becomes again. */
markdown_core_context *markdown_core_context_new(void);
markdown_core_status markdown_core_context_locate(markdown_core_context *context,
                                                  const markdown_core_document *document, const uint8_t *source,
                                                  size_t length, markdown_core_position position);
void markdown_core_context_clear(markdown_core_context *context);

/* The scopes of `node` in the published tree `root` parsed from `source`,
 * with columns in `unit`, in a new array markdown_core_scopes_free frees;
 * markdown_core_document_scope is this query over a document's tree and
 * unit, behind its public check that the source covers the node. False when
 * an allocation failed. */
bool markdown_core_tree_scope(const markdown_core_node *root, const markdown_core_node *node, const uint8_t *source,
                              size_t length, markdown_core_text_unit unit, markdown_core_scope **scopes, size_t *count);

/* ONE RELATION of a node: a node-valued field of the canonical AST, in the
 * canonical field order. Its `count` nodes are `items`, the storage that
 * holds them: the children array of `holder`, a slice of it, or the owner's
 * field that holds a single node, for which `holder` is NULL. A relation
 * with no nodes may have no storage. `group` names the list when the
 * canonical dump draws it as a group line (`Title`, `CitationPrefix`, a
 * table's row groups, a definition's term and bodies), and is NULL when its
 * nodes are drawn directly under the owner. A node's extent is relative to
 * the previous node of its relation, or to the owner's start, and its first
 * run to where the source of that node ends, or the owner's begins. A
 * relation that `continues` goes on from the one before it, as a table's
 * content and foot rows go on from its head rows: its first node is measured
 * from the last of the one before, or from where that one is. */
typedef struct markdown_core_relation {
    const char *group;
    markdown_core_node **items;
    size_t count;
    bool continues;
    const markdown_core_node *holder;
} markdown_core_relation;

/* The relations of one node, one at a time. This is the one place that knows
 * which fields each kind owns and in what order: publishing, scope queries
 * and the canonical dump all walk the tree through it. */
typedef struct markdown_core_relation_cursor {
    const markdown_core_node *owner;
    /* The owner kind's shape of relations (ast.c), read once. */
    uint8_t shape;
    int step;
    /* Where the next of a table's row groups starts among its rows, or the
     * next of a definition's bodies among its children. */
    uint32_t index;
} markdown_core_relation_cursor;

void markdown_core_relations_begin(markdown_core_relation_cursor *cursor, const markdown_core_node *owner);
bool markdown_core_relations_next(markdown_core_relation_cursor *cursor, markdown_core_relation *relation);

/* THE CANONICAL WALK: every node of a published document's tree in canonical
 * walk order, each with its source window, and the group lines of the
 * canonical dump between them, read from the runs. An explicit stack of
 * relation cursors, so its depth is the tree's and never the C stack's. */
typedef struct markdown_core_walk_item {
    /* The node, or NULL for a group line. */
    const markdown_core_node *node;
    /* Where its source lies: from where its first run starts to where its
     * last ends. */
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
    /* The next node of the relation in hand, counted from 0. */
    size_t next;
    /* Where the owner's source starts, and where the first run of the next
     * node of the relation in hand is measured from. */
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
    /* The source ranges markdown_core_walk_ranges answers with. */
    markdown_core_place *ranges;
    size_t range_capacity;
} markdown_core_walk;

void markdown_core_walk_begin(markdown_core_walk *walk, const markdown_core_node *root);
/* The next item, or false at the end or when the walk could not allocate
 * (`failed`). */
bool markdown_core_walk_next(markdown_core_walk *walk, markdown_core_walk_item *item);
/* The source ranges of `item`, the node the walk returned last: its runs,
 * in source order. They live in the walk until its next call. False, with the
 * walk `failed`, when they could not be allocated. */
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
