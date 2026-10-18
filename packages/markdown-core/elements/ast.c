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
    size_t source_length;
    size_t *lines;
    size_t line_count;
} dump_buffer;

/* Where a Footnote, Specimen, Reference or Heading was written, for the
 * definition tables. */
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

/* THE DEFINITION TABLES a publish fills, one per kind of node the
 * document finds by label. Every Heading enters its table; the ones whose
 * text declares a reference label are the labeled ones. */
typedef enum { TABLE_FOOTNOTES, TABLE_SPECIMENS, TABLE_REFERENCES, TABLE_HEADINGS, TABLE_COUNT } definition_kind;

/* Whether a definition has a label, and the label in `label`. */
static bool definition_label(const markdown_core_node *node, markdown_core_chunk *label) {
    switch (node->kind) {
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        *label = node->as.footnote->label.value;
        return node->as.footnote->label.has_value;
    case MARKDOWN_CORE_NODE_SPECIMEN:
        *label = node->as.specimen->label.value;
        return node->as.specimen->label.has_value;
    case MARKDOWN_CORE_NODE_REFERENCE:
        *label = node->as.reference->label;
        return label->len > 0;
    default:
        *label = node->as.heading->label;
        return label->len > 0;
    }
}

/* Two labels in byte order, the shorter first on a common prefix. */
static int label_compare(const uint8_t *a, size_t a_length, const uint8_t *b, size_t b_length) {
    size_t common = a_length < b_length ? a_length : b_length;
    int order = common ? memcmp(a, b, common) : 0;
    return order ? order : (a_length > b_length) - (a_length < b_length);
}

/* A labeled definition as the label sort reads it (map.h, A KEYED ENTRY):
 * its label, where it starts and its node. */
typedef markdown_core_key_entry label_entry;

static inline label_entry label_entry_of(const uint8_t *label, size_t length, const markdown_core_node *node,
                                         uint32_t start) {
    return (label_entry){markdown_core_key_head(label, (bufsize_t)length), label, (bufsize_t)length, start, node};
}

static inline bool label_before(const label_entry *a, const label_entry *b) { return markdown_core_key_before(a, b); }

/* Sorts `count` entries by label, keeping the order of equal labels: the
 * runs already in order, which end at `ends`, are merged two by two, from one
 * array into the other, until one run holds them all. Returns the array that
 * holds them sorted, `entries` or `spare`. */
static label_entry *label_sort(label_entry *entries, label_entry *spare, size_t *ends, size_t count) {
    size_t runs = 0;
    for (size_t end = 1; end <= count; end++) {
        if (end == count || label_before(&entries[end], &entries[end - 1])) {
            ends[runs++] = end;
        }
    }
    while (runs > 1) {
        size_t merged = 0, low = 0;
        for (size_t run = 0; run < runs; run += 2) {
            const size_t middle = ends[run], high = run + 1 < runs ? ends[run + 1] : middle;
            size_t left = low, right = middle, out = low;
            while (left < middle && right < high) {
                spare[out++] = label_before(&entries[right], &entries[left]) ? entries[right++] : entries[left++];
            }
            while (left < middle) {
                spare[out++] = entries[left++];
            }
            while (right < high) {
                spare[out++] = entries[right++];
            }
            ends[merged++] = high;
            low = high;
        }
        runs = merged;
        label_entry *sorted = spare;
        spare = entries;
        entries = sorted;
    }
    return entries;
}

/* Whether two entries hold the same label. */
static inline bool label_same(const label_entry *a, const label_entry *b) { return markdown_core_key_same(a, b); }

/* Room for the entries of `count` labels, the spare array their sort merges
 * into, and the ends of its runs, in one allocation the caller frees. */
static label_entry *label_room(size_t count) {
    return markdown_core_realloc(NULL, count * (2 * sizeof(label_entry) + sizeof(size_t)));
}

/* The first `taken` entries of the room `label_room` made for `count`, in
 * label order and in their order among equal labels. */
static const label_entry *label_order(label_entry *entries, size_t taken, size_t count) {
    return label_sort(entries, entries + taken, (size_t *)(entries + 2 * count), taken);
}

/* The table's entries in source order. */
static inline bool table_order(markdown_core_parser *parser, definition_table *table) {
    return markdown_core_order_source_entries(&parser->source_order, table->values, table->count,
                                              sizeof(*table->values), definition_key);
}

/* The table's nodes, in the order of its entries, as node handles the
 * document borrows and owns from here. The handles take the entries' place:
 * each is smaller than an entry, so the one written lands on entries already
 * read. Byte copies, as the storage changes what it holds. */
static const markdown_core_node **table_handles(definition_table *table) {
    unsigned char *storage = (unsigned char *)table->values;
    for (size_t i = 0; i < table->count; i++) {
        const markdown_core_node *node;
        memcpy(&node, storage + i * sizeof(definition_entry) + offsetof(definition_entry, node), sizeof(node));
        memcpy(storage + i * sizeof(node), &node, sizeof(node));
    }
    table->values = NULL;
    table->capacity = 0;
    return (const markdown_core_node **)storage;
}

/* The table's nodes in source order, as node handles the document borrows,
 * into `*nodes`. */
static bool table_nodes(markdown_core_parser *parser, definition_table *table, const markdown_core_node ***nodes) {
    *nodes = NULL;
    if (!table->count) {
        return true;
    }
    if (!table_order(parser, table)) {
        return false;
    }
    *nodes = table_handles(table);
    return true;
}

/* The table in source order, and its labeled definitions in label order, as
 * node handles the document borrows. */
static inline bool table_seal(markdown_core_parser *parser, definition_table *table, markdown_core_definitions *out) {
    *out = (markdown_core_definitions){0};
    const markdown_core_node **nodes;
    if (!table_nodes(parser, table, &nodes)) {
        return false;
    }
    if (!nodes) {
        return true;
    }
    const markdown_core_node **labeled = markdown_core_alloc(table->count, sizeof(*labeled));
    label_entry *entries = label_room(table->count);
    if (!labeled || !entries) {
        markdown_core_free((void *)nodes);
        markdown_core_free((void *)labeled);
        markdown_core_free(entries);
        return false;
    }
    size_t labels = 0;
    for (size_t i = 0; i < table->count; i++) {
        markdown_core_chunk label;
        if (definition_label(nodes[i], &label)) {
            entries[labels++] = label_entry_of(label.data, (size_t)label.len, nodes[i], 0);
        }
    }
    const label_entry *sorted = label_order(entries, labels, table->count);
    for (size_t i = 0; i < labels; i++) {
        labeled[i] = sorted[i].value;
    }
    markdown_core_free(entries);
    *out = (markdown_core_definitions){{nodes, table->count, table->count, NULL}, labeled, labels};
    return true;
}

/* The children of `holder`, or no nodes when it is NULL. */
static bool relation_children(markdown_core_relation *relation, const char *group, const markdown_core_node *holder) {
    *relation = holder ? (markdown_core_relation){group, holder->children, holder->child_count, false, holder}
                       : (markdown_core_relation){group, NULL, 0, false, NULL};
    return true;
}

/* The node the owner's field `slot` holds. */
static bool relation_one(markdown_core_relation *relation, markdown_core_node **slot) {
    *relation = (markdown_core_relation){NULL, slot, 1, false, NULL};
    return true;
}

/* THE SHAPE of a kind's relations: most kinds own only their children; the
 * rest own fields too, in the order `markdown_core_relations_next` gives.
 * The shapes whose places a field holds come last (places_at). */
typedef enum {
    SHAPE_CHILDREN = 0,
    SHAPE_DOCUMENT,
    SHAPE_TABLE,
    SHAPE_DIRECTIVE,
    SHAPE_CITE,
    SHAPE_CITATION,
    SHAPE_CALLOUT,
    SHAPE_DEFINITION
} relation_shape;

/* A kind's slot: its value, and one bit for its class. The slot holds the
 * kind's shape and, above it, the definition table that lists nodes of the
 * kind, one more than its index, or 0 when none does. */
#define SHAPE_SLOTS 64
#define SHAPE_INDEX(kind) ((((kind) >> 9) | (kind)) & 0x3f)
#define SLOT_SHAPE 0x0f
#define SLOT_LOOKUP(table) (((table) + 1) << 4)
#define SLOT_TABLE(slot) ((slot) >> 4)
/* C99 has no _Static_assert: an array of negative size fails the build when a
 * condition is false. */
typedef char kind_slots_are_distinct[(MARKDOWN_CORE_NODE_KIND_COUNT <= 0x20 &&
                                      (MARKDOWN_CORE_NODE_TYPE_INLINE ^ MARKDOWN_CORE_NODE_TYPE_BLOCK) == 0x20 << 9 &&
                                      (MARKDOWN_CORE_NODE_TYPE_PRESENT >> 9 & 0x3f) == 0)
                                         ? 1
                                         : -1];
typedef char shapes_fit_their_slot[SHAPE_DEFINITION <= SLOT_SHAPE && SLOT_LOOKUP(TABLE_COUNT) <= 0xff ? 1 : -1];

static const uint8_t kind_slots[SHAPE_SLOTS] = {
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DOCUMENT)] = SHAPE_DOCUMENT,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_TABLE)] = SHAPE_TABLE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DIRECTIVE)] = SHAPE_DIRECTIVE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK)] = SHAPE_DIRECTIVE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_CALLOUT)] = SHAPE_CALLOUT,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_CITE)] = SHAPE_CITE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_CITATION)] = SHAPE_CITATION,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DEFINITION)] = SHAPE_DEFINITION,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_FOOTNOTE)] = SHAPE_CHILDREN | SLOT_LOOKUP(TABLE_FOOTNOTES),
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_SPECIMEN)] = SHAPE_CHILDREN | SLOT_LOOKUP(TABLE_SPECIMENS),
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_REFERENCE)] = SHAPE_CHILDREN | SLOT_LOOKUP(TABLE_REFERENCES),
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_HEADING)] = SHAPE_CHILDREN | SLOT_LOOKUP(TABLE_HEADINGS),
};

static inline unsigned slot_of(const markdown_core_node *node) { return kind_slots[SHAPE_INDEX(node->kind)]; }

static inline relation_shape shape_of(const markdown_core_node *node) {
    return (relation_shape)(slot_of(node) & SLOT_SHAPE);
}

/* Whether stepping `node`'s relations would find neither a node nor a group
 * line, read from its fields: a kind whose relations are all optional or
 * ungrouped, with nothing in them. A group the dump always draws keeps its
 * owner's relations open. */
static bool fields_empty(const markdown_core_node *node, relation_shape shape);

static inline bool relations_empty(const markdown_core_node *node) {
    relation_shape shape = shape_of(node);
    return shape == SHAPE_CHILDREN ? !node->child_count : fields_empty(node, shape);
}

