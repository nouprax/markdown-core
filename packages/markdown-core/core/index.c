#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "element.h"
#include "index.h"
#include "node_type.h"
#include "parser.h"
#include "tree.h"

static size_t index_line_start(const markdown_core_parser *parser, size_t line) {
    size_t start, end;
    markdown_core_parser_line_span(parser, line, &start, &end);
    return start;
}

static void nest_free(markdown_core_nest *nest) {
    markdown_core_tree_free(&nest->widths.tree);
    markdown_core_free(nest->folds.flags);
    markdown_core_free(nest->folds.passes);
}

/* The extent of `child`, which a publish gave it: a definition's body, no
 * node of the tree, has one too (ast.c, publish_body). */
static inline markdown_core_extent child_extent(const markdown_core_node *child) { return child->extent; }

static size_t width_start(const void *entry) { return ((const markdown_core_width *)entry)->start; }

static size_t width_reach(const void *entry) { return ((const markdown_core_width *)entry)->reach; }

static void width_shift(void *entry, int64_t delta) {
    markdown_core_width *width = entry;
    width->start = (uint32_t)((int64_t)width->start + delta);
    width->end = (uint32_t)((int64_t)width->end + delta);
    width->reach = (uint32_t)((int64_t)width->reach + delta);
}

static const markdown_core_tree_ops width_ops = {
    sizeof(markdown_core_width), width_start, NULL, width_shift, NULL, width_reach};

MARKDOWN_CORE_TREE_OPS(width_tree, markdown_core_width)

static size_t *nest_slot(const markdown_core_index *edits, const markdown_core_node *holder);
static inline int64_t node_own(const markdown_core_node *node);
static uint32_t node_reach(markdown_core_index *index, const markdown_core_node *node, size_t depth, bool nests);

/* Child `child` of `holder`, measured from where the one before it ends,
 * `after`; its reach from its nest when it has one and `nests` says so. */
static inline markdown_core_width width_of(markdown_core_index *index, const markdown_core_node *holder, size_t child,
                                           uint32_t after, bool nests) {
    const markdown_core_extent extent = child_extent(holder->children[child]);
    const uint32_t start = (uint32_t)((int64_t)after + extent.lead);
    const uint32_t reach = node_reach(index, holder->children[child], 0, nests);
    return (markdown_core_width){start, start + (uint32_t)extent.span, start + reach, 0};
}

/* Child `child` of `holder` began its reading before it starts: the
 * children before it from where it did, from `low` on, are covered by it,
 * as far as no nearer child covers them. */
static void widths_cover(markdown_core_tree *tree, const markdown_core_node *holder, size_t child, size_t low) {
    const markdown_core_node *node = holder->children[child];
    if (!node->opened) {
        return;
    }
    const markdown_core_width *width = width_tree_at(tree, child);
    const uint32_t from = width->start - node->opened;
    for (size_t i = child; i-- > low;) {
        markdown_core_width *before = width_tree_at(tree, i);
        if (before->start < from) {
            break;
        }
        if (!before->cover || i + before->cover > child) {
            before->cover = (uint32_t)(child - i);
        }
    }
}

/* The widths of `holder`'s children made whole. False, with the widths to
 * be made again, when storage could not be had. */
static bool widths_build(markdown_core_index *index, markdown_core_widths *widths, const markdown_core_node *holder,
                         bool nests) {
    markdown_core_tree_free(&widths->tree);
    widths->tree = markdown_core_tree_make(&width_ops);
    widths->bare = false;
    uint32_t after = 0;
    for (size_t i = 0; i < holder->child_count; i++) {
        markdown_core_width *width = width_tree_push(&widths->tree);
        if (!width) {
            widths->made = false;
            return false;
        }
        *width = width_of(index, holder, i, after, nests);
        after = width->end;
        widths->bare |= markdown_core_child_bare(holder->children[i]);
        widths_cover(&widths->tree, holder, i, 0);
    }
    widths->made = true;
    return true;
}

/* After an edit that moved the text after it by `delta`, of `holder`'s
 * children, `count` of them before, the ones from `first` up to `tail` read
 * again, and those from `tail` on taken back, or none when `tail` is
 * UINT32_MAX: the read go and come again, and the taken move. False, with
 * the widths to be made again, when storage could not be had. */
static bool widths_mend(markdown_core_index *index, markdown_core_widths *widths, const markdown_core_node *holder,
                        size_t first, uint32_t tail, size_t count, int64_t delta) {
    markdown_core_tree *tree = &widths->tree;
    const size_t now = holder->child_count;
    const size_t taken = tail == UINT32_MAX ? 0 : now - tail;
    const size_t end = tail == UINT32_MAX ? now : tail;
    assert(tree->count == count && taken <= count && first <= end);
    markdown_core_tree_erase(tree, first, count - taken - first);
    markdown_core_tree_shift(tree, first, delta);
    uint32_t after = first ? width_tree_at(tree, first - 1)->end : 0;
    /* The child before, which the reading kept, holds what a block it
     * read that yields no node read (blocks.c, S_reads_pass): it reaches
     * further, as far as it now does. Only its own readings grew, unless
     * it is the chain's, whose nest the reading mended: what is below
     * another is as it was, and is not walked again. */
    if (first) {
        markdown_core_width *before = width_tree_at(tree, first - 1);
        const markdown_core_node *kept = holder->children[first - 1];
        const size_t *slot = nest_slot(index, kept);
        const uint32_t reach =
            before->start + (slot && *slot ? node_reach(index, kept, 0, true) : (uint32_t)node_own(kept));
        if (reach > before->reach) {
            before->reach = reach;
            markdown_core_tree_raise(tree, first - 1);
        }
    }
    for (size_t i = first; i < end; i++) {
        const markdown_core_width width = width_of(index, holder, i, after, true);
        if (!width_tree_insert(tree, i, width, false)) {
            widths->made = false;
            return false;
        }
        after = width.end;
        widths->bare |= markdown_core_child_bare(holder->children[i]);
    }
    if (end < now) {
        widths->bare |= markdown_core_child_bare(holder->children[end]);
    }
    /* The children read cover, and so does the one before, which may
     * have taken a reading that began before it, and the first taken back
     * when its reading began among them. */
    for (size_t i = first ? first - 1 : 0; i < now && i <= end; i++) {
        widths_cover(tree, holder, i, 0);
    }
    return true;
}

/* Makes the widths when they are not made. */
static bool widths_make(markdown_core_index *index, markdown_core_widths *widths, const markdown_core_node *root) {
    return widths->made || widths_build(index, widths, root, true);
}

/* Where the child `child` of `holder`, whose first child is measured from
 * `base`, starts. */
