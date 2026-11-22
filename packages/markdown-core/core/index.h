#ifndef MARKDOWN_CORE_INDEX_H
#define MARKDOWN_CORE_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "node.h"
#include "parser.h"
#include "tree.h"

/* THE CHILD INDEX (docs/plans/no-flush.md, 10.10): where the children of
 * the root, and of the blocks below it a rest point was looked in, are,
 * and how far the readings of each child and below it read, which the
 * rest of an edit and of a locate asks (markdown_core_index_crossed). */
/* THE WIDTHS: where the root's children are, from their extents alone
 * (docs/plans/relative-positions.md): a tree (tree.h) of where each child
 * starts and ends, measured from where its holder's first child is, which
 * an edit, which moves no child it takes back, mends: the children it read
 * go and come again, and those it took back move with it. A child is found
 * at an offset, and where it is, in time logarithmic in the children. They
 * are made at the first edit or locate, and an edit that could not have
 * their storage leaves them to the next to make again. */
typedef struct {
    uint32_t start, end;
    /* The furthest any reading of the child or below it read (node.h,
     * `closed`, `look`), or `start` for none: the children whose readings
     * read past a byte are found by it (markdown_core_tree_past; THE READS
     * PAST). */
    uint32_t reach;
    /* How many children on is the nearest later child whose reading
     * began at or before this one starts (node.h, `opened`: a paragraph
     * its definitions were cut from), or 0 for none. */
    uint32_t cover;
} markdown_core_width;

typedef struct {
    markdown_core_tree tree;
    /* `bare`: a child the widths measured since they were made from the
     * first has no extent (child_extent: a definition's body that holds no
     * node), so its holder has no source of its own to measure. */
    bool made, bare;
} markdown_core_widths;

/* The widths of the children of `holder`, a block below the root, and the
 * marks of what they fold into it (parser.h, THE FOLD MARKS). */
typedef struct {
    const markdown_core_node *holder;
    markdown_core_widths widths;
    markdown_core_folds folds;
} markdown_core_nest;

/* The widths of the root's children and the nests, for the parse
 * `parser` whose document's root is `root`, as markdown_core_index_make
 * last made or found them. All zero is an index with nothing made. */
typedef struct markdown_core_index {
    markdown_core_widths widths;
    /* The widths of the blocks below the root a rest point was looked for
     * in: those of the last edit's chain, which it mended, and those a
     * locate since looked in. */
    markdown_core_nest *nests;
    size_t nest_count, nest_capacity;
    /* The nests by holder: an open table of `slot_count` slots, a power
     * of two, each a nest's number plus one, or 0 (nest_find). */
    size_t *slots;
    size_t slot_count;
    const markdown_core_parser *parser;
    const markdown_core_node *root;
} markdown_core_index;

/* Whether `child` has no extent to measure: a holder with no runs of its
 * own that holds no node (a definition's empty body), which stands where
 * the one before it ends. */
static inline bool markdown_core_child_bare(const markdown_core_node *child) {
    return !child->source.runs && !child->child_count;
}

/* Where the children of `holder`, which starts at `start`, are measured
 * from: its start, or, for a holder with no runs of its own (a
 * definition's body), where the relation its nodes go on from ends, which
 * its extent is measured from too (ast.c, publish_body). */
static inline uint32_t markdown_core_holder_base(const markdown_core_node *holder, size_t start) {
    return holder->source.runs ? (uint32_t)start : (uint32_t)((int64_t)start - holder->extent.lead);
}

void markdown_core_index_free(markdown_core_index *index);
/* The root's widths made when they are not, for `root` of `parser`.
 * False when storage could not be had. */
bool markdown_core_index_make(markdown_core_index *index, const markdown_core_parser *parser,
                              const markdown_core_node *root);
/* The root's widths to be made again at the next make. */
void markdown_core_index_drop(markdown_core_index *index);
/* Where the child `child` of `holder`, whose first child is measured from
 * `base`, starts; the child before the first that ends past `offset`, or
 * its first; and its source, from its extent. */
size_t markdown_core_widths_start(const markdown_core_widths *widths, const markdown_core_node *holder, int64_t base,
                                  size_t child);
uint32_t markdown_core_widths_child(const markdown_core_widths *widths, const markdown_core_node *holder, int64_t base,
                                    size_t offset);
markdown_core_place markdown_core_widths_window(const markdown_core_widths *widths, const markdown_core_node *holder,
                                                uint32_t base, size_t child);
/* The widths of the children of `holder`, a block below the root, made
 * when there are none; NULL when storage could not be had. */
const markdown_core_widths *markdown_core_index_nest(markdown_core_index *index, const markdown_core_node *holder);
/* The widths of the children of `holder`, found, never made, or NULL. */
const markdown_core_widths *markdown_core_index_held(const markdown_core_index *index,
                                                     const markdown_core_node *holder);