/* The same question for a kind that owns fields besides its children. */
static bool fields_empty(const markdown_core_node *node, relation_shape shape) {
    switch (shape) {
    case SHAPE_CHILDREN:
    case SHAPE_CITE:
        return !node->child_count;
    case SHAPE_DIRECTIVE:
        return !node->child_count && !markdown_core_directive_label(node);
    case SHAPE_CALLOUT:
        return !node->child_count && !node->as.callout->title;
    case SHAPE_DOCUMENT:
        return !node->child_count && !node->as.document->metadata;
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
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool relations_next(markdown_core_relation_cursor *cursor,
                                                                           markdown_core_relation *relation,
                                                                           bool *more) {
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
        markdown_core_table *table = node->opaque;
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
        /* A table's rows, in head, content and foot order, are its children,
         * which a table always has: each group is a slice of them. */
        const uint32_t count = step == 1 ? table->head_count : step == 2 ? table->content_count : table->foot_count;
        *relation =
            (markdown_core_relation){table_groups[step - 1], node->children + cursor->index, count, step > 1, node};
        cursor->index += count;
        *more = step < 3;
        return true;
    }
    case SHAPE_DIRECTIVE:
        if (step == 0) {
            markdown_core_directive_value *directive = node->opaque;
            if (directive->label) {
                *more = true;
                return relation_one(relation, &directive->label);
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
    case SHAPE_CITE:
        *more = false;
        return step == 0 && relation_children(relation, NULL, node);
    case SHAPE_CITATION: {
        markdown_core_citation_item *citation = node->as.citation;
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
        /* The term, then each body, a child of the definition. */
        if (step == 0) {
            *more = node->child_count > 0;
            return relation_children(relation, "DefinitionTerm", node->as.definition->term);
        }
        if (cursor->index == node->child_count) {
            return false;
        }
        const markdown_core_node *body = node->children[cursor->index++];
        *more = cursor->index < node->child_count;
        return relation_children(relation, "DefinitionBody", body);
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

/* WHERE A NODE'S PLACES ARE HELD while the parse builds the tree (node.h):
 * by the node itself, or by the private node a callout's title or a
 * definition's term hangs from, which reads the callout's or the
 * definition's inline content; NULL for a callout without a title.
 * Publishing gives every node its runs in its own `source`. */
static inline markdown_core_places **places_at(const markdown_core_node *node, relation_shape shape) {
    if (shape < SHAPE_CALLOUT) {
        return (markdown_core_places **)&node->source.places;
    }
    if (shape == SHAPE_CALLOUT) {
        return node->as.callout->title ? &node->as.callout->title->source.places : NULL;
    }
    return &node->as.definition->term->source.places;
}

/* AN INLINE ROOT'S PLACES IN CONTENT ORDER: the content offset each place's
 * bytes start at, how many there are, and the source range they were read
 * from. A copied place, whose source is as long as its content, reads each
 * content byte from one source byte; any other reads all of its content from
 * all of its source, and one of length 0 reads none. Content and source both
 * increase along them, and the source between two of them is not the
 * root's. */
typedef struct {
    uint32_t content, length, start, end;
} content_place;

typedef struct {
    content_place *items;
    size_t count, capacity;
} content_places;

/* Reads a root's places into `table`, whose storage it reuses. False when it
 * could not grow. */
static bool content_places_read(content_places *table, const markdown_core_places *places) {
    content_place *grown = markdown_core_reserve(table->items, &table->capacity, places->count, sizeof(*table->items));
    if (!grown) {
        return false;
    }
    table->items = grown;
    uint32_t content = 0;
    for (uint32_t i = 0; i < places->count; i++) {
        const markdown_core_source_place place = places->items[i];
        table->items[i] = (content_place){content, place.length, place.start, place.end};
        content += place.length;
    }
    table->count = places->count;
    return true;
}

static inline bool content_place_copied(const content_place *place) {
    return place->end - place->start == place->length;
}

/* The place content offset `offset` is in: the last that starts at or
 * before it and reads content, or the first. */
static inline const content_place *content_place_at(const content_places *table, uint32_t offset) {
    const content_place *at = table->items;
    for (size_t count = table->count; count > 1;) {
        const size_t half = count / 2;
        at = at[half].content <= offset ? at + half : at;
        count -= half;
    }
    while (at > table->items && !at->length) {
        at--;
    }
    return at;
}

/* Where a published node's source lies, its first run measured from
 * `anchor`: from where its first run starts to where its last ends. */
static markdown_core_place runs_window(const markdown_core_node *node, uint32_t anchor) {
    const markdown_core_runs *runs = node->source.runs;
    int64_t at = anchor;
    markdown_core_place window;
    for (uint32_t i = 0; i < runs->count; i++) {
        const markdown_core_run run = runs->items[i];
        at += run.lead;
        if (!i) {
            window.start = (uint32_t)at;
        }
        at += run.span;
    }
    window.end = (uint32_t)at;
    return window;
}

/* PUBLISHING CONTINUES THE PREVIOUS TREE (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.9). The walk that gives each node its id and extent also
 * matches it, relation by relation, to the old node it continues, and
 * decides in post-order whether it equals that old node as a value.
 *
 * Matching runs within the relation of a matched owner. A node's source
 * runs from where its first run starts to where its last ends. An old node's
 * anchor is the first byte of its source, which an append leaves at its
 * offset, and a new node of its kind whose source holds the anchor continues
 * the earliest such old sibling not yet passed; a node with no source has no
 * anchor. Both relations are in source order, so one pointer into the old
 * relation moves forward with the new one, past each old node matched and
 * those before it (publish_match). The root continues the old root. A new
 * node that continues nothing takes the next id.
 *
 * A matched node is SAME when its scalars, extent and runs equal its old
 * node's, both have the same relations, every old node of each relation was
 * matched and every new node of it is same: matching is monotone and
 * one-to-one, so the old node then holds exactly those old nodes, in order. Each same pair
 * exchanges its nodes whole, so the old object holds the new state and the
 * storage it owns and borrows, and takes the new node's slot in the new tree
 * (publish_continue). The old tree, which then holds the new objects with
 * the old states and every old node nothing matched, is released with the
 * old storage. A fresh parse continues no tree, so no node is matched and
 * every node takes the next id in walk order. */

/* A same pair, recorded in post-order in the streaming parse's room for
 * them (parser.h, `same_pairs`): `node` and `old` exchange their states,
 * and each takes the slot that held the other, `slot` in the new tree and
 * `old_slot` in the old. */
typedef struct markdown_core_same_pair {
    markdown_core_node *node, *old;
    markdown_core_node **slot, **old_slot;
} publish_swap;

static inline bool is_link(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_LINK || node->kind == MARKDOWN_CORE_NODE_EMBEDDED;
}

/* A node whose relations are being published: where the node and its
 * source start, its cursor and whether that may hold another relation, and,
 * while a nested node's relations are published, the rest of the relation in
 * hand, the offset its next node's extent is relative to (the end of the
 * previous node of its relation, or the owner's start) and where its next
 * node's first run is measured from (where the source of the previous node of
 * its relation ends, or the owner's begins). */
typedef struct {
    markdown_core_relation_cursor cursor;
    markdown_core_node **item, **end;
    uint32_t start, source_start, anchor, source;
    bool more;
    /* Whether the relation in hand is in an inline root's content, and
     * whether this frame's node is the root whose content it is. */
    bool content, root;
} publish_frame;

/* The frames live on the parser's walk stack, which completion has just
 * left, above the `below` bytes that hold the frames of the matching walk
 * that is publishing them (A STREAMING PUBLISH). */
typedef struct {
    markdown_core_parser *parser;
    size_t below;
    publish_frame *frames;
    size_t capacity;
} publish_stack;

/* The stack's frames, above `below` bytes of the walk stack as it is now. */
static void publish_above(publish_stack *stack, size_t below) {
    const markdown_core_parser *parser = stack->parser;
    stack->below = below;
    stack->frames = parser->walk_stack ? (publish_frame *)((unsigned char *)parser->walk_stack + below) : NULL;
    stack->capacity = parser->walk_stack_size > below ? (parser->walk_stack_size - below) / sizeof(publish_frame) : 0;
}

static bool publish_grow(publish_stack *stack) {
    size_t capacity = stack->capacity ? stack->capacity * 2 : 16;
    if (!markdown_core_parser_walk_stack(stack->parser, 1, stack->below + capacity * sizeof(publish_frame))) {
        return false;
    }
    publish_above(stack, stack->below);
    return true;
}

/* The relation in hand, the nodes from `item` to `end` of which `item` is the
 * next to publish: the walk keeps it apart from its frame, which holds it
 * only while the frame waits. */
typedef struct {
    markdown_core_node **item, **end;
    uint32_t anchor, source;
} publish_relation;

/* The end of the `count` nodes at `items`, the range a relation steps
 * through; a relation that holds none has no storage to point into. */
static inline markdown_core_node **range_end(markdown_core_node **items, size_t count) {
    return count ? items + count : items;
}

/* The `count` nodes at `items` as the relation in hand. */
static inline void publish_range(publish_relation *hand, markdown_core_node **items, size_t count) {
    hand->item = items;
    hand->end = range_end(items, count);
}

/* `node`'s first relation, which holds a node or a group, the node whose
 * children it holds, if any, and whether more may follow it: the one
 * relation of a kind that owns only its children, or the first its cursor
 * finds. False when they hold no node. */
static inline bool publish_first(const markdown_core_node *node, relation_shape shape,
                                 markdown_core_relation_cursor *cursor, publish_relation *first,
                                 const markdown_core_node **holder, bool *more) {
    if (shape == SHAPE_CHILDREN) {
        *more = false;
        if (!node->child_count) {
            return false;
        }
        first->item = node->children;
        first->end = node->children + node->child_count;
        *holder = node;
        return true;
    }
    markdown_core_relation relation;
    *cursor = (markdown_core_relation_cursor){node, (uint8_t)shape, 0, 0};
    if (!relations_next(cursor, &relation, more)) {
        return false;
    }
    publish_range(first, relation.items, relation.count);
    *holder = relation.holder;
    return relation.count > 0 || *more;
}

/* The relation `relation` of the node `frame` is for, after the one in
 * hand: measured from where the one in hand ended when it goes on from it,
 * and from the node's start otherwise. */
static inline publish_relation publish_next(const publish_frame *frame, const publish_relation *hand,
                                            const markdown_core_relation *relation) {
    publish_relation next = relation->continues ? (publish_relation){NULL, NULL, hand->anchor, hand->source}
                                                : (publish_relation){NULL, NULL, frame->start, frame->source_start};
    publish_range(&next, relation->items, relation->count);
    return next;
}

/* What one publish carries through its walks: the walk stack, how many
 * same pairs matching has recorded, the lookup tables, the last id issued,
 * the levels of the spine (A STREAMING PUBLISH, below) and how many
 * committed definitions it lists. */
typedef struct {
    publish_stack stack;
    size_t swap_count;
    definition_table tables[TABLE_COUNT];
    uint64_t next_id;
    const markdown_core_level *levels;
    size_t level_count;
    /* How many levels from the root differ whatever the walk finds
     * (parser.h, `touched_levels`). */
    size_t touched_levels;
    size_t kept_count;
    /* The places of the inline root whose content the walk is in, which
     * give its nodes their runs: roots never nest, so one table serves every
     * root in turn. */
    content_places places;
    /* The pool the runs are cut from. */
    markdown_core_node_pool *pool;
} publish_walk;

/* Where a node published from `hand` lies: its range, in the content of the
 * root whose places the walk holds when it is in one, its source, and
 * whether it is an inline root, whose places the walk now holds. */
typedef struct {
    markdown_core_place place, source;
    bool root;
} publish_placed;

/* The bytes of a list of `count` runs. */
static inline size_t runs_bytes(uint32_t count) {
    return sizeof(markdown_core_runs) + (size_t)count * sizeof(markdown_core_run);
}

/* A run fits where a place was. */
typedef char runs_fit_in_places[sizeof(markdown_core_run) <= sizeof(markdown_core_source_place) &&
                                        offsetof(markdown_core_runs, items) + sizeof(markdown_core_run) <=
                                            offsetof(markdown_core_places, items) + sizeof(markdown_core_source_place)
                                    ? 1
                                    : -1];

/* A block's runs as they are published (node.h), from its places, which
 * `held` holds: its places within its place, those of length 0 clipped to
 * it, an empty one an empty run, with the source before the first and after
 * the last its own, so they run from its start to its end, and the runs that
 * touch joined; the first measured from `anchor`, each other from the end of
 * the run before. The node takes its places' list as its runs, written in
 * place: a run is no larger than a place, and the first place opens no run,
 * so each run lands on places already read. The list's bytes are copied, as
 * its storage changes what it holds. */
static void publish_place_runs(markdown_core_node *node, markdown_core_places **held, markdown_core_place place,
                               uint32_t anchor) {
    unsigned char *const list = (unsigned char *)*held;
    const uint32_t places = (*held)->count;
    assert(places > 0);
    uint32_t count = 0, start = place.start, end = place.start, from = anchor;
    bool first = true;
    for (uint32_t i = 0; i < places; i++) {
        markdown_core_source_place item;
        memcpy(&item, list + offsetof(markdown_core_places, items) + i * sizeof(item), sizeof(item));
        if (!item.length) {
            /* Clipped to the node's place: one that only touches it is not
             * the node's, and an empty one within it is an empty run. */
            const uint32_t clipped_start = item.start > place.start ? item.start : place.start;
            const uint32_t clipped_end = item.end < place.end ? item.end : place.end;
            if (clipped_start > clipped_end || (clipped_start == clipped_end && item.start < item.end)) {
                continue;
            }
            item.start = clipped_start;
            item.end = clipped_end;
        }
        /* A place apart from the one before ends a run. */
        if (item.start > end && !first) {
            const markdown_core_run run = {(int32_t)((int64_t)start - from), end - start};
            memcpy(list + offsetof(markdown_core_runs, items) + count++ * sizeof(run), &run, sizeof(run));
            from = end;
            start = item.start;
        }
        first = false;
        end = item.end > end ? item.end : end;
    }
    assert(end <= place.end);
    const markdown_core_run run = {(int32_t)((int64_t)start - from), place.end - start};
    memcpy(list + offsetof(markdown_core_runs, items) + count++ * sizeof(run), &run, sizeof(run));
    memcpy(list + offsetof(markdown_core_runs, count), &count, sizeof(count));
    *held = NULL;
    assert(!node->source.runs);
    node->source.runs = (markdown_core_runs *)list;
}

/* The runs of a block without places: its range, measured from `anchor`.
 * False when they could not be allocated. */
static inline bool publish_range_run(markdown_core_node_pool *pool, markdown_core_node *node, markdown_core_place place,
                                     uint32_t anchor) {
    markdown_core_runs *runs = markdown_core_node_pool_bytes(pool, runs_bytes(1));
    if (!runs) {
        return false;
    }
    runs->count = 1;
    runs->items[0] = (markdown_core_run){(int32_t)((int64_t)place.start - anchor), place.end - place.start};
    assert(!node->source.runs);
    node->source.runs = runs;
    return true;
}

/* An inline node's runs, its content range `place` measured in the root
 * whose places the walk holds: its source `window`, from where its first
 * content byte was read to where its last was, or the empty window where its
 * start was read when it holds none, less the gaps between the root's places;
 * the first measured from `anchor`, each other from the end of the run
 * before. One pass over the places from the one its start is read from to
 * the one its last byte is: each gap between two of them in the window ends a
 * run. False when they could not be allocated. */
static inline bool publish_inline_runs(publish_walk *walk, markdown_core_node *node, markdown_core_place place,
                                       uint32_t anchor, markdown_core_place *window) {
    assert(!node->source.places);
    const content_place *const past = walk->places.items + walk->places.count;
    const content_place *const at = content_place_at(&walk->places, place.start);
    const uint32_t first = place.start >= at->content + at->length ? at->end
                           : content_place_copied(at)              ? at->start + (place.start - at->content)
                                                                   : at->start;
    uint32_t end = first;
    /* The place its last byte is read from: the last that starts before its
     * end and reads content. */
    const content_place *to = at;
    if (place.end > place.start) {
        for (const content_place *p = at + 1; p < past && p->content < place.end; p++) {
            if (p->length) {
                to = p;
            }
        }
        end = place.end - 1 >= to->content + to->length || !content_place_copied(to)
                  ? to->end
                  : to->start + (place.end - to->content);
    }
    *window = (markdown_core_place){first, end};
    markdown_core_runs *runs = markdown_core_node_pool_bytes(walk->pool, runs_bytes((uint32_t)(to - at + 1)));
    if (!runs) {
        return false;
    }
    uint32_t count = 0, from = anchor, start = first;
    const content_place *p = at;
    while (p + 1 < past && p->end <= start) {
        p++;
    }
    for (; end > first && p + 1 < past && p->end < end; p++) {
        const uint32_t gap = p->end, next = p[1].start;
        if (next <= gap) {
            continue;
        }
        if (gap > start) {
            runs->items[count++] = (markdown_core_run){(int32_t)((int64_t)start - from), gap - start};
            from = gap;
        }
        start = next > start ? next : start;
    }
    if (start < end || !count) {
        end = start < end ? end : start;
        runs->items[count++] = (markdown_core_run){(int32_t)((int64_t)start - from), end - start};
    }
    runs->count = count;
    node->source.runs = runs;
    return true;
}

/* Lists the entry `entry` of table `table` as a COMMITTED definition the
 * streaming parse keeps once the publish is committed. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool publish_keep(publish_walk *walk, uint32_t table, size_t entry) {
    markdown_core_parser *parser = walk->stack.parser;
    markdown_core_kept *kept = markdown_core_reserve(parser->stream->kept, &parser->stream->kept_capacity,
                                                     walk->kept_count + 1, sizeof(*kept));
    if (!kept) {
        return false;
    }
    parser->stream->kept = kept;
    kept[walk->kept_count++] = (markdown_core_kept){table, (uint32_t)entry};
    return true;
}

/* Records `node` in table `table` at `start`: a streaming parse keeps it
 * there too when it is COMMITTED. A definition is a block, and every block
 * in the subtree of a COMMITTED block settled in a commit, so it is one
 * (blocks.c, A BLOCK SETTLES). */
static inline bool publish_list(publish_walk *walk, markdown_core_node *node, uint32_t table, uint32_t start) {
    definition_table *values = &walk->tables[table];
    return table_add(values, node, start) &&
           (!walk->stack.parser->stream || !(node->flags & MARKDOWN_CORE_NODE__COMMITTED) ||
            publish_keep(walk, table, values->count - 1));
}

/* Gives `node`, of slot `slot`, its extent against the relation in hand and
 * its runs, and records it in its lookup table when its slot says the tables
 * find it, at its source start. Its place is in the content of the root whose
 * places the walk holds when `content` says so; a block whose places read
 * content is an inline root, and the walk takes its places first. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool publish_node(publish_walk *walk, markdown_core_node *node,
                                                                         unsigned slot, const publish_relation *hand,
                                                                         bool content, publish_placed *placed) {
    const markdown_core_place place = node->where.place;
    node->where.extent =
        (markdown_core_extent){(int32_t)((int64_t)place.start - (int64_t)hand->anchor), place.end - place.start};
    placed->place = place;
    placed->root = false;
    if (content) {
        if (!publish_inline_runs(walk, node, place, hand->source, &placed->source)) {
            return false;
        }
    } else {
        markdown_core_places **held = places_at(node, (relation_shape)(slot & SLOT_SHAPE));
        const markdown_core_places *places = held ? *held : NULL;
        placed->source = place;
        if (places && places->content) {
            if (!content_places_read(&walk->places, places)) {
                return false;
            }
            placed->root = true;
        }
        if (places) {
            publish_place_runs(node, held, place, hand->source);
        } else if (!publish_range_run(walk->pool, node, place, hand->source)) {
            return false;
        }
    }
    return !SLOT_TABLE(slot) || publish_list(walk, node, SLOT_TABLE(slot) - 1, placed->source.start);
}

/* The relation in hand becomes the first of a node placed as `placed`: an
 * inline root's content, which runs from 0 and whose places the walk holds,
 * or one in the coordinates of the relation the node is in. */
static inline void publish_enter(publish_frame *frame, const publish_placed *placed, publish_relation *hand,
                                 bool content) {
    frame->root = placed->root;
    frame->content = content || frame->root;
    frame->start = placed->place.start;
    frame->source_start = placed->source.start;
    hand->anchor = frame->root ? 0 : placed->place.start;
    hand->source = placed->source.start;
}

/* PUBLISHING WHAT CONTINUES NOTHING: every node below `node`, placed as
 * `placed`, takes the next id in canonical walk order: a node, then the
 * nodes of its relations in order, with one frame per node whose relations
 * are open. A node's place becomes its extent as it is published, so each
 * frame keeps the offsets its later nodes are relative to, and a frame with
 * nothing left gives its slot to its last node's own. The frames sit above
 * what the stack holds below them. A fresh parse publishes its whole tree
 * this way, below the root. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool publish_fresh(publish_walk *walk,
                                                                          const markdown_core_node *node,
                                                                          relation_shape shape,
                                                                          const publish_placed *placed, bool content) {
    publish_stack *stack = &walk->stack;
    /* The ids the walk issues stay in hand until it ends. */
    size_t count = 0;
    uint64_t next_id = walk->next_id;
    markdown_core_relation_cursor cursor;
    publish_relation hand, first;
    const markdown_core_node *holder;
    publish_placed child_placed;
    bool more;
    if (!publish_first(node, shape, &cursor, &first, &holder, &more)) {
        return true;
    }
    if (count == stack->capacity && !publish_grow(stack)) {
        return false;
    }
    publish_frame *frame = &stack->frames[count++];
    *frame = (publish_frame){.cursor = cursor, .more = more};
    hand = first;
    publish_enter(frame, placed, &hand, content);
    while (count) {
        if (hand.item == hand.end) {
            markdown_core_relation relation;
            if (frame->more && relations_next(&frame->cursor, &relation, &frame->more)) {
                hand = publish_next(frame, &hand, &relation);
                /* A root's later relations are not its content. */
                if (frame->root) {
                    frame->root = frame->content = false;
                }
            } else if (--count) {
                frame--;
                hand = (publish_relation){frame->item, frame->end, frame->anchor, frame->source};
            }
            continue;
        }
        markdown_core_node *child = *hand.item++;
        unsigned slot = slot_of(child);
        relation_shape child_shape = (relation_shape)(slot & SLOT_SHAPE);
        content = frame->content;
        /* Every node it publishes is new: a node the previous tree published
         * is matched, or skipped as one it shares (A STREAMING PUBLISH). */
        assert(!child->id);
        if (!publish_node(walk, child, slot, &hand, content, &child_placed)) {
            return false;
        }
        child->id = ++next_id;
        hand.anchor = child_placed.place.end;
        hand.source = child_placed.source.end;
        if (!publish_first(child, child_shape, &cursor, &first, &holder, &more)) {
            continue;
        }
        /* A frame with nothing left gives its slot to the node's own; any
         * other waits with the relation in hand. */
        if (hand.item != hand.end || frame->more) {
            frame->item = hand.item;
            frame->end = hand.end;
            frame->anchor = hand.anchor;
            frame->source = hand.source;
            if (count == stack->capacity) {
                if (!publish_grow(stack)) {
                    return false;
                }
                frame = &stack->frames[count - 1];
            }
            frame++;
            count++;
        }
        frame->more = more;
        if (more) {
            frame->cursor = cursor;
        }
        hand = first;
        publish_enter(frame, &child_placed, &hand, content);
    }
    walk->next_id = next_id;
    return true;
}

/* A STREAMING PUBLISH (blocks.c, THE LEVELS) continues a tree that shares
 * the committed blocks with it. The publish before left a level for each
 * builder of the committed parse and for its pending block: the object the
 * previous tree holds for it, and how many of its children that tree
 * published as they are. Those children are COMMITTED, one object in both
 * trees, which nothing writes, as is every node below them, so the publish
 * skips them.
 *
 * The spine's object at a level's depth is the copy of the level's builder
 * while it is open, and the builder itself once it has closed, settled at
 * the index its parent's level published next. A matched node that is the
 * spine's object at a depth whose level published children is matched to
 * the level's old object, the first old node of its relation. A relation
 * whose holder is the object of a level -- a node's children, a table's row
 * groups, a definition's body -- goes on, on both sides, from the level's
 * count, its next node measured from where the last one skipped ends, and a
 * definition at a level goes on from the body at the level's count. Any
 * other node the previous tree published that the new tree holds, such as
 * the metadata a copy of the document shares, is one object in both, which
 * the publish leaves as it is where it meets it. */

/* A matched node whose relations are being published: its frame of the
 * walk, the slots that hold the node and the old node it continues, the old
 * node's relations not yet paired, the old relation paired with the
 * one in hand -- its old nodes from `old_item` to `old_end`, of which
 * `old_item` is the next, and where that one's first run is measured from
 * (where the source of the old node before it ends, or the old node's
 * begins) -- and whether the node is same so far. */
typedef struct {
    publish_frame walk;
    markdown_core_node **slot, **old_slot;
    bool same;
    markdown_core_relation_cursor old_cursor;
    markdown_core_relation old_relation;
    /* The old relation's field, or INT_MAX when the old node has no more. */
    int old_field;
    markdown_core_node **old_item, **old_end;
    uint32_t old_start, old_source;
    /* Its lookup table entry counted from 1 (0 for none). */
    size_t entry;
    /* The depth of the spine whose object the node is, matched to the
     * level's old object, and the level whose object holds the relation in
     * hand; -1 for none. */
    int spine, level;
} publish_match_frame;

/* The field a cursor's last relation holds: stepping leaves the cursor one
 * past it. A kind that owns only its children has field 0. */
static inline int relation_field(const markdown_core_relation_cursor *cursor, relation_shape shape) {
    return shape == SHAPE_CHILDREN ? 0 : cursor->step - 1;
}

/* The level whose object holds a relation of the node at depth `spine`: the
 * node's own, or, for a definition's body, the next. -1 for none. */
static inline int publish_level_of(const publish_walk *walk, int spine, const markdown_core_node *holder) {
    if (spine >= 0 && holder) {
        if (walk->levels[spine].object == holder) {
            return spine;
        }
        if ((size_t)spine + 1 < walk->level_count && walk->levels[spine + 1].object == holder) {
            return spine + 1;
        }
    }
    return -1;
}

/* The relation in hand, which the object of level `level` holds, past the
 * children the level published as they are: its next node is measured from
 * where the last of them ends. A level that published none skips none. */
static inline void publish_skip(const publish_walk *walk, int level, publish_relation *hand) {
    if (level < 0 || !walk->levels[level].count) {
        return;
    }
    const markdown_core_level *at = &walk->levels[level];
    markdown_core_node **past = at->object->children + at->count;
    if (hand->item < past) {
        hand->item = past < hand->end ? past : hand->end;
        hand->anchor = hand->source = at->end;
    }
}

/* The old relation paired with the one in hand, which the level's old
 * object holds, past the same children. */
static inline void publish_old_skip(const publish_walk *walk, publish_match_frame *frame) {
    if (frame->level < 0 || !walk->levels[frame->level].count) {
        return;
    }
    const markdown_core_level *at = &walk->levels[frame->level];
    markdown_core_node **past = at->published->children + at->count;
    if (frame->old_item < past) {
        frame->old_item = past < frame->old_end ? past : frame->old_end;
        frame->old_source = at->end;
    }
}

/* A definition the spine's object at depth `spine` goes on, past its first
 * relation, from the body at the level's count. */
static inline void publish_skip_bodies(const publish_walk *walk, int spine, relation_shape shape,
                                       markdown_core_relation_cursor *cursor) {
    if (spine >= 0 && shape == SHAPE_DEFINITION) {
        cursor->index = walk->levels[spine].count;
    }
}

static void old_relation_next(publish_match_frame *frame) {
    bool more;
    frame->old_field =
        relations_next(&frame->old_cursor, &frame->old_relation, &more) ? frame->old_cursor.step - 1 : INT_MAX;
}

/* Pairs the new node's relation `field` with the old node's. A relation
 * either node has and the other lacks makes the node differ. A relation that
 * goes on from the one before is one relation with it, on both sides: its
 * nodes follow the ones before in the same storage, so the old relation
 * paired with the first holds the old nodes of those that go on from it too,
 * and one that goes on pairs with nothing new. */
static void publish_pair(const publish_walk *walk, publish_match_frame *frame, int field, bool continues) {
    if (continues) {
        return;
    }
    while (frame->old_field < field) {
        frame->same = false;
        old_relation_next(frame);
    }
    if (frame->old_field == field) {
        markdown_core_node **items = frame->old_relation.items;
        size_t count = frame->old_relation.count;
        assert(frame->level < 0 || frame->old_relation.holder == walk->levels[frame->level].published);
        frame->old_source = frame->old_start;
        old_relation_next(frame);
        while (frame->old_field != INT_MAX && frame->old_relation.continues) {
            count += frame->old_relation.count;
            old_relation_next(frame);
        }
        frame->old_item = items;
        frame->old_end = range_end(items, count);
        publish_old_skip(walk, frame);
    } else {
        frame->same = false;
        frame->old_item = frame->old_end = NULL;
    }
}

/* Pairs the new node's first relation, field `field`, with the old node's,
 * as publish_first finds it: a kind that owns only its children has that one
 * relation on both sides, and its old nodes are the old node's children. */
static inline void publish_pair_first(const publish_walk *walk, publish_match_frame *frame,
                                      const markdown_core_node *old, relation_shape shape, int field) {
    if (shape == SHAPE_CHILDREN) {
        frame->old_field = INT_MAX;
        frame->old_source = frame->old_start;
        frame->old_item = old->children;
        frame->old_end = range_end(old->children, old->child_count);
        publish_old_skip(walk, frame);
        return;
    }
    markdown_core_relations_begin(&frame->old_cursor, old);
    publish_skip_bodies(walk, frame->spine, shape, &frame->old_cursor);
    old_relation_next(frame);
    publish_pair(walk, frame, field, false);
}

/* The slot of the old node a node whose source is `source` continues in the
 * old relation `owner` pairs with the one in hand, and where the matched old
 * node's source starts; NULL when it continues none. The relation stands at
 * the earliest old node not yet passed: a node that continues one passes it
 * and every old node before it, and the old nodes after it whose anchors the
 * node's source holds remain for the next node, as the cells of a grid table
 * that span rows start inside the sources of the cells before them (plan
 * 5.9). A node that continues none passes the old nodes whose anchors lie
 * before its source, which no later node, starting no earlier, holds. An old
 * node passed without being matched makes the owner differ. */
static markdown_core_node **publish_match(publish_match_frame *owner, const markdown_core_node *node,
                                          markdown_core_place source, uint32_t *old_start) {
    uint32_t from = owner->old_source;
    for (markdown_core_node **slot = owner->old_item; slot != owner->old_end; slot++) {
        const markdown_core_node *old = *slot;
        const markdown_core_place window = runs_window(old, from);
        const bool anchored = window.start < window.end;
        if (anchored && window.start >= source.end) {
            break;
        }
        if (anchored && window.start >= source.start && old->kind == node->kind) {
            if (slot != owner->old_item) {
                owner->same = false;
            }
            owner->old_item = slot + 1;
            owner->old_source = window.end;
            *old_start = window.start;
            return slot;
        }
        from = window.end;
    }
    owner->same = false;
    while (owner->old_item != owner->old_end) {
        const markdown_core_place window = runs_window(*owner->old_item, owner->old_source);
        if (window.start < window.end && window.start >= source.start) {
            break;
        }
        owner->old_source = window.end;
        owner->old_item++;
    }
    return NULL;
}

/* The slot of the old object of level `spine`, which the spine's object at
 * its depth continues when the level published children: the first old node
 * of the relation `owner` pairs with the one in hand, past those the
 * relation skipped. NULL when the level published none. */
static markdown_core_node **publish_spine_match(const publish_walk *walk, publish_match_frame *owner, int spine,
                                                uint32_t *old_start) {
    const markdown_core_level *at = &walk->levels[spine];
    if (!at->count || owner->old_item == owner->old_end || *owner->old_item != at->published) {
        return NULL;
    }
    assert(at->published->kind == at->object->kind);
    markdown_core_node **slot = owner->old_item++;
    const markdown_core_place window = runs_window(at->published, owner->old_source);
    *old_start = window.start;
    owner->old_source = window.end;
    return slot;
}

/* A node the previous tree published, or one superseding it with its id
 * (blocks.c, A BLOCK SUPERSEDED), that the new tree holds: the old relation
 * steps past the old node of its id, an old node before it or an old node
 * that is not the node making the owner differ, and so does the relation in
 * hand, the node keeping its extent and runs. */
static void publish_shared(publish_match_frame *owner, const markdown_core_node *node, publish_relation *hand) {
    bool found = false;
    while (!found && owner->old_item != owner->old_end) {
        const markdown_core_node *old = *owner->old_item++;
        owner->old_source = runs_window(old, owner->old_source).end;
        found = old->id == node->id;
        if (old != node) {
            owner->same = false;
        }
    }
    if (!found) {
        owner->same = false;
    }
    hand->anchor = (uint32_t)((int64_t)hand->anchor + node->where.extent.lead) + node->where.extent.span;
    hand->source = runs_window(node, hand->source).end;
}

static bool scalars_equal(const markdown_core_node *a, const markdown_core_node *b);

static bool runs_equal(const markdown_core_runs *a, const markdown_core_runs *b) {
    if (a->count != b->count) {
        return false;
    }
    for (uint32_t i = 0; i < a->count; i++) {
        const markdown_core_run x = a->items[i], y = b->items[i];
        if (x.lead != y.lead || x.span != y.span) {
            return false;
        }
    }
    return true;
}

/* Whether a matched node equals its old node as a value, before their
 * relations are compared. */
static bool publish_same(const markdown_core_node *node, const markdown_core_node *old) {
    return node->where.extent.lead == old->where.extent.lead && node->where.extent.span == old->where.extent.span &&
           runs_equal(node->source.runs, old->source.runs) && scalars_equal(node, old);
}

/* A matched node's verdict, once its relations are published: a same node
 * is recorded after the pairs of its subtree, and the lookup tables name the
 * old object, which holds its state once the pair is committed; a node that
 * differs makes its owner differ. False when the pair could not be
 * recorded. */
static bool publish_verdict(publish_walk *walk, publish_match_frame *owner, markdown_core_node **slot,
                            markdown_core_node **old_slot, bool same, size_t entry) {
    if (!same) {
        owner->same = false;
        return true;
    }
    markdown_core_stream *stream = walk->stack.parser->stream;
    publish_swap *swaps =
        markdown_core_reserve(stream->same_pairs, &stream->same_pair_capacity, walk->swap_count + 1, sizeof(*swaps));
    if (!swaps) {
        return false;
    }
    stream->same_pairs = swaps;
    markdown_core_node *node = *slot, *old = *old_slot;
    swaps[walk->swap_count++] = (publish_swap){node, old, slot, old_slot};
    if (entry) {
        walk->tables[SLOT_TABLE(slot_of(node)) - 1].values[entry - 1].node = old;
    }
    return true;
}

/* PUBLISHING WHAT CONTINUES THE PREVIOUS TREE: the root continues the
 * previous root, measured against `start`, and each matched node pairs its
 * relations with its old node's and matches their nodes in turn, one frame
 * per matched node whose relations are open, on the walk stack, each waiting
 * for its verdict.
 * A node that continues nothing takes the next id, and so does everything
 * below it, which publish_fresh publishes. `*same` is the root's verdict. */
static bool publish_matched(publish_walk *walk, markdown_core_node *root, markdown_core_node *previous,
                            const publish_relation *start, bool *same) {
    markdown_core_parser *parser = walk->stack.parser;
    publish_match_frame *frame;
    size_t count = 0;
    markdown_core_relation_cursor cursor, old_cursor;
    publish_relation hand = *start, first, old_first;
    const markdown_core_node *holder, *old_holder;
    publish_placed placed;
    bool more, old_more, ok = true;
    unsigned slot = slot_of(root);
    relation_shape shape = (relation_shape)(slot & SLOT_SHAPE);
    if (!publish_node(walk, root, slot, &hand, false, &placed)) {
        return false;
    }
    root->id = previous->id;
    /* Whether the node about to be entered is same so far. */
    bool verdict = publish_same(root, previous);
    if (!publish_first(root, shape, &cursor, &first, &holder, &more)) {
        *same = verdict && !publish_first(previous, shape, &old_cursor, &old_first, &old_holder, &old_more);
        return true;
    }
    markdown_core_node *node = root, *old = previous;
    markdown_core_node **node_slot = NULL, **old_slot = NULL;
    /* Where the old root's source starts, measured as its relation gave it. */
    uint32_t old_start = runs_window(previous, start->source).start;
    size_t entry = 0;
    bool content = false;
    /* The spine of a streaming publish starts at the roots. */
    int spine = walk->level_count && walk->levels[0].object == root && walk->levels[0].published == previous ? 0 : -1;
    /* Enters a matched node that has relations: its frame waits for its
     * verdict while they are published. */
    for (;;) {
        publish_match_frame *frames = markdown_core_parser_walk_stack(parser, count + 1, sizeof(*frames));
        if (!frames) {
            ok = false;
            break;
        }
        /* Every field is set before the walk reads it, as publish_fresh's
         * frames are: these here, the node's place by publish_enter, the old
         * relation by publish_pair, and the relation in hand when the walk
         * enters a node below. */
        frame = &frames[count++];
        frame->walk.cursor = cursor;
        frame->walk.more = more;
        frame->slot = node_slot;
        frame->old_slot = old_slot;
        frame->same = verdict && (spine < 0 || (size_t)spine >= walk->touched_levels);
        frame->old_start = old_start;
        frame->entry = entry;
        frame->spine = spine;
        hand = first;
        publish_enter(&frame->walk, &placed, &hand, content);
        publish_skip_bodies(walk, spine, shape, &frame->walk.cursor);
        frame->level = publish_level_of(walk, spine, holder);
        publish_pair_first(walk, frame, old, shape, relation_field(&cursor, shape));
        publish_skip(walk, frame->level, &hand);
        /* The next matched node with relations, or the end of the walk. */
        for (node = NULL; ok && count && !node;) {
            if (hand.item == hand.end) {
                /* Old nodes left over when a relation ends, with those that
                 * go on from it, make the node differ, as do old relations
                 * left over when the node's relations end. */
                markdown_core_relation relation;
                if (frame->walk.more && relations_next(&frame->walk.cursor, &relation, &frame->walk.more)) {
                    if (!relation.continues && frame->old_item != frame->old_end) {
                        frame->same = false;
                    }
                    hand = publish_next(&frame->walk, &hand, &relation);
                    /* A root's later relations are not its content. */
                    if (frame->walk.root) {
                        frame->walk.root = frame->walk.content = false;
                    }
                    frame->level = publish_level_of(walk, frame->spine, relation.holder);
                    publish_pair(walk, frame, frame->walk.cursor.step - 1, relation.continues);
                    publish_skip(walk, frame->level, &hand);
                    continue;
                }
                if (frame->old_item != frame->old_end || frame->old_field != INT_MAX) {
                    frame->same = false;
                }
                if (!--count) {
                    *same = frame->same;
                    break;
                }
                if (!(ok = publish_verdict(walk, frame - 1, frame->slot, frame->old_slot, frame->same, frame->entry))) {
                    break;
                }
                frame--;
                hand = (publish_relation){frame->walk.item, frame->walk.end, frame->walk.anchor, frame->walk.source};
                continue;
            }
            markdown_core_node **child_slot = hand.item++;
            markdown_core_node *child = *child_slot;
            if (child->id) {
                publish_shared(frame, child, &hand);
                continue;
            }
            slot = slot_of(child);
            shape = (relation_shape)(slot & SLOT_SHAPE);
            content = frame->walk.content;
            if (!(ok = publish_node(walk, child, slot, &hand, content, &placed))) {
                break;
            }
            entry = SLOT_TABLE(slot) ? walk->tables[SLOT_TABLE(slot) - 1].count : 0;
            hand.anchor = placed.place.end;
            hand.source = placed.source.end;
            /* The spine's object at the next depth is the first node past
             * those the level holding the relation published as they are. */
            spine = frame->level >= 0 && (size_t)frame->level + 1 < walk->level_count &&
                            walk->levels[frame->level + 1].object == child
                        ? frame->level + 1
                        : -1;
            markdown_core_node **match_slot = spine >= 0 ? publish_spine_match(walk, frame, spine, &old_start) : NULL;
            if (!match_slot) {
                match_slot = publish_match(frame, child, placed.source, &old_start);
                if (spine >= 0 && (!match_slot || *match_slot != walk->levels[spine].published)) {
                    spine = -1;
                }
            }
            if (!match_slot) {
                /* What continues nothing is published above the frames,
                 * which find the stack where its growth left it. */
                child->id = ++walk->next_id;
                publish_above(&walk->stack, count * sizeof(*frame));
                ok = publish_fresh(walk, child, shape, &placed, content);
                frame = (publish_match_frame *)parser->walk_stack + count - 1;
                continue;
            }
            markdown_core_node *match = *match_slot;
            child->id = match->id;
            verdict = publish_same(child, match);
            if (!publish_first(child, shape, &cursor, &first, &holder, &more)) {
                verdict = verdict && !publish_first(match, shape, &old_cursor, &old_first, &old_holder, &old_more);
                ok = publish_verdict(walk, frame, child_slot, match_slot, verdict, entry);
                continue;
            }
            frame->walk.item = hand.item;
            frame->walk.end = hand.end;
            frame->walk.anchor = hand.anchor;
            frame->walk.source = hand.source;
            node = child;
            old = match;
            node_slot = child_slot;
            old_slot = match_slot;
        }
        if (!ok || !node) {
            break;
        }
    }
    return ok;
}

/* Commits a published tree that continues the previous one: each same pair,
 * children before the node that holds them, exchanges its two nodes whole
 * (markdown_core_node_swap), and each tree's slot for it takes the
 * other's node, so the new tree holds the old object with the new state and
 * the old tree the new object with the old state, each object's state with
 * the storage it owns and borrows. A slot of a node's record moves with the
 * node, which is why a node's pairs come first. The root is then the
 * previous root when it is same, and whatever tree is left over is released
 * into the pool (publish_continue). */
static void publish_commit(const markdown_core_stream *stream, size_t swap_count) {
    for (size_t i = 0; i < swap_count; i++) {
        const publish_swap *swap = &stream->same_pairs[i];
        markdown_core_node_swap(swap->node, swap->old);
        *swap->slot = swap->old;
        *swap->old_slot = swap->node;
    }
}

static void publish_continue(markdown_core_parser *parser, size_t swap_count, bool same) {
    markdown_core_node *root = parser->root, *previous = parser->revision->previous;
    publish_commit(parser->stream, swap_count);
    if (same) {
        markdown_core_node_swap(root, previous);
        parser->root = previous;
        markdown_core_node_pool_release(parser->pool, root);
        return;
    }
    markdown_core_node_pool_release(parser->pool, previous);
}

/* Takes `node`, which starts at `start`, into the `*taken` entries when its
 * label is not empty. */
static inline void label_take(label_entry *entries, size_t *taken, markdown_core_chunk label,
                              const markdown_core_node *node, uint32_t start) {
    if (label.len > 0) {
        entries[(*taken)++] = label_entry_of(label.data, (size_t)label.len, node, start);
    }
}

/* The node each label a reference occurrence can name resolves to, in label
 * order: the first Reference declaring it, or, when none does, the first
 * Heading whose text declares it. The References in source order, then the
 * Headings in source order, sorted by label keeping that order among equal
 * labels, put each label's target first among its own. */
static bool reference_targets(markdown_core_parser *parser, markdown_core_document_value *value,
                              definition_table *headings) {
    value->reference_targets = (markdown_core_node_table){0};
    if (headings->count && !markdown_core_order_source_entries(&parser->source_order, headings->values, headings->count,
                                                               sizeof(*headings->values), definition_key)) {
        return false;
    }
    const size_t total = value->references.all.count + headings->count;
    if (!total) {
        return true;
    }
    const markdown_core_node **declared = markdown_core_alloc(total, sizeof(*declared));
    label_entry *entries = label_room(total);
    if (!declared || !entries) {
        markdown_core_free((void *)declared);
        markdown_core_free(entries);
        return false;
    }
    size_t labels = 0;
    for (size_t i = 0; i < value->references.all.count; i++) {
        const markdown_core_node *node = value->references.all.nodes[i];
        label_take(entries, &labels, node->as.reference->label, node, 0);
    }
    for (size_t i = 0; i < headings->count; i++) {
        const markdown_core_node *node = headings->values[i].node;
        label_take(entries, &labels, node->as.heading->label, node, 0);
    }
    const label_entry *sorted = label_order(entries, labels, total);
    size_t count = 0;
    for (size_t i = 0; i < labels; i++) {
        if (!i || !label_same(&sorted[i], &sorted[i - 1])) {
            declared[count++] = sorted[i].value;
        }
    }
    markdown_core_free(entries);
    if (!count) {
        markdown_core_free((void *)declared);
        return true;
    }
    value->reference_targets = (markdown_core_node_table){declared, count, count, NULL};
    return true;
}

/* THE SPINE of a streaming publish (A STREAMING PUBLISH): the levels the
 * publish before left, as deep as the previous tree holds an object for one
 * and the new tree has its object, which each of them names while the
 * publish is made. */
static void publish_spine(publish_walk *walk, markdown_core_parser *parser) {
    if (!parser->revision->previous) {
        return;
    }
    size_t depth = 0;
    for (const markdown_core_node *object; (object = markdown_core_parser_level_object(parser, depth)); depth++) {
        parser->stream->levels[depth].object = object;
    }
    walk->levels = parser->stream->levels;
    walk->level_count = depth;
    walk->touched_levels = parser->stream->touched_levels;
}

/* THE DEFINITION TABLES A STREAMING PARSE KEEPS (parser.h, `committed`):
 * the definitions the published document holds as COMMITTED, each the
 * object the publish placed, which no later publish walks, or the object it
 * moved to since (blocks.c, A BLOCK SUPERSEDED), which the next publish
 * names first, found at where its block starts, which the move records. A
 * snapshot's publish keeps the COMMITTED definitions its walk lists, and its
 * document borrows the tables (A SNAPSHOT'S TABLES). The finalize lists them
 * first, and its document, as the document of a parse that takes no
 * snapshot, holds every table whole: its candidates withdraw definitions
 * anywhere (blocks.c), so it reads no label table. */

/* The label table of each definition table: the labeled Footnotes, the
 * labeled Specimens, and the reference targets, which References and
 * Headings declare. */
enum { LABELS_FOOTNOTES, LABELS_SPECIMENS, LABELS_TARGETS };

static const uint32_t table_labels[TABLE_COUNT] = {LABELS_FOOTNOTES, LABELS_SPECIMENS, LABELS_TARGETS, LABELS_TARGETS};

typedef char committed_tables_fit[(int)TABLE_COUNT == (int)MARKDOWN_CORE_DEFINITION_TABLES &&
                                          (int)LABELS_TARGETS + 1 == (int)MARKDOWN_CORE_LABEL_TABLES
                                      ? 1
                                      : -1];

/* Whether `node`, a definition of label table `labels` that starts at
 * `start`, is its label's definition before `entry`'s: a Reference before a
 * Heading, and otherwise the one that starts first. */
static inline bool label_wins(uint32_t labels, const markdown_core_node *node, uint32_t start,
                              const markdown_core_key_entry *entry) {
    if (labels == LABELS_TARGETS) {
        const bool reference = node->kind == MARKDOWN_CORE_NODE_REFERENCE;
        if (reference != (((const markdown_core_node *)entry->value)->kind == MARKDOWN_CORE_NODE_REFERENCE)) {
            return reference;
        }
    }
    return start < entry->extra;
}

/* `node`, a definition of table `table` that starts at `start`, in the
 * committed tables: at its start, and at its label when it has one and is
 * the label's definition. False when storage could not be had. */
static bool committed_keep(markdown_core_committed_tables *committed, uint32_t table, const markdown_core_node *node,
                           uint32_t start) {
    const markdown_core_key_entry at = markdown_core_key_number(start);
    bool made;
    markdown_core_key_entry *entry = markdown_core_key_tree_entry(&committed->starts[table], &at, &made);
    if (!entry) {
        return false;
    }
    /* No two definitions of one kind start at one place. */
    assert(made);
    entry->extra = start;
    entry->value = node;
    markdown_core_chunk label;
    if (!definition_label(node, &label)) {
        return true;
    }
    const uint32_t labels = table_labels[table];
    const label_entry named = label_entry_of(label.data, (size_t)label.len, node, start);
    if (!(entry = markdown_core_key_tree_entry(&committed->labels[labels], &named, &made))) {
        return false;
    }
    if (made || label_wins(labels, node, start, entry)) {
        *entry = named;
    }
    return true;
}

/* The committed tables name the objects that moved since the last publish:
 * each retired slot of a definition they list where its block starts, and
 * at its label too when `labels` says the label tables are read. */
static void committed_moves(markdown_core_parser *parser, bool labels) {
    markdown_core_committed_tables *committed = &parser->stream->committed;
    for (size_t i = 0; i < parser->stream->retired_count; i++) {
        const markdown_core_retired *retired = &parser->stream->retired[i];
        const markdown_core_node *old = retired->storage;
        const unsigned table = retired->kind == MARKDOWN_CORE_RETIRED_SLOT ? SLOT_TABLE(slot_of(old)) : 0;
        if (!table) {
            continue;
        }
        const markdown_core_key_entry at = markdown_core_key_number(retired->start);
        markdown_core_key_entry *entry = markdown_core_key_tree_find(&committed->starts[table - 1], &at, NULL);
        if (!entry || entry->value != old) {
            continue;
        }
        const markdown_core_node *moved = markdown_core_node_moved(old);
        entry->value = moved;
        markdown_core_chunk label;
        if (labels && definition_label(moved, &label)) {
            const label_entry named = label_entry_of(label.data, (size_t)label.len, moved, 0);
            entry = markdown_core_key_tree_find(&committed->labels[table_labels[table - 1]], &named, NULL);
            if (entry && entry->value == old) {
                entry->value = moved;
            }
        }
    }
}

/* The COMMITTED definitions the walk listed, kept in the committed tables
 * and taken out of the walk's, which then list the snapshot's own. False
 * when storage could not be had. */
static bool keep_committed(markdown_core_parser *parser, const publish_walk *walk) {
    for (size_t i = 0; i < walk->kept_count; i++) {
        const markdown_core_kept kept = parser->stream->kept[i];
        definition_entry *entry = &walk->tables[kept.table].values[kept.entry];
        if (!committed_keep(&parser->stream->committed, kept.table, entry->node, (uint32_t)entry->start)) {
            return false;
        }
        entry->node = NULL;
    }
    return true;
}

/* The finalize lists the committed definitions first, in source order, all
 * but those its candidates withdrew. */
static bool publish_prefill(publish_walk *walk, markdown_core_parser *parser) {
    const markdown_core_key_entry first = markdown_core_key_number(0);
    for (size_t table = 0; table < TABLE_COUNT; table++) {
        markdown_core_key_tree_cursor cursor;
        markdown_core_key_tree_seek(&parser->stream->committed.starts[table], &first, &cursor);
        for (const markdown_core_key_entry *entry; (entry = markdown_core_key_tree_next(&cursor));) {
            if (entry->value && !table_add(&walk->tables[table], entry->value, entry->extra)) {
                return false;
            }
        }
    }
    return true;
}

/* A SNAPSHOT'S TABLES. A snapshot's document borrows the committed tables
 * and lists its own entries: the definitions its tail adds, which its walk
 * listed and no commit keeps. Each own entry holds where it falls among the
 * document's entries, by where it starts or by its label, and the index of
 * the committed entry after it, so the entry at an index is an own one or
 * the committed one past the last own entry before it. A label's own entry
 * is listed only when it is the label's definition, before the label's
 * committed one, in whose place it then stands. The own entries are in the
 * room the streaming parse keeps for them (parser.h, `own_tables`), which
 * every snapshot reuses, so a snapshot's document borrows all of its tables:
 * it reads them until the next append changes them, as long as the session
 * lends it. */

/* THE ROOM'S BLOCKS each start where any object may: a block takes a whole
 * number of units. */
typedef union {
    long double real;
    uint64_t integer;
    void *pointer;
} room_unit;

static inline size_t room_bytes(size_t bytes) {
    return (bytes + sizeof(room_unit) - 1) / sizeof(room_unit) * sizeof(room_unit);
}

static inline void *room_take(unsigned char **at, size_t bytes) {
    void *taken = *at;
    *at += room_bytes(bytes);
    return taken;
}

/* The table's own entries, the ones the walk listed and no commit keeps, in
 * source order. */
static bool snapshot_own(markdown_core_parser *parser, definition_table *table) {
    size_t own = 0;
    for (size_t i = 0; i < table->count; i++) {
        if (table->values[i].node) {
            table->values[own++] = table->values[i];
        }
    }
    table->count = own;
    return !own || table_order(parser, table);
}

/* The own entries of `table`, which `tree` keeps committed, into `nodes`, and
 * where each falls into `places`. */
static void snapshot_starts(const definition_table *table, const markdown_core_key_tree *tree,
                            const markdown_core_node **nodes, markdown_core_table_place *places) {
    for (size_t j = 0; j < table->count; j++) {
        const markdown_core_key_entry at = markdown_core_key_number((uint32_t)table->values[j].start);
        size_t rank;
        markdown_core_key_tree_find(tree, &at, &rank);
        nodes[j] = table->values[j].node;
        places[j] = (markdown_core_table_place){rank + j, rank};
    }
}

/* The labeled own entries of `table`, in source order, that are their
 * labels' definitions before those `tree` keeps committed for label table
 * `labels`, by label, into `labeled`, sorted in `entries`, room for as many
 * labels as the table holds, into `out`. */
static void snapshot_labeled(const definition_table *table, uint32_t labels, const markdown_core_key_tree *tree,
                             label_entry *entries, const markdown_core_node **labeled, markdown_core_definitions *out) {
    size_t taken = 0;
    for (size_t i = 0; i < table->count; i++) {
        const definition_entry *own = &table->values[i];
        markdown_core_chunk label;
        if (!definition_label(own->node, &label)) {
            continue;
        }
        const label_entry named = label_entry_of(label.data, (size_t)label.len, own->node, (uint32_t)own->start);
        const markdown_core_key_entry *held = markdown_core_key_tree_find(tree, &named, NULL);
        if (!held || label_wins(labels, own->node, named.extra, held)) {
            entries[taken++] = named;
        }
    }
    const label_entry *sorted = label_order(entries, taken, table->count);
    for (size_t i = 0; i < taken; i++) {
        labeled[i] = sorted[i].value;
    }
    out->labeled = taken ? labeled : NULL;
    out->labeled_count = taken;
}

/* The own reference targets: the own References and Headings, in source
 * order in their tables, that are their labels' definitions before those
 * `tree` keeps committed, by label, into `nodes` and `places`, sorted in
 * `entries`, room for as many labels as both tables hold, into `out`. */
static void snapshot_targets(const definition_table *references, const definition_table *headings,
                             const markdown_core_key_tree *tree, label_entry *entries, const markdown_core_node **nodes,
                             markdown_core_table_place *places, markdown_core_node_table *out) {
    size_t labels = 0;
    const definition_table *const tables[] = {references, headings};
    for (size_t t = 0; t < 2; t++) {
        for (size_t i = 0; i < tables[t]->count; i++) {
            const definition_entry *own = &tables[t]->values[i];
            markdown_core_chunk label;
            if (definition_label(own->node, &label)) {
                label_take(entries, &labels, label, own->node, (uint32_t)own->start);
            }
        }
    }
    const label_entry *sorted = label_order(entries, labels, references->count + headings->count);
    size_t own = 0, added = 0;
    for (size_t i = 0; i < labels; i++) {
        if (i && label_same(&sorted[i], &sorted[i - 1])) {
            continue;
        }
        size_t rank;
        const markdown_core_key_entry *held = markdown_core_key_tree_find(tree, &sorted[i], &rank);
        if (held && !label_wins(LABELS_TARGETS, sorted[i].value, sorted[i].extra, held)) {
            continue;
        }
        nodes[own] = sorted[i].value;
        places[own++] = (markdown_core_table_place){rank + added, held ? rank + 1 : rank};
        added += !held;
    }
    *out = (markdown_core_node_table){own ? nodes : NULL, tree->count + added, own, own ? places : NULL};
}

/* A snapshot's document's tables (A SNAPSHOT'S TABLES), from the walk's,
 * which list its own entries. False when storage could not be had, before
 * the document lists anything. */
static bool snapshot_tables(markdown_core_parser *parser, publish_walk *walk, markdown_core_document_value *value) {
    markdown_core_stream *stream = parser->stream;
    const markdown_core_committed_tables *committed = &stream->committed;
    definition_table *const tables = walk->tables;
    for (size_t table = 0; table < TABLE_COUNT; table++) {
        if (!snapshot_own(parser, &tables[table])) {
            return false;
        }
    }
    const size_t footnotes = tables[TABLE_FOOTNOTES].count, specimens = tables[TABLE_SPECIMENS].count;
    const size_t listed = footnotes + specimens + tables[TABLE_REFERENCES].count;
    const size_t targets = tables[TABLE_REFERENCES].count + tables[TABLE_HEADINGS].count;
    size_t labels = targets > footnotes ? targets : footnotes;
    labels = labels > specimens ? labels : specimens;
    const size_t entry_bytes = labels * (2 * sizeof(label_entry) + sizeof(size_t));
    const size_t node_bytes = (listed + footnotes + specimens + targets) * sizeof(const markdown_core_node *);
    const size_t place_bytes = (listed + targets) * sizeof(markdown_core_table_place);
    const size_t bytes = room_bytes(entry_bytes) + room_bytes(node_bytes) + room_bytes(place_bytes);
    /* A snapshot that lists nothing of its own takes its empty blocks from
     * no room. */
    static room_unit no_room;
    unsigned char *at = (unsigned char *)&no_room;
    if (bytes) {
        if (!(at = markdown_core_reserve(stream->own_tables, &stream->own_table_capacity, bytes, 1))) {
            return false;
        }
        stream->own_tables = at;
    }
    label_entry *entries = room_take(&at, entry_bytes);
    const markdown_core_node **nodes = room_take(&at, node_bytes);
    markdown_core_table_place *places = room_take(&at, place_bytes);
    markdown_core_definitions *const all[] = {&value->footnotes, &value->specimens, &value->references};
    for (size_t table = 0; table < TABLE_HEADINGS; table++) {
        const size_t count = tables[table].count;
        snapshot_starts(&tables[table], &committed->starts[table], nodes, places);
        all[table]->all = (markdown_core_node_table){count ? nodes : NULL, committed->starts[table].count + count,
                                                     count, count ? places : NULL};
        nodes += count;
        places += count;
    }
    snapshot_labeled(&tables[TABLE_FOOTNOTES], LABELS_FOOTNOTES, &committed->labels[LABELS_FOOTNOTES], entries, nodes,
                     &value->footnotes);
    nodes += footnotes;
    snapshot_labeled(&tables[TABLE_SPECIMENS], LABELS_SPECIMENS, &committed->labels[LABELS_SPECIMENS], entries, nodes,
                     &value->specimens);
    nodes += specimens;
    snapshot_targets(&tables[TABLE_REFERENCES], &tables[TABLE_HEADINGS], &committed->labels[LABELS_TARGETS], entries,
                     nodes, places, &value->reference_targets);
    value->committed = committed;
    return true;
}

/* A publish's walk starts empty, on the walk stack and the storage every
 * publish of the parse reuses (parser.h, `definition_values`), and gives the
 * storage back when it ends: a table whose entries became a document's
 * nodes (table_handles) gives none. */
static void publish_start(publish_walk *walk, markdown_core_parser *parser) {
    walk->stack.parser = parser;
    publish_above(&walk->stack, 0);
    walk->swap_count = 0;
    for (size_t table = 0; table < TABLE_COUNT; table++) {
        walk->tables[table] =
            (definition_table){parser->definition_values[table], 0, parser->definition_capacity[table]};
    }
    walk->next_id = parser->revision->last_id;
    walk->levels = NULL;
    walk->level_count = walk->touched_levels = 0;
    walk->kept_count = 0;
    walk->places = (content_places){parser->content_places, 0, parser->content_place_capacity};
    walk->pool = parser->pool;
}

static void publish_end(const publish_walk *walk, markdown_core_parser *parser) {
    for (size_t table = 0; table < TABLE_COUNT; table++) {
        parser->definition_values[table] = walk->tables[table].values;
        parser->definition_capacity[table] = walk->tables[table].capacity;
    }
    parser->content_places = walk->places.items;
    parser->content_place_capacity = walk->places.capacity;
}

bool markdown_core_publish_tree(markdown_core_parser *parser) {
    markdown_core_revision *revision = parser->revision;
    markdown_core_node *root = parser->root, *previous = revision->previous;
    markdown_core_document_value *value = root->as.document;
    publish_walk walk;
    publish_start(&walk, parser);
    publish_placed placed;
    /* A streaming publish starts at the levels, and a snapshot's keeps the
     * COMMITTED definitions; the finalize lists them first. */
    const bool snapshot = parser->stream && parser->snapshot;
    if (parser->stream) {
        publish_spine(&walk, parser);
        committed_moves(parser, snapshot);
    }
    value->footnotes = value->specimens = value->references = (markdown_core_definitions){0};
    value->reference_targets = (markdown_core_node_table){0};
    value->committed = NULL;
    bool same = false, ok = snapshot || !parser->stream || publish_prefill(&walk, parser);
    if (ok && previous) {
        const publish_relation start = {0};
        ok = publish_matched(&walk, root, previous, &start, &same);
    } else if (ok) {
        unsigned slot = slot_of(root);
        const publish_relation hand = {0};
        ok = publish_node(&walk, root, slot, &hand, false, &placed);
        root->id = ++walk.next_id;
        ok = ok && publish_fresh(&walk, root, (relation_shape)(slot & SLOT_SHAPE), &placed, false);
    }
    if (ok && snapshot) {
        ok = keep_committed(parser, &walk) && snapshot_tables(parser, &walk, value);
    } else if (ok) {
        definition_table *references = &walk.tables[TABLE_REFERENCES];
        ok = table_seal(parser, &walk.tables[TABLE_FOOTNOTES], &value->footnotes) &&
             table_seal(parser, &walk.tables[TABLE_SPECIMENS], &value->specimens) &&
             table_nodes(parser, references, &value->references.all.nodes);
        value->references.all.count = value->references.all.own = ok ? references->count : 0;
        ok = ok && reference_targets(parser, value, &walk.tables[TABLE_HEADINGS]);
    }
    /* Nothing below can fail: the result is committed. */
    if (ok && previous) {
        publish_continue(parser, walk.swap_count, same);
        revision->previous = NULL;
    }
    if (ok) {
        revision->last_id = walk.next_id;
    }
    if (ok && parser->stream) {
        parser->stream->touched_levels = 0;
    }
    publish_end(&walk, parser);
    return ok;
}

/* PUBLISHING A CANDIDATE (blocks.c, A LOOKUP THAT FOUND NOTHING): a block
 * completed again continues the block it is a candidate for, as a publish
 * continues a tree, measured from the offsets the relation holding the
 * block gave it: where its extent starts, its start less the block's lead,
 * and where its first run starts, its start less that run's lead. The
 * candidate's own lookup tables go with the walk, and the definitions it
 * lists below it join those the parser's candidates list, which join the
 * committed tables once its completion ends (blocks.c); the block's own
 * entry names the object that holds its state (THE DEFINITION TABLES A
 * STREAMING PARSE KEEPS). A candidate that differs commits its same pairs. */
bool markdown_core_publish_candidate(markdown_core_parser *parser, markdown_core_node *candidate,
                                     markdown_core_node *block, bool *same) {
    publish_walk walk;
    publish_start(&walk, parser);
    const uint32_t start = candidate->where.place.start;
    const publish_relation hand = {NULL, NULL, (uint32_t)((int64_t)start - block->where.extent.lead),
                                   (uint32_t)((int64_t)start - block->source.runs->items[0].lead)};
    bool ok = publish_matched(&walk, candidate, block, &hand, same);
    if (ok && walk.kept_count) {
        markdown_core_defined *defined =
            markdown_core_reserve(parser->stream->defined, &parser->stream->defined_capacity,
                                  parser->stream->defined_count + walk.kept_count, sizeof(*defined));
        ok = defined != NULL;
        parser->stream->defined = ok ? defined : parser->stream->defined;
    }
    if (ok) {
        if (!*same) {
            publish_commit(parser->stream, walk.swap_count);
        }
        for (size_t i = 0; i < walk.kept_count; i++) {
            const markdown_core_kept kept = parser->stream->kept[i];
            const definition_entry *entry = &walk.tables[kept.table].values[kept.entry];
            if (entry->node != candidate) {
                parser->stream->defined[parser->stream->defined_count++] =
                    (markdown_core_defined){(uint32_t)entry->start, kept.table, entry->node};
            }
        }
        parser->revision->last_id = walk.next_id;
    }
    publish_end(&walk, parser);
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
    [MARKDOWN_CORE_NODE_REFERENCE & MARKDOWN_CORE_NODE_VALUE_MASK] = MARKDOWN_CORE_KIND_REFERENCE,
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
    "Reference",
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
typedef char kinds_fit_a_set[MARKDOWN_CORE_KIND_REFERENCE < 64 ? 1 : -1];

static inline bool node_is(const markdown_core_node *node, uint64_t kinds) {
    return (kinds >> public_kind(node) & 1) != 0;
}

uint64_t markdown_core_node_id(const markdown_core_node *node) { return node->id; }

markdown_core_extent markdown_core_node_extent(const markdown_core_node *node) { return node->where.extent; }

const markdown_core_run *markdown_core_node_runs(const markdown_core_node *node, size_t *count) {
    *count = node->source.runs->count;
    return node->source.runs->items;
}

void markdown_core_walk_begin(markdown_core_walk *walk, const markdown_core_node *root) {
    *walk = (markdown_core_walk){.root = root};
}

void markdown_core_walk_end(markdown_core_walk *walk) {
    markdown_core_free(walk->frames);
    markdown_core_free(walk->ranges);
    *walk = (markdown_core_walk){.root = walk->root, .failed = walk->failed};
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
    frame->owner_start = frame->anchor = start;
    markdown_core_relations_begin(&frame->cursor, node);
    return true;
}

/* Whether the owner of `frame` has another line at its own level after the
 * current relation: a later group, or a later relation with nodes. */
static bool walk_more_after(const markdown_core_walk_frame *frame) {
    markdown_core_relation_cursor cursor = frame->cursor;
    markdown_core_relation relation;
    while (markdown_core_relations_next(&cursor, &relation)) {
        if (relation.group || relation.count) {
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
        markdown_core_place place = runs_window(walk->root, 0);
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
            /* A relation that goes on from the one before measures its first
             * node from where that one ended. */
            if (!frame->relation.continues) {
                frame->anchor = frame->owner_start;
            }
            frame->active = true;
            frame->group_pending = frame->relation.group != NULL;
            frame->next = 0;
        }
        if (frame->group_pending) {
            frame->group_pending = false;
            *item =
                (markdown_core_walk_item){NULL, {0, 0}, frame->relation.group, frame->relation.count, frame->level + 1};
            walk->at_group = true;
            walk->owner = walk->count;
            return true;
        }
        if (frame->next < frame->relation.count) {
            const markdown_core_node *node = frame->relation.items[frame->next++];
            markdown_core_place place = runs_window(node, frame->anchor);
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
    return owner->next < owner->relation.count || (!owner->relation.group && walk_more_after(owner));
}

bool markdown_core_walk_ranges(markdown_core_walk *walk, const markdown_core_walk_item *item,
                               const markdown_core_place **ranges, size_t *count) {
    const markdown_core_runs *runs = item->node->source.runs;
    markdown_core_place *grown =
        markdown_core_reserve(walk->ranges, &walk->range_capacity, runs->count, sizeof(*walk->ranges));
    if (!grown) {
        walk->failed = true;
        return false;
    }
    walk->ranges = grown;
    /* The window starts where the first run does. */
    int64_t at = (int64_t)item->place.start - runs->items[0].lead;
    for (uint32_t i = 0; i < runs->count; i++) {
        const markdown_core_run run = runs->items[i];
        at += run.lead;
        walk->ranges[i] = (markdown_core_place){(uint32_t)at, (uint32_t)(at + run.span)};
        at += run.span;
    }
    *count = runs->count;
    *ranges = walk->ranges;
    return true;
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

/* The source ranges of `target`, a node of the tree `root`, found by one
 * canonical walk, in a new array; false when it could not be allocated. */
static bool tree_ranges(const markdown_core_node *root, const markdown_core_node *target, markdown_core_place **ranges,
                        size_t *count) {
    markdown_core_walk walk;
    markdown_core_walk_item item;
    const markdown_core_place *found = NULL;
    *ranges = NULL;
    *count = 0;
    markdown_core_walk_begin(&walk, root);
    while (markdown_core_walk_next(&walk, &item)) {
        if (item.node == target) {
            if (markdown_core_walk_ranges(&walk, &item, &found, count)) {
                *ranges = markdown_core_alloc(*count, sizeof(**ranges));
                if (*ranges) {
                    memcpy(*ranges, found, *count * sizeof(**ranges));
                }
            }
            break;
        }
    }
    bool ran = !walk.failed && *ranges;
    markdown_core_walk_end(&walk);
    return ran;
}

/* The scopes of `ranges`, within `length` bytes of `source`, in a new array;
 * NULL when it or the line table could not be allocated. */
static markdown_core_scope *ranges_scopes(const uint8_t *source, size_t length, const markdown_core_place *ranges,
                                          size_t count, markdown_core_text_unit unit) {
    source_lines lines;
    if (!source_lines_read(&lines, source, length)) {
        return NULL;
    }
    markdown_core_scope *scopes = markdown_core_alloc(count, sizeof(*scopes));
    for (size_t i = 0; scopes && i < count; i++) {
        scopes[i] = source_scope(&lines, source, ranges[i], unit);
    }
    markdown_core_free(lines.starts);
    return scopes;
}

bool markdown_core_tree_scope(const markdown_core_node *root, const markdown_core_node *node, const uint8_t *source,
                              size_t length, markdown_core_text_unit unit, markdown_core_scope **scopes,
                              size_t *count) {
    markdown_core_place *ranges;
    if (!tree_ranges(root, node, &ranges, count)) {
        return false;
    }
    *scopes = ranges_scopes(source, length, ranges, *count, unit);
    markdown_core_free(ranges);
    return *scopes != NULL;
}

/* The source must cover the node: counting its columns reads every byte of
 * its lines up to its end. */
markdown_core_status markdown_core_document_scope(const markdown_core_document *document,
                                                  const markdown_core_node *node, const uint8_t *source, size_t length,
                                                  markdown_core_scope **scopes, size_t *count) {
    markdown_core_place *ranges;
    size_t found;
    if (!tree_ranges(document->root, node, &ranges, &found)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_status status = MARKDOWN_CORE_OK;
    if (ranges[found - 1].end > length) {
        status = MARKDOWN_CORE_OUT_OF_BOUNDS;
    } else if (!(*scopes = ranges_scopes(source, length, ranges, found, document->unit))) {
        status = MARKDOWN_CORE_ALLOCATION_FAILED;
    } else {
        *count = found;
    }
    markdown_core_free(ranges);
    return status;
}

void markdown_core_scopes_free(markdown_core_scope *scopes) { markdown_core_free(scopes); }

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
            const markdown_core_place *ranges;
            size_t count;
            if (!item.node || !markdown_core_walk_ranges(&walk, &item, &ranges, &count)) {
                continue;
            }
            for (size_t i = 0; i < count; i++) {
                if (ranges[i].start <= offset && offset < ranges[i].end) {
                    found = item.node;
                    break;
                }
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

/* The children of `holder` as the public model reads them, or none when it
 * is NULL. */
static inline markdown_core_nodes nodes_of(const markdown_core_node *holder) {
    return holder ? (markdown_core_nodes){(const markdown_core_node *const *)holder->children, holder->child_count}
                  : (markdown_core_nodes){NULL, 0};
}

/* A cite's items and a definition's bodies are its children in storage and
 * its fields in the public model, which reads them through their accessors. */
markdown_core_nodes markdown_core_node_children(const markdown_core_node *node) {
    return node->kind != MARKDOWN_CORE_NODE_CITE && node->kind != MARKDOWN_CORE_NODE_DEFINITION
               ? nodes_of(node)
               : (markdown_core_nodes){NULL, 0};
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
    *literal = chunk_string(markdown_core_elements_formula_literal(node));
}

static const markdown_core_table *table_of(const markdown_core_node *node) { return node->opaque; }

static markdown_core_optional_string directive_name(const markdown_core_node *node) {
    const char *value = markdown_core_elements_get_directive_name(node);
    return value ? (markdown_core_optional_string){true, cstr_string(value)} : (markdown_core_optional_string){0};
}

static markdown_core_string attribute_class(const markdown_core_node *node, size_t index) {
    return chunk_string(node->attributes.classes[index]);
}
static void attribute_record(const markdown_core_node *node, size_t index, markdown_core_string *name,
                             markdown_core_string *value) {
    const markdown_core_record *record = &node->attributes.records[index];
    *name = chunk_string(record->name);
    *value = chunk_string(record->value);
}

/* The resource a direct `Link` or `Embedded`, or a `Reference`, states; NULL
 * for a reference occurrence, which names its definition by label. */
static const markdown_core_resource *resource_of(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_REFERENCE ? node->as.reference->resource : node->as.link->resource;
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
    if (is_link(node) || node->kind == MARKDOWN_CORE_NODE_REFERENCE) {
        const markdown_core_resource *resource = resource_of(node);
        if (resource) {
            destination.kind = MARKDOWN_CORE_DESTINATION_URL;
            destination.url = chunk_string(resource->url);
        } else {
            destination.kind = MARKDOWN_CORE_DESTINATION_REFERENCE;
            destination.label = chunk_string(node->as.link->label);
        }
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
    const markdown_core_resource *resource = resource_of(node);
    return resource ? optional_chunk_string(resource->title) : (markdown_core_optional_string){0};
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
    switch (x.kind) {
    case MARKDOWN_CORE_DESTINATION_URL:
        return string_equal(x.url, y.url);
    case MARKDOWN_CORE_DESTINATION_REFERENCE:
        return string_equal(x.label, y.label);
    default:
        return string_equal(x.path, y.path) && optional_string_equal(x.anchor, y.anchor);
    }
}

static bool dimensions_equal(const markdown_core_node *a, const markdown_core_node *b) {
    const markdown_core_dimensions *x = dimensions_of(a), *y = dimensions_of(b);
    return x == y || (x && y && x->width == y->width && optional_int_equal(x->height, y->height));
}

/* Whether two `Link`, `Embedded` or `Reference` nodes state equal
 * destinations and titles. */
static bool links_equal(const markdown_core_node *a, const markdown_core_node *b) {
    return destination_equal(a, b) && optional_string_equal(link_title(a), link_title(b));
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
    /* Its rows are one relation, so where the head ends and the foot begins
     * is the table's own. */
    if (p->column_count != q->column_count || p->head_count != q->head_count || p->content_count != q->content_count ||
        p->foot_count != q->foot_count) {
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
        return links_equal(a, b) && (kind == MARKDOWN_CORE_KIND_LINK || dimensions_equal(a, b));
    case MARKDOWN_CORE_KIND_REFERENCE:
        return links_equal(a, b) && chunk_equal(a->as.reference->label, b->as.reference->label);
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
#define RESOURCE_KINDS (LINK_KINDS | KIND_BIT(MARKDOWN_CORE_KIND_REFERENCE))
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

markdown_core_status markdown_core_node_definition_term(const markdown_core_node *node, markdown_core_nodes *term) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DEFINITION));
    *term = nodes_of(node->as.definition->term);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_definition_body_count(const markdown_core_node *node, size_t *count) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DEFINITION));
    *count = node->child_count;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_definition_body_at(const markdown_core_node *node, size_t index,
                                                           markdown_core_nodes *content) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DEFINITION));
    if (index >= node->child_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *content = nodes_of(node->children[index]);
    return MARKDOWN_CORE_OK;
}

const markdown_core_attribute_value *markdown_core_node_attributes(const markdown_core_node *node) {
    return &node->attributes;
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
    return markdown_core_attribute_value_anchor(&node->attributes);
}

size_t markdown_core_node_attribute_class_count(const markdown_core_node *node) { return node->attributes.class_count; }

markdown_core_status markdown_core_node_attribute_class_at(const markdown_core_node *node, size_t index,
                                                           markdown_core_string *value) {
    if (index >= markdown_core_node_attribute_class_count(node)) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *value = attribute_class(node, index);
    return MARKDOWN_CORE_OK;
}

size_t markdown_core_node_attribute_record_count(const markdown_core_node *node) {
    return node->attributes.record_count;
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

markdown_core_status markdown_core_node_callout_title(const markdown_core_node *node, markdown_core_nodes *title) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_CALLOUT));
    *title = nodes_of(node->as.callout->title);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_destination(const markdown_core_node *node,
                                                    markdown_core_destination *destination) {
    REQUIRE_KIND(node, RESOURCE_KINDS | CROSS_KINDS);
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
    REQUIRE_KIND(node, RESOURCE_KINDS);
    *title = link_title(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_cite_citations(const markdown_core_node *node, markdown_core_nodes *citations) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_CITE));
    *citations = nodes_of(node);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_citation_referent(const markdown_core_node *citation,
                                                     markdown_core_referent *referent) {
    REQUIRE_KIND(citation, KIND_BIT(MARKDOWN_CORE_KIND_CITATION));
    *referent = citation_referent(citation);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_citation_prefix(const markdown_core_node *citation, markdown_core_nodes *prefix) {
    REQUIRE_KIND(citation, KIND_BIT(MARKDOWN_CORE_KIND_CITATION));
    *prefix = nodes_of(citation->as.citation->prefix);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_citation_suffix(const markdown_core_node *citation, markdown_core_nodes *suffix) {
    REQUIRE_KIND(citation, KIND_BIT(MARKDOWN_CORE_KIND_CITATION));
    *suffix = nodes_of(citation->as.citation->suffix);
    return MARKDOWN_CORE_OK;
}

/* The definition tables publishing recorded in the document's root. */
static const markdown_core_document_value *document_value(const markdown_core_document *document) {
    return document->root->as.document;
}

/* The committed tree a snapshot's document reads for definition table
 * `table`, by start, or for label table `labels`, by label; NULL for any
 * other document. */
static const markdown_core_key_tree *committed_starts(const markdown_core_document_value *value, size_t table) {
    return value->committed ? &value->committed->starts[table] : NULL;
}

static const markdown_core_key_tree *committed_labels(const markdown_core_document_value *value, size_t labels) {
    return value->committed ? &value->committed->labels[labels] : NULL;
}

/* The entry at `index`, below the count, of a table of a document: one the
 * table holds, or, in a snapshot's document, which reads the committed
 * entries in `tree`, an own one or the committed one past the last own entry
 * before it (A SNAPSHOT'S TABLES). */
static const markdown_core_node *table_entry(const markdown_core_node_table *table, const markdown_core_key_tree *tree,
                                             size_t index) {
    if (!tree) {
        return table->nodes[index];
    }
    size_t low = 0, high = table->own;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (table->places[middle].at < index) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < table->own && table->places[low].at == index) {
        return table->nodes[low];
    }
    const markdown_core_table_place *before = low ? &table->places[low - 1] : NULL;
    return markdown_core_key_tree_at(tree, before ? before->next + (index - before->at - 1) : index)->value;
}

static markdown_core_status definition_at(const markdown_core_document *document, size_t table, size_t index,
                                          const markdown_core_node **node) {
    const markdown_core_document_value *value = document_value(document);
    const markdown_core_definitions *const tables[] = {&value->footnotes, &value->specimens, &value->references};
    if (index >= tables[table]->all.count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *node = table_entry(&tables[table]->all, committed_starts(value, table), index);
    return MARKDOWN_CORE_OK;
}

size_t markdown_core_document_footnote_count(const markdown_core_document *document) {
    return document_value(document)->footnotes.all.count;
}

markdown_core_status markdown_core_document_footnote_at(const markdown_core_document *document, size_t index,
                                                        const markdown_core_node **footnote) {
    return definition_at(document, TABLE_FOOTNOTES, index, footnote);
}

size_t markdown_core_document_specimen_count(const markdown_core_document *document) {
    return document_value(document)->specimens.all.count;
}

markdown_core_status markdown_core_document_specimen_at(const markdown_core_document *document, size_t index,
                                                        const markdown_core_node **specimen) {
    return definition_at(document, TABLE_SPECIMENS, index, specimen);
}

/* The first definition in source order whose label is `label`, byte for
 * byte, among `labeled`, `count` of them by label: the lowest of that label
 * in the label order; and, in a snapshot's document when none is, the one
 * the committed label table `tree` keeps. */
static const markdown_core_node *definition_for(const markdown_core_node *const *labeled, size_t count,
                                                const markdown_core_key_tree *tree, markdown_core_string label) {
    size_t lo = 0, hi = count;
    markdown_core_chunk candidate;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        definition_label(labeled[mid], &candidate);
        if (label_compare(candidate.data, (size_t)candidate.len, label.data, label.length) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < count) {
        definition_label(labeled[lo], &candidate);
        if (label_compare(candidate.data, (size_t)candidate.len, label.data, label.length) == 0) {
            return labeled[lo];
        }
    }
    /* No label a document holds is as long as a bufsize_t cannot count. */
    if (!tree || label.length > INT32_MAX) {
        return NULL;
    }
    const markdown_core_key_entry key = {markdown_core_key_head(label.data, (bufsize_t)label.length), label.data,
                                         (bufsize_t)label.length, 0, NULL};
    const markdown_core_key_entry *entry = markdown_core_key_tree_find(tree, &key, NULL);
    return entry ? entry->value : NULL;
}

const markdown_core_node *markdown_core_document_footnote_for(const markdown_core_document *document,
                                                              markdown_core_string label) {
    const markdown_core_document_value *value = document_value(document);
    return definition_for(value->footnotes.labeled, value->footnotes.labeled_count,
                          committed_labels(value, LABELS_FOOTNOTES), label);
}

const markdown_core_node *markdown_core_document_specimen_for(const markdown_core_document *document,
                                                              markdown_core_string label) {
    const markdown_core_document_value *value = document_value(document);
    return definition_for(value->specimens.labeled, value->specimens.labeled_count,
                          committed_labels(value, LABELS_SPECIMENS), label);
}

markdown_core_status markdown_core_footnote_label(const markdown_core_node *footnote,
                                                  markdown_core_optional_string *label) {
    REQUIRE_KIND(footnote, KIND_BIT(MARKDOWN_CORE_KIND_FOOTNOTE));
    *label = optional_chunk_string(footnote->as.footnote->label);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_footnote_content(const markdown_core_node *footnote, markdown_core_nodes *content) {
    REQUIRE_KIND(footnote, KIND_BIT(MARKDOWN_CORE_KIND_FOOTNOTE));
    *content = nodes_of(footnote);
    return MARKDOWN_CORE_OK;
}

size_t markdown_core_document_reference_count(const markdown_core_document *document) {
    return document_value(document)->references.all.count;
}

markdown_core_status markdown_core_document_reference_at(const markdown_core_document *document, size_t index,
                                                         const markdown_core_node **reference) {
    return definition_at(document, TABLE_REFERENCES, index, reference);
}

const markdown_core_node *markdown_core_document_reference_for(const markdown_core_document *document,
                                                               markdown_core_string label) {
    const markdown_core_document_value *value = document_value(document);
    return definition_for(value->reference_targets.nodes, value->reference_targets.own,
                          committed_labels(value, LABELS_TARGETS), label);
}

size_t markdown_core_document_reference_label_count(const markdown_core_document *document) {
    return document_value(document)->reference_targets.count;
}

markdown_core_status markdown_core_document_reference_label_at(const markdown_core_document *document, size_t index,
                                                               markdown_core_string *label,
                                                               const markdown_core_node **target) {
    const markdown_core_document_value *value = document_value(document);
    if (index >= value->reference_targets.count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    const markdown_core_node *node =
        table_entry(&value->reference_targets, committed_labels(value, LABELS_TARGETS), index);
    markdown_core_chunk chunk;
    definition_label(node, &chunk);
    *label = chunk_string(chunk);
    *target = node;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_reference_label(const markdown_core_node *reference, markdown_core_string *label) {
    REQUIRE_KIND(reference, KIND_BIT(MARKDOWN_CORE_KIND_REFERENCE));
    *label = chunk_string(reference->as.reference->label);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_specimen_properties(const markdown_core_node *specimen,
                                                       markdown_core_optional_string *label,
                                                       markdown_core_optional_i64 *start) {
    REQUIRE_KIND(specimen, KIND_BIT(MARKDOWN_CORE_KIND_SPECIMEN));
    specimen_properties(specimen, label, start);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_specimen_content(const markdown_core_node *specimen, markdown_core_nodes *content) {
    REQUIRE_KIND(specimen, KIND_BIT(MARKDOWN_CORE_KIND_SPECIMEN));
    *content = nodes_of(specimen);
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
    if (destination.kind == MARKDOWN_CORE_DESTINATION_REFERENCE) {
        buffer_cstr(buffer, "reference(");
        buffer_json_string(buffer, destination.label);
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
    case MARKDOWN_CORE_KIND_REFERENCE:
        buffer_cstr(buffer, " label=");
        buffer_json_string(buffer, chunk_string(node->as.reference->label));
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

/* The scopes of a node's source ranges, in UTF-8 columns (source_scope),
 * joined by commas. */
static void buffer_scope(dump_buffer *buffer, const markdown_core_place *ranges, size_t count) {
    source_lines lines = {buffer->lines, buffer->line_count};
    for (size_t i = 0; i < count; i++) {
        markdown_core_scope scope = source_scope(&lines, buffer->source, ranges[i], MARKDOWN_CORE_TEXT_UNIT_UTF8);
        if (i) {
            buffer_cstr(buffer, ",");
        }
        buffer_i64(buffer, scope.start.line);
        buffer_cstr(buffer, ":");
        buffer_i64(buffer, scope.start.column);
        buffer_cstr(buffer, "..");
        buffer_i64(buffer, scope.end.line);
        buffer_cstr(buffer, ":");
        buffer_i64(buffer, scope.end.column);
    }
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
static void dump_node(dump_buffer *buffer, const markdown_core_node *node, const markdown_core_place *ranges,
                      size_t count, size_t depth) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    /* `children` counts structural children, a cite's items and a
     * definition's bodies among them: the node's children array. */
    size_t child_count = node->child_count;
    dump_prefix(buffer, depth);
    buffer_cstr(buffer, S_kind_name[kind]);
    buffer_cstr(buffer, " scope=");
    buffer_scope(buffer, ranges, count);
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

/* Draws the tree under `node`, a node of `document`, one line per item of the
 * canonical walk of the document from `node` up to the next item outside its
 * tree: a node's line, or a group line naming a node-valued list, drawn at
 * its level below `node`. The walk's stack is the tree's depth, never the C
 * stack's. False when a node ends past the source. */
static bool dump_tree(dump_buffer *buffer, const markdown_core_document *document, const markdown_core_node *node) {
    markdown_core_walk walk;
    markdown_core_walk_item item;
    bool inside = false, covered = true;
    size_t base = 0;
    markdown_core_walk_begin(&walk, document->root);
    while (!buffer->failed && markdown_core_walk_next(&walk, &item)) {
        if (!inside) {
            inside = item.node == node;
            base = item.level;
            if (!inside) {
                continue;
            }
        } else if (item.level <= base) {
            break;
        }
        size_t level = item.level - base;
        if (!item.node) {
            dump_group_line(buffer, item.group, item.count, level - 1, markdown_core_walk_has_next(&walk));
            continue;
        }
        const markdown_core_place *ranges;
        size_t count;
        if (!markdown_core_walk_ranges(&walk, &item, &ranges, &count)) {
            break;
        }
        if (ranges[count - 1].end > buffer->source_length) {
            covered = false;
            break;
        }
        if (level) {
            if (!ensure_level(buffer, level - 1)) {
                break;
            }
            buffer->more[level - 1] = markdown_core_walk_has_next(&walk);
            extend_prefix(buffer, level - 1);
        }
        dump_node(buffer, item.node, ranges, count, level);
    }
    buffer->failed = buffer->failed || walk.failed;
    markdown_core_walk_end(&walk);
    return covered;
}

/* The source must cover the tree: the dump draws the scope of every node of
 * it. */
markdown_core_status markdown_core_document_dump(const markdown_core_document *document, const markdown_core_node *node,
                                                 const uint8_t *source, size_t source_length, uint8_t **output,
                                                 size_t *length) {
    dump_buffer buffer = {0};
    source_lines lines;
    if (!source_lines_read(&lines, source, source_length)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    buffer.source = source;
    buffer.source_length = source_length;
    buffer.lines = lines.starts;
    buffer.line_count = lines.count;
    bool covered = dump_tree(&buffer, document, node);
    free(buffer.more);
    free(buffer.prefix);
    free(buffer.prefix_end);
    markdown_core_free(lines.starts);
    if (buffer.failed || !covered) {
        free(buffer.data);
        return buffer.failed ? MARKDOWN_CORE_ALLOCATION_FAILED : MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *output = buffer.data;
    *length = buffer.size;
    return MARKDOWN_CORE_OK;
}

void markdown_core_dump_free(uint8_t *output) { free(output); }