size_t markdown_core_widths_start(const markdown_core_widths *widths, const markdown_core_node *holder, int64_t base,
                                  size_t child) {
    (void)holder;
    return (size_t)(base + width_tree_at((markdown_core_tree *)&widths->tree, child)->start);
}

/* The child of `holder`, whose first child is measured from `base`, before
 * the first that ends past `offset`, or its first. */
uint32_t markdown_core_widths_child(const markdown_core_widths *widths, const markdown_core_node *root, int64_t base,
                                    size_t offset) {
    (void)root;
    if ((int64_t)offset < base) {
        return 0;
    }
    /* The first that ends past the offset: the last that starts at or
     * before it, when it does, or the one after it, as children are in
     * order and apart. */
    markdown_core_tree *tree = (markdown_core_tree *)&widths->tree;
    const size_t at = (size_t)((int64_t)offset - base);
    size_t past = markdown_core_tree_from(tree, at + 1, 0, tree->count);
    if (past && width_tree_at(tree, past - 1)->end > at) {
        past--;
    }
    return past ? (uint32_t)(past - 1) : 0;
}

#ifndef NDEBUG
/* The widths an edit mended are the widths of their block made anew. */
static void nest_check(markdown_core_index *index, const markdown_core_widths *widths,
                       const markdown_core_node *holder) {
    markdown_core_widths fresh = {0};
    if (widths_build(index, &fresh, holder, false)) {
        assert(fresh.tree.count == widths->tree.count);
        for (size_t i = 0; i < fresh.tree.count; i++) {
            const markdown_core_width *made = width_tree_at(&fresh.tree, i);
            const markdown_core_width *kept = width_tree_at((markdown_core_tree *)&widths->tree, i);
            assert(made->start == kept->start && made->end == kept->end && made->reach == kept->reach &&
                   made->cover == kept->cover);
            (void)made;
            (void)kept;
        }
    }
    markdown_core_tree_free(&fresh.tree);
}

#endif

/* The slot of the table that holds the nest of `holder`, or the empty one
 * where it would go; NULL when there is no table. */
static size_t *nest_slot(const markdown_core_index *edits, const markdown_core_node *holder) {
    if (!edits->slot_count) {
        return NULL;
    }
    const size_t mask = edits->slot_count - 1;
    size_t at = (size_t)(((uint64_t)(uintptr_t)holder >> 4) * UINT64_C(0x9E3779B97F4A7C15) >> 32) & mask;
    while (edits->slots[at] && edits->nests[edits->slots[at] - 1].holder != holder) {
        at = (at + 1) & mask;
    }
    return &edits->slots[at];
}

/* THE REACH OF A NODE: the furthest any reading of `node` or of a node
 * below it read, or where a child of a node below it starts when that is
 * further, measured from where the node starts: as the widths of a
 * node's children hold it (`reach`, `start` for none), from the widths of
 * a node's children when it has a nest and `nests` says so, else from its
 * children's own, to any depth. The children of a block of inlines read
 * nothing of their own. Reads as far as any when storage could not be
 * had. */
typedef struct {
    const markdown_core_node *node;
    int64_t at;
    size_t next;
    uint32_t after;
} index_reach_frame;

static inline bool node_reads_none_below(markdown_core_index *index, const markdown_core_node *node) {
    if (!node->child_count) {
        return true;
    }
    if (MARKDOWN_CORE_NODE_TYPE_INLINE_P(node->kind) ||
        (markdown_core_parser_block_kind(index->parser, node)->flags & MARKDOWN_CORE_KIND_INLINES)) {
#ifndef NDEBUG
        for (size_t i = 0; i < node->child_count; i++) {
            assert(!node->children[i]->closed && !node->children[i]->look);
        }
#endif
        return true;
    }
    return false;
}

static inline int64_t node_own(const markdown_core_node *node) {
    return node->closed > node->look ? node->closed : node->look;
}

static uint32_t node_reach(markdown_core_index *index, const markdown_core_node *node, size_t depth, bool nests) {
    (void)depth;
    int64_t reach = node_own(node);
    index_reach_frame room[16], *stack = room;
    size_t count = 0, capacity = 16;
    bool failed = false;
    /* A node to read below, at `at` from `node`. */
#define REACH_PUSH(child, offset)                                                                                      \
    do {                                                                                                               \
        if (count == capacity) {                                                                                       \
            index_reach_frame *grown = markdown_core_alloc(capacity * 2, sizeof(*grown));                              \
            if (!grown) {                                                                                              \
                failed = true;                                                                                         \
                break;                                                                                                 \
            }                                                                                                          \
            memcpy(grown, stack, count * sizeof(*grown));                                                              \
            if (stack != room) {                                                                                       \
                markdown_core_free(stack);                                                                             \
            }                                                                                                          \
            stack = grown;                                                                                             \
            capacity *= 2;                                                                                             \
        }                                                                                                              \
        stack[count++] = (index_reach_frame){(child), (offset), 0, 0};                                                 \
    } while (0)
    REACH_PUSH(node, 0);
    while (count && !failed) {
        index_reach_frame *frame = &stack[count - 1];
        const markdown_core_node *holder = frame->node;
        if (!frame->next) {
            if (node_reads_none_below(index, holder)) {
                count--;
                continue;
            }
            const size_t *slot = nests ? nest_slot(index, holder) : NULL;
            if (slot && *slot && index->nests[*slot - 1].widths.made) {
                const int64_t base = holder->source.runs ? 0 : -(int64_t)holder->extent.lead;
                const int64_t far =
                    frame->at + base + (int64_t)markdown_core_tree_furthest(&index->nests[*slot - 1].widths.tree);
                reach = far > reach ? far : reach;
                count--;
                continue;
            }
        }
        if (frame->next == holder->child_count) {
            count--;
            continue;
        }
        const int64_t base = holder->source.runs ? 0 : -(int64_t)holder->extent.lead;
        const markdown_core_node *child = holder->children[frame->next++];
        const markdown_core_extent extent = child_extent(child);
        const uint32_t start = (uint32_t)((int64_t)frame->after + extent.lead);
        frame->after = start + (uint32_t)extent.span;
        const int64_t at = frame->at + base + start;
        const int64_t far = at + node_own(child);
        reach = far > reach ? far : reach;
        REACH_PUSH(child, at);
    }
#undef REACH_PUSH
    if (stack != room) {
        markdown_core_free(stack);
    }
    return failed ? UINT32_MAX / 2 : (uint32_t)reach;
}

/* The table made again with `count` slots, a power of two, from the nests.
 * False, with the table as it was, when storage could not be had. */
