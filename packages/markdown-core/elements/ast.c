#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "directive.h"
#include "formula.h"
#include "markdown-core-elements.h"
#include "strikethrough.h"
#include "table.h"

#include <node_type.h>
#include <node.h>
#include <parser.h>

typedef struct dump_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
    /* The file-tree connectors of every open nesting level. `more[depth]` says
     * whether the node drawn at `depth` has a following sibling: it decides
     * that node's own connector and the segment every line below it carries
     * for that level. `prefix` holds those segments as bytes and
     * `prefix_end[depth]` is where the segments above `depth` end, so a line
     * copies its lead-in once instead of rebuilding it a level at a time. */
    bool *more;
    uint8_t *prefix;
    size_t *prefix_end;
    size_t level_capacity;
    /* The source the scopes are computed from, and the start of each of its
     * lines, found once per dump. */
    const uint8_t *source;
    size_t *lines;
    size_t line_count;
} dump_buffer;

/* Where a Footnote or Specimen was written, for the definition tables. */
typedef struct {
    uint64_t start;
    const markdown_core_node *node;
} definition_entry;

static uint64_t definition_key(const void *entry) { return ((const definition_entry *)entry)->start; }

typedef struct {
    definition_entry *values;
    size_t count, capacity;
} definition_table;

static bool table_add(definition_table *table, const markdown_core_node *node, uint32_t start) {
    if (table->count == table->capacity) {
        size_t capacity = table->capacity ? table->capacity * 2 : 8;
        if (capacity > SIZE_MAX / sizeof(*table->values)) {
            return false;
        }
        definition_entry *values = markdown_core_realloc(table->values, capacity * sizeof(*values));
        if (!values) {
            return false;
        }
        table->values = values;
        table->capacity = capacity;
    }
    table->values[table->count++] = (definition_entry){start, node};
    return true;
}

static const markdown_core_optional_chunk *definition_label(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_FOOTNOTE ? &node->as.footnote->label : &node->as.specimen->label;
}

/* Two labels in byte order, the shorter first on a common prefix. */
static int label_compare(const uint8_t *a, size_t a_length, const uint8_t *b, size_t b_length) {
    size_t common = a_length < b_length ? a_length : b_length;
    int order = common ? memcmp(a, b, common) : 0;
    return order ? order : (a_length > b_length) - (a_length < b_length);
}

/* Labeled definitions by label, and in source order among equal labels, so
 * the first of a label is the one a lookup answers. */
static int label_entry_compare(const void *left, const void *right) {
    const definition_entry *a = left, *b = right;
    const markdown_core_optional_chunk *x = definition_label(a->node), *y = definition_label(b->node);
    int order = label_compare(x->value.data, (size_t)x->value.len, y->value.data, (size_t)y->value.len);
    return order ? order : (a->start > b->start) - (a->start < b->start);
}

/* The table in source order, and its labeled definitions in label order, as
 * node handles the document borrows. */
static inline bool table_seal(definition_table *table, markdown_core_source_order *order,
                              markdown_core_definitions *out) {
    *out = (markdown_core_definitions){0};
    if (!table->count) {
        return true;
    }
    if (!markdown_core_order_source_entries(order, table->values, table->count, sizeof(*table->values),
                                            definition_key)) {
        return false;
    }
    const markdown_core_node **nodes = markdown_core_alloc(table->count, sizeof(*nodes));
    const markdown_core_node **labeled = markdown_core_alloc(table->count, sizeof(*labeled));
    if (!nodes || !labeled) {
        markdown_core_free((void *)nodes);
        markdown_core_free((void *)labeled);
        return false;
    }
    size_t labels = 0;
    for (size_t i = 0; i < table->count; i++) {
        nodes[i] = table->values[i].node;
        /* The source index replaces the start as the tie-break, now that the
         * entries are in source order. */
        table->values[i].start = i;
        if (definition_label(nodes[i])->has_value) {
            table->values[labels++] = table->values[i];
        }
    }
    qsort(table->values, labels, sizeof(*table->values), label_entry_compare);
    for (size_t i = 0; i < labels; i++) {
        labeled[i] = table->values[i].node;
    }
    *out = (markdown_core_definitions){nodes, table->count, labeled, labels};
    return true;
}

/* A relation of the children [start, end) of `holder`, NULL when it holds
 * nothing; of all its children; of the one node a field holds. */
static bool relation_range(markdown_core_relation *relation, const char *group, const markdown_core_node *holder,
                           size_t start, size_t end) {
    *relation = (markdown_core_relation){group, NULL, (markdown_core_node *)holder, start, end};
    return true;
}

static bool relation_children(markdown_core_relation *relation, const char *group, const markdown_core_node *holder) {
    return relation_range(relation, group, holder, 0, holder ? markdown_core_node_children_count(holder) : 0);
}

static bool relation_one(markdown_core_relation *relation, markdown_core_node *const *field) {
    *relation = (markdown_core_relation){NULL, (markdown_core_node **)field, NULL, 0, 1};
    return true;
}

/* THE SHAPE of a kind's relations: most kinds own only their children; the
 * rest own fields too, in the order `markdown_core_relations_next` gives. */
typedef enum {
    SHAPE_CHILDREN = 0,
    SHAPE_DOCUMENT,
    SHAPE_TABLE,
    SHAPE_DIRECTIVE,
    SHAPE_CALLOUT,
    SHAPE_CITATION,
    SHAPE_DEFINITION
} relation_shape;

/* A kind's slot: its value, and one bit for its class. The slot holds the
 * kind's shape and, in SLOT_LOOKUP, whether the document's lookup tables
 * find nodes of it by label. */
#define SHAPE_SLOTS 64
#define SHAPE_INDEX(kind) ((((kind) >> 9) | (kind)) & 0x3f)
#define SLOT_SHAPE 0x0f
#define SLOT_LOOKUP 0x10
/* C99 has no _Static_assert: an array of negative size fails the build when a
 * condition is false. */
typedef char kind_slots_are_distinct[(MARKDOWN_CORE_NODE_KIND_COUNT <= 0x20 &&
                                      (MARKDOWN_CORE_NODE_TYPE_INLINE ^ MARKDOWN_CORE_NODE_TYPE_BLOCK) == 0x20 << 9 &&
                                      (MARKDOWN_CORE_NODE_TYPE_PRESENT >> 9 & 0x3f) == 0)
                                         ? 1
                                         : -1];
typedef char shapes_fit_their_slot[SHAPE_DEFINITION <= SLOT_SHAPE ? 1 : -1];

static const uint8_t kind_slots[SHAPE_SLOTS] = {
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DOCUMENT)] = SHAPE_DOCUMENT,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_TABLE)] = SHAPE_TABLE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DIRECTIVE)] = SHAPE_DIRECTIVE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK)] = SHAPE_DIRECTIVE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_CALLOUT)] = SHAPE_CALLOUT,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_CITATION)] = SHAPE_CITATION,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DEFINITION)] = SHAPE_DEFINITION,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_FOOTNOTE)] = SHAPE_CHILDREN | SLOT_LOOKUP,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_SPECIMEN)] = SHAPE_CHILDREN | SLOT_LOOKUP,
};

static inline unsigned slot_of(const markdown_core_node *node) { return kind_slots[SHAPE_INDEX(node->kind)]; }

static inline relation_shape shape_of(const markdown_core_node *node) {
    return (relation_shape)(slot_of(node) & SLOT_SHAPE);
}

/* Whether stepping `node`'s relations would find neither a node nor a group
 * line, read from its fields: a kind whose relations are all optional or
 * ungrouped, with nothing in them. A group the dump always draws keeps its
 * owner's relations open. */
static bool relations_empty(const markdown_core_node *node) {
    switch (shape_of(node)) {
    case SHAPE_CHILDREN:
        return !node->children;
    case SHAPE_DIRECTIVE:
        return !node->children && !markdown_core_directive_label(node);
    case SHAPE_CALLOUT:
        return !node->children && !node->as.callout->title;
    case SHAPE_DOCUMENT:
        return !node->children && !node->as.document->metadata;
    case SHAPE_TABLE:
    case SHAPE_CITATION:
    case SHAPE_DEFINITION:
        return false;
    }
    return false;
}

void markdown_core_relations_begin(markdown_core_relation_cursor *cursor, const markdown_core_node *owner) {
    *cursor = (markdown_core_relation_cursor){owner, (uint8_t)shape_of(owner), 0, 0};
}

/* Each kind's relations in canonical field order. An optional field that is
 * absent yields no relation; a group the dump always draws yields one even
 * when it is empty. `more` says whether stepping again may find another. */
static inline bool relations_next(markdown_core_relation_cursor *cursor, markdown_core_relation *relation, bool *more) {
    static const char *const table_groups[] = {"TableHead", "TableBody", "TableFoot"};
    const markdown_core_node *node = cursor->owner;
    int step = cursor->step++;
    /* An absent optional field steps on to the next relation at once. */
    switch ((relation_shape)cursor->shape) {
    case SHAPE_DOCUMENT:
        if (step == 0) {
            if (node->as.document->metadata) {
                *more = true;
                return relation_one(relation, &node->as.document->metadata);
            }
            step = cursor->step++;
        }
        *more = false;
        return step == 1 && relation_children(relation, NULL, node);
    case SHAPE_TABLE: {
        const markdown_core_table *table = node->opaque;
        if (step == 0) {
            if (table->caption) {
                *more = true;
                return relation_one(relation, &table->caption);
            }
            step = cursor->step++;
        }
        if (step > 3) {
            return false;
        }
        size_t count = step == 1 ? table->head_count : step == 2 ? table->content_count : table->foot_count;
        size_t start = cursor->at, rows = markdown_core_node_children_count(node);
        cursor->at = count < rows - start ? start + count : rows;
        *more = step < 3;
        return relation_range(relation, table_groups[step - 1], node, start, cursor->at);
    }
    case SHAPE_DIRECTIVE:
        if (step == 0) {
            if (markdown_core_directive_label(node)) {
                *more = true;
                return relation_one(relation, &((const markdown_core_directive_value *)node->opaque)->label);
            }
            step = cursor->step++;
        }
        *more = false;
        return step == 1 && relation_children(relation, NULL, node);
    case SHAPE_CALLOUT:
        if (step == 0) {
            if (node->as.callout->title) {
                *more = true;
                return relation_children(relation, "Title", node->as.callout->title);
            }
            step = cursor->step++;
        }
        *more = false;
        return step == 1 && relation_children(relation, NULL, node);
    case SHAPE_CITATION: {
        const markdown_core_citation_item *citation = node->as.citation;
        *more = true;
        if (step == 0) {
            if (citation->note) {
                return relation_one(relation, &citation->note);
            }
            step = cursor->step++;
        }
        if (step == 1) {
            return relation_children(relation, "CitationPrefix", citation->prefix);
        }
        *more = false;
        return step == 2 && relation_children(relation, "CitationSuffix", citation->suffix);
    }
    case SHAPE_DEFINITION: {
        size_t bodies = markdown_core_node_children_count(node);
        if (step == 0) {
            *more = bodies != 0;
            return relation_children(relation, "DefinitionTerm", node->as.definition->term);
        }
        if (cursor->at == bodies) {
            return false;
        }
        *more = ++cursor->at != bodies;
        return relation_children(relation, "DefinitionBody", markdown_core_node_child(node, cursor->at - 1));
    }
    case SHAPE_CHILDREN:
        *more = false;
        return step == 0 && relation_children(relation, NULL, node);
    }
    return false;
}

bool markdown_core_relations_next(markdown_core_relation_cursor *cursor, markdown_core_relation *relation) {
    bool more;
    return relations_next(cursor, relation, &more);
}

/* PUBLISHING CONTINUES THE PREVIOUS TREE (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.9). The walk that gives each node its id and extent also
 * matches it, relation by relation, to the old node it continues, and
 * decides in post-order whether it equals that old node as a value.
 *
 * Matching runs within the relation of a matched owner. An old node's anchor
 * is the first byte of its range that survived the edits, and a new node of
 * its kind whose range holds the anchor's image continues the earliest such
 * old sibling. Both relations are in source order, so one position in the
 * old relation moves forward with the new one. The root continues the old
 * root. A new node that continues nothing takes the next id.
 *
 * A matched node is SAME when its scalars and extent equal its old node's,
 * both have the same relations, every old node of each relation was matched
 * and every new node of it is same: matching is monotone and one-to-one, so
 * the old node then holds exactly those old nodes, in order, and equals the
 * new node as a value. The old tree is shared (5.11): each largest same
 * subtree's old node takes the place of its new node, which is released.
 * When the root itself is same the old tree is the result. Either way the
 * publish then drops its hold on the old tree, and only the old nodes the
 * result does not share go back to the pool. A fresh parse continues no
 * tree, so no node is matched and every node takes the next id in walk
 * order. */

/* A same pair, recorded in post-order: `old` takes the place of `node` when
 * `node` is the root of a largest same subtree. The place is the field that
 * holds `node`, or else its index among the children of `holder`. The pairs
 * of the node's subtree are those from `first` on. */
typedef struct {
    markdown_core_node *node, *old;
    markdown_core_node **field;
    markdown_core_node *holder;
    size_t index, first;
} publish_swap;

typedef struct {
    const markdown_core_byte_edit *edits;
    size_t count;
    /* shift[i] is the length change of edits [0, i). */
    int64_t *shift;
    /* Room for a pair per old node: each is matched at most once. */
    publish_swap *swaps;
    size_t swap_count;
} publish_identity;

/* The image of the first byte of [start, end) that no edit replaced, or false
 * when every byte of it was replaced (5.2). */
static bool anchor_mapped(const publish_identity *identity, uint32_t start, uint32_t end, uint32_t *mapped) {
    const markdown_core_byte_edit *edits = identity->edits;
    size_t lo = 0, hi = identity->count, x = start;
    /* The first edit that ends after x: every one before it ends at or
     * before x, so x is past it and shifted by it. */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (edits[mid].end <= x) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    while (lo < identity->count && edits[lo].start <= x) {
        if (x < edits[lo].end) {
            x = edits[lo].end;
        }
        lo++;
    }
    if (x >= end) {
        return false;
    }
    *mapped = (uint32_t)((int64_t)x + identity->shift[lo]);
    return true;
}