/* The fold marks of the nest of `holder`, found, never made, or NULL. */
markdown_core_folds *markdown_core_index_folds(markdown_core_index *index, const markdown_core_node *holder);
/* Whether a reading that starts before `point` read past `past`, but a
 * look that ended no later than `from`, where the edit begins or the
 * look's cursor is, and the closes of the `count` blocks that start at
 * `opens`, each once, as their reopen notes them again; where the first
 * began in `*start` (THE READS PAST). */
bool markdown_core_index_crossed(markdown_core_index *index, size_t from, size_t point, size_t past,
                                 const uint32_t *opens, size_t count, size_t *start);
/* THE REST (docs/plans/no-flush.md, 10.15): where a reading of the text
 * from before an edit, or before a look's cursor, reads as the old reading
 * did, so that it may begin there. It is a position: a line, and the
 * blocks the old reading held open at the line's start, from the root's
 * child down (the chain), each found from its node alone -- a block that
 * holds the line, or one whose close read past what the line decides
 * (node.h, `closed`), as the line closed it. The reading begins at the
 * line with the chain reopened (blocks.c, THE NESTED REOPEN, THE LEAP): a
 * block of it that holds blocks keeps its children before the line, and a
 * leaf, the chain's last, its lines. A position stands when no reading the
 * line's reading does not do again read past what the line decides, but
 * those of the chain's blocks (THE READS PAST), every block of the chain
 * reopens there as its element says (element.h, the edit's `reopens`, THE
 * LEAP), the child before the line, at the chain's end, is none a later
 * line goes on in or writes (`awaits`, a TRAILED kind), the line before
 * reads to the chain as the reopen can tell (blocks.c,
 * markdown_core_parser_rest_blank), and what the edit writes neither
 * writes the closer a leaf of the chain found (`shuts`) nor a stop line of
 * an opener read to the end before it (`stops`). Where one does not stand,
 * an earlier line is tried, where the reading that read past began, or
 * where the block that does not reopen starts, and so on back to the
 * text's start, which stands. Edits and looks find their rest so alike. */
#define MARKDOWN_CORE_REST_DEPTH 32

typedef struct {
    markdown_core_node *chain[MARKDOWN_CORE_REST_DEPTH];
    /* Each block's of the chain the child of the block before, or of the
     * root, and at `depth` the first child of the chain's last block the
     * reading reads again, or 0 for a leaf. */
    uint32_t path[MARKDOWN_CORE_REST_DEPTH + 1];
    /* Where each block of the chain starts, and where its child before the
     * one on the path ends, or where it starts when that is its first. */
    uint32_t starts[MARKDOWN_CORE_REST_DEPTH], priors[MARKDOWN_CORE_REST_DEPTH];
    size_t depth, line;
    /* Where the chain's last block ends, when it is a leaf, or 0. */
    size_t last;
    /* The root's first child the reading reads again or takes back: the
     * one after the chain's, when there is a chain. */
    uint32_t child;
} markdown_core_rest;

/* What the reading from the rest reads again: an edit of the bytes from
 * `from` to `to`, which begins on line `line` and writes the `length`
 * bytes of `text`, or a look, whose text ends at its cursor, `from` and
 * `to`; `prose` when a paragraph may be the chain's leaf (THE LEAP): a look
 * reads the text of the paragraph it is in whole, and so does a reading of
 * an altered label's uses. */
typedef struct {
    size_t line, from, to;
    const uint8_t *text;
    size_t length;
    bool prose, look;
} markdown_core_rest_edit;

/* The rest of `edit` in the text `source` of `size` bytes, the flushed
 * one, in `*rest`. False when storage could not be had. */
bool markdown_core_index_rest(markdown_core_index *index, const char *source, size_t size,
                              const markdown_core_rest_edit *edit, markdown_core_rest *rest);
/* After an edit that moved the text after it by `delta`, of the root's
 * children, `count` of them before, those from `first` up to `tail` read
 * again, and those from `tail` on taken back, or none when `tail` is
 * UINT32_MAX: the widths mended. False, with them to be made again, when
 * storage could not be had. */
bool markdown_core_index_mend(markdown_core_index *index, const markdown_core_node *root, size_t first, uint32_t tail,
                              size_t count, int64_t delta);
/* After a reading through the `depth` blocks of its chain, `chain`, on the
 * path `path` (blocks.c, THE NESTED REOPEN), which had `counts` children
 * each: their nests mended, the deepest first, and every other let go. */
void markdown_core_index_mend_nests(markdown_core_index *index, markdown_core_node *const *chain, const uint32_t *path,
                                    size_t depth, const uint32_t *counts, int64_t delta);
#ifndef NDEBUG
/* The root's widths are those made anew. */
void markdown_core_index_check(markdown_core_index *index);
#endif

#endif