static bool nest_index(markdown_core_index *edits, size_t count) {
    size_t *slots = markdown_core_alloc(count, sizeof(*slots));
    if (!slots) {
        return false;
    }
    markdown_core_free(edits->slots);
    edits->slots = slots;
    edits->slot_count = count;
    for (size_t i = 0; i < edits->nest_count; i++) {
        *nest_slot(edits, edits->nests[i].holder) = i + 1;
    }
    return true;
}

/* The nest of `holder`, a block below the root, made when there is none;
 * NULL when storage could not be had. */
static markdown_core_nest *nest_find(markdown_core_index *index, const markdown_core_node *holder) {
    markdown_core_index *edits = index;
    size_t *slot = nest_slot(edits, holder);
    if (slot && *slot) {
        return &edits->nests[*slot - 1];
    }
    /* Room in the table for one more, at most half full. */
    if ((edits->nest_count + 1) * 2 > edits->slot_count) {
        if (!nest_index(edits, edits->slot_count ? edits->slot_count * 2 : 16)) {
            return NULL;
        }
        slot = nest_slot(edits, holder);
    }
    markdown_core_nest *grew =
        markdown_core_reserve(edits->nests, &edits->nest_capacity, edits->nest_count + 1, sizeof(*edits->nests));
    if (!grew) {
        return NULL;
    }
    edits->nests = grew;
    markdown_core_nest *nest = &grew[edits->nest_count];
    *nest = (markdown_core_nest){.holder = holder};
    if (!widths_make(index, &nest->widths, holder)) {
        nest_free(nest);
        return NULL;
    }
    *slot = ++edits->nest_count;
    return nest;
}

/* The widths of the children of `holder`, as nest_find makes them. */
const markdown_core_widths *markdown_core_index_nest(markdown_core_index *index, const markdown_core_node *holder) {
    const markdown_core_nest *nest = nest_find(index, holder);
    return nest ? &nest->widths : NULL;
}

/* The source of the child `child` of `holder`, a block below the root that
 * starts at `base`, from its extent. */
markdown_core_place markdown_core_widths_window(const markdown_core_widths *widths, const markdown_core_node *holder,
                                                uint32_t base, size_t child) {
    const size_t start = markdown_core_widths_start(widths, holder, base, child);
    const markdown_core_place place = {(uint32_t)start, (uint32_t)(start + child_extent(holder->children[child]).span)};
    return place;
}

/* THE READS PAST (docs/plans/no-flush.md, 10.10): whether a reading that
 * began before `point` read past `past` -- a block's close (node.h,
 * `closed`) but those of the blocks the reading reopens, which start at
 * `opens`, each once; a look (`look`, and a line's) that read past `from`
 * too -- and where the first such began, from the nodes and the lines
 * alone: down the blocks that hold the point, the children before each,
 * found by how far their readings reached (THE REACH OF A NODE), and the
 * lines before it, by how far their looks read. */
typedef struct {
    markdown_core_index *index;
    size_t point, past, look;
    const uint32_t *opens;
    size_t count;
    uint64_t used;
    /* Where the first reading found began, or SIZE_MAX. */
    size_t start;
} index_past;

static inline void past_found(index_past *past, size_t from) { past->start = from < past->start ? from : past->start; }

/* Whether `node`, which starts at `start`, read past by a reading of its
 * own: its close, unless it is `exempt`, or a look. */
static inline bool past_own(const index_past *past, const markdown_core_node *node, size_t start, bool exempt) {
    return (!exempt && node->closed && start + node->closed > past->past) ||
           (node->look && start + node->look > past->look);
}

/* Whether the close of `child`, which starts at `at`, is that of a block
 * the reading reopens, one of `opens`, each once: its reopen notes it
 * again. */
static inline bool past_exempt(index_past *past, const markdown_core_node *child, size_t at) {
    for (size_t k = 0; k < past->count; k++) {
        if (!(past->used >> k & 1) && (past->opens[k] == at || past->opens[k] == at - child->opened)) {
            past->used |= UINT64_C(1) << k;
            return true;
        }
    }
    return false;
}

/* The readings below `holder`, which starts at `start`, of its children
 * from `low` up to `high`, which start before the point and are before
 * it whole, and below them, to any depth. */
typedef struct {
    const markdown_core_node *holder;
    size_t start, next, high;
} index_past_frame;

static void past_children(index_past *past, const markdown_core_node *holder, size_t start, size_t low, size_t high) {
    markdown_core_index *index = past->index;
    index_past_frame room[16], *stack = room;
    size_t count = 1, capacity = 16;
    stack[0] = (index_past_frame){holder, start, low, high};
    while (count) {
        index_past_frame *frame = &stack[count - 1];
        const markdown_core_node *at_holder = frame->holder;
        const bool root = at_holder == index->root;
        /* A nest made below moves the nests: the holder's is found again. */
        const markdown_core_widths *widths = root ? &index->widths : markdown_core_index_nest(index, at_holder);
        if (!widths || widths->bare) {
            /* No children to read apart: the reading is taken to begin
             * where the holder does. */
            past_found(past, frame->start - at_holder->opened);
            count--;
            continue;
        }
        const int64_t base =
            root ? (int64_t)at_holder->extent.lead : (int64_t)markdown_core_holder_base(at_holder, frame->start);
        const size_t at = (int64_t)past->past > base ? (size_t)((int64_t)past->past - base) : 0;
        const size_t i = frame->next < frame->high
                             ? markdown_core_tree_past(&widths->tree, at, frame->next, frame->high)
                             : frame->high;
        if (i == frame->high) {
            count--;
            continue;
        }
        frame->next = i + 1;
        const markdown_core_node *child = at_holder->children[i];
        const size_t from = (size_t)(base + width_tree_at((markdown_core_tree *)&widths->tree, i)->start);
        if (from - child->opened >= past->start) {
            count--;
            continue;
        }
        if (past_own(past, child, from, past_exempt(past, child, from))) {
            past_found(past, from - child->opened);
            continue;
        }
        if (!child->child_count || MARKDOWN_CORE_NODE_TYPE_INLINE_P(child->kind) ||
            (markdown_core_parser_block_kind(index->parser, child)->flags & MARKDOWN_CORE_KIND_INLINES)) {
            continue;
        }
        if (count == capacity) {
            index_past_frame *grown = markdown_core_alloc(capacity * 2, sizeof(*grown));
            if (!grown) {
                /* What is below goes unread: its reading is taken to begin
                 * where it does. */
                past_found(past, from - child->opened);
                continue;
            }
            memcpy(grown, stack, count * sizeof(*grown));
            if (stack != room) {
                markdown_core_free(stack);
            }
            stack = grown;
            capacity *= 2;
        }
        stack[count++] = (index_past_frame){child, from, 0, child->child_count};
    }
    if (stack != room) {
        markdown_core_free(stack);
    }
}