static bool publish_reserve(void **values, size_t *capacity, size_t count, size_t size) {
    if (count < *capacity) {
        return true;
    }
    size_t grown = *capacity ? *capacity * 2 : 16;
    if (grown > SIZE_MAX / size) {
        return false;
    }
    void *resized = markdown_core_realloc(*values, grown * size);
    if (!resized) {
        return false;
    }
    *values = resized;
    *capacity = grown;
    return true;
}

/* The relation in hand: its next node is at `relation.start`, and that
 * node's extent is relative to `anchor`. */
typedef struct {
    markdown_core_relation relation;
    uint32_t anchor;
} publish_relation;

/* A node whose relations are being published: where the node starts, its
 * cursor and whether that may hold another relation, and, while a nested
 * node's relations are published, the rest of the relation in hand. */
typedef struct {
    markdown_core_relation_cursor cursor;
    publish_relation rest;
    uint32_t start;
    bool more;
} publish_frame;

/* The frames live on the parser's walk stack, which the finish walk has
 * just left. */
typedef struct {
    markdown_core_parser *parser;
    publish_frame *frames;
    size_t count, capacity;
} publish_stack;

static bool publish_grow(publish_stack *stack) {
    size_t capacity = stack->capacity ? stack->capacity * 2 : 16;
    publish_frame *frames = markdown_core_parser_walk_stack(stack->parser, capacity, sizeof(*frames));
    if (!frames) {
        return false;
    }
    stack->frames = frames;
    stack->capacity = stack->parser->walk_stack_size / sizeof(*frames);
    return true;
}

/* `node`'s first relation, which holds a node or a group, and whether more
 * may follow it: the one relation of a kind that owns only its children, or
 * the first its cursor finds. False when they hold no node. */
static inline bool publish_first(const markdown_core_node *node, relation_shape shape,
                                 markdown_core_relation_cursor *cursor, markdown_core_relation *first, bool *more) {
    if (shape == SHAPE_CHILDREN) {
        relation_children(first, NULL, node);
        *more = false;
        return first->end != 0;
    }
    *cursor = (markdown_core_relation_cursor){node, (uint8_t)shape, 0, 0};
    if (!relations_next(cursor, first, more)) {
        return false;
    }
    return first->start != first->end || *more;
}

/* What one publish carries through its walks: the walk stack, what matching
 * needs, the lookup tables, the last id issued and the nodes matched. Every
 * other node takes an id, so the published tree counts the ids issued and
 * the nodes matched. */
typedef struct {
    publish_stack stack;
    publish_identity identity;
    definition_table footnotes, specimens;
    uint64_t next_id;
    size_t matched;
} publish_walk;

/* Gives `node` its id and its extent against `anchor`, and records it in its
 * lookup table when its slot says the tables find it. Returns where it
 * ended. */
static inline bool publish_node(publish_walk *walk, markdown_core_node *node, unsigned slot, uint32_t anchor,
                                uint64_t id, markdown_core_place *place) {
    *place = node->where.place;
    node->id = id;
    node->where.extent =
        (markdown_core_extent){(int32_t)((int64_t)place->start - (int64_t)anchor), place->end - place->start};
    if (!(slot & SLOT_LOOKUP)) {
        return true;
    }
    return table_add(node->kind == MARKDOWN_CORE_NODE_FOOTNOTE ? &walk->footnotes : &walk->specimens, node,
                     place->start);
}

/* PUBLISHING WHAT CONTINUES NOTHING: every node below `node`, which starts
 * at `start`, takes the next id in canonical walk order: a node, then the
 * nodes of its relations in order, with one frame per node whose relations
 * are open. A node's place becomes its extent as it is published, so each
 * frame keeps the offsets its later nodes are relative to, and a frame with
 * nothing left gives its slot to its last node's own. The frames sit above
 * those already on the stack. A fresh parse publishes its whole tree this
 * way, below the root. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool publish_fresh(publish_walk *walk,
                                                                          const markdown_core_node *node,
                                                                          relation_shape shape, uint32_t start) {
    publish_stack *stack = &walk->stack;
    /* The walk ends with the stack as it found it, so its count and the ids
     * it issues stay in hand until then. */
    size_t base = stack->count, count = base;
    uint64_t next_id = walk->next_id;
    markdown_core_relation_cursor cursor;
    markdown_core_relation first;
    publish_relation hand;
    markdown_core_place place;
    bool more;
    if (!publish_first(node, shape, &cursor, &first, &more)) {
        return true;
    }
    if (count == stack->capacity && !publish_grow(stack)) {
        return false;
    }
    publish_frame *frame = &stack->frames[count++];
    frame->cursor = cursor;
    frame->start = start;
    frame->more = more;
    hand = (publish_relation){first, start};
    while (count > base) {
        if (hand.relation.start == hand.relation.end) {
            if (frame->more && relations_next(&frame->cursor, &hand.relation, &frame->more)) {
                hand.anchor = frame->start;
            } else if (--count > base) {
                frame--;
                hand = frame->rest;
            }
            continue;
        }
        markdown_core_node *child = markdown_core_relation_node(&hand.relation, hand.relation.start++);
        unsigned slot = slot_of(child);
        if (!publish_node(walk, child, slot, hand.anchor, ++next_id, &place)) {
            return false;
        }
        hand.anchor = place.end;
        if (!publish_first(child, (relation_shape)(slot & SLOT_SHAPE), &cursor, &first, &more)) {
            continue;
        }
        /* A frame with nothing left gives its slot to the node's own; any
         * other waits with the relation in hand. */
        if (hand.relation.start != hand.relation.end || frame->more) {
            frame->rest = hand;
            if (count == stack->capacity) {
                if (!publish_grow(stack)) {
                    return false;
                }
                frame = &stack->frames[count - 1];
            }
            frame++;
            count++;
        }
        frame->start = place.start;
        frame->more = more;
        if (more) {
            frame->cursor = cursor;
        }
        hand = (publish_relation){first, place.start};
    }
    walk->next_id = next_id;
    return true;
}

/* A matched node whose relations are being published: its frame of the
 * walk, the node, its place and the old node it continues, the old node's
 * relations not yet paired, the old relation paired with the one in hand --
 * its next old node, and the end of the node before that (the old node's
 * start for the first) -- and whether the node is same so far. */
typedef struct {
    publish_frame walk;
    markdown_core_node *node, *old;
    markdown_core_node **field;
    markdown_core_node *holder;
    size_t index;
    bool same;
    markdown_core_relation_cursor old_cursor;
    markdown_core_relation old_relation;
    /* The old relation's field, or INT_MAX when the old node has no more. */
    int old_field;
    /* The old relation paired with the one in hand; its next node is at
     * `start`, or it is empty when none pairs. */
    markdown_core_relation old_hand;
    uint32_t old_start, old_anchor;
    /* The swaps pending when the node was entered, and its lookup table
     * entry counted from 1 (0 for none). */
    size_t swaps, entry;
} publish_match_frame;

/* The field a cursor's last relation holds: stepping leaves the cursor one
 * past it. A kind that owns only its children has field 0. */
static inline int relation_index(const markdown_core_relation_cursor *cursor, relation_shape shape) {
    return shape == SHAPE_CHILDREN ? 0 : cursor->step - 1;
}

static void old_relation_next(publish_match_frame *frame) {
    bool more;
    frame->old_field =
        relations_next(&frame->old_cursor, &frame->old_relation, &more) ? frame->old_cursor.step - 1 : INT_MAX;
}

/* Pairs the new node's relation `field` with the old node's. A relation
 * either node has and the other lacks makes the node differ. */
static void publish_pair(publish_match_frame *frame, int field) {
    while (frame->old_field < field) {
        frame->same = false;
        old_relation_next(frame);
    }
    frame->old_anchor = frame->old_start;
    if (frame->old_field == field) {
        frame->old_hand = frame->old_relation;
        old_relation_next(frame);
    } else {
        frame->same = false;
        frame->old_hand = (markdown_core_relation){0};
    }
}

/* The old node `node` continues in the old relation `owner` pairs with the
 * one in hand, stepping past every old node whose anchor's image lies before
 * `node`'s end. An old node stepped past without being matched makes the
 * owner differ. */
static markdown_core_node *publish_match(const publish_identity *identity, publish_match_frame *owner,
                                         const markdown_core_node *node, uint32_t *old_start) {
    markdown_core_place place = node->where.place;
    markdown_core_node *match = NULL;
    markdown_core_relation *old_hand = &owner->old_hand;
    while (old_hand->start != old_hand->end) {
        markdown_core_node *old = markdown_core_relation_node(old_hand, old_hand->start);
        uint32_t start = (uint32_t)((int64_t)owner->old_anchor + old->where.extent.lead);
        uint32_t end = start + old->where.extent.span, mapped;
        bool anchored = anchor_mapped(identity, start, end, &mapped);
        if (anchored && mapped >= place.end) {
            break;
        }
        if (anchored && mapped >= place.start && !match && old->kind == node->kind) {
            match = old;
            *old_start = start;
        } else {
            owner->same = false;
        }
        owner->old_anchor = end;
        old_hand->start++;
    }
    if (!match) {
        owner->same = false;
    }
    return match;
}

static bool scalars_equal(const markdown_core_node *a, const markdown_core_node *b);

/* Whether a matched node equals its old node as a value, before their
 * relations are compared. */
static bool publish_same(const markdown_core_node *node, const markdown_core_node *old) {
    return node->where.extent.lead == old->where.extent.lead && node->where.extent.span == old->where.extent.span &&
           scalars_equal(node, old);
}

/* A matched node's verdict, once its relations are published: a same node
 * is recorded after the pairs of its subtree, from `swaps` on, with its
 * place, and the lookup tables name the old node; a node that differs makes
 * its owner differ. */
static void publish_verdict(publish_walk *walk, publish_match_frame *owner, const publish_swap *swap, bool same,
                            size_t entry) {
    publish_identity *identity = &walk->identity;
    if (!same) {
        owner->same = false;
        return;
    }
    identity->swaps[identity->swap_count++] = *swap;
    if (entry) {
        (swap->node->kind == MARKDOWN_CORE_NODE_FOOTNOTE ? &walk->footnotes : &walk->specimens)
            ->values[entry - 1]
            .node = swap->old;
    }
}

/* The length change before each edit, which anchor images shift by, and
 * room for a same pair per old node. */
static bool publish_prepare(publish_identity *identity, size_t old_count) {
    identity->shift = markdown_core_alloc(identity->count + 1, sizeof(*identity->shift));
    identity->swaps = markdown_core_realloc(NULL, old_count * sizeof(*identity->swaps));
    if (!identity->shift || !identity->swaps) {
        return false;
    }
    for (size_t i = 0; i < identity->count; i++) {
        const markdown_core_byte_edit *edit = &identity->edits[i];
        identity->shift[i + 1] = identity->shift[i] + (int64_t)edit->size - (int64_t)(edit->end - edit->start);
    }
    return true;
}

/* PUBLISHING WHAT CONTINUES THE PREVIOUS TREE: the root continues the
 * previous root, and each matched node pairs its relations with its old
 * node's and matches their nodes in turn, one frame per matched node whose
 * relations are open, each waiting for its verdict. A node that continues
 * nothing takes the next id, and so does everything below it, which
 * publish_fresh publishes. `*same` is the root's verdict. */
static bool publish_matched(publish_walk *walk, markdown_core_node *root, markdown_core_node *previous, bool *same) {
    publish_identity *identity = &walk->identity;
    publish_match_frame *frames = NULL, *frame;
    size_t count = 0, capacity = 0;
    markdown_core_relation_cursor cursor, old_cursor;
    markdown_core_relation first, old_first;
    publish_relation hand;
    markdown_core_place place;
    bool more, old_more, ok = true;
    unsigned slot = slot_of(root);
    relation_shape shape = (relation_shape)(slot & SLOT_SHAPE);
    if (!publish_node(walk, root, slot, 0, previous->id, &place)) {
        return false;
    }
    walk->matched++;
    /* Whether the node about to be entered is same so far. */
    bool verdict = publish_same(root, previous);
    if (!publish_first(root, shape, &cursor, &first, &more)) {
        *same = verdict && !publish_first(previous, shape, &old_cursor, &old_first, &old_more);
        return true;
    }
    publish_swap at = {root, previous, NULL, NULL, 0, 0};
    uint32_t old_start = 0;
    size_t entry = 0;
    /* Enters a matched node that has relations: its frame waits for its
     * verdict while they are published. */
    for (;;) {
        if (!publish_reserve((void **)&frames, &capacity, count, sizeof(*frames))) {
            ok = false;
            break;
        }
        frame = &frames[count++];
        *frame = (publish_match_frame){
            .walk = {.cursor = cursor, .start = place.start, .more = more},
            .node = at.node,
            .old = at.old,
            .field = at.field,
            .holder = at.holder,
            .index = at.index,
            .same = verdict,
            .old_start = old_start,
            .swaps = identity->swap_count,
            .entry = entry,
        };
        hand = (publish_relation){first, place.start};
        markdown_core_relations_begin(&frame->old_cursor, at.old);
        old_relation_next(frame);
        publish_pair(frame, relation_index(&cursor, shape));
        /* The next matched node with relations, or the end of the walk. */
        for (at.node = NULL; ok && count && !at.node;) {
            if (hand.relation.start == hand.relation.end) {
                if (frame->old_hand.start != frame->old_hand.end) {
                    frame->same = false;
                }
                if (frame->walk.more && relations_next(&frame->walk.cursor, &hand.relation, &frame->walk.more)) {
                    hand.anchor = frame->walk.start;
                    publish_pair(frame, frame->walk.cursor.step - 1);
                    continue;
                }
                if (frame->old_field != INT_MAX) {
                    frame->same = false;
                }
                if (!--count) {
                    *same = frame->same;
                    break;
                }
                publish_verdict(
                    walk, frame - 1,
                    &(publish_swap){frame->node, frame->old, frame->field, frame->holder, frame->index, frame->swaps},
                    frame->same, frame->entry);
                frame--;
                hand = frame->walk.rest;
                continue;
            }
            size_t index = hand.relation.start++;
            markdown_core_node *child = markdown_core_relation_node(&hand.relation, index);
            slot = slot_of(child);
            shape = (relation_shape)(slot & SLOT_SHAPE);
            markdown_core_node *match = publish_match(identity, frame, child, &old_start);
            ok = publish_node(walk, child, slot, hand.anchor, match ? match->id : ++walk->next_id, &place);
            hand.anchor = place.end;
            if (!ok) {
                break;
            }
            if (!match) {
                ok = publish_fresh(walk, child, shape, place.start);
                continue;
            }
            walk->matched++;
            definition_table *table = child->kind == MARKDOWN_CORE_NODE_FOOTNOTE ? &walk->footnotes : &walk->specimens;
            entry = slot & SLOT_LOOKUP ? table->count : 0;
            verdict = publish_same(child, match);
            at = (publish_swap){child, match, hand.relation.field, hand.relation.holder, index, identity->swap_count};
            if (!publish_first(child, shape, &cursor, &first, &more)) {
                verdict = verdict && !publish_first(match, shape, &old_cursor, &old_first, &old_more);
                publish_verdict(walk, frame, &at, verdict, entry);
                at.node = NULL;
                continue;
            }
            frame->walk.rest = hand;
        }
        if (!ok || !at.node) {
            break;
        }
    }
    markdown_core_free(frames);
    return ok;
}

