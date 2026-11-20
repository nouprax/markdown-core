#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
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
        if (past_own(past, child, from, false)) {
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
#ifndef NDEBUG
    else {
    }
#endif
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
        bool exempt = false;
        for (size_t k = 0; k < count && !exempt; k++) {
            if (!(past.used >> k & 1) && (opens[k] == at || opens[k] == at - child->opened)) {
                past.used |= UINT64_C(1) << k;
                exempt = true;
            }
        }
        if (past_own(&past, child, at, exempt)) {
            if (at - child->opened < past.start) {
            }
            past_found(&past, at - child->opened);
        }
        holder = child;
        holder_start = at;
    }
    *start = past.start;
    return past.start != SIZE_MAX;
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