bool markdown_core_index_crossed(markdown_core_index *index, size_t from, size_t point, size_t past_at,
                                 const uint32_t *opens, size_t count, size_t *start) {
    const markdown_core_parser *parser = index->parser;
    index_past past = {index, point, past_at, past_at > from ? past_at : from, opens, count, 0, SIZE_MAX};
    if (!point) {
        return false;
    }
    /* The lines' looks. */
    const size_t lines = markdown_core_parser_line_of(parser, point - 1) + 1;
    const size_t looked = markdown_core_parser_line_looked(parser, lines, past.look);
    if (looked < lines) {
        past.start = index_line_start(index->parser, looked);
    }
    const markdown_core_node *holder = index->root;
    if (holder->look && holder->look > past.look) {
        past.start = 0;
    }
    size_t holder_start = 0;
    while (holder->child_count) {
        if (MARKDOWN_CORE_NODE_TYPE_INLINE_P(holder->kind) ||
            (markdown_core_parser_block_kind(parser, holder)->flags & MARKDOWN_CORE_KIND_INLINES)) {
            break;
        }
        const bool root = holder == index->root;
        const markdown_core_widths *widths = root ? &index->widths : markdown_core_index_nest(index, holder);
        if (!widths || widths->bare) {
            past_found(&past, holder_start - holder->opened);
            break;
        }
        markdown_core_tree *tree = (markdown_core_tree *)&widths->tree;
        const int64_t base =
            root ? (int64_t)holder->extent.lead : (int64_t)markdown_core_holder_base(holder, holder_start);
        const size_t count_children = holder->child_count;
        /* The first child that starts at or past the point, and the one
         * before it when it holds the point. */
        const size_t first = (int64_t)point > base
                                 ? markdown_core_tree_from(tree, (size_t)((int64_t)point - base), 0, count_children)
                                 : 0;
        size_t held = SIZE_MAX;
        if (first && (size_t)(base + width_tree_at(tree, first - 1)->end) > point) {
            held = first - 1;
        }
        past_children(&past, holder, holder_start, 0, held == SIZE_MAX ? first : held);
        /* A child from the point on whose reading began before it, as a
         * paragraph its definitions were cut from (`opened`): the child
         * that holds the point, or the first after it, is covered by it. */
        widths = root ? &index->widths : markdown_core_index_nest(index, holder);
        if (!widths) {
            past_found(&past, holder_start - holder->opened);
            break;
        }
        tree = (markdown_core_tree *)&widths->tree;
        for (size_t k = held == SIZE_MAX ? first : held; k < count_children;) {
            const markdown_core_width *width = width_tree_at(tree, k);
            const markdown_core_node *child = holder->children[k];
            const size_t at = (size_t)(base + width->start);
            /* Its close, when the reading reopens where it began. */
            bool exempt = false;
            for (size_t o = 0; o < count && !exempt && k >= first; o++) {
                if (!(past.used >> o & 1) && opens[o] == at - child->opened) {
                    past.used |= UINT64_C(1) << o;
                    exempt = true;
                }
            }
            if (k >= first && at - child->opened < point && past_own(&past, child, at, exempt)) {
                past_found(&past, at - child->opened);
            }
            if (!width->cover) {
                break;
            }
            k += width->cover;
        }
        if (held == SIZE_MAX) {
            break;
        }
        const markdown_core_node *child = holder->children[held];
        const size_t at = (size_t)(base + width_tree_at(tree, held)->start);
        /* The block's own close, when the reading reopens it. */
        if (past_own(&past, child, at, past_exempt(&past, child, at))) {
            past_found(&past, at - child->opened);
        }
        holder = child;
        holder_start = at;
    }
    *start = past.start;
    return past.start != SIZE_MAX;
}

/* THE REST (index.h). The search tries a line after line, each earlier
 * than the one before, back to the text's start; at each it finds the
 * position and asks whether it stands (rest_at). */
#define REST_STANDS SIZE_MAX
#define REST_FAILED (SIZE_MAX - 1)

typedef struct {
    markdown_core_index *index;
    const markdown_core_parser *parser;
    const char *source;
    size_t size;
    const markdown_core_rest_edit *edit;
    markdown_core_rest *rest;
    /* The first line the reading reads otherwise: the edit's, or the line
     * of the opener whose closer, or stop line, it writes, which reads
     * otherwise then. */
    size_t edited;
} rest_search;

static inline size_t rest_line(const rest_search *search, size_t offset) {
    return markdown_core_parser_line_of(search->parser, offset);
}

static inline const markdown_core_edit_ops *rest_ops(const markdown_core_parser *parser,
                                                     const markdown_core_node *block) {
    const markdown_core_element_instance *structure = markdown_core_parser_block_kind(parser, block)->structure;
    return structure ? structure->element->edit : NULL;
}

/* The widths of the children of the holder at `level` of the chain, the
 * root's or the block's before it, and where they are measured from; NULL
 * when storage could not be had. A nest made moves the nests: the widths
 * hold until the next is made. */
static const markdown_core_widths *rest_widths(const rest_search *search, size_t level,
                                               const markdown_core_node **holder, int64_t *base) {
    markdown_core_index *index = search->index;
    if (!level) {
        *holder = index->root;
        *base = (int64_t)index->root->extent.lead;
        return &index->widths;
    }
    *holder = search->rest->chain[level - 1];
    *base = (int64_t)markdown_core_holder_base(*holder, search->rest->starts[level - 1]);
    return markdown_core_index_nest(index, *holder);
}

/* Whether a line of the `length` bytes at `lines` passes `test` of
 * `self`'s element. */
static bool rest_line_in(const markdown_core_element_instance *self,
                         bool (*test)(const markdown_core_element_instance *, const unsigned char *, size_t),
                         const uint8_t *lines, size_t length) {
    for (size_t from = 0; from <= length;) {
        const size_t end = from + markdown_core_line_end(lines + from, length - from);
        if (test(self, lines + from, end - from)) {
            return true;
        }
        from = end + 1;
    }
    return false;
}

/* THE LINES AN EDIT WRITES: the edited line's bytes before it, what it
 * writes, and the last edited line's after it, in `*lines`, which is `room` when they fit its
 * `capacity` bytes and the caller frees otherwise, and in `*size`; and the
 * old lines they replace, from `*old` on. False when storage could not be
 * had. */