/* Puts each largest same subtree's old node in place of its new node: the
 * pairs backwards, where a pair past the last root's first pair is the root
 * of a largest same subtree. False when copying a shared run failed. */
static bool publish_share(markdown_core_parser *parser, const publish_identity *identity) {
    size_t roots = identity->swap_count;
    for (size_t i = identity->swap_count; i--;) {
        const publish_swap *swap = &identity->swaps[i];
        if (i >= roots) {
            continue;
        }
        markdown_core_node *replaced = swap->node;
        markdown_core_node_retain(swap->old);
        if (swap->field) {
            *swap->field = swap->old;
        } else if (!markdown_core_children_replace(parser->pool, &swap->holder->children, swap->index, swap->old,
                                                   &replaced)) {
            markdown_core_node_pool_release(parser->pool, swap->old);
            return false;
        }
        parser->nodes_freed += markdown_core_node_pool_release(parser->pool, replaced);
        roots = swap->first;
    }
    return true;
}

bool markdown_core_publish_tree(markdown_core_parser *parser) {
    markdown_core_revision *revision = parser->revision;
    markdown_core_node *root = parser->root, *previous = revision->previous;
    markdown_core_document_value *value = root->as.document;
    publish_walk walk;
    walk.stack = (publish_stack){parser, parser->walk_stack, 0, parser->walk_stack_size / sizeof(publish_frame)};
    walk.footnotes = walk.specimens = (definition_table){0};
    walk.next_id = revision->last_id;
    walk.matched = 0;
    markdown_core_place place;
    bool ok, same = false;
    if (previous) {
        walk.identity = (publish_identity){revision->edits, revision->edit_count, NULL, NULL, 0};
        ok = publish_prepare(&walk.identity, revision->node_count) && publish_matched(&walk, root, previous, &same);
    } else {
        unsigned slot = slot_of(root);
        ok = publish_node(&walk, root, slot, 0, ++walk.next_id, &place) &&
             publish_fresh(&walk, root, (relation_shape)(slot & SLOT_SHAPE), place.start);
    }
    if (ok && !same) {
        ok = table_seal(&walk.footnotes, &parser->source_order, &value->footnotes) &&
             table_seal(&walk.specimens, &parser->source_order, &value->specimens);
    }
    if (ok && previous && !same) {
        ok = publish_share(parser, &walk.identity);
    }
    /* Nothing below can fail: the result is committed, and it holds the
     * revision's hold on the previous tree or drops it. */
    if (ok && previous) {
        if (same) {
            parser->root = previous;
            markdown_core_node_pool_release(parser->pool, root);
        } else {
            markdown_core_node_pool_release(parser->pool, previous);
        }
    }
    if (ok) {
        revision->node_count = (size_t)(walk.next_id - revision->last_id) + walk.matched;
        revision->last_id = walk.next_id;
    }
    markdown_core_free(walk.footnotes.values);
    markdown_core_free(walk.specimens.values);
    if (previous) {
        markdown_core_free(walk.identity.shift);
        markdown_core_free(walk.identity.swaps);
    }
    return ok;
}

void markdown_core_document_free(markdown_core_document *document) {
    if (!document) {
        return;
    }
    markdown_core_node_free(document->root);
    markdown_core_free(document);
}

markdown_core_text_unit markdown_core_document_unit(const markdown_core_document *document) { return document->unit; }

const markdown_core_node *markdown_core_document_root(const markdown_core_document *document) { return document->root; }

/* The facade's view of the native types: the public kind each reports, and
 * each public kind's name. Generated from packages/markdown-core/node-types.json
 * and docs/specs/canonical-ast.json. */