static bool rest_written(const rest_search *search, uint8_t *room, size_t capacity, uint8_t **lines, size_t *size,
                         size_t *old, size_t *old_end) {
    const markdown_core_rest_edit *edit = search->edit;
    const uint8_t *source = (const uint8_t *)search->source;
    const size_t line_start = index_line_start(search->parser, edit->line);
    size_t line_end = edit->to;
    while (line_end < search->size && source[line_end] != '\n' && source[line_end] != '\r') {
        line_end++;
    }
    *old = line_start;
    *old_end = line_end;
    *size = (edit->from - line_start) + edit->length + (line_end - edit->to);
    *lines = *size <= capacity ? room : markdown_core_alloc(*size, 1);
    if (!*lines) {
        return false;
    }
    memcpy(*lines, source + line_start, edit->from - line_start);
    if (edit->length) {
        memcpy(*lines + (edit->from - line_start), edit->text, edit->length);
    }
    memcpy(*lines + (edit->from - line_start) + edit->length, source + edit->to, line_end - edit->to);
    return true;
}

/* THE OPENERS AN EDIT MAY CLOSE (blocks.c, AN OPENER READ TO THE END): of
 * the openers read to the end that start before `before`, the first whose
 * element has a stop line among the lines the edit writes, in `*start`.
 * False when none has. */
static bool rest_stops(const rest_search *search, size_t before, size_t *start) {
    size_t count;
    const markdown_core_unclosed *openers = markdown_core_parser_openers(search->parser, &count);
    size_t first = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (!openers[i].aside && openers[i].start < before && openers[i].start < first) {
            first = openers[i].start;
        }
    }
    if (first == SIZE_MAX) {
        return false;
    }
    uint8_t room[256], *lines;
    size_t size, old, old_end;
    if (!rest_written(search, room, sizeof(room), &lines, &size, &old, &old_end)) {
        /* What the edit writes is not looked at: it reads from before the
         * first. */
        *start = first;
        return true;
    }
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        const markdown_core_unclosed *opener = &openers[i];
        if (!opener->aside && opener->start < before && (!found || opener->start < *start) &&
            rest_line_in(opener->owner, opener->owner->element->edit->stops, lines, size)) {
            *start = opener->start;
            found = true;
        }
    }
    if (lines != room) {
        markdown_core_free(lines);
    }
    return found;
}

/* Whether the edit writes, or replaces, a line that may write `prior`
 * (markdown_core_parser_writes). */
static bool rest_trails(const rest_search *search, const markdown_core_node *prior) {
    const markdown_core_parser *parser = search->parser;
    if (!(markdown_core_parser_block_kind(parser, prior)->flags & MARKDOWN_CORE_KIND_TRAILED)) {
        return false;
    }
    uint8_t room[256], *lines;
    size_t size, old, old_end;
    if (!rest_written(search, room, sizeof(room), &lines, &size, &old, &old_end)) {
        return true;
    }
    const bool writes =
        markdown_core_parser_writes(parser, prior, lines, size) ||
        markdown_core_parser_writes(parser, prior, (const unsigned char *)search->source + old, old_end - old);
    if (lines != room) {
        markdown_core_free(lines);
    }
    return writes;
}

/* Whether the line of the text before the edit at `start` may write
 * `block` (markdown_core_parser_writes). */
static bool rest_line_writes(const rest_search *search, const markdown_core_node *block, size_t start) {
    const unsigned char *line = (const unsigned char *)search->source + start;
    return markdown_core_parser_writes(search->parser, block, line, markdown_core_line_end(line, search->size - start));
}

/* THE CLOSER AN EDIT WRITES (element.h, the edit's `shuts`): the leaf the
 * chain ends in, whose opener found its closer, its last line, decided only
 * that it found one, which an edit that writes on or past the start of that
 * line may undo. Below the
 * root, the closer was one under the same prefixes (blocks.c,
 * markdown_core_parser_lookahead_end): an edit that writes a line's
 * prefixes, or a line end, which the prefixes of a new line follow, may
 * undo it too. */
static bool rest_shut(const rest_search *search) {
    const markdown_core_parser *parser = search->parser;
    const markdown_core_rest *rest = search->rest;
    const markdown_core_rest_edit *edit = search->edit;
    const markdown_core_node *block = rest->chain[rest->depth - 1];
    const markdown_core_kind_record *kind = markdown_core_parser_block_kind(parser, block);
    const markdown_core_edit_ops *ops = kind->structure ? kind->structure->element->edit : NULL;
    /* A block another opener read (element.h, THE FORMER KIND) found no
     * closer of the element's. */
    if (!ops || !ops->shuts ||
        (kind->made && kind->made->former && kind->made->former(kind->maker, block) != MARKDOWN_CORE_NODE_NONE)) {
        return false;
    }
    if (edit->to >= index_line_start(parser, markdown_core_parser_line_of(parser, rest->last - 1))) {
        return true;
    }
    if (rest->depth == 1) {
        return false;
    }
    const size_t line = markdown_core_parser_line_of(parser, edit->from);
    size_t start, end;
    markdown_core_parser_line_span(parser, line, &start, &end);
    size_t past = end;
    past -= past > start && search->source[past - 1] == '\n';
    past -= past > start && search->source[past - 1] == '\r';
    if (edit->from < markdown_core_parser_line_own(parser, line) || edit->to > past) {
        return true;
    }
    for (size_t i = 0; i < edit->length; i++) {
        if (edit->text[i] == '\n' || edit->text[i] == '\r') {
            return true;
        }
    }
    return false;
}

/* Whether the line that starts at `start` holds only spaces, tabs and
 * container prefix bytes. */
static bool rest_blank_line(const rest_search *search, size_t start) {
    const markdown_core_dialect *dialect = search->parser->dialect;
    for (size_t at = start; at < search->size && search->source[at] != '\n' && search->source[at] != '\r'; at++) {
        const unsigned char c = (unsigned char)search->source[at];
        if (c != ' ' && c != '\t' && !dialect->container_prefix[c]) {
            return false;
        }
    }
    return true;
}

/* Whether the edit leaves where its line ends as it was, and so what
 * follows it: it takes no line ending away, writes none and parts no CR
 * from its LF. */
static bool rest_keeps_end(const rest_search *search) {
    const markdown_core_rest_edit *edit = search->edit;
    if (edit->from && search->source[edit->from - 1] == '\r') {
        return false;
    }
    for (size_t at = edit->from; at < edit->to; at++) {
        if (search->source[at] == '\n' || search->source[at] == '\r') {
            return false;
        }
    }
    for (size_t at = 0; at < edit->length; at++) {
        if (edit->text[at] == '\n' || edit->text[at] == '\r') {
            return false;
        }
    }
    return true;
}

/* A REST INSIDE A LINE (docs/plans/no-flush.md, 10.16). The edited line
 * `line`, which starts at `start`, may have opened the paragraph the edit
 * is in: its reading stood, as the paragraph took the rest of the line as
 * text, where the paragraph starts, with the blocks that hold that point
 * open -- those open at the line's start that the line went on in, and
 * those it opened before it, from the `opened`th of the chain on -- and
 * every block it closed closed. When it read no byte the edit writes as it
 * gave that text away (markdown_core_parser_line_decided), a reading that
 * stands there reads on as it did: the position is that point, with the
 * blocks that hold it the chain, from the root's child down by where the
 * edit begins, and it stands as the line's start would, but that only the
 * readings before the point are not read again: no reading that began
 * before it read past what the line decided there, but the closes of the
 * chain's blocks (THE READS PAST), and the line before reads to the blocks
 * open at the line's start as the reopen can tell. 1 when it stands, with
 * the position in `search->rest`, 0 when it does not, -1 when storage
 * could not be had. */
static int rest_inside(rest_search *search, size_t line, size_t start) {
    const markdown_core_parser *parser = search->parser;
    const markdown_core_rest_edit *edit = search->edit;
    markdown_core_rest *rest = search->rest;
    bool past;
    const size_t decided = markdown_core_parser_line_decided(parser, line, &past);
    if (!decided || decided > edit->from || (past && !rest_keeps_end(search))) {
        return 0;
    }
    size_t opened = SIZE_MAX;
    for (size_t level = 0; level < MARKDOWN_CORE_REST_DEPTH; level++) {
        const markdown_core_node *holder;
        int64_t base;
        const markdown_core_widths *widths = rest_widths(search, level, &holder, &base);
        if (!widths) {
            return -1;
        }
        markdown_core_tree *tree = (markdown_core_tree *)&widths->tree;
        const size_t count = holder->child_count;
        if ((level && widths->bare) || !count || (int64_t)edit->from < base) {
            return 0;
        }
        const size_t k = markdown_core_tree_from(tree, (size_t)((int64_t)edit->from - base) + 1, 0, count);
        if (!k) {
            return 0;
        }
        const markdown_core_width *width = width_tree_at(tree, k - 1);
        const size_t at = (size_t)(base + width->start), end = (size_t)(base + width->end);
        if (end < edit->from || (at < start && end <= start)) {
            return 0;
        }
        markdown_core_node *child = holder->children[k - 1];
        rest->chain[level] = child;
        rest->path[level] = (uint32_t)(k - 1);
        rest->starts[level] = (uint32_t)at;
        if (level) {
            rest->priors[level - 1] = k > 1 ? (uint32_t)(base + width_tree_at(tree, k - 2)->end) : (uint32_t)base;
        }
        rest->depth = level + 1;
        if (at >= start && opened == SIZE_MAX) {
            if (rest_line(search, at) != line) {
                return 0;
            }
            opened = level;
        }
        if (markdown_core_parser_leaf(parser, child)) {
            if (opened == SIZE_MAX || !markdown_core_parser_leaps(parser, child) ||
                markdown_core_parser_reads_lines(parser, child)) {
                return 0;
            }
            rest->priors[level] = (uint32_t)at;
            rest->path[level + 1] = 0;
            rest->last = end;
            rest->mid = at;
            rest->inside = true;
            break;
        }
        const markdown_core_edit_ops *ops = rest_ops(parser, child);
        /* A block the line opened that made a field of its own of the
         * line's text (THE TEXT OF ITS LINE, element.h) is the chain's last
         * when the edit begins in that text: its children start on later
         * lines, and the reading stands where the text starts, which is as
         * far from the block's start as when it was read. */
        const markdown_core_node *text = ops && ops->text && opened == level ? *ops->text(child) : NULL;
        if (text && text->place.start >= child->place.start &&
            at + (text->place.start - child->place.start) <= edit->from) {
            rest->priors[level] = (uint32_t)at;
            rest->path[level + 1] = 0;
            rest->last = end;
            rest->mid = at + (text->place.start - child->place.start);
            rest->inside = true;
            break;
        }
        if (!ops || !ops->reopens || ops->headed || !child->child_count) {
            return 0;
        }
    }
    if (!rest->inside) {
        return 0;
    }
    /* A block of the chain whose first child the line opened started on
     * the line before. */
    if (opened && !rest->path[opened] && rest_line(search, rest->starts[opened - 1]) + 1 != line) {
        return 0;
    }
    size_t reached;
    if (markdown_core_index_crossed(search->index, edit->from, rest->mid, decided, rest->starts, rest->depth,
                                    &reached)) {
        return 0;
    }
    return 1;
}

/* The position at line `line` in `search->rest`, and REST_STANDS when it
 * stands; otherwise the earlier line the search tries next, or REST_FAILED
 * when storage could not be had. */