/* BEGIN GENERATED by scripts/tooling/generate-node-kinds.mjs; edit the node-kind schemas instead. */
/* The public kind each native type reports; a private type reads as NONE. */
static const markdown_core_node_kind S_block_kind[MARKDOWN_CORE_NODE_KIND_COUNT] = {
    [MARKDOWN_CORE_NODE_DOCUMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_DOCUMENT,
    [MARKDOWN_CORE_NODE_CALLOUT & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_CALLOUT,
    [MARKDOWN_CORE_NODE_LIST & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_LIST,
    [MARKDOWN_CORE_NODE_LIST_ITEM & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_LIST_ITEM,
    [MARKDOWN_CORE_NODE_CODE_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_CODE_BLOCK,
    [MARKDOWN_CORE_NODE_HTML_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_HTML_BLOCK,
    [MARKDOWN_CORE_NODE_PARAGRAPH & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_PARAGRAPH,
    [MARKDOWN_CORE_NODE_HEADING & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_HEADING,
    [MARKDOWN_CORE_NODE_THEMATIC_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_THEMATIC_BREAK,
    [MARKDOWN_CORE_NODE_FOOTNOTE & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_FOOTNOTE,
    [MARKDOWN_CORE_NODE_TABLE & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_TABLE,
    [MARKDOWN_CORE_NODE_TABLE_ROW & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_TABLE_ROW,
    [MARKDOWN_CORE_NODE_TABLE_CELL & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_TABLE_CELL,
    [MARKDOWN_CORE_NODE_FORMULA_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_FORMULA_BLOCK,
    [MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK,
    [MARKDOWN_CORE_NODE_COMMENT_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_COMMENT,
    [MARKDOWN_CORE_NODE_SPECIMEN & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_SPECIMEN,
    [MARKDOWN_CORE_NODE_DEFINITION_LIST & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_DEFINITION_LIST,
    [MARKDOWN_CORE_NODE_DEFINITION & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_DEFINITION,
    [MARKDOWN_CORE_NODE_TABLE_CAPTION & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_TABLE_CAPTION,
    [MARKDOWN_CORE_NODE_METADATA & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_METADATA,
};

static const markdown_core_node_kind S_inline_kind[MARKDOWN_CORE_NODE_KIND_COUNT] = {
    [MARKDOWN_CORE_NODE_TEXT & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_TEXT,
    [MARKDOWN_CORE_NODE_SOFT_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_SOFT_BREAK,
    [MARKDOWN_CORE_NODE_LINE_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_LINE_BREAK,
    [MARKDOWN_CORE_NODE_CODE & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_CODE,
    [MARKDOWN_CORE_NODE_HTML & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_HTML,
    [MARKDOWN_CORE_NODE_EMPHASIS & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_EMPHASIS,
    [MARKDOWN_CORE_NODE_STRONG & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_STRONG,
    [MARKDOWN_CORE_NODE_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_LINK,
    [MARKDOWN_CORE_NODE_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_EMBEDDED,
    [MARKDOWN_CORE_NODE_CITE & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_CITE,
    [MARKDOWN_CORE_NODE_STRIKETHROUGH & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_STRIKETHROUGH,
    [MARKDOWN_CORE_NODE_FORMULA & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_FORMULA,
    [MARKDOWN_CORE_NODE_DIRECTIVE & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_DIRECTIVE,
    [MARKDOWN_CORE_NODE_DIRECTIVE_LABEL & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_DIRECTIVE_LABEL,
    [MARKDOWN_CORE_NODE_COMMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_COMMENT,
    [MARKDOWN_CORE_NODE_CITATION & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_CITATION,
    [MARKDOWN_CORE_NODE_CROSS_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_CROSS_LINK,
    [MARKDOWN_CORE_NODE_MARK & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_MARK,
    [MARKDOWN_CORE_NODE_CROSS_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_CROSS_EMBEDDED,
    [MARKDOWN_CORE_NODE_INSERTION & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_INSERTION,
    [MARKDOWN_CORE_NODE_SPAN & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_SPAN,
    [MARKDOWN_CORE_NODE_SUPERSCRIPT & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_SUPERSCRIPT,
    [MARKDOWN_CORE_NODE_SUBSCRIPT & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_SUBSCRIPT,
};

/* clang-format off */
static const char *const S_kind_name[] = {
    "None",
    "Document",
    "Callout",
    "Paragraph",
    "Heading",
    "ThematicBreak",
    "List",
    "ListItem",
    "CodeBlock",
    "HTMLBlock",
    "FormulaBlock",
    "Table",
    "DirectiveBlock",
    "Text",
    "SoftBreak",
    "LineBreak",
    "Code",
    "HTML",
    "Formula",
    "Emphasis",
    "Strong",
    "Strikethrough",
    "Link",
    "Embedded",
    "Directive",
    "Cite",
    "TableRow",
    "TableCell",
    "DirectiveLabel",
    "Comment",
    "CrossLink",
    "Mark",
    "CrossEmbedded",
    "Insertion",
    "Span",
    "Superscript",
    "Subscript",
    "DefinitionList",
    "Definition",
    "TableCaption",
    "Citation",
    "Footnote",
    "Specimen",
    "Metadata",
};
/* clang-format on */
/* END GENERATED */

static inline markdown_core_node_kind public_kind(const markdown_core_node *node) {
    unsigned index = (unsigned)node->kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    return ((unsigned)node->kind & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_INLINE
               ? S_inline_kind[index]
               : S_block_kind[index];
}

markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node) { return public_kind(node); }

markdown_core_status markdown_core_node_kind_name(markdown_core_node_kind kind, const char **name) {
    if ((unsigned)kind >= sizeof(S_kind_name) / sizeof(*S_kind_name)) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *name = S_kind_name[kind];
    return MARKDOWN_CORE_OK;
}

/* THE KIND CHECK of a kind-specific accessor, the one check its public entry
 * makes: whether `node`'s public kind is one of `kinds`, a set of
 * KIND_BIT()s. Every public kind is below 64. */
#define KIND_BIT(kind) (UINT64_C(1) << (kind))
typedef char kinds_fit_a_set[MARKDOWN_CORE_KIND_METADATA < 64 ? 1 : -1];

static inline bool node_is(const markdown_core_node *node, uint64_t kinds) {
    return (kinds >> public_kind(node) & 1) != 0;
}

uint64_t markdown_core_node_id(const markdown_core_node *node) { return node->id; }

markdown_core_extent markdown_core_node_extent(const markdown_core_node *node) { return node->where.extent; }

void markdown_core_walk_begin(markdown_core_walk *walk, const markdown_core_node *root) {
    *walk = (markdown_core_walk){root, 0, false, false, false, NULL, 0, 0, 0};
}

void markdown_core_walk_end(markdown_core_walk *walk) {
    markdown_core_free(walk->frames);
    walk->frames = NULL;
    walk->count = walk->capacity = 0;
}

/* Where `node` is, given the offset its extent is relative to. */
static markdown_core_place walk_place(const markdown_core_node *node, uint32_t anchor) {
    markdown_core_extent extent = node->where.extent;
    markdown_core_place place;
    place.start = (uint32_t)((int64_t)anchor + extent.lead);
    place.end = place.start + extent.span;
    return place;
}

/* A node whose relations hold nothing takes no frame. */
static bool walk_push(markdown_core_walk *walk, const markdown_core_node *node, size_t level, uint32_t start) {
    if (relations_empty(node)) {
        return true;
    }
    if (walk->count == walk->capacity) {
        size_t capacity = walk->capacity ? walk->capacity * 2 : 32;
        markdown_core_walk_frame *frames;
        if (capacity > SIZE_MAX / sizeof(*frames) ||
            !(frames = markdown_core_realloc(walk->frames, capacity * sizeof(*frames)))) {
            walk->failed = true;
            return false;
        }
        walk->frames = frames;
        walk->capacity = capacity;
    }
    markdown_core_walk_frame *frame = &walk->frames[walk->count++];
    frame->level = level;
    frame->active = false;
    frame->owner_start = start;
    markdown_core_relations_begin(&frame->cursor, node);
    return true;
}

/* Whether the owner of `frame` has another line at its own level after the
 * current relation: a later group, or a later relation with nodes. */
static bool walk_more_after(const markdown_core_walk_frame *frame) {
    markdown_core_relation_cursor cursor = frame->cursor;
    markdown_core_relation relation;
    while (markdown_core_relations_next(&cursor, &relation)) {
        if (relation.group || relation.start != relation.end) {
            return true;
        }
    }
    return false;
}

bool markdown_core_walk_next(markdown_core_walk *walk, markdown_core_walk_item *item) {
    if (walk->failed) {
        return false;
    }
    if (!walk->started) {
        walk->started = true;
        markdown_core_place place = walk_place(walk->root, walk->anchor);
        *item = (markdown_core_walk_item){walk->root, place, NULL, 0, 0};
        return walk_push(walk, walk->root, 0, place.start);
    }
    while (walk->count) {
        markdown_core_walk_frame *frame = &walk->frames[walk->count - 1];
        if (!frame->active) {
            if (!markdown_core_relations_next(&frame->cursor, &frame->relation)) {
                walk->count--;
                continue;
            }
            frame->active = true;
            frame->group_pending = frame->relation.group != NULL;
            frame->anchor = frame->owner_start;
        }
        if (frame->group_pending) {
            frame->group_pending = false;
            *item = (markdown_core_walk_item){
                NULL, {0, 0}, frame->relation.group, markdown_core_relation_count(&frame->relation), frame->level + 1};
            walk->at_group = true;
            walk->owner = walk->count;
            return true;
        }
        if (frame->relation.start != frame->relation.end) {
            markdown_core_node *node = markdown_core_relation_node(&frame->relation, frame->relation.start++);
            markdown_core_place place = walk_place(node, frame->anchor);
            frame->anchor = place.end;
            size_t level = frame->level + (frame->relation.group ? 2 : 1);
            *item = (markdown_core_walk_item){node, place, NULL, 0, level};
            walk->at_group = false;
            walk->owner = walk->count;
            return walk_push(walk, node, level, place.start);
        }
        frame->active = false;
    }
    return false;
}

bool markdown_core_walk_has_next(const markdown_core_walk *walk) {
    if (!walk->owner) {
        return false;
    }
    const markdown_core_walk_frame *owner = &walk->frames[walk->owner - 1];
    /* A group's own nodes follow it one level down, so what follows it at its
     * own level is the owner's next relation. */
    if (walk->at_group) {
        return walk_more_after(owner);
    }
    return owner->relation.start != owner->relation.end || (!owner->relation.group && walk_more_after(owner));
}

/* THE LINES OF A SOURCE: the offset each begins at. A line ends after LF,
 * after CR, or after CRLF, which is one terminator. */
typedef struct {
    size_t *starts;
    size_t count;
} source_lines;

static bool source_lines_read(source_lines *lines, const uint8_t *source, size_t length) {
    size_t count = 1;
    for (size_t i = 0; i < length; i++) {
        if (source[i] == '\n' || (source[i] == '\r' && !(i + 1 < length && source[i + 1] == '\n'))) {
            count++;
        }
    }
    lines->starts = markdown_core_alloc(count, sizeof(*lines->starts));
    if (!lines->starts) {
        return false;
    }
    lines->count = 0;
    lines->starts[lines->count++] = 0;
    for (size_t i = 0; i < length; i++) {
        if (source[i] == '\n' || (source[i] == '\r' && !(i + 1 < length && source[i + 1] == '\n'))) {
            lines->starts[lines->count++] = i + 1;
        }
    }
    return true;
}

/* The index of the line holding `offset`: the last line starting at or
 * before it. */
static size_t source_line_of(const source_lines *lines, size_t offset) {
    size_t lo = 0, hi = lines->count;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (lines->starts[mid] <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* The columns between two offsets of one line, in `unit`. A UTF-8 byte that
 * begins a four-byte scalar is two UTF-16 units; a continuation byte is none. */
static size_t source_columns(const uint8_t *source, size_t from, size_t to, markdown_core_text_unit unit) {
    if (unit == MARKDOWN_CORE_TEXT_UNIT_UTF8) {
        return to - from;
    }
    size_t units = 0;
    for (size_t i = from; i < to; i++) {
        uint8_t byte = source[i];
        units += (byte & 0xC0) == 0x80 ? 0 : byte >= 0xF0 ? 2 : 1;
    }
    return units;
}

/* A byte range as editor coordinates: the start is the position of its first
 * byte, and the end the line holding its exclusive end and the columns from
 * that line's start to it. */
static markdown_core_scope source_scope(const source_lines *lines, const uint8_t *source, markdown_core_place place,
                                        markdown_core_text_unit unit) {
    size_t start_line = source_line_of(lines, place.start), end_line = source_line_of(lines, place.end);
    markdown_core_scope scope;
    scope.start.line = (int32_t)(start_line + 1);
    scope.start.column = (int32_t)(source_columns(source, lines->starts[start_line], place.start, unit) + 1);
    scope.end.line = (int32_t)(end_line + 1);
    scope.end.column = (int32_t)source_columns(source, lines->starts[end_line], place.end, unit);
    return scope;
}

/* The absolute range of `target`, a node of the tree `root`, found by one
 * canonical walk; false when the walk could not allocate its frames. */
static bool tree_place(const markdown_core_node *root, const markdown_core_node *target, markdown_core_place *place) {
    markdown_core_walk walk;
    markdown_core_walk_item item;
    markdown_core_walk_begin(&walk, root);
    while (markdown_core_walk_next(&walk, &item)) {
        if (item.node == target) {
            *place = item.place;
            break;
        }
    }
    bool ran = !walk.failed;
    markdown_core_walk_end(&walk);
    return ran;
}

/* The scope of `place`, a range within `length` bytes of `source`; false
 * when the line table could not be allocated. */
static bool place_scope(const uint8_t *source, size_t length, markdown_core_place place, markdown_core_text_unit unit,
                        markdown_core_scope *scope) {
    source_lines lines;
    if (!source_lines_read(&lines, source, length)) {
        return false;
    }
    *scope = source_scope(&lines, source, place, unit);
    markdown_core_free(lines.starts);
    return true;
}

bool markdown_core_tree_scope(const markdown_core_node *root, const markdown_core_node *node, const uint8_t *source,
                              size_t length, markdown_core_text_unit unit, markdown_core_scope *scope) {
    markdown_core_place place = {0};
    return tree_place(root, node, &place) && place_scope(source, length, place, unit, scope);
}

/* The source must cover the node: counting its columns reads every byte of
 * its lines up to its end. */
markdown_core_status markdown_core_document_scope(const markdown_core_document *document,
                                                  const markdown_core_node *node, const uint8_t *source, size_t length,
                                                  markdown_core_scope *scope) {
    markdown_core_place place = {0};
    if (!tree_place(document->root, node, &place)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    if (place.end > length) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    return place_scope(source, length, place, document->unit, scope) ? MARKDOWN_CORE_OK
                                                                     : MARKDOWN_CORE_ALLOCATION_FAILED;
}

/* A position is a line and a column counted from 1: one below either names
 * nothing a source could hold. A position past the source, or inside a
 * scalar, is a real position no node holds. */
markdown_core_status markdown_core_document_node_at(const markdown_core_document *document,
                                                    markdown_core_position position, const uint8_t *source,
                                                    size_t length, const markdown_core_node **node) {
    if (position.line < 1 || position.column < 1) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    source_lines lines;
    if (!source_lines_read(&lines, source, length)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    size_t line = (size_t)position.line - 1;
    bool held = line < lines.count;
    size_t offset = held ? lines.starts[line] : 0;
    size_t end = held && line + 1 < lines.count ? lines.starts[line + 1] : length;
    /* Step over the line's scalars up to the column; the position must land
     * on a byte of the line, at a scalar boundary. */
    int64_t column = 1;
    while (held && column < position.column && offset < end) {
        size_t next = offset + 1;
        while (next < end && (source[next] & 0xC0) == 0x80) {
            next++;
        }
        column += (int64_t)source_columns(source, offset, next, document->unit);
        offset = next;
    }
    markdown_core_free(lines.starts);
    const markdown_core_node *found = NULL;
    if (held && column == position.column && offset < end) {
        markdown_core_walk walk;
        markdown_core_walk_item item;
        markdown_core_walk_begin(&walk, document->root);
        while (markdown_core_walk_next(&walk, &item)) {
            if (item.node && item.place.start <= offset && offset < item.place.end) {
                found = item.node;
            }
        }
        bool failed = walk.failed;
        markdown_core_walk_end(&walk);
        if (failed) {
            return MARKDOWN_CORE_ALLOCATION_FAILED;
        }
    }
    *node = found;
    return MARKDOWN_CORE_OK;
}

/* A sequence is the children of the node that holds it: the run at the root
 * of their children tree (children.h). */
static const markdown_core_nodes *sequence_of(const markdown_core_node *holder) {
    return holder ? (const markdown_core_nodes *)holder->children : NULL;
}

size_t markdown_core_nodes_count(const markdown_core_nodes *nodes) {
    return markdown_core_children_count((const markdown_core_run *)nodes);
}

markdown_core_status markdown_core_nodes_at(const markdown_core_nodes *nodes, size_t index,
                                            const markdown_core_node **node) {
    if (index >= markdown_core_nodes_count(nodes)) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *node = markdown_core_children_at((const markdown_core_run *)nodes, index);
    return MARKDOWN_CORE_OK;
}

const markdown_core_nodes *markdown_core_node_children(const markdown_core_node *node) {
    return node->kind != MARKDOWN_CORE_NODE_DEFINITION ? sequence_of(node) : NULL;
}

/* THE READERS. Each reads a field of the kind it is named for, and the
 * canonical dump calls it on a node whose kind it has already switched on.
 * The public accessor below each is the one place its kind is checked. */

/* The chunk's bytes are LENT, not copied: the string points into the document
 * and dies with it, which is what `markdown_core_string` documents. */
static markdown_core_string chunk_string(markdown_core_chunk chunk) {
    return (markdown_core_string){chunk.data, (size_t)chunk.len};
}

/* THE FACADE FOLDS NOTHING (requirement 14). It carries the presence the
 * engine recorded and does not re-derive it from a length or a pointer. */
static markdown_core_optional_string optional_chunk_string(markdown_core_optional_chunk chunk) {
    return (markdown_core_optional_string){chunk.has_value, chunk_string(chunk.value)};
}

static markdown_core_string cstr_string(const char *value) {
    return (markdown_core_string){(const uint8_t *)value, strlen(value)};
}

static void list_properties(const markdown_core_node *node, markdown_core_list_flavor *flavor,
                            markdown_core_optional_i64 *start, markdown_core_ordered_list_variant *variant,
                            markdown_core_ordered_list_delimiter *delimiter, bool *tight) {
    *flavor = node->as.list->flavor;
    start->has_value = *flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED;
    start->value = node->as.list->start;
    *variant = node->as.list->variant;
    *delimiter = node->as.list->delimiter;
    *tight = node->as.list->tight;
}

static void code_block_properties(const markdown_core_node *node, markdown_core_optional_string *info,
                                  markdown_core_optional_string *language, markdown_core_string *literal, bool *fenced,
                                  bool *closed) {
    size_t start = 0;
    size_t end;
    *info = optional_chunk_string(node->as.code->info);
    *literal = chunk_string(node->as.code->literal);
    /* `if (info->length == 0) info->data = NULL;` STOOD HERE, and it is the
     * fold requirement 14 names: the parse had already decided whether a fence
     * wrote an info string, and this line decided it again from a length. */
    language->has_value = false;
    language->value.data = NULL;
    language->value.length = 0;
    while (start < info->value.length && markdown_core_is_whitespace(info->value.data[start])) {
        start++;
    }
    end = start;
    while (end < info->value.length && !markdown_core_is_whitespace(info->value.data[end])) {
        end++;
    }
    if (info->has_value && end > start) {
        language->has_value = true;
        language->value.data = info->value.data + start;
        language->value.length = end - start;
    }
    *fenced = node->as.code->fenced != 0;
    *closed = !*fenced || node->as.code->fence_closed != 0;
}

static markdown_core_string literal_of(const markdown_core_node *node) {
    return chunk_string(node->kind == MARKDOWN_CORE_NODE_HTML_BLOCK ? node->as.html_block->literal : *node->as.literal);
}

static void formula_properties(const markdown_core_node *node, markdown_core_placement *mode,
                               markdown_core_string *literal) {
    *mode = markdown_core_elements_get_formula_mode((markdown_core_node *)node) == MARKDOWN_CORE_FORMULA_MODE_EMBEDDED
                ? MARKDOWN_CORE_PLACEMENT_EMBEDDED
                : MARKDOWN_CORE_PLACEMENT_STANDALONE;
    *literal = cstr_string(markdown_core_elements_get_formula_literal((markdown_core_node *)node));
}

static const markdown_core_table *table_of(const markdown_core_node *node) { return node->opaque; }

static markdown_core_optional_string directive_name(const markdown_core_node *node) {
    const char *value = markdown_core_elements_get_directive_name((markdown_core_node *)node);
    return value ? (markdown_core_optional_string){true, cstr_string(value)} : (markdown_core_optional_string){0};
}

static inline bool is_link(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_LINK || node->kind == MARKDOWN_CORE_NODE_EMBEDDED;
}

/* A node's attributes merge its own over the ones its resource contributes:
 * a link or image reads through a resource, and no other kind has one, so
 * its inherited counts are zero. */
static const markdown_core_attribute_value *inherited_attributes(const markdown_core_node *node) {
    return &node->as.link->resource->attributes;
}
static size_t inherited_class_count(const markdown_core_node *node) {
    return is_link(node) ? inherited_attributes(node)->class_count : 0;
}
static size_t inherited_record_count(const markdown_core_node *node) {
    return is_link(node) ? inherited_attributes(node)->record_count : 0;
}
static markdown_core_string attribute_class(const markdown_core_node *node, size_t index) {
    size_t count = inherited_class_count(node);
    const markdown_core_attribute_value *attributes = index < count ? inherited_attributes(node) : &node->attributes;
    return chunk_string(attributes->classes[index < count ? index : index - count]);
}
static void attribute_record(const markdown_core_node *node, size_t index, markdown_core_string *name,
                             markdown_core_string *value) {
    size_t count = inherited_record_count(node);
    const markdown_core_attribute_value *attributes = index < count ? inherited_attributes(node) : &node->attributes;
    const markdown_core_record *record = &attributes->records[index < count ? index : index - count];
    *name = chunk_string(record->name);
    *value = chunk_string(record->value);
}

static const markdown_core_dimensions *dimensions_of(const markdown_core_node *node) {
    const markdown_core_optional_dimensions *dimensions =
        node->kind == MARKDOWN_CORE_NODE_EMBEDDED ? &node->as.link->dimensions : &node->as.cross_embedded->dimensions;
    return dimensions->has_value ? &dimensions->value : NULL;
}

/* A metadata field the source wrote, or NULL: an absent field has no kind. */
static const markdown_core_metadata_value *metadata_field(const markdown_core_metadata_value *value) {
    return value->kind ? value : NULL;
}

static void callout_properties(const markdown_core_node *node, markdown_core_optional_string *variant,
                               markdown_core_optional_bool *collapsed) {
    *variant = optional_chunk_string(node->as.callout->variant);
    *collapsed = node->as.callout->collapsed;
}

static markdown_core_destination destination_of(const markdown_core_node *node) {
    markdown_core_destination destination = {0};
    if (is_link(node)) {
        destination.kind = MARKDOWN_CORE_DESTINATION_URL;
        destination.url = chunk_string(node->as.link->resource->url);
        return destination;
    }
    const markdown_core_cross_reference *cross = markdown_core_node_cross_reference(node);
    destination.kind = MARKDOWN_CORE_DESTINATION_CROSS;
    destination.path = chunk_string(cross->path);
    destination.anchor = optional_chunk_string(cross->anchor);
    return destination;
}

static markdown_core_optional_string cross_label(const markdown_core_node *node) {
    return optional_chunk_string(markdown_core_node_cross_reference(node)->label);
}

static markdown_core_optional_string link_title(const markdown_core_node *node) {
    return optional_chunk_string(node->as.link->resource->title);
}

static markdown_core_referent citation_referent(const markdown_core_node *citation) {
    const markdown_core_citation_item *item = citation->as.citation;
    markdown_core_referent referent = {0};
    switch (item->referent) {
    case MARKDOWN_CORE_NODE_REFERENT_BIB:
        referent.kind = MARKDOWN_CORE_REFERENT_BIB;
        referent.key = chunk_string(item->value);
        referent.mode = (markdown_core_bib_mode)item->mode;
        break;
    case MARKDOWN_CORE_NODE_REFERENT_FOOTNOTE:
        referent.kind = MARKDOWN_CORE_REFERENT_FOOTNOTE;
        referent.note = item->note;
        if (!item->note) {
            referent.label = chunk_string(item->value);
        }
        break;
    case MARKDOWN_CORE_NODE_REFERENT_SPECIMEN:
        referent.kind = MARKDOWN_CORE_REFERENT_SPECIMEN;
        referent.label = chunk_string(item->value);
        break;
    }
    return referent;
}

static void specimen_properties(const markdown_core_node *specimen, markdown_core_optional_string *label,
                                markdown_core_optional_i64 *start) {
    *label = optional_chunk_string(specimen->as.specimen->label);
    start->has_value = specimen->as.specimen->has_start;
    start->value = specimen->as.specimen->start;
}

/* A NODE'S SCALARS, compared field by field: everything a binding builds
 * into the node's value (the MCB3 record, wire/markdown_core_wire.c) except
 * its id, its extent and its node-valued fields, which publishing compares
 * itself. Each comparison reads the node through the same reader its public
 * accessor answers from, so the two cannot disagree about what a value is. */

static bool string_equal(markdown_core_string a, markdown_core_string b) {
    return a.length == b.length && (!a.length || !memcmp(a.data, b.data, a.length));
}

static bool optional_string_equal(markdown_core_optional_string a, markdown_core_optional_string b) {
    return a.has_value == b.has_value && (!a.has_value || string_equal(a.value, b.value));
}

static bool optional_int_equal(markdown_core_optional_i64 a, markdown_core_optional_i64 b) {
    return a.has_value == b.has_value && (!a.has_value || a.value == b.value);
}

static bool chunk_equal(markdown_core_chunk a, markdown_core_chunk b) {
    return a.len == b.len && (!a.len || !memcmp(a.data, b.data, (size_t)a.len));
}

/* An attribute value is compared from its representation, which its
 * accessors (`markdown_core_attribute_value_*`) present member for member:
 * the anchor, whose empty bytes are no anchor, then the classes and the
 * records in order. */
static bool attributes_equal(const markdown_core_attributes *a, const markdown_core_attributes *b) {
    if (a->class_count != b->class_count || a->record_count != b->record_count || !chunk_equal(a->anchor, b->anchor)) {
        return false;
    }
    for (uint32_t i = 0; i < a->class_count; i++) {
        if (!chunk_equal(a->classes[i], b->classes[i])) {
            return false;
        }
    }
    for (uint32_t i = 0; i < a->record_count; i++) {
        if (!chunk_equal(a->records[i].name, b->records[i].name) ||
            !chunk_equal(a->records[i].value, b->records[i].value)) {
            return false;
        }
    }
    return true;
}

static bool destination_equal(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_destination x = destination_of(a), y = destination_of(b);
    if (x.kind != y.kind) {
        return false;
    }
    return x.kind == MARKDOWN_CORE_DESTINATION_URL
               ? string_equal(x.url, y.url)
               : string_equal(x.path, y.path) && optional_string_equal(x.anchor, y.anchor);
}

static bool dimensions_equal(const markdown_core_node *a, const markdown_core_node *b) {
    const markdown_core_dimensions *x = dimensions_of(a), *y = dimensions_of(b);
    return x == y || (x && y && x->width == y->width && optional_int_equal(x->height, y->height));
}

/* Whether two `Link` or `Embedded` nodes read equal resources. */
static bool resources_equal(const markdown_core_node *a, const markdown_core_node *b) {
    if (a->as.link->resource == b->as.link->resource) {
        return true;
    }
    return destination_equal(a, b) && optional_string_equal(link_title(a), link_title(b)) &&
           attributes_equal(inherited_attributes(a), inherited_attributes(b));
}

static bool metadata_value_equal(const markdown_core_metadata_value *a, const markdown_core_metadata_value *b) {
    if (!a || !b) {
        return a == b;
    }
    if (a->kind != b->kind) {
        return false;
    }
    if (a->kind == MARKDOWN_CORE_METADATA_SCALAR) {
        markdown_core_metadata_scalar x = a->as.scalar, y = b->as.scalar;
        return x.kind == y.kind &&
               (x.kind == MARKDOWN_CORE_METADATA_NULL ||
                (x.kind == MARKDOWN_CORE_METADATA_BOOL ? x.value.boolean == y.value.boolean
                                                       : string_equal(x.value.string, y.value.string)));
    }
    if (a->as.list.count != b->as.list.count) {
        return false;
    }
    for (size_t i = 0; i < a->as.list.count; i++) {
        markdown_core_metadata_list_item x = a->as.list.items[i], y = b->as.list.items[i];
        if (x.kind != y.kind || !string_equal(x.value, y.value)) {
            return false;
        }
    }
    return true;
}

static bool metadata_equal(const markdown_core_node *a, const markdown_core_node *b) {
    const markdown_core_metadata_fields *x = a->as.metadata, *y = b->as.metadata;
    return metadata_value_equal(metadata_field(&x->name), metadata_field(&y->name)) &&
           metadata_value_equal(metadata_field(&x->title), metadata_field(&y->title)) &&
           metadata_value_equal(metadata_field(&x->subtitle), metadata_field(&y->subtitle)) &&
           metadata_value_equal(metadata_field(&x->time), metadata_field(&y->time)) &&
           metadata_value_equal(metadata_field(&x->date), metadata_field(&y->date)) &&
           metadata_value_equal(metadata_field(&x->authors), metadata_field(&y->authors)) &&
           metadata_value_equal(metadata_field(&x->keywords), metadata_field(&y->keywords)) &&
           metadata_value_equal(metadata_field(&x->abstract), metadata_field(&y->abstract)) &&
           metadata_value_equal(metadata_field(&x->state), metadata_field(&y->state)) &&
           metadata_value_equal(metadata_field(&x->comment), metadata_field(&y->comment));
}

static bool list_equal(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_list_flavor x_flavor, y_flavor;
    markdown_core_optional_i64 x_start, y_start;
    markdown_core_ordered_list_variant x_variant, y_variant;
    markdown_core_ordered_list_delimiter x_delimiter, y_delimiter;
    bool x_tight, y_tight;
    list_properties(a, &x_flavor, &x_start, &x_variant, &x_delimiter, &x_tight);
    list_properties(b, &y_flavor, &y_start, &y_variant, &y_delimiter, &y_tight);
    if (x_flavor != y_flavor || x_tight != y_tight || !optional_int_equal(x_start, y_start)) {
        return false;
    }
    /* The ordered-marker facts exist exactly when the list has a start. */
    if (!x_start.has_value) {
        return true;
    }
    bool cased = x_variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA ||
                 x_variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN;
    bool closable = x_delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS;
    return x_variant.kind == y_variant.kind && (!cased || x_variant.lowercased == y_variant.lowercased) &&
           x_delimiter.kind == y_delimiter.kind && (!closable || x_delimiter.closed == y_delimiter.closed);
}

static bool table_equal(const markdown_core_node *a, const markdown_core_node *b) {
    const markdown_core_table *p = table_of(a), *q = table_of(b);
    if (p->column_count != q->column_count) {
        return false;
    }
    for (size_t i = 0; i < p->column_count; i++) {
        markdown_core_table_column x = p->columns[i], y = q->columns[i];
        if (x.flow != y.flow || x.relative.has_value != y.relative.has_value ||
            (x.relative.has_value && x.relative.value != y.relative.value)) {
            return false;
        }
    }
    return true;
}

static bool referent_equal(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_referent x = citation_referent(a), y = citation_referent(b);
    if (x.kind != y.kind) {
        return false;
    }
    switch (x.kind) {
    case MARKDOWN_CORE_REFERENT_BIB:
        return x.mode == y.mode && string_equal(x.key, y.key);
    case MARKDOWN_CORE_REFERENT_FOOTNOTE:
        /* An inline note is a node-valued field; a label is a scalar. */
        return (x.note != NULL) == (y.note != NULL) && (x.note || string_equal(x.label, y.label));
    case MARKDOWN_CORE_REFERENT_SPECIMEN:
        return string_equal(x.label, y.label);
    }
    return false;
}

static bool scalars_equal(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_node_kind kind = public_kind(a);
    markdown_core_optional_string x, y, x_second, y_second;
    markdown_core_string x_literal, y_literal;
    if (!attributes_equal(&a->attributes, &b->attributes)) {
        return false;
    }
    switch (kind) {
    case MARKDOWN_CORE_KIND_CALLOUT: {
        markdown_core_optional_bool x_collapsed, y_collapsed;
        callout_properties(a, &x, &x_collapsed);
        callout_properties(b, &y, &y_collapsed);
        return optional_string_equal(x, y) && x_collapsed.has_value == y_collapsed.has_value &&
               (!x_collapsed.has_value || x_collapsed.value == y_collapsed.value);
    }
    case MARKDOWN_CORE_KIND_HEADING:
        return a->as.heading->level == b->as.heading->level;
    case MARKDOWN_CORE_KIND_LIST:
        return list_equal(a, b);
    case MARKDOWN_CORE_KIND_LIST_ITEM:
        return optional_string_equal(optional_chunk_string(a->as.list->task_marker),
                                     optional_chunk_string(b->as.list->task_marker));
    case MARKDOWN_CORE_KIND_CODE_BLOCK: {
        bool x_fenced, y_fenced, x_closed, y_closed;
        code_block_properties(a, &x, &x_second, &x_literal, &x_fenced, &x_closed);
        code_block_properties(b, &y, &y_second, &y_literal, &y_fenced, &y_closed);
        return x_fenced == y_fenced && x_closed == y_closed && optional_string_equal(x, y) &&
               optional_string_equal(x_second, y_second) && string_equal(x_literal, y_literal);
    }
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_COMMENT:
        return string_equal(literal_of(a), literal_of(b));
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
    case MARKDOWN_CORE_KIND_FORMULA: {
        markdown_core_placement x_mode, y_mode;
        formula_properties(a, &x_mode, &x_literal);
        formula_properties(b, &y_mode, &y_literal);
        return x_mode == y_mode && string_equal(x_literal, y_literal);
    }
    case MARKDOWN_CORE_KIND_TABLE:
        return table_equal(a, b);
    case MARKDOWN_CORE_KIND_TABLE_CELL:
        return a->as.table_cell->rowspan == b->as.table_cell->rowspan &&
               a->as.table_cell->colspan == b->as.table_cell->colspan;
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE:
        return optional_string_equal(directive_name(a), directive_name(b));
    case MARKDOWN_CORE_KIND_CROSS_LINK:
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED:
        return destination_equal(a, b) && optional_string_equal(cross_label(a), cross_label(b)) &&
               (kind == MARKDOWN_CORE_KIND_CROSS_LINK || dimensions_equal(a, b));
    case MARKDOWN_CORE_KIND_LINK:
    case MARKDOWN_CORE_KIND_EMBEDDED:
        return resources_equal(a, b) && (kind == MARKDOWN_CORE_KIND_LINK || dimensions_equal(a, b));
    case MARKDOWN_CORE_KIND_DEFINITION:
        return a->as.definition->compact == b->as.definition->compact;
    case MARKDOWN_CORE_KIND_CITATION:
        return referent_equal(a, b);
    case MARKDOWN_CORE_KIND_FOOTNOTE:
        return optional_string_equal(optional_chunk_string(a->as.footnote->label),
                                     optional_chunk_string(b->as.footnote->label));
    case MARKDOWN_CORE_KIND_SPECIMEN: {
        markdown_core_optional_i64 x_start, y_start;
        specimen_properties(a, &x, &x_start);
        specimen_properties(b, &y, &y_start);
        return optional_string_equal(x, y) && optional_int_equal(x_start, y_start);
    }
    case MARKDOWN_CORE_KIND_METADATA:
        return metadata_equal(a, b);
    case MARKDOWN_CORE_KIND_DOCUMENT:
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK:
    case MARKDOWN_CORE_KIND_SOFT_BREAK:
    case MARKDOWN_CORE_KIND_LINE_BREAK:
    case MARKDOWN_CORE_KIND_NONE:
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_TABLE_CAPTION:
    case MARKDOWN_CORE_KIND_TABLE_ROW:
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_MARK:
    case MARKDOWN_CORE_KIND_INSERTION:
    case MARKDOWN_CORE_KIND_SPAN:
    case MARKDOWN_CORE_KIND_SUPERSCRIPT:
    case MARKDOWN_CORE_KIND_SUBSCRIPT:
    case MARKDOWN_CORE_KIND_DEFINITION_LIST:
    case MARKDOWN_CORE_KIND_CITE:
        return true;
    }
    return true;
}

/* THE PUBLIC ACCESSORS: each checks its kind, and an `_at` its index, then
 * reads. */
#define REQUIRE_KIND(node, kinds)                                                                                      \
    do {                                                                                                               \
        if (!node_is((node), (kinds))) {                                                                               \
            return MARKDOWN_CORE_KIND_MISMATCH;                                                                        \
        }                                                                                                              \
    } while (0)

#define LITERAL_KINDS                                                                                                  \
    (KIND_BIT(MARKDOWN_CORE_KIND_TEXT) | KIND_BIT(MARKDOWN_CORE_KIND_CODE) | KIND_BIT(MARKDOWN_CORE_KIND_HTML) |       \
     KIND_BIT(MARKDOWN_CORE_KIND_HTML_BLOCK) | KIND_BIT(MARKDOWN_CORE_KIND_COMMENT))
#define FORMULA_KINDS (KIND_BIT(MARKDOWN_CORE_KIND_FORMULA) | KIND_BIT(MARKDOWN_CORE_KIND_FORMULA_BLOCK))
#define DIRECTIVE_KINDS (KIND_BIT(MARKDOWN_CORE_KIND_DIRECTIVE) | KIND_BIT(MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK))
#define LINK_KINDS (KIND_BIT(MARKDOWN_CORE_KIND_LINK) | KIND_BIT(MARKDOWN_CORE_KIND_EMBEDDED))
#define CROSS_KINDS (KIND_BIT(MARKDOWN_CORE_KIND_CROSS_LINK) | KIND_BIT(MARKDOWN_CORE_KIND_CROSS_EMBEDDED))
#define DIMENSIONS_KINDS (KIND_BIT(MARKDOWN_CORE_KIND_EMBEDDED) | KIND_BIT(MARKDOWN_CORE_KIND_CROSS_EMBEDDED))

markdown_core_status markdown_core_node_heading_level(const markdown_core_node *node, int32_t *level) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_HEADING));
    *level = node->as.heading->level;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_list_properties(const markdown_core_node *node,
                                                        markdown_core_list_flavor *flavor,
                                                        markdown_core_optional_i64 *start,
                                                        markdown_core_ordered_list_variant *variant,
                                                        markdown_core_ordered_list_delimiter *delimiter, bool *tight) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_LIST));
    list_properties(node, flavor, start, variant, delimiter, tight);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_list_item_marker(const markdown_core_node *node,
                                                         markdown_core_optional_string *marker) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_LIST_ITEM));
    *marker = optional_chunk_string(node->as.list->task_marker);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_code_block_properties(const markdown_core_node *node,
                                                              markdown_core_optional_string *info,
                                                              markdown_core_optional_string *language,
                                                              markdown_core_string *literal, bool *fenced,
                                                              bool *closed) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_CODE_BLOCK));
    code_block_properties(node, info, language, literal, fenced, closed);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_literal(const markdown_core_node *node, markdown_core_string *literal) {
    REQUIRE_KIND(node, LITERAL_KINDS);
    *literal = literal_of(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_formula_properties(const markdown_core_node *node,
                                                           markdown_core_placement *mode,
                                                           markdown_core_string *literal) {
    REQUIRE_KIND(node, FORMULA_KINDS);
    formula_properties(node, mode, literal);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_table_properties(const markdown_core_node *node, size_t *column_count,
                                                         size_t *head_count, size_t *content_count,
                                                         size_t *foot_count) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_TABLE));
    const markdown_core_table *table = table_of(node);
    *column_count = table->column_count;
    *head_count = table->head_count;
    *content_count = table->content_count;
    *foot_count = table->foot_count;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_table_column_at(const markdown_core_node *node, size_t index,
                                                        markdown_core_table_column *column) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_TABLE));
    const markdown_core_table *table = table_of(node);
    if (index >= table->column_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *column = table->columns[index];
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_table_caption(const markdown_core_node *node,
                                                      const markdown_core_node **caption) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_TABLE));
    *caption = table_of(node)->caption;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_table_cell_spans(const markdown_core_node *node, int64_t *rowspan,
                                                         int64_t *colspan) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_TABLE_CELL));
    *rowspan = node->as.table_cell->rowspan;
    *colspan = node->as.table_cell->colspan;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_directive_properties(const markdown_core_node *node,
                                                             markdown_core_optional_string *name) {
    REQUIRE_KIND(node, DIRECTIVE_KINDS);
    *name = directive_name(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_definition_compact(const markdown_core_node *node, bool *compact) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DEFINITION));
    *compact = node->as.definition->compact;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_definition_term(const markdown_core_node *node,
                                                        const markdown_core_nodes **term) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DEFINITION));
    *term = sequence_of(node->as.definition->term);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_definition_body_count(const markdown_core_node *node, size_t *count) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DEFINITION));
    *count = markdown_core_node_children_count(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_definition_body_at(const markdown_core_node *node, size_t index,
                                                           const markdown_core_nodes **body) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DEFINITION));
    if (index >= markdown_core_node_children_count(node)) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *body = sequence_of(markdown_core_node_child(node, index));
    return MARKDOWN_CORE_OK;
}

const markdown_core_attribute_value *markdown_core_node_primary_attributes(const markdown_core_node *node) {
    return &node->attributes;
}

markdown_core_status markdown_core_node_inherited_attributes(const markdown_core_node *node,
                                                             const markdown_core_attribute_value **attributes) {
    REQUIRE_KIND(node, LINK_KINDS);
    *attributes = inherited_attributes(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_optional_string markdown_core_attribute_value_anchor(const markdown_core_attribute_value *attributes) {
    return (markdown_core_optional_string){attributes->anchor.len > 0, chunk_string(attributes->anchor)};
}

size_t markdown_core_attribute_value_class_count(const markdown_core_attribute_value *attributes) {
    return attributes->class_count;
}

markdown_core_status markdown_core_attribute_value_class_at(const markdown_core_attribute_value *attributes,
                                                            size_t index, markdown_core_string *value) {
    if (index >= attributes->class_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *value = chunk_string(attributes->classes[index]);
    return MARKDOWN_CORE_OK;
}

size_t markdown_core_attribute_value_record_count(const markdown_core_attribute_value *attributes) {
    return attributes->record_count;
}

markdown_core_status markdown_core_attribute_value_record_at(const markdown_core_attribute_value *attributes,
                                                             size_t index, markdown_core_string *name,
                                                             markdown_core_string *value) {
    if (index >= attributes->record_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *name = chunk_string(attributes->records[index].name);
    *value = chunk_string(attributes->records[index].value);
    return MARKDOWN_CORE_OK;
}

markdown_core_optional_string markdown_core_node_anchor(const markdown_core_node *node) {
    const markdown_core_chunk *anchor = markdown_core_node_anchor_chunk(node);
    return (markdown_core_optional_string){anchor->len > 0, chunk_string(*anchor)};
}

size_t markdown_core_node_attribute_class_count(const markdown_core_node *node) {
    return inherited_class_count(node) + node->attributes.class_count;
}

markdown_core_status markdown_core_node_attribute_class_at(const markdown_core_node *node, size_t index,
                                                           markdown_core_string *value) {
    if (index >= markdown_core_node_attribute_class_count(node)) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *value = attribute_class(node, index);
    return MARKDOWN_CORE_OK;
}

size_t markdown_core_node_attribute_record_count(const markdown_core_node *node) {
    return inherited_record_count(node) + node->attributes.record_count;
}

markdown_core_status markdown_core_node_attribute_record_at(const markdown_core_node *node, size_t index,
                                                            markdown_core_string *name, markdown_core_string *value) {
    if (index >= markdown_core_node_attribute_record_count(node)) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    attribute_record(node, index, name, value);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_dimensions(const markdown_core_node *node,
                                                   const markdown_core_dimensions **dimensions) {
    REQUIRE_KIND(node, DIMENSIONS_KINDS);
    *dimensions = dimensions_of(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_document_metadata(const markdown_core_node *node,
                                                          const markdown_core_node **metadata) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DOCUMENT));
    *metadata = node->as.document->metadata;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_name(const markdown_core_node *metadata,
                                                 const markdown_core_metadata_value **value) {
    REQUIRE_KIND(metadata, KIND_BIT(MARKDOWN_CORE_KIND_METADATA));
    *value = metadata_field(&metadata->as.metadata->name);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_title(const markdown_core_node *metadata,
                                                  const markdown_core_metadata_value **value) {
    REQUIRE_KIND(metadata, KIND_BIT(MARKDOWN_CORE_KIND_METADATA));
    *value = metadata_field(&metadata->as.metadata->title);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_subtitle(const markdown_core_node *metadata,
                                                     const markdown_core_metadata_value **value) {
    REQUIRE_KIND(metadata, KIND_BIT(MARKDOWN_CORE_KIND_METADATA));
    *value = metadata_field(&metadata->as.metadata->subtitle);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_time(const markdown_core_node *metadata,
                                                 const markdown_core_metadata_value **value) {
    REQUIRE_KIND(metadata, KIND_BIT(MARKDOWN_CORE_KIND_METADATA));
    *value = metadata_field(&metadata->as.metadata->time);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_date(const markdown_core_node *metadata,
                                                 const markdown_core_metadata_value **value) {
    REQUIRE_KIND(metadata, KIND_BIT(MARKDOWN_CORE_KIND_METADATA));
    *value = metadata_field(&metadata->as.metadata->date);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_authors(const markdown_core_node *metadata,
                                                    const markdown_core_metadata_value **value) {
    REQUIRE_KIND(metadata, KIND_BIT(MARKDOWN_CORE_KIND_METADATA));
    *value = metadata_field(&metadata->as.metadata->authors);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_keywords(const markdown_core_node *metadata,
                                                     const markdown_core_metadata_value **value) {
    REQUIRE_KIND(metadata, KIND_BIT(MARKDOWN_CORE_KIND_METADATA));
    *value = metadata_field(&metadata->as.metadata->keywords);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_abstract(const markdown_core_node *metadata,
                                                     const markdown_core_metadata_value **value) {
    REQUIRE_KIND(metadata, KIND_BIT(MARKDOWN_CORE_KIND_METADATA));
    *value = metadata_field(&metadata->as.metadata->abstract);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_state(const markdown_core_node *metadata,
                                                  const markdown_core_metadata_value **value) {
    REQUIRE_KIND(metadata, KIND_BIT(MARKDOWN_CORE_KIND_METADATA));
    *value = metadata_field(&metadata->as.metadata->state);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_comment(const markdown_core_node *metadata,
                                                    const markdown_core_metadata_value **value) {
    REQUIRE_KIND(metadata, KIND_BIT(MARKDOWN_CORE_KIND_METADATA));
    *value = metadata_field(&metadata->as.metadata->comment);
    return MARKDOWN_CORE_OK;
}

markdown_core_metadata_value_kind markdown_core_metadata_value_get_kind(const markdown_core_metadata_value *value) {
    return value->kind;
}

markdown_core_status markdown_core_metadata_value_scalar(const markdown_core_metadata_value *value,
                                                         markdown_core_metadata_scalar *scalar) {
    if (value->kind != MARKDOWN_CORE_METADATA_SCALAR) {
        return MARKDOWN_CORE_KIND_MISMATCH;
    }
    *scalar = value->as.scalar;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_value_item_count(const markdown_core_metadata_value *value, size_t *count) {
    if (value->kind != MARKDOWN_CORE_METADATA_LIST) {
        return MARKDOWN_CORE_KIND_MISMATCH;
    }
    *count = value->as.list.count;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_metadata_value_item_at(const markdown_core_metadata_value *value, size_t index,
                                                          markdown_core_metadata_list_item *item) {
    if (value->kind != MARKDOWN_CORE_METADATA_LIST) {
        return MARKDOWN_CORE_KIND_MISMATCH;
    }
    if (index >= value->as.list.count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *item = value->as.list.items[index];
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_directive_label(const markdown_core_node *node,
                                                        const markdown_core_node **label) {
    REQUIRE_KIND(node, DIRECTIVE_KINDS);
    *label = markdown_core_directive_label(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_callout_properties(const markdown_core_node *node,
                                                           markdown_core_optional_string *variant,
                                                           markdown_core_optional_bool *collapsed) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_CALLOUT));
    callout_properties(node, variant, collapsed);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_callout_title(const markdown_core_node *node,
                                                      const markdown_core_nodes **title) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_CALLOUT));
    *title = sequence_of(node->as.callout->title);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_destination(const markdown_core_node *node,
                                                    markdown_core_destination *destination) {
    REQUIRE_KIND(node, LINK_KINDS | CROSS_KINDS);
    *destination = destination_of(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_cross_label(const markdown_core_node *node,
                                                    markdown_core_optional_string *label) {
    REQUIRE_KIND(node, CROSS_KINDS);
    *label = cross_label(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_title(const markdown_core_node *node, markdown_core_optional_string *title) {
    REQUIRE_KIND(node, LINK_KINDS);
    *title = link_title(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_resource(const markdown_core_node *node,
                                                 const markdown_core_resource **resource) {
    REQUIRE_KIND(node, LINK_KINDS);
    *resource = node->as.link->resource;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_citation_referent(const markdown_core_node *citation,
                                                     markdown_core_referent *referent) {
    REQUIRE_KIND(citation, KIND_BIT(MARKDOWN_CORE_KIND_CITATION));
    *referent = citation_referent(citation);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_citation_prefix(const markdown_core_node *citation,
                                                   const markdown_core_nodes **prefix) {
    REQUIRE_KIND(citation, KIND_BIT(MARKDOWN_CORE_KIND_CITATION));
    *prefix = sequence_of(citation->as.citation->prefix);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_citation_suffix(const markdown_core_node *citation,
                                                   const markdown_core_nodes **suffix) {
    REQUIRE_KIND(citation, KIND_BIT(MARKDOWN_CORE_KIND_CITATION));
    *suffix = sequence_of(citation->as.citation->suffix);
    return MARKDOWN_CORE_OK;
}

/* The definition tables publishing recorded in the document's root. */
static const markdown_core_definitions *document_footnotes(const markdown_core_document *document) {
    return &document->root->as.document->footnotes;
}

static const markdown_core_definitions *document_specimens(const markdown_core_document *document) {
    return &document->root->as.document->specimens;
}

static markdown_core_status definition_at(const markdown_core_definitions *table, size_t index,
                                          const markdown_core_node **node) {
    if (index >= table->count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *node = table->nodes[index];
    return MARKDOWN_CORE_OK;
}

size_t markdown_core_document_footnote_count(const markdown_core_document *document) {
    return document_footnotes(document)->count;
}

markdown_core_status markdown_core_document_footnote_at(const markdown_core_document *document, size_t index,
                                                        const markdown_core_node **footnote) {
    return definition_at(document_footnotes(document), index, footnote);
}

size_t markdown_core_document_specimen_count(const markdown_core_document *document) {
    return document_specimens(document)->count;
}

markdown_core_status markdown_core_document_specimen_at(const markdown_core_document *document, size_t index,
                                                        const markdown_core_node **specimen) {
    return definition_at(document_specimens(document), index, specimen);
}

/* The first definition in source order whose label is `label`, byte for
 * byte: the lowest of that label in the label order. */
static const markdown_core_node *definition_for(const markdown_core_definitions *table, markdown_core_string label) {
    size_t lo = 0, hi = table->labeled_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const markdown_core_optional_chunk *candidate = definition_label(table->labeled[mid]);
        if (label_compare(candidate->value.data, (size_t)candidate->value.len, label.data, label.length) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == table->labeled_count) {
        return NULL;
    }
    const markdown_core_optional_chunk *found = definition_label(table->labeled[lo]);
    return label_compare(found->value.data, (size_t)found->value.len, label.data, label.length) == 0
               ? table->labeled[lo]
               : NULL;
}

const markdown_core_node *markdown_core_document_footnote_for(const markdown_core_document *document,
                                                              markdown_core_string label) {
    return definition_for(document_footnotes(document), label);
}

const markdown_core_node *markdown_core_document_specimen_for(const markdown_core_document *document,
                                                              markdown_core_string label) {
    return definition_for(document_specimens(document), label);
}

markdown_core_status markdown_core_footnote_label(const markdown_core_node *footnote,
                                                  markdown_core_optional_string *label) {
    REQUIRE_KIND(footnote, KIND_BIT(MARKDOWN_CORE_KIND_FOOTNOTE));
    *label = optional_chunk_string(footnote->as.footnote->label);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_footnote_content(const markdown_core_node *footnote,
                                                    const markdown_core_nodes **content) {
    REQUIRE_KIND(footnote, KIND_BIT(MARKDOWN_CORE_KIND_FOOTNOTE));
    *content = sequence_of(footnote);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_specimen_properties(const markdown_core_node *specimen,
                                                       markdown_core_optional_string *label,
                                                       markdown_core_optional_i64 *start) {
    REQUIRE_KIND(specimen, KIND_BIT(MARKDOWN_CORE_KIND_SPECIMEN));
    specimen_properties(specimen, label, start);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_specimen_content(const markdown_core_node *specimen,
                                                    const markdown_core_nodes **content) {
    REQUIRE_KIND(specimen, KIND_BIT(MARKDOWN_CORE_KIND_SPECIMEN));
    *content = sequence_of(specimen);
    return MARKDOWN_CORE_OK;
}

static void buffer_reserve(dump_buffer *buffer, size_t additional) {
    size_t needed;
    size_t capacity;
    uint8_t *data;
    if (buffer->failed || additional > SIZE_MAX - buffer->size - 1) {
        buffer->failed = true;
        return;
    }
    needed = buffer->size + additional + 1;
    if (needed <= buffer->capacity) {
        return;
    }
    capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    data = (uint8_t *)realloc(buffer->data, capacity);
    if (!data) {
        buffer->failed = true;
        return;
    }
    buffer->data = data;
    buffer->capacity = capacity;
}

static void buffer_bytes(dump_buffer *buffer, const void *bytes, size_t length) {
    buffer_reserve(buffer, length);
    if (buffer->failed) {
        return;
    }
    if (length) {
        memcpy(buffer->data + buffer->size, bytes, length);
    }
    buffer->size += length;
    buffer->data[buffer->size] = 0;
}

static void buffer_cstr(dump_buffer *buffer, const char *value) { buffer_bytes(buffer, value, strlen(value)); }

static void buffer_i64(dump_buffer *buffer, int64_t value) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%lld", (long long)value);
    if (length > 0) {
        buffer_bytes(buffer, text, (size_t)length);
    }
}

static void buffer_json_string(dump_buffer *buffer, markdown_core_string value) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    buffer_cstr(buffer, "\"");
    for (i = 0; i < value.length; i++) {
        uint8_t c = value.data[i];
        switch (c) {
        case '\"':
            buffer_cstr(buffer, "\\\"");
            break;
        case '\\':
            buffer_cstr(buffer, "\\\\");
            break;
        case '\b':
            buffer_cstr(buffer, "\\b");
            break;
        case '\f':
            buffer_cstr(buffer, "\\f");
            break;
        case '\n':
            buffer_cstr(buffer, "\\n");
            break;
        case '\r':
            buffer_cstr(buffer, "\\r");
            break;
        case '\t':
            buffer_cstr(buffer, "\\t");
            break;
        default:
            if (c < 0x20) {
                char escaped[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0xf]};
                buffer_bytes(buffer, escaped, sizeof(escaped));
            } else {
                buffer_bytes(buffer, &c, 1);
            }
            break;
        }
    }
    buffer_cstr(buffer, "\"");
}

/* `null` and `""` are two answers, not one, and this reads the presence flag
 * rather than the pointer -- which is the same rule the dump already applied
 * to an optional Int and an optional Bool (requirement 14). */
static void buffer_optional_string(dump_buffer *buffer, markdown_core_optional_string value) {
    if (!value.has_value) {
        buffer_cstr(buffer, "null");
    } else {
        buffer_json_string(buffer, value.value);
    }
}

#define DUMP_SEGMENT_MAX 6 /* the bytes of "│   " */

static bool ensure_level(dump_buffer *buffer, size_t depth) {
    bool *more;
    uint8_t *prefix;
    size_t *prefix_end;
    size_t capacity;
    if (depth < buffer->level_capacity) {
        return true;
    }
    capacity = buffer->level_capacity ? buffer->level_capacity : 16;
    while (capacity <= depth) {
        capacity *= 2;
    }
    more = (bool *)realloc(buffer->more, capacity * sizeof(*more));
    if (more) {
        buffer->more = more;
    }
    prefix = (uint8_t *)realloc(buffer->prefix, capacity * DUMP_SEGMENT_MAX);
    if (prefix) {
        buffer->prefix = prefix;
    }
    prefix_end = (size_t *)realloc(buffer->prefix_end, capacity * sizeof(*prefix_end));
    if (prefix_end) {
        buffer->prefix_end = prefix_end;
    }
    if (!more || !prefix || !prefix_end) {
        buffer->failed = true;
        return false;
    }
    if (!buffer->level_capacity) {
        buffer->prefix_end[0] = 0;
    }
    buffer->level_capacity = capacity;
    return true;
}

/* Extends the segments to cover `depth`: the lines nested below the node at
 * `depth` lead with the segments above it plus the one its own connector
 * decides. Called once that node's `more` flag is set and before anything is
 * drawn below it. */
static void extend_prefix(dump_buffer *buffer, size_t depth) {
    size_t base;
    if (!depth) {
        return;
    }
    base = buffer->prefix_end[depth - 1];
    if (buffer->more[depth - 1]) {
        memcpy(buffer->prefix + base, "│   ", 6);
        buffer->prefix_end[depth] = base + 6;
    } else {
        memcpy(buffer->prefix + base, "    ", 4);
        buffer->prefix_end[depth] = base + 4;
    }
}

static const char *flow_name(markdown_core_flow flow) {
    static const char *const names[] = {"none", "left", "center", "right"};
    return names[flow];
}

static const char *mode_name(markdown_core_placement mode) {
    return mode == MARKDOWN_CORE_PLACEMENT_EMBEDDED ? "embedded" : "standalone";
}

/* A tagged value prints its branch and its named fields with no spaces
 * (canonical-ast-dump.md): `url("...")`, or `cross(path="...",anchor=null)`.
 * Kept OUTSIDE `dump_fields`, whose body the projection audit reads for the
 * `name=` literals a kind prints: the branch fields are the value's, not the
 * node's. */
static void buffer_dimensions(dump_buffer *buffer, const markdown_core_dimensions *value) {
    if (!value) {
        buffer_cstr(buffer, "null");
        return;
    }
    buffer_cstr(buffer, "(width=");
    buffer_i64(buffer, value->width);
    buffer_cstr(buffer, ",height=");
    if (value->height.has_value) {
        buffer_i64(buffer, value->height.value);
    } else {
        buffer_cstr(buffer, "null");
    }
    buffer_cstr(buffer, ")");
}

static void buffer_destination(dump_buffer *buffer, markdown_core_destination destination) {
    if (destination.kind == MARKDOWN_CORE_DESTINATION_CROSS) {
        buffer_cstr(buffer, "cross(path=");
        buffer_json_string(buffer, destination.path);
        buffer_cstr(buffer, ",anchor=");
        buffer_optional_string(buffer, destination.anchor);
        buffer_cstr(buffer, ")");
        return;
    }
    buffer_cstr(buffer, "url(");
    buffer_json_string(buffer, destination.url);
    buffer_cstr(buffer, ")");
}

static void buffer_optional_bool(dump_buffer *buffer, markdown_core_optional_bool value) {
    if (value.has_value) {
        buffer_cstr(buffer, value.value ? "true" : "false");
    } else {
        buffer_cstr(buffer, "null");
    }
}

/* Find the shortest significant digit sequence that round-trips. Use the
 * process locale only inside snprintf/strtod, then emit ASCII digits with a
 * fixed decimal/exponent policy, so locale never reaches the dump. Widths are
 * finite and positive by the table contract. */
static void buffer_double(dump_buffer *buffer, double value) {
    char scientific[64], digits[18];
    for (int precision = 0; precision < 17; precision++) {
        snprintf(scientific, sizeof(scientific), "%.*e", precision, value);
        if (strtod(scientific, NULL) == value) {
            break;
        }
    }
    char *exponent = strchr(scientific, 'e');
    int power = (int)strtol(exponent + 1, NULL, 10);
    size_t length = 0;
    for (char *c = scientific; c < exponent; c++) {
        if (*c >= '0' && *c <= '9') {
            digits[length++] = *c;
        }
    }
    while (length > 1 && digits[length - 1] == '0') {
        length--;
    }
    if (power < -6 || power >= 21) {
        buffer_bytes(buffer, (const uint8_t *)digits, 1);
        if (length > 1) {
            buffer_cstr(buffer, ".");
            buffer_bytes(buffer, (const uint8_t *)digits + 1, length - 1);
        }
        buffer_cstr(buffer, "e");
        if (power >= 0) {
            buffer_cstr(buffer, "+");
        }
        buffer_i64(buffer, power);
    } else if (power < 0) {
        buffer_cstr(buffer, "0.");
        for (int i = -1; i > power; i--) {
            buffer_cstr(buffer, "0");
        }
        buffer_bytes(buffer, (const uint8_t *)digits, length);
    } else {
        for (int i = 0; i <= power || i < (int)length; i++) {
            if (i == power + 1) {
                buffer_cstr(buffer, ".");
            }
            buffer_bytes(buffer, (const uint8_t *)(i < (int)length ? &digits[i] : "0"), 1);
        }
    }
}

static void buffer_referent(dump_buffer *buffer, markdown_core_referent referent);
static void dump_metadata_value(dump_buffer *buffer, const markdown_core_metadata_value *record);

static void dump_fields(dump_buffer *buffer, const markdown_core_node *node, markdown_core_node_kind kind) {
    markdown_core_string a, c;
    markdown_core_optional_string oa, ob;
    markdown_core_optional_i64 start;
    markdown_core_optional_bool collapsed;
    markdown_core_ordered_list_variant variant;
    markdown_core_ordered_list_delimiter delimiter;
    markdown_core_list_flavor flavor;
    markdown_core_placement mode;
    bool x, y;
    size_t i;
    switch (kind) {
    case MARKDOWN_CORE_KIND_CITATION:
        buffer_cstr(buffer, " referent=");
        buffer_referent(buffer, citation_referent(node));
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE:
        buffer_cstr(buffer, " label=");
        buffer_optional_string(buffer, optional_chunk_string(node->as.footnote->label));
        break;
    case MARKDOWN_CORE_KIND_SPECIMEN:
        specimen_properties(node, &oa, &start);
        buffer_cstr(buffer, " label=");
        buffer_optional_string(buffer, oa);
        buffer_cstr(buffer, " start=");
        if (start.has_value) {
            buffer_i64(buffer, start.value);
        } else {
            buffer_cstr(buffer, "null");
        }
        break;
    case MARKDOWN_CORE_KIND_METADATA:
        buffer_cstr(buffer, " name=");
        dump_metadata_value(buffer, metadata_field(&node->as.metadata->name));
        buffer_cstr(buffer, " title=");
        dump_metadata_value(buffer, metadata_field(&node->as.metadata->title));
        buffer_cstr(buffer, " subtitle=");
        dump_metadata_value(buffer, metadata_field(&node->as.metadata->subtitle));
        buffer_cstr(buffer, " time=");
        dump_metadata_value(buffer, metadata_field(&node->as.metadata->time));
        buffer_cstr(buffer, " date=");
        dump_metadata_value(buffer, metadata_field(&node->as.metadata->date));
        buffer_cstr(buffer, " authors=");
        dump_metadata_value(buffer, metadata_field(&node->as.metadata->authors));
        buffer_cstr(buffer, " keywords=");
        dump_metadata_value(buffer, metadata_field(&node->as.metadata->keywords));
        buffer_cstr(buffer, " abstract=");
        dump_metadata_value(buffer, metadata_field(&node->as.metadata->abstract));
        buffer_cstr(buffer, " state=");
        dump_metadata_value(buffer, metadata_field(&node->as.metadata->state));
        buffer_cstr(buffer, " comment=");
        dump_metadata_value(buffer, metadata_field(&node->as.metadata->comment));
        break;
    case MARKDOWN_CORE_KIND_CALLOUT:
        callout_properties(node, &oa, &collapsed);
        buffer_cstr(buffer, " variant=");
        buffer_optional_string(buffer, oa);
        buffer_cstr(buffer, " collapsed=");
        buffer_optional_bool(buffer, collapsed);
        break;
    case MARKDOWN_CORE_KIND_DEFINITION:
        buffer_cstr(buffer, " compact=");
        buffer_cstr(buffer, node->as.definition->compact ? "true" : "false");
        break;
    case MARKDOWN_CORE_KIND_HEADING:
        buffer_cstr(buffer, " level=");
        buffer_i64(buffer, node->as.heading->level);
        break;
    case MARKDOWN_CORE_KIND_LIST:
        list_properties(node, &flavor, &start, &variant, &delimiter, &x);
        buffer_cstr(buffer, " flavor=");
        buffer_cstr(buffer, flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? "ordered" : "bullet");
        buffer_cstr(buffer, " start=");
        if (start.has_value) {
            buffer_i64(buffer, start.value);
        } else {
            buffer_cstr(buffer, "null");
        }
        buffer_cstr(buffer, " variant=");
        if (flavor == MARKDOWN_CORE_LIST_FLAVOR_BULLET) {
            buffer_cstr(buffer, "null");
        } else if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA ||
                   variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN) {
            buffer_cstr(buffer, variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA ? "alpha(lowercased"
                                                                                         : "roman(lowercased");
            buffer_cstr(buffer, variant.lowercased ? "=true)" : "=false)");
        } else if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT) {
            buffer_cstr(buffer, "default");
        } else {
            buffer_cstr(buffer, "decimal");
        }
        buffer_cstr(buffer, " delimiter=");
        if (flavor == MARKDOWN_CORE_LIST_FLAVOR_BULLET) {
            buffer_cstr(buffer, "null");
        } else {
            if (delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS) {
                buffer_cstr(buffer, "parenthesis(closed");
                buffer_cstr(buffer, delimiter.closed ? "=true)" : "=false)");
            } else if (delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT) {
                buffer_cstr(buffer, "default");
            } else {
                buffer_cstr(buffer, "period");
            }
        }
        buffer_cstr(buffer, " tight=");
        buffer_cstr(buffer, x ? "true" : "false");
        break;
    case MARKDOWN_CORE_KIND_LIST_ITEM:
        buffer_cstr(buffer, " marker=");
        buffer_optional_string(buffer, optional_chunk_string(node->as.list->task_marker));
        break;
    case MARKDOWN_CORE_KIND_CODE_BLOCK:
        code_block_properties(node, &oa, &ob, &c, &x, &y);
        buffer_cstr(buffer, " info=");
        buffer_optional_string(buffer, oa);
        buffer_cstr(buffer, " language=");
        buffer_optional_string(buffer, ob);
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, c);
        buffer_cstr(buffer, " fenced=");
        buffer_cstr(buffer, x ? "true" : "false");
        buffer_cstr(buffer, " closed=");
        buffer_cstr(buffer, y ? "true" : "false");
        break;
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_COMMENT:
    case MARKDOWN_CORE_KIND_CODE:
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, literal_of(node));
        break;
    case MARKDOWN_CORE_KIND_FORMULA:
        /* The only kind whose mode is a fact about the SOURCE: `$x$` is
         * embedded and `$$x$$` is standalone inside the same paragraph.  The
         * other five carried a mode that their kind already implied, and Q29
         * deleted all five at 15A.4. */
        formula_properties(node, &mode, &a);
        buffer_cstr(buffer, " mode=");
        buffer_cstr(buffer, mode_name(mode));
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
        formula_properties(node, &mode, &a);
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_TABLE: {
        const markdown_core_table *table = table_of(node);
        buffer_cstr(buffer, " columns=[");
        for (i = 0; i < table->column_count; i++) {
            markdown_core_table_column column = table->columns[i];
            if (i) {
                buffer_cstr(buffer, ",");
            }
            buffer_cstr(buffer, flow_name(column.flow));
            buffer_cstr(buffer, ":");
            if (column.relative.has_value) {
                buffer_double(buffer, column.relative.value);
            } else {
                buffer_cstr(buffer, "null");
            }
        }
        buffer_cstr(buffer, "]");
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE_CELL: {
        buffer_cstr(buffer, " rowspan=");
        buffer_i64(buffer, node->as.table_cell->rowspan);
        buffer_cstr(buffer, " colspan=");
        buffer_i64(buffer, node->as.table_cell->colspan);
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE:
        buffer_cstr(buffer, " name=");
        buffer_optional_string(buffer, directive_name(node));
        break;
    /* A DESTINATION IS REQUIRED (Q26): `dest=` is the tagged value and is
     * never `null`. `[a]()` used to print `destination=null`, which said the
     * author wrote no destination when the empty parentheses are the
     * destination they wrote; it is `dest=url("")` now. */
    case MARKDOWN_CORE_KIND_LINK:
        buffer_cstr(buffer, " dest=");
        buffer_destination(buffer, destination_of(node));
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, link_title(node));
        break;
    case MARKDOWN_CORE_KIND_CROSS_LINK:
        buffer_cstr(buffer, " dest=");
        buffer_destination(buffer, destination_of(node));
        buffer_cstr(buffer, " label=");
        buffer_optional_string(buffer, cross_label(node));
        break;
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED:
        buffer_cstr(buffer, " dest=");
        buffer_destination(buffer, destination_of(node));
        buffer_cstr(buffer, " label=");
        buffer_optional_string(buffer, cross_label(node));
        buffer_cstr(buffer, " dimensions=");
        buffer_dimensions(buffer, dimensions_of(node));
        break;
    case MARKDOWN_CORE_KIND_EMBEDDED:
        buffer_cstr(buffer, " dest=");
        buffer_destination(buffer, destination_of(node));
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, link_title(node));
        buffer_cstr(buffer, " dimensions=");
        buffer_dimensions(buffer, dimensions_of(node));
        break;
    default:
        break;
    }
}

/* The file-tree connectors that lead a line at `depth`. */
static void dump_prefix(dump_buffer *buffer, size_t depth) {
    if (!depth) {
        return;
    }
    buffer_bytes(buffer, buffer->prefix, buffer->prefix_end[depth - 1]);
    buffer_cstr(buffer, buffer->more[depth - 1] ? "├── " : "└── ");
}

/* A group line nests a node-valued list under its owner: `Kind children=N`
 * with no scope and no fields, at the owner's nesting depth, and the list's
 * nodes one level below it. */
static void dump_group_line(dump_buffer *buffer, const char *name, size_t count, size_t depth, bool has_next) {
    if (!ensure_level(buffer, depth)) {
        return;
    }
    buffer->more[depth] = has_next;
    extend_prefix(buffer, depth);
    dump_prefix(buffer, depth + 1);
    buffer_cstr(buffer, name);
    buffer_cstr(buffer, " children=");
    buffer_i64(buffer, (int64_t)count);
    buffer_cstr(buffer, "\n");
}

/* The scope of a byte range, in UTF-8 columns (source_scope). */
static void buffer_scope(dump_buffer *buffer, markdown_core_place place) {
    source_lines lines = {buffer->lines, buffer->line_count};
    markdown_core_scope scope = source_scope(&lines, buffer->source, place, MARKDOWN_CORE_TEXT_UNIT_UTF8);
    buffer_i64(buffer, scope.start.line);
    buffer_cstr(buffer, ":");
    buffer_i64(buffer, scope.start.column);
    buffer_cstr(buffer, "..");
    buffer_i64(buffer, scope.end.line);
    buffer_cstr(buffer, ":");
    buffer_i64(buffer, scope.end.column);
}

static const char *bib_mode_name(markdown_core_bib_mode mode) {
    static const char *const names[] = {"normal", "authorInText", "suppressAuthor"};
    return names[mode - MARKDOWN_CORE_BIB_MODE_NORMAL];
}

/* A tagged value prints its branch and named fields with no spaces, as
 * `dest` does. An inline note is drawn under its Citation, so its branch
 * prints no field. */
static void buffer_referent(dump_buffer *buffer, markdown_core_referent referent) {
    if (referent.kind == MARKDOWN_CORE_REFERENT_BIB) {
        buffer_cstr(buffer, "bib(key=");
        buffer_json_string(buffer, referent.key);
        buffer_cstr(buffer, ",mode=");
        buffer_cstr(buffer, bib_mode_name(referent.mode));
        buffer_cstr(buffer, ")");
    } else if (referent.kind == MARKDOWN_CORE_REFERENT_FOOTNOTE && referent.note) {
        buffer_cstr(buffer, "footnote(note)");
    } else {
        buffer_cstr(buffer, referent.kind == MARKDOWN_CORE_REFERENT_SPECIMEN ? "specimen(label=" : "footnote(label=");
        buffer_json_string(buffer, referent.label);
        buffer_cstr(buffer, ")");
    }
}

static void dump_metadata_value(dump_buffer *buffer, const markdown_core_metadata_value *record) {
    if (!record) {
        buffer_cstr(buffer, "null");
        return;
    }
    if (record->kind == MARKDOWN_CORE_METADATA_SCALAR) {
        markdown_core_metadata_scalar value = record->as.scalar;
        buffer_cstr(buffer, "scalar(");
        switch (value.kind) {
        case MARKDOWN_CORE_METADATA_NULL:
            buffer_cstr(buffer, "null");
            break;
        case MARKDOWN_CORE_METADATA_BOOL:
            buffer_cstr(buffer, value.value.boolean ? "bool(true)" : "bool(false)");
            break;
        case MARKDOWN_CORE_METADATA_NUMBER:
        case MARKDOWN_CORE_METADATA_TEXT:
            buffer_cstr(buffer, value.kind == MARKDOWN_CORE_METADATA_NUMBER ? "number(" : "text(");
            buffer_json_string(buffer, value.value.string);
            buffer_cstr(buffer, ")");
            break;
        }
        buffer_cstr(buffer, ")");
    } else {
        buffer_cstr(buffer, "list([");
        for (size_t i = 0; i < record->as.list.count; i++) {
            markdown_core_metadata_list_item item = record->as.list.items[i];
            if (i) {
                buffer_cstr(buffer, ",");
            }
            buffer_cstr(buffer, item.kind == MARKDOWN_CORE_METADATA_ITEM_NUMBER ? "number(" : "text(");
            buffer_json_string(buffer, item.value);
            buffer_cstr(buffer, ")");
        }
        buffer_cstr(buffer, "])");
    }
}

/* Draws the node's own line. What it nests is the canonical walk's to
 * deliver, as the lines after it. */
static void dump_node(dump_buffer *buffer, const markdown_core_node *node, markdown_core_place place, size_t depth) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    /* `children` counts structural children: a cite's are its items and a
     * definition's its bodies. */
    size_t child_count = markdown_core_node_children_count(node);
    dump_prefix(buffer, depth);
    buffer_cstr(buffer, S_kind_name[kind]);
    buffer_cstr(buffer, " scope=");
    buffer_scope(buffer, place);
    buffer_cstr(buffer, " anchor=");
    buffer_optional_string(buffer, markdown_core_node_anchor(node));
    buffer_cstr(buffer, " attributes={");
    size_t classes = markdown_core_node_attribute_class_count(node);
    size_t records = markdown_core_node_attribute_record_count(node);
    for (size_t i = 0; i < classes; i++) {
        markdown_core_string value = attribute_class(node, i);
        if (i) {
            buffer_cstr(buffer, " ");
        }
        buffer_cstr(buffer, ".");
        bool plain = value.length != 0;
        for (size_t j = 0; j < value.length; j++) {
            unsigned char c = (unsigned char)value.data[j];
            if (c <= 32 || c >= 127 || strchr("\"\\{}[]()=", c)) {
                plain = false;
            }
        }
        if (plain) {
            buffer_bytes(buffer, value.data, value.length);
        } else {
            buffer_json_string(buffer, value);
        }
    }
    for (size_t i = 0; i < records; i++) {
        markdown_core_string name, value;
        attribute_record(node, i, &name, &value);
        if (i || classes) {
            buffer_cstr(buffer, " ");
        }
        buffer_bytes(buffer, name.data, name.length);
        buffer_cstr(buffer, "=");
        buffer_json_string(buffer, value);
    }
    buffer_cstr(buffer, "}");
    dump_fields(buffer, node, kind);
    buffer_cstr(buffer, " children=");
    buffer_i64(buffer, (int64_t)child_count);
    buffer_cstr(buffer, "\n");
}

/* Draws the tree under `root`, one line per item of the canonical walk: a
 * node's line, or a group line naming a node-valued list. The walk's stack is
 * the tree's depth, never the C stack's. `anchor` is the absolute offset the
 * root's extent is relative to. */
static void dump_tree(dump_buffer *buffer, const markdown_core_node *root, uint32_t anchor) {
    markdown_core_walk walk;
    markdown_core_walk_item item;
    markdown_core_walk_begin(&walk, root);
    walk.anchor = anchor;
    while (!buffer->failed && markdown_core_walk_next(&walk, &item)) {
        if (!item.node) {
            dump_group_line(buffer, item.group, item.count, item.level - 1, markdown_core_walk_has_next(&walk));
            continue;
        }
        if (item.level) {
            if (!ensure_level(buffer, item.level - 1)) {
                break;
            }
            buffer->more[item.level - 1] = markdown_core_walk_has_next(&walk);
            extend_prefix(buffer, item.level - 1);
        }
        dump_node(buffer, item.node, item.place, item.level);
    }
    buffer->failed = buffer->failed || walk.failed;
    markdown_core_walk_end(&walk);
}

/* The source must cover the node: the dump draws the scope of every node
 * under it. */
markdown_core_status markdown_core_document_dump(const markdown_core_document *document, const markdown_core_node *node,
                                                 const uint8_t *source, size_t source_length, uint8_t **output,
                                                 size_t *length) {
    dump_buffer buffer = {0};
    markdown_core_place place = {0};
    source_lines lines;
    if (!tree_place(document->root, node, &place)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    if (place.end > source_length) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    if (!source_lines_read(&lines, source, source_length)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    buffer.source = source;
    buffer.lines = lines.starts;
    buffer.line_count = lines.count;
    /* A node's walk starts at its own extent, which is relative to the anchor
     * its relation had where it was written. */
    dump_tree(&buffer, node, (uint32_t)((int64_t)place.start - node->where.extent.lead));
    free(buffer.more);
    free(buffer.prefix);
    free(buffer.prefix_end);
    markdown_core_free(lines.starts);
    if (buffer.failed) {
        free(buffer.data);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    *output = buffer.data;
    *length = buffer.size;
    return MARKDOWN_CORE_OK;
}

void markdown_core_dump_free(uint8_t *output) { free(output); }