static size_t rest_at(rest_search *search, size_t line) {
    markdown_core_index *index = search->index;
    const markdown_core_parser *parser = search->parser;
    const markdown_core_rest_edit *edit = search->edit;
    markdown_core_rest *rest = search->rest;
    const size_t start = index_line_start(parser, line);
    /* A reading that read no further than the line decided the same when
     * the edit begins on a later line: the line reads as it did. On the
     * text's last line, the close the text's end makes of a block begun
     * before it read past it all the same. */
    size_t past = start;
    if (line < search->edited) {
        const size_t next = index_line_start(parser, line + 1);
        past = next < search->size ? next : start;
    }
    rest->line = line;
    rest->depth = 0;
    rest->last = 0;
    rest->mid = 0;
    rest->inside = false;
    if (line == edit->line && line == search->edited && edit->prose) {
        const int inside = rest_inside(search, line, start);
        if (inside < 0) {
            return REST_FAILED;
        }
        if (inside) {
            size_t opened;
            if (rest_stops(search, rest->mid, &opened)) {
                search->edited = rest_line(search, opened);
                return search->edited;
            }
            rest->child = rest->path[0] + 1;
            return REST_STANDS;
        }
        rest->depth = 0;
        rest->last = 0;
        rest->mid = 0;
        rest->inside = false;
    }
    /* Down the blocks open at the line's start: of each holder, its child
     * before the first that starts at or past it, when that child holds
     * the line or its close read past what the line decides. */
    size_t level = 0;
    uint32_t first = 0, count = 0;
    int64_t base = 0;
    /* Where the child before the first at or past the line starts and ends,
     * and where that first starts. */
    size_t prior = 0, before = 0, next = SIZE_MAX, next_end = 0;
    bool leaf = false;
    /* Whether a child at some level of the chain starts on the line. */
    bool starts = false;
    for (;;) {
        const markdown_core_node *holder;
        const markdown_core_widths *widths = rest_widths(search, level, &holder, &base);
        if (!widths) {
            return REST_FAILED;
        }
        /* A holder whose children have no source of their own to measure
         * (a definition's body that holds no node) is read again whole. */
        if (level && widths->bare && holder->child_count) {
            return rest_line(search, rest->starts[level - 1]);
        }
        markdown_core_tree *tree = (markdown_core_tree *)&widths->tree;
        count = holder->child_count;
        first = count && (int64_t)start > base
                    ? (uint32_t)markdown_core_tree_from(tree, (size_t)((int64_t)start - base), 0, count)
                    : 0;
        before = first ? (size_t)(base + width_tree_at(tree, first - 1)->end) : (size_t)base;
        prior = first ? (size_t)(base + width_tree_at(tree, first - 1)->start) : (size_t)base;
        next = first < count ? (size_t)(base + width_tree_at(tree, first)->start) : SIZE_MAX;
        next_end = first < count ? (size_t)(base + width_tree_at(tree, first)->end) : 0;
        starts = starts || (next != SIZE_MAX && rest_line(search, next) == line);
        if (level) {
            rest->priors[level - 1] = (uint32_t)before;
        }
        if (!first) {
            break;
        }
        const markdown_core_width *width = width_tree_at(tree, first - 1);
        const size_t at = (size_t)(base + width->start), end = (size_t)(base + width->end);
        markdown_core_node *child = holder->children[first - 1];
        if (end <= start && at + child->closed <= past) {
            /* A child whose close read the line was open as the line's
             * reading began, and a reading of the line that read past the
             * edit may have closed it (a definition body's blank line):
             * the line is read again with the child open. */
            if (line && at + child->closed > start &&
                markdown_core_parser_line_looked(parser, line + 1, past > edit->from ? past : edit->from) <= line) {
                return line - 1;
            }
            break;
        }
        if (level == MARKDOWN_CORE_REST_DEPTH) {
            return rest_line(search, at);
        }
        rest->chain[level] = child;
        rest->path[level] = first - 1;
        rest->starts[level] = (uint32_t)at;
        if (level) {
            rest->priors[level - 1] =
                (uint32_t)(first > 1 ? (size_t)(base + width_tree_at(tree, first - 2)->end) : (size_t)base);
        }
        rest->depth = level + 1;
        if (markdown_core_parser_leaf(parser, child)) {
            /* A leaf the reading leaps in (blocks.c, THE LEAP): one of
             * literal lines, or a paragraph when the reading may rest in
             * one. */
            if (!markdown_core_parser_leaps(parser, child) ||
                (!edit->prose && !markdown_core_parser_reads_lines(parser, child))) {
                return rest_line(search, at);
            }
            rest->priors[level] = (uint32_t)at;
            rest->path[level + 1] = 0;
            rest->last = end;
            leaf = true;
            break;
        }
        const markdown_core_edit_ops *ops = rest_ops(parser, child);
        if (!ops || !ops->reopens || !child->child_count) {
            return rest_line(search, at);
        }
        level++;
    }
    if (!leaf) {
        const markdown_core_node *holder = level ? rest->chain[level - 1] : index->root;
        /* The first child of a block whose children abut starts the block's
         * own first line: the block, reopened with none, reads it as a later
         * child's (blocks.c, markdown_core_parser_abuts), when it starts on
         * a later line than the block above it, and the edit is on a later
         * line of it: what the line opens the block with is read again
         * otherwise. */
        bool abuts = false;
        if (first < count && line < search->edited && level < MARKDOWN_CORE_REST_DEPTH &&
            rest_line(search, next) == line && next_end > index_line_start(parser, search->edited) &&
            (!level || rest_line(search, rest->starts[level - 1]) < line)) {
            markdown_core_node *child = holder->children[first];
            const markdown_core_edit_ops *ops = rest_ops(parser, child);
            if (ops && ops->reopens && !ops->headed && child->child_count &&
                markdown_core_parser_abuts(parser, child)) {
                const markdown_core_widths *nest = markdown_core_index_nest(index, child);
                if (!nest) {
                    return REST_FAILED;
                }
                const uint32_t own = markdown_core_holder_base(child, next);
                if (!nest->bare &&
                    rest_line(search, own + width_tree_at((markdown_core_tree *)&nest->tree, 0)->start) == line) {
                    rest->chain[level] = child;
                    rest->path[level] = first;
                    rest->starts[level] = (uint32_t)next;
                    rest->priors[level] = own;
                    rest->depth = level + 1;
                    /* What the line opens the block with must not have read
                     * the edited line, from anywhere on its line. */
                    const size_t edited = index_line_start(parser, search->edited);
                    size_t reached;
                    abuts = !markdown_core_index_crossed(index, edit->from, edited, edited, rest->starts, rest->depth,
                                                         &reached) ||
                            rest_line(search, reached) != line;
                    if (abuts) {
                        first = 0;
                    } else {
                        rest->depth = level;
                    }
                }
            }
        }
        if (!abuts && first) {
            /* The child before, which a later line may go on in, or write
             * when the edit writes the line (element.h, `awaits`;
             * markdown_core_follow, `closed`). */
            const markdown_core_node *child = holder->children[first - 1];
            if ((markdown_core_parser_block_kind(parser, child)->flags & MARKDOWN_CORE_KIND_AWAITS) ||
                (line == search->edited && rest_trails(search, child))) {
                return rest_line(search, prior);
            }
        }
        /* The line before reads to the chain as the reopen can tell: a block
         * of the chain that holds the line before with no child before the
         * line does not. And a line of the chain's that starts no child at
         * any level of it and is not blank is one of the block's own (a
         * table's delimiter row, a closing fence), which its element reads
         * as the block's: the child before is read again. */
        if (rest->depth && !abuts && !starts && !rest_blank_line(search, start)) {
            /* One that writes a block of the chain (markdown_core_follow,
             * `closed`), which the block's reopen keeps what it wrote of:
             * the block is read again from its start. */
            for (size_t i = 0; i < rest->depth; i++) {
                if (rest_line_writes(search, rest->chain[i], start)) {
                    return rest_line(search, rest->starts[i]);
                }
            }
            return rest_line(search, first ? prior : rest->starts[rest->depth - 1]);
        }
        if (rest->depth && !abuts) {
            /* With none, the line before is the block's first, its opener's,
             * read with no child, as the reopen reads it. */
            if (!first && rest_line(search, rest->starts[rest->depth - 1]) + 1 != line) {
                return rest_line(search, rest->starts[rest->depth - 1]);
            }
            if (first && markdown_core_parser_rest_blank(parser, (const unsigned char *)search->source, rest->chain,
                                                         rest->depth, line, before) == SIZE_MAX) {
                return rest_line(search, prior);
            }
        }
        rest->path[rest->depth] = first;
    }
    /* No reading the line's does not do again read past what it decides,
     * but the closes of the chain's blocks, which their reopen notes
     * again. */
    size_t reached;
    if (markdown_core_index_crossed(index, edit->from, start, past, rest->starts, rest->depth, &reached)) {
        return rest_line(search, reached);
    }
    if (leaf && rest_shut(search)) {
        search->edited = rest_line(search, rest->starts[rest->depth - 1]);
        return search->edited;
    }
    size_t opened;
    if (rest_stops(search, start, &opened)) {
        search->edited = rest_line(search, opened);
        return search->edited;
    }
    rest->child = rest->depth ? rest->path[0] + 1 : first;
    /* Before the root's first child, the text's start. */
    if (!rest->depth && !first) {
        rest->line = 0;
    }
    return REST_STANDS;
}

bool markdown_core_index_rest(markdown_core_index *index, const char *source, size_t size,
                              const markdown_core_rest_edit *edit, markdown_core_rest *rest) {
    rest_search search = {index, index->parser, source, size, edit, rest, edit->line};
    size_t line = edit->line;
    for (;;) {
        const size_t next = rest_at(&search, line);
        if (next == REST_STANDS) {
            return true;
        }
        if (next == REST_FAILED) {
            return false;
        }
        assert(next < line);
        line = next;
    }
}

/* The table of the nests made again for those kept, or none of them. */
static void nest_reindex(markdown_core_index *edits) {
    size_t slots = 16;
    while (slots < (edits->nest_count + 1) * 2) {
        slots *= 2;
    }
    if (!nest_index(edits, slots)) {
        for (size_t i = 0; i < edits->nest_count; i++) {
            nest_free(&edits->nests[i]);
        }
        edits->nest_count = 0;
        markdown_core_free(edits->slots);
        edits->slots = NULL;
        edits->slot_count = 0;
    }
}

/* After a reading through the `depth` blocks of its chain, `chain`, on
 * the path `path` (blocks.c, THE NESTED REOPEN), which
 * had `counts` children each: the widths of each block of the chain are
 * made again from the child on its path, through the first it took back
 * when its count of children is as it was, the deepest first, as a block's
 * reach is its children's (node_reach); those of every other block are let
 * go first, as the reading may have let the block go, and a block it made
 * may stand where it stood. */
void markdown_core_index_mend_nests(markdown_core_index *index, markdown_core_node *const *chain, const uint32_t *path,
                                    size_t depth, const uint32_t *counts, int64_t delta) {
    markdown_core_index *edits = index;
    size_t kept = 0;
    for (size_t i = 0; i < edits->nest_count; i++) {
        markdown_core_nest nest = edits->nests[i];
        size_t level = 0;
        while (level < depth && chain[level] != nest.holder) {
            level++;
        }
        if (level < depth) {
            edits->nests[kept++] = nest;
        } else {
            nest_free(&nest);
        }
    }
    edits->nest_count = kept;
    nest_reindex(edits);
    for (size_t level = depth; level--;) {
        size_t *slot = nest_slot(edits, chain[level]);
        if (!slot || !*slot) {
            continue;
        }
        markdown_core_nest *nest = &edits->nests[*slot - 1];
        const markdown_core_node *holder = nest->holder;
        const uint32_t tail = markdown_core_parser_tail_in(index->parser, level);
        const size_t last = tail != UINT32_MAX && holder->child_count == counts[level] ? tail : SIZE_MAX;
        const size_t marked = path[level + 1] / MARKDOWN_CORE_FOLD_CHUNK;
        nest->folds.count = nest->folds.count < marked ? nest->folds.count : marked;
        /* The passes of the chunks the reading wrote go, and all from
         * there on when the children after them moved. */
        if (last == SIZE_MAX) {
            nest->folds.pass_count = nest->folds.pass_count < marked ? nest->folds.pass_count : marked;
        }
        for (size_t chunk = marked;
             last != SIZE_MAX && chunk <= last / MARKDOWN_CORE_FOLD_CHUNK && chunk < nest->folds.pass_count; chunk++) {
            nest->folds.passes[chunk].count = 0;
        }
        if (widths_mend(index, &nest->widths, holder, path[level + 1], tail, counts[level], delta)) {
#ifndef NDEBUG
            nest_check(index, &nest->widths, holder);
#endif
        }
    }
    /* The nests a mend could not make go. */
    kept = 0;
    for (size_t i = 0; i < edits->nest_count; i++) {
        if (edits->nests[i].widths.made) {
            edits->nests[kept++] = edits->nests[i];
        } else {
            nest_free(&edits->nests[i]);
        }
    }
    if (kept != edits->nest_count) {
        edits->nest_count = kept;
        nest_reindex(edits);
    }
}

void markdown_core_index_free(markdown_core_index *index) {
    markdown_core_tree_free(&index->widths.tree);
    for (size_t i = 0; i < index->nest_count; i++) {
        nest_free(&index->nests[i]);
    }
    markdown_core_free(index->nests);
    markdown_core_free(index->slots);
    *index = (markdown_core_index){0};
}

bool markdown_core_index_make(markdown_core_index *index, const markdown_core_parser *parser,
                              const markdown_core_node *root) {
    index->parser = parser;
    index->root = root;
    return widths_make(index, &index->widths, root);
}

void markdown_core_index_drop(markdown_core_index *index) { index->widths.made = false; }

const markdown_core_widths *markdown_core_index_held(const markdown_core_index *index,
                                                     const markdown_core_node *holder) {
    const size_t *slot = nest_slot(index, holder);
    return slot && *slot && index->nests[*slot - 1].widths.made ? &index->nests[*slot - 1].widths : NULL;
}

markdown_core_folds *markdown_core_index_folds(markdown_core_index *index, const markdown_core_node *holder) {
    const size_t *slot = nest_slot(index, holder);
    return slot && *slot ? &index->nests[*slot - 1].folds : NULL;
}

bool markdown_core_index_mend(markdown_core_index *index, const markdown_core_node *root, size_t first, uint32_t tail,
                              size_t count, int64_t delta) {
    index->root = root;
    if (!widths_mend(index, &index->widths, root, first, tail, count, delta)) {
        return false;
    }
#ifndef NDEBUG
    markdown_core_index_check(index);
#endif
    return true;
}

#ifndef NDEBUG
void markdown_core_index_check(markdown_core_index *index) { nest_check(index, &index->widths, index->root); }
#endif
