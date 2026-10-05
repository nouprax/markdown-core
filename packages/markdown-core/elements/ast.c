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

/* A labeled definition and its label, as the label sort reads them: the
 * label's first eight bytes, big-endian and padded with zero bytes, order two
 * labels whose heads differ there; equal heads leave it to the lengths, or to
 * the bytes past the eighth. */
typedef struct {
    uint64_t head;
    const uint8_t *label;
    size_t length;
    const markdown_core_node *node;
} label_entry;

static label_entry label_entry_of(const uint8_t *label, size_t length, const markdown_core_node *node) {
    uint64_t head = 0;
    for (size_t i = 0; i < 8; i++) {
        head = head << 8 | (i < length ? label[i] : 0);
    }
    return (label_entry){head, label, length, node};
}

static inline bool label_before(const label_entry *a, const label_entry *b) {
    if (a->head != b->head) {
        return a->head < b->head;
    }
    /* Equal heads hold all of two labels no longer than eight bytes: the
     * shorter is first. */
    if (a->length <= 8 && b->length <= 8) {
        return a->length < b->length;
    }
    return label_compare(a->label, a->length, b->label, b->length) < 0;
}

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

/* The labeled ones of `count` nodes in label order, keeping their order
 * among equal labels, into `labeled`; `*labels` says how many. False when
 * the sort could not be done. */
static bool label_order(const markdown_core_node *const *nodes, size_t count, const markdown_core_node **labeled,
                        size_t *labels) {
    /* The entries, the spare array the merges write into, and the ends of
     * the runs, in one allocation. */
    label_entry *entries = markdown_core_realloc(NULL, count * (2 * sizeof(*entries) + sizeof(size_t)));
    if (!entries) {
        return false;
    }
    size_t *ends = (size_t *)(entries + 2 * count);
    size_t taken = 0;
    for (size_t i = 0; i < count; i++) {
        markdown_core_chunk label;
        if (definition_label(nodes[i], &label)) {
            entries[taken++] = label_entry_of(label.data, (size_t)label.len, nodes[i]);
        }
    }
    const label_entry *sorted = label_sort(entries, entries + taken, ends, taken);
    for (size_t i = 0; i < taken; i++) {
        labeled[i] = sorted[i].node;
    }
    *labels = taken;
    markdown_core_free(entries);
    return true;
}

/* The table's nodes in source order, as node handles the document borrows,
 * into `*nodes`. */
static bool table_nodes(definition_table *table, markdown_core_source_order *order, const markdown_core_node ***nodes) {
    *nodes = NULL;
    if (!table->count) {
        return true;
    }
    if (!markdown_core_order_source_entries(order, table->values, table->count, sizeof(*table->values),
                                            definition_key)) {
        return false;
    }
    *nodes = markdown_core_alloc(table->count, sizeof(**nodes));
    if (!*nodes) {
        return false;
    }
    for (size_t i = 0; i < table->count; i++) {
        (*nodes)[i] = table->values[i].node;
    }
    return true;
}

/* The table in source order, and its labeled definitions in label order, as
 * node handles the document borrows. */
static bool table_seal(definition_table *table, markdown_core_source_order *order, markdown_core_definitions *out) {
    *out = (markdown_core_definitions){0};
    const markdown_core_node **nodes;
    if (!table_nodes(table, order, &nodes)) {
        return false;
    }
    if (!nodes) {
        return true;
    }
    const markdown_core_node **labeled = markdown_core_alloc(table->count, sizeof(*labeled));
    size_t labels = 0;
    if (!labeled || !label_order(nodes, table->count, labeled, &labels)) {
        markdown_core_free((void *)nodes);
        markdown_core_free((void *)labeled);
        return false;
    }
    *out = (markdown_core_definitions){nodes, table->count, labeled, labels};
    return true;
}

static bool relation_chain(markdown_core_relation *relation, const char *group, const markdown_core_node *first) {
    *relation = (markdown_core_relation){group, first, NULL};
    return true;
}

static bool relation_one(markdown_core_relation *relation, const markdown_core_node *node) {
    *relation = (markdown_core_relation){NULL, node, node->next};
    return true;
}

size_t markdown_core_relation_count(const markdown_core_relation *relation) {
    size_t count = 0;
    for (const markdown_core_node *node = relation->first; node != relation->end; node = node->next) {
        count++;
    }
    return count;
}

/* THE SHAPE of a kind's relations: most kinds own only their children; the
 * rest own fields too, in the order `markdown_core_relations_next` gives. */
typedef enum {
    SHAPE_CHILDREN = 0,
    SHAPE_DOCUMENT,
    SHAPE_TABLE,
    SHAPE_DIRECTIVE,
    SHAPE_CALLOUT,
    SHAPE_CITE,
    SHAPE_CITATION,
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
    return shape == SHAPE_CHILDREN ? !node->first_child : fields_empty(node, shape);
}

/* The same question for a kind that owns fields besides its children. */
static bool fields_empty(const markdown_core_node *node, relation_shape shape) {
    switch (shape) {
    case SHAPE_CHILDREN:
        return !node->first_child;
    case SHAPE_DIRECTIVE:
        return !node->first_child && !markdown_core_directive_label(node);
    case SHAPE_CALLOUT:
        return !node->first_child && !node->as.callout->title;
    case SHAPE_CITE:
        return !node->as.cite->citations;
    case SHAPE_DOCUMENT:
        return !node->first_child && !node->as.document->metadata;
    case SHAPE_TABLE:
    case SHAPE_CITATION:
    case SHAPE_DEFINITION:
        return false;
    }
    return false;
}

void markdown_core_relations_begin(markdown_core_relation_cursor *cursor, const markdown_core_node *owner) {
    *cursor = (markdown_core_relation_cursor){owner, (uint8_t)shape_of(owner), 0, NULL};
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
                return relation_one(relation, node->as.document->metadata);
            }
            step = cursor->step++;
        }
        *more = false;
        return step == 1 && relation_chain(relation, NULL, node->first_child);
    case SHAPE_TABLE: {
        const markdown_core_table *table = node->opaque;
        if (step == 0) {
            cursor->next = node->first_child;
            if (table->caption) {
                *more = true;
                return relation_one(relation, table->caption);
            }
            step = cursor->step++;
        }
        if (step > 3) {
            return false;
        }
        size_t count = step == 1 ? table->head_count : step == 2 ? table->content_count : table->foot_count;
        const markdown_core_node *first = cursor->next, *end = first;
        for (; count && end; count--) {
            end = end->next;
        }
        cursor->next = end;
        *relation = (markdown_core_relation){table_groups[step - 1], first, end};
        *more = step < 3;
        return true;
    }
    case SHAPE_DIRECTIVE:
        if (step == 0) {
            const markdown_core_node *label = markdown_core_directive_label(node);
            if (label) {
                *more = true;
                return relation_one(relation, label);
            }
            step = cursor->step++;
        }
        *more = false;
        return step == 1 && relation_chain(relation, NULL, node->first_child);
    case SHAPE_CALLOUT:
        if (step == 0) {
            if (node->as.callout->title) {
                *more = true;
                return relation_chain(relation, "Title", node->as.callout->title->first_child);
            }
            step = cursor->step++;
        }
        *more = false;
        return step == 1 && relation_chain(relation, NULL, node->first_child);
    case SHAPE_CITE:
        *more = false;
        return step == 0 && relation_chain(relation, NULL, node->as.cite->citations);
    case SHAPE_CITATION: {
        const markdown_core_citation_item *citation = node->as.citation;
        *more = true;
        if (step == 0) {
            if (citation->note) {
                return relation_one(relation, citation->note);
            }
            step = cursor->step++;
        }
        if (step == 1) {
            return relation_chain(relation, "CitationPrefix", citation->prefix ? citation->prefix->first_child : NULL);
        }
        *more = false;
        return step == 2 &&
               relation_chain(relation, "CitationSuffix", citation->suffix ? citation->suffix->first_child : NULL);
    }
    case SHAPE_DEFINITION: {
        if (step == 0) {
            cursor->next = node->first_child;
            *more = cursor->next != NULL;
            return relation_chain(relation, "DefinitionTerm", node->as.definition->term->first_child);
        }
        const markdown_core_node *body = cursor->next;
        if (!body) {
            return false;
        }
        cursor->next = body->next;
        *more = cursor->next != NULL;
        return relation_chain(relation, "DefinitionBody", body->first_child);
    }
    case SHAPE_CHILDREN:
        *more = false;
        return step == 0 && relation_chain(relation, NULL, node->first_child);
    }
    return false;
}

bool markdown_core_relations_next(markdown_core_relation_cursor *cursor, markdown_core_relation *relation) {
    bool more;
    return relations_next(cursor, relation, &more);
}

/* THE RUNS OF A NODE'S INLINE CONTENT, which is its first relation when it
 * is an inline root's: the runs of the node itself, or of the private node a
 * callout's title or a definition's term hangs from; NULL when that relation
 * is not an inline root's content. Its places are offsets in that content,
 * and every place below them is too. Roots never nest. */
static inline markdown_core_runs *shape_runs(const markdown_core_node *node, relation_shape shape) {
    switch (shape) {
    case SHAPE_CALLOUT:
        return node->as.callout->title ? node->as.callout->title->runs : NULL;
    case SHAPE_DEFINITION:
        return node->as.definition->term->runs;
    default:
        return node->runs;
    }
}

static markdown_core_runs *content_runs(const markdown_core_node *node) { return shape_runs(node, shape_of(node)); }

bool markdown_core_source_runs_read(markdown_core_source_runs *table, const markdown_core_runs *runs, uint32_t origin) {
    markdown_core_source_run *grown =
        markdown_core_reserve(table->runs, &table->capacity, runs->count, sizeof(*table->runs));
    if (!grown) {
        return false;
    }
    table->runs = grown;
    uint32_t content = 0;
    int64_t at = origin;
    for (uint32_t i = 0; i < runs->count; i++) {
        const markdown_core_run run = runs->items[i].run;
        const uint32_t start = (uint32_t)(at + run.lead);
        table->runs[i] = (markdown_core_source_run){content, run.length, start, start + run.span};
        content += run.length;
        at = start + run.span;
    }
    table->count = runs->count;
    return true;
}

static inline bool source_run_copied(const markdown_core_source_run *run) {
    return run->end - run->start == run->length;
}

/* The run content offset `offset` is in: the last that starts at or before
 * it. */
static size_t source_run_at(const markdown_core_source_runs *table, uint32_t offset) {
    size_t lo = 0, hi = table->count;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (table->runs[mid].content <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Where content offset `offset` is read from: its source byte, the start of
 * the run that reads it whole, or, past the content, where the content
 * ends. */
static uint32_t source_run_place(const markdown_core_source_runs *table, uint32_t offset) {
    const markdown_core_source_run *run = &table->runs[source_run_at(table, offset)];
    if (offset >= run->content + run->length) {
        return run->end;
    }
    return source_run_copied(run) ? run->start + (offset - run->content) : run->start;
}

size_t markdown_core_source_runs_ranges(const markdown_core_source_runs *table, markdown_core_place place,
                                        markdown_core_place *ranges, size_t capacity) {
    if (!table->count) {
        return 0;
    }
    if (place.end <= place.start) {
        if (capacity) {
            uint32_t at = source_run_place(table, place.start);
            ranges[0] = (markdown_core_place){at, at};
        }
        return 1;
    }
    size_t count = 0;
    markdown_core_place last = {0, 0};
    for (size_t i = source_run_at(table, place.start); i < table->count && table->runs[i].content < place.end; i++) {
        const markdown_core_source_run *run = &table->runs[i];
        const uint32_t from = place.start > run->content ? place.start : run->content;
        const uint32_t to = place.end < run->content + run->length ? place.end : run->content + run->length;
        if (from >= to) {
            continue;
        }
        markdown_core_place part = source_run_copied(run) ? (markdown_core_place){run->start + (from - run->content),
                                                                                  run->start + (to - run->content)}
                                                          : (markdown_core_place){run->start, run->end};
        if (count && last.end == part.start) {
            last.end = part.end;
            if (count <= capacity) {
                ranges[count - 1] = last;
            }
            continue;
        }
        last = part;
        if (count < capacity) {
            ranges[count] = part;
        }
        count++;
    }
    return count;
}

/* PUBLISHING CONTINUES THE PREVIOUS TREE (docs/plans/2026-09-29-incremental-
 * parsing.md, 5.9). The walk that gives each node its id and extent also
 * matches it, relation by relation, to the old node it continues, and
 * decides in post-order whether it equals that old node as a value.
 *
 * Matching runs within the relation of a matched owner. An old node's anchor
 * is the first byte of its range that survived the edits, and a new node of
 * its kind whose range holds the anchor's image continues the earliest such
 * old sibling. Both relations are in source order, so one pointer into the
 * old relation moves forward with the new one. The root continues the old
 * root. A new node that continues nothing takes the next id.
 *
 * A matched node is SAME when its scalars and extent equal its old node's,
 * both have the same relations, every old node of each relation was matched
 * and every new node of it is same: matching is monotone and one-to-one, so
 * the old node then holds exactly those old nodes, in order. Each same pair
 * exchanges values, so the old node holds the new value and the storage it
 * borrows; the largest same subtrees are then swapped into the new tree in
 * place of their new nodes. The old tree, which now holds the new nodes they
 * replaced and every old node nothing matched, is released with the old
 * storage. When the root itself is same the old tree is the result and the
 * new one is released. A fresh parse continues no tree, so no node is
 * matched and every node takes the next id in walk order. */

/* A same pair, recorded in post-order: `node` gives `old` its value and, at
 * the root of a largest same subtree, its place. The pairs of the node's
 * subtree are those from `first` on. */
typedef struct {
    markdown_core_node *node, *old, *owner, *old_owner;
    size_t first;
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

/* The first edit that ends after old byte `x`: every one before it ends at
 * or before x, so x is past it and shifted by it. */
static size_t edit_after(const publish_identity *identity, uint32_t x) {
    const markdown_core_byte_edit *edits = identity->edits;
    size_t lo = 0, hi = identity->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (edits[mid].end <= x) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* The image of the first byte of [start, end) that no edit replaced, or false
 * when every byte of it was replaced (5.2). */
static bool anchor_mapped(const publish_identity *identity, uint32_t start, uint32_t end, uint32_t *mapped) {
    const markdown_core_byte_edit *edits = identity->edits;
    size_t lo = edit_after(identity, start), x = start;
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

static inline bool is_link(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_LINK || node->kind == MARKDOWN_CORE_NODE_EMBEDDED;
}

/* THE SLOT THAT HOLDS `node` when no sibling precedes it: its parent's first
 * child, or the field of `owner` the node heads. */
static markdown_core_node **head_slot(markdown_core_node *owner, markdown_core_node *node) {
    if (node->parent && node->parent->first_child == node) {
        return &node->parent->first_child;
    }
    switch (shape_of(owner)) {
    case SHAPE_DOCUMENT:
        return &owner->as.document->metadata;
    case SHAPE_TABLE:
        return &((markdown_core_table *)owner->opaque)->caption;
    case SHAPE_DIRECTIVE:
        return &((markdown_core_directive_value *)owner->opaque)->label;
    case SHAPE_CITE:
        return &owner->as.cite->citations;
    case SHAPE_CITATION:
        return &owner->as.citation->note;
    case SHAPE_CALLOUT:
    case SHAPE_DEFINITION:
    case SHAPE_CHILDREN:
        break;
    }
    return NULL;
}

/* Exchanges the places of two nodes of two trees: each takes the other's
 * siblings, parent and the slot that held the other. */
static void swap_places(const publish_swap *swap) {
    markdown_core_node *a = swap->node, *b = swap->old;
    markdown_core_node *a_prev = a->prev, *a_next = a->next, *a_parent = a->parent;
    markdown_core_node *b_prev = b->prev, *b_next = b->next, *b_parent = b->parent;
    markdown_core_node **a_head = a_prev ? &a_prev->next : head_slot(swap->owner, a);
    markdown_core_node **b_head = b_prev ? &b_prev->next : head_slot(swap->old_owner, b);
    markdown_core_node **a_tail = a_next                                  ? &a_next->prev
                                  : a_parent && a_parent->last_child == a ? &a_parent->last_child
                                                                          : NULL;
    markdown_core_node **b_tail = b_next                                  ? &b_next->prev
                                  : b_parent && b_parent->last_child == b ? &b_parent->last_child
                                                                          : NULL;
    *a_head = b;
    *b_head = a;
    if (a_tail) {
        *a_tail = b;
    }
    if (b_tail) {
        *b_tail = a;
    }
    b->prev = a_prev;
    b->next = a_next;
    b->parent = a_parent;
    a->prev = b_prev;
    a->next = b_next;
    a->parent = b_parent;
}

/* The private nodes a same pair's relations hang from, such as a callout's
 * title, a definition's term and bodies and a citation's affixes, hold the
 * storage their nodes borrow: each pair of them exchanges values too. */
static void swap_containers(markdown_core_node *node, markdown_core_node *old) {
    if (shape_of(node) == SHAPE_CHILDREN) {
        return;
    }
    markdown_core_relation_cursor cursor, old_cursor;
    markdown_core_relation relation, old_relation;
    bool more;
    markdown_core_relations_begin(&cursor, node);
    markdown_core_relations_begin(&old_cursor, old);
    while (relations_next(&cursor, &relation, &more) && relations_next(&old_cursor, &old_relation, &more)) {
        markdown_core_node *holder = relation.first ? relation.first->parent : NULL;
        if (holder && holder != node) {
            markdown_core_node_swap_values(holder, old_relation.first->parent);
        }
    }
}

/* A node whose relations are being published: where the node starts, its
 * cursor and whether that may hold another relation, and, while a nested
 * node's relations are published, the rest of the relation in hand and the
 * offset its next node's extent is relative to (the end of the previous node
 * of its relation, or the owner's start). */
typedef struct {
    markdown_core_relation_cursor cursor;
    const markdown_core_node *item, *end;
    uint32_t start, anchor;
    bool more;
    /* Whether the relation in hand is in an inline root's content, and
     * whether this frame's node is the root whose content it is. */
    bool content, root;
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

/* The relation in hand: the walk keeps it apart from its frame, which holds
 * it only while the frame waits. */
typedef struct {
    const markdown_core_node *item, *end;
    uint32_t anchor;
} publish_relation;

/* `node`'s first relation, which holds a node or a group, and whether more
 * may follow it: the one relation of a kind that owns only its children, or
 * the first its cursor finds. False when they hold no node. */
static inline bool publish_first(const markdown_core_node *node, relation_shape shape,
                                 markdown_core_relation_cursor *cursor, publish_relation *first, bool *more) {
    if (shape == SHAPE_CHILDREN) {
        first->item = node->first_child;
        first->end = NULL;
        *more = false;
        return node->first_child != NULL;
    }
    markdown_core_relation relation;
    *cursor = (markdown_core_relation_cursor){node, (uint8_t)shape, 0, NULL};
    if (!relations_next(cursor, &relation, more)) {
        return false;
    }
    first->item = relation.first;
    first->end = relation.end;
    return relation.first != relation.end || *more;
}

/* What one publish carries through its walks: the walk stack, what matching
 * needs, the lookup tables, the last id issued and the nodes matched. Every
 * other node takes an id, so the published tree counts the ids issued and
 * the nodes matched. */
/* THE IMAGES OF AN OLD ROOT'S CONTENT in the new root's (5.2): for each part
 * of the old content whose bytes the new root reads too, in content order,
 * the new content offset its first byte is read at, and whether the rest
 * follow it byte for byte. An old copied run's part reads each content byte
 * from one source byte that no edit replaced and the new root reads; any
 * other old run is one part, imaged where the new root reads the first of
 * its source bytes it still reads. `hint` is where the last lookup ended. */
typedef struct {
    uint32_t from, to, at;
    bool copied;
} content_image;

typedef struct {
    content_image *items;
    size_t count, capacity, hint;
} content_images;

typedef struct {
    publish_stack stack;
    publish_identity identity;
    definition_table tables[TABLE_COUNT];
    uint64_t next_id;
    size_t matched;
    /* The runs of the inline root whose content the walk is in, in absolute
     * offsets, and the images of the content of the old root it continues:
     * roots never nest, so one of each serves every root in turn. The
     * root's own runs, and where it starts, wait in `entered` until a node of
     * its content asks for the table. */
    markdown_core_source_runs runs;
    content_images images;
    const markdown_core_runs *entered;
    uint32_t entered_start;
} publish_walk;

/* The runs of the root whose content the walk is in, read into `runs` when
 * they are first asked for. False when they could not be read. */
static bool publish_runs_read(publish_walk *walk) {
    if (walk->entered) {
        if (!markdown_core_source_runs_read(&walk->runs, walk->entered, walk->entered_start)) {
            return false;
        }
        walk->entered = NULL;
    }
    return true;
}

/* A node's pieces as it is published (node.h): each within its place, those
 * that touch joined into one, and measured from the end of the piece before,
 * or from the node's start. A node whose range is one piece has none. */
static void publish_pieces(markdown_core_node_pool *pool, markdown_core_node *node, markdown_core_place place) {
    markdown_core_pieces *pieces = node->pieces;
    uint32_t count = 0;
    for (uint32_t i = 0; i < pieces->count; i++) {
        markdown_core_place piece = pieces->items[i].place;
        uint32_t start = piece.start > place.start ? piece.start : place.start;
        uint32_t end = piece.end < place.end ? piece.end : place.end;
        if (start >= end) {
            continue;
        }
        if (count && pieces->items[count - 1].place.end == start) {
            pieces->items[count - 1].place.end = end;
            continue;
        }
        pieces->items[count++].place = (markdown_core_place){start, end};
    }
    if (count < 2) {
        markdown_core_node_pool_bytes_free(pool, pieces);
        node->pieces = NULL;
        return;
    }
    uint32_t anchor = place.start;
    for (uint32_t i = 0; i < count; i++) {
        markdown_core_place piece = pieces->items[i].place;
        pieces->items[i].piece =
            (markdown_core_piece){(int32_t)((int64_t)piece.start - anchor), piece.end - piece.start};
        anchor = piece.end;
    }
    pieces->count = count;
}

/* An inline root's runs as their holder is published (node.h): each
 * measured from the end of the run before, or from `start`, where the node
 * whose content they are starts. */
static void publish_runs(markdown_core_runs *runs, uint32_t start) {
    uint32_t anchor = start;
    for (uint32_t i = 0; i < runs->count; i++) {
        const uint32_t from = runs->items[i].place.start, to = runs->items[i].place.end,
                       length = runs->items[i].place.length;
        runs->items[i].run = (markdown_core_run){(int32_t)((int64_t)from - anchor), to - from, length};
        anchor = to;
    }
}

/* Gives `node` its id and its extent against `anchor`, its pieces and the
 * runs of its content, `runs`, and records it in its lookup table when its
 * slot says the tables find it, at its source start: its place is in the
 * content of the root whose runs the walk holds when `content` says so.
 * Returns where it ended. */
static inline bool publish_node(publish_walk *walk, markdown_core_node *node, unsigned slot, markdown_core_runs *runs,
                                uint32_t anchor, uint64_t id, bool content, markdown_core_place *place) {
    *place = node->where.place;
    node->id = id;
    node->where.extent =
        (markdown_core_extent){(int32_t)((int64_t)place->start - (int64_t)anchor), place->end - place->start};
    if (node->pieces) {
        publish_pieces(walk->stack.parser->pool, node, *place);
    }
    if (runs) {
        publish_runs(runs, place->start);
    }
    if (!SLOT_TABLE(slot)) {
        return true;
    }
    if (content && !publish_runs_read(walk)) {
        return false;
    }
    return table_add(&walk->tables[SLOT_TABLE(slot) - 1], node,
                     content ? source_run_place(&walk->runs, place->start) : place->start);
}

/* The relation in hand becomes the first of a node whose content's runs are
 * `runs`, which starts at `start`: an inline root's content, which runs from
 * 0 and whose runs the walk holds, or one in the coordinates of the relation
 * the node is in. */
static inline void publish_enter(publish_walk *walk, const markdown_core_runs *runs, publish_frame *frame,
                                 publish_relation *hand, bool content) {
    frame->root = runs != NULL;
    frame->content = content || frame->root;
    if (frame->root) {
        hand->anchor = 0;
        walk->entered = runs;
        walk->entered_start = frame->start;
    }
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
                                                                          relation_shape shape, uint32_t start,
                                                                          bool content) {
    publish_stack *stack = &walk->stack;
    /* The walk ends with the stack as it found it, so its count and the ids
     * it issues stay in hand until then. */
    size_t base = stack->count, count = base;
    uint64_t next_id = walk->next_id;
    markdown_core_relation_cursor cursor;
    publish_relation hand, first;
    markdown_core_place place;
    bool more;
    if (!publish_first(node, shape, &cursor, &first, &more)) {
        return true;
    }
    if (count == stack->capacity && !publish_grow(stack)) {
        return false;
    }
    publish_frame *frame = &stack->frames[count++];
    *frame = (publish_frame){.cursor = cursor, .start = start, .more = more};
    hand = (publish_relation){first.item, first.end, start};
    publish_enter(walk, shape_runs(node, shape), frame, &hand, content);
    while (count > base) {
        if (hand.item == hand.end) {
            markdown_core_relation relation;
            if (frame->more && relations_next(&frame->cursor, &relation, &frame->more)) {
                hand = (publish_relation){relation.first, relation.end, frame->start};
                /* A root's later relations are not its content. */
                if (frame->root) {
                    frame->root = frame->content = false;
                }
            } else if (--count > base) {
                frame--;
                hand = (publish_relation){frame->item, frame->end, frame->anchor};
            }
            continue;
        }
        markdown_core_node *child = (markdown_core_node *)hand.item;
        hand.item = child->next;
        unsigned slot = slot_of(child);
        relation_shape child_shape = (relation_shape)(slot & SLOT_SHAPE);
        markdown_core_runs *runs = shape_runs(child, child_shape);
        content = frame->content;
        if (!publish_node(walk, child, slot, runs, hand.anchor, ++next_id, content, &place)) {
            return false;
        }
        hand.anchor = place.end;
        if (!publish_first(child, child_shape, &cursor, &first, &more)) {
            continue;
        }
        /* A frame with nothing left gives its slot to the node's own; any
         * other waits with the relation in hand. */
        if (hand.item != hand.end || frame->more) {
            frame->item = hand.item;
            frame->end = hand.end;
            frame->anchor = hand.anchor;
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
        hand = (publish_relation){first.item, first.end, place.start};
        publish_enter(walk, runs, frame, &hand, content);
    }
    walk->next_id = next_id;
    return true;
}

/* A matched node whose relations are being published: its frame of the
 * walk, the node and the old node it continues, the old node's relations not
 * yet paired, the old relation paired with the one in hand -- its next old
 * node, and the end of the node before that (the old node's start for the
 * first) -- and whether the node is same so far. */
typedef struct {
    publish_frame walk;
    markdown_core_node *node, *old;
    bool same;
    markdown_core_relation_cursor old_cursor;
    markdown_core_relation old_relation;
    /* The old relation's field, or INT_MAX when the old node has no more. */
    int old_field;
    markdown_core_node *old_item;
    const markdown_core_node *old_end;
    uint32_t old_start, old_anchor;
    /* The swaps pending when the node was entered, and its lookup table
     * entry counted from 1 (0 for none). */
    size_t swaps, entry;
} publish_match_frame;

/* The field a cursor's last relation holds: stepping leaves the cursor one
 * past it. A kind that owns only its children has field 0. */
static inline int relation_field(const markdown_core_relation_cursor *cursor, relation_shape shape) {
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
    frame->old_anchor = frame->walk.root ? 0 : frame->old_start;
    if (frame->old_field == field) {
        frame->old_item = (markdown_core_node *)frame->old_relation.first;
        frame->old_end = frame->old_relation.end;
        old_relation_next(frame);
    } else {
        frame->same = false;
        frame->old_item = NULL;
        frame->old_end = NULL;
    }
}

static bool content_image_add(content_images *images, content_image image) {
    if (!publish_reserve((void **)&images->items, &images->capacity, images->count, sizeof(*images->items))) {
        return false;
    }
    images->items[images->count++] = image;
    return true;
}

/* Reads the images of the content of the old root whose runs are `runs`,
 * measured from `origin`, against the new root's runs the walk holds. The
 * old runs, the edits and the new runs all increase in source, so one pass
 * over each pairs them. False when the table could not grow. */
static bool content_images_read(publish_walk *walk, const markdown_core_runs *runs, uint32_t origin) {
    content_images *images = &walk->images;
    const publish_identity *identity = &walk->identity;
    const markdown_core_byte_edit *edits = identity->edits;
    const markdown_core_source_runs *fresh = &walk->runs;
    images->count = images->hint = 0;
    if (!runs || !runs->count) {
        return true;
    }
    int64_t at = origin;
    uint32_t content = 0;
    size_t edit = edit_after(identity, (uint32_t)(at + runs->items[0].run.lead)), next = 0;
    for (uint32_t i = 0; i < runs->count; i++) {
        const markdown_core_run run = runs->items[i].run;
        const uint32_t start = (uint32_t)(at + run.lead), end = start + run.span, from = content;
        const bool copied = run.span == run.length;
        at = end;
        content += run.length;
        if (!run.length) {
            continue;
        }
        /* Each stretch of the run's source that no edit replaced, and the
         * parts of its image the new runs read. */
        for (uint32_t x = start; x < end;) {
            while (edit < identity->count && edits[edit].end <= x) {
                edit++;
            }
            if (edit < identity->count && edits[edit].start <= x) {
                x = (uint32_t)edits[edit].end;
                continue;
            }
            const uint32_t stop = edit < identity->count && edits[edit].start < end ? (uint32_t)edits[edit].start : end;
            const int64_t shift = identity->shift[edit];
            const uint32_t low = (uint32_t)(x + shift), high = (uint32_t)(stop + shift);
            while (next < fresh->count && fresh->runs[next].end <= low) {
                next++;
            }
            for (size_t j = next; j < fresh->count && fresh->runs[j].start < high; j++) {
                const markdown_core_source_run *read = &fresh->runs[j];
                const uint32_t first = low > read->start ? low : read->start;
                const uint32_t last = high < read->end ? high : read->end;
                if (first >= last) {
                    continue;
                }
                const bool follows = source_run_copied(read);
                const uint32_t image = follows ? read->content + (first - read->start) : read->content;
                if (!copied) {
                    if (!content_image_add(images, (content_image){from, content, image, false})) {
                        return false;
                    }
                    x = end;
                    break;
                }
                const uint32_t old_first = from + (uint32_t)(first - shift - start);
                if (!content_image_add(images,
                                       (content_image){old_first, old_first + (last - first), image, follows})) {
                    return false;
                }
            }
            if (x < end) {
                x = stop;
            }
        }
    }
    return true;
}

/* The image of an old content range [start, end): the content offset at
 * which the new root reads the first source byte that the old root read for
 * the range, that no edit replaced, and that the new root reads too (5.2).
 * False when there is none. Matching asks in content order, so the lookup
 * steps on from where the last one ended. */
static bool content_anchor(publish_walk *walk, uint32_t start, uint32_t end, uint32_t *mapped) {
    content_images *images = &walk->images;
    size_t i = images->hint;
    if (i && images->items[i - 1].to > start) {
        size_t lo = 0, hi = i;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (images->items[mid].to <= start) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        i = lo;
    }
    while (i < images->count && images->items[i].to <= start) {
        i++;
    }
    images->hint = i;
    if (i == images->count || images->items[i].from >= end) {
        return false;
    }
    const content_image *image = &images->items[i];
    *mapped = image->at + (image->copied && start > image->from ? start - image->from : 0);
    return true;
}

/* The old node `node` continues in the old relation `owner` pairs with the
 * one in hand, stepping past every old node whose anchor's image lies before
 * `node`'s end. An old node stepped past without being matched makes the
 * owner differ. */
static markdown_core_node *publish_match(publish_walk *walk, publish_match_frame *owner, const markdown_core_node *node,
                                         uint32_t *old_start) {
    markdown_core_place place = node->where.place;
    markdown_core_node *match = NULL;
    while (owner->old_item != owner->old_end) {
        markdown_core_node *old = owner->old_item;
        uint32_t start = (uint32_t)((int64_t)owner->old_anchor + old->where.extent.lead);
        uint32_t end = start + old->where.extent.span, mapped;
        bool anchored = owner->walk.content ? content_anchor(walk, start, end, &mapped)
                                            : anchor_mapped(&walk->identity, start, end, &mapped);
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
        owner->old_item = old->next;
    }
    if (!match) {
        owner->same = false;
    }
    return match;
}

static bool scalars_equal(const markdown_core_node *a, const markdown_core_node *b);

static bool pieces_equal(const markdown_core_pieces *a, const markdown_core_pieces *b) {
    if (!a || !b) {
        return a == b;
    }
    if (a->count != b->count) {
        return false;
    }
    for (uint32_t i = 0; i < a->count; i++) {
        if (a->items[i].piece.lead != b->items[i].piece.lead || a->items[i].piece.span != b->items[i].piece.span) {
            return false;
        }
    }
    return true;
}

static bool runs_equal(const markdown_core_runs *a, const markdown_core_runs *b) {
    if (!a || !b) {
        return a == b;
    }
    if (a->count != b->count) {
        return false;
    }
    for (uint32_t i = 0; i < a->count; i++) {
        const markdown_core_run x = a->items[i].run, y = b->items[i].run;
        if (x.lead != y.lead || x.span != y.span || x.length != y.length) {
            return false;
        }
    }
    return true;
}

/* Whether a matched node equals its old node as a value, before their
 * relations are compared. */
static bool publish_same(const markdown_core_node *node, const markdown_core_node *old) {
    return node->where.extent.lead == old->where.extent.lead && node->where.extent.span == old->where.extent.span &&
           pieces_equal(node->pieces, old->pieces) && runs_equal(content_runs(node), content_runs(old)) &&
           scalars_equal(node, old);
}

/* A matched node's verdict, once its relations are published: a same node
 * is recorded after the pairs of its subtree, from `swaps` on, and the
 * lookup tables name the old node; a node that differs makes its owner
 * differ. */
static void publish_verdict(publish_walk *walk, publish_match_frame *owner, markdown_core_node *node,
                            markdown_core_node *old, bool same, size_t swaps, size_t entry) {
    publish_identity *identity = &walk->identity;
    if (!same) {
        owner->same = false;
        return;
    }
    identity->swaps[identity->swap_count++] = (publish_swap){node, old, owner->node, owner->old, swaps};
    if (entry) {
        walk->tables[SLOT_TABLE(slot_of(node)) - 1].values[entry - 1].node = old;
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
    publish_relation hand, first, old_first;
    markdown_core_place place;
    bool more, old_more, ok = true;
    unsigned slot = slot_of(root);
    relation_shape shape = (relation_shape)(slot & SLOT_SHAPE);
    if (!publish_node(walk, root, slot, shape_runs(root, shape), 0, previous->id, false, &place)) {
        return false;
    }
    walk->matched++;
    /* Whether the node about to be entered is same so far. */
    bool verdict = publish_same(root, previous);
    if (!publish_first(root, shape, &cursor, &first, &more)) {
        *same = verdict && !publish_first(previous, shape, &old_cursor, &old_first, &old_more);
        return true;
    }
    markdown_core_node *node = root, *old = previous;
    uint32_t old_start = 0;
    size_t entry = 0;
    bool content = false;
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
            .node = node,
            .old = old,
            .same = verdict,
            .old_start = old_start,
            .swaps = identity->swap_count,
            .entry = entry,
        };
        hand = (publish_relation){first.item, first.end, place.start};
        /* Matching reads the new root's runs as it pairs the old root's
         * nodes. */
        publish_enter(walk, content_runs(node), &frame->walk, &hand, content);
        if (!publish_runs_read(walk)) {
            ok = false;
            break;
        }
        /* The old node's content is read by its own runs, if any. */
        if (frame->walk.root && !content_images_read(walk, content_runs(old), old_start)) {
            ok = false;
            break;
        }
        markdown_core_relations_begin(&frame->old_cursor, old);
        old_relation_next(frame);
        publish_pair(frame, relation_field(&cursor, shape));
        /* The next matched node with relations, or the end of the walk. */
        for (node = NULL; ok && count && !node;) {
            if (hand.item == hand.end) {
                markdown_core_relation relation;
                if (frame->old_item != frame->old_end) {
                    frame->same = false;
                }
                if (frame->walk.more && relations_next(&frame->walk.cursor, &relation, &frame->walk.more)) {
                    hand = (publish_relation){relation.first, relation.end, frame->walk.start};
                    /* A root's later relations are not its content. */
                    if (frame->walk.root) {
                        frame->walk.root = frame->walk.content = false;
                    }
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
                publish_verdict(walk, frame - 1, frame->node, frame->old, frame->same, frame->swaps, frame->entry);
                frame--;
                hand = (publish_relation){frame->walk.item, frame->walk.end, frame->walk.anchor};
                continue;
            }
            markdown_core_node *child = (markdown_core_node *)hand.item;
            hand.item = child->next;
            slot = slot_of(child);
            shape = (relation_shape)(slot & SLOT_SHAPE);
            markdown_core_node *match = publish_match(walk, frame, child, &old_start);
            content = frame->walk.content;
            ok = publish_node(walk, child, slot, shape_runs(child, shape), hand.anchor,
                              match ? match->id : ++walk->next_id, content, &place);
            hand.anchor = place.end;
            if (!ok) {
                break;
            }
            if (!match) {
                ok = publish_fresh(walk, child, shape, place.start, content);
                continue;
            }
            walk->matched++;
            entry = SLOT_TABLE(slot) ? walk->tables[SLOT_TABLE(slot) - 1].count : 0;
            verdict = publish_same(child, match);
            if (!publish_first(child, shape, &cursor, &first, &more)) {
                verdict = verdict && !publish_first(match, shape, &old_cursor, &old_first, &old_more);
                publish_verdict(walk, frame, child, match, verdict, identity->swap_count, entry);
                continue;
            }
            frame->walk.item = hand.item;
            frame->walk.end = hand.end;
            frame->walk.anchor = hand.anchor;
            node = child;
            old = match;
        }
        if (!ok || !node) {
            break;
        }
    }
    markdown_core_free(frames);
    return ok;
}

/* Commits a published tree that continues the previous one: the previous
 * tree is the result when its root is `same`, and otherwise its largest same
 * subtrees take the place of their new nodes. Whatever tree is left over is
 * released into the pool. */
static void publish_continue(markdown_core_parser *parser, const publish_identity *identity, bool same) {
    markdown_core_node *root = parser->root, *previous = parser->revision->previous;
    if (same) {
        parser->root = previous;
        markdown_core_node_pool_release(parser->pool, root);
        return;
    }
    /* Backwards, a pair past the last root's first pair is the root of a
     * largest same subtree. */
    size_t roots = identity->swap_count;
    for (size_t i = identity->swap_count; i--;) {
        const publish_swap *swap = &identity->swaps[i];
        swap_containers(swap->node, swap->old);
        markdown_core_node_swap_values(swap->node, swap->old);
        if (i < roots) {
            swap_places(swap);
            roots = swap->first;
        }
    }
    markdown_core_node_pool_release(parser->pool, previous);
}

/* The node each label a reference occurrence can name resolves to, in label
 * order: the first Reference declaring it, or, when none does, the first
 * Heading whose text declares it. The References in source order, then the
 * Headings in source order, sorted by label keeping that order among equal
 * labels, put each label's target first among its own. */
static bool reference_targets(markdown_core_document_value *value, definition_table *headings,
                              markdown_core_source_order *order) {
    value->reference_targets = NULL;
    value->reference_target_count = 0;
    if (headings->count && !markdown_core_order_source_entries(order, headings->values, headings->count,
                                                               sizeof(*headings->values), definition_key)) {
        return false;
    }
    const size_t total = value->references.count + headings->count;
    const markdown_core_node **declared = total ? markdown_core_alloc(total, sizeof(*declared)) : NULL;
    if (total && !declared) {
        return false;
    }
    for (size_t i = 0; i < value->references.count; i++) {
        declared[i] = value->references.nodes[i];
    }
    for (size_t i = 0; i < headings->count; i++) {
        declared[value->references.count + i] = headings->values[i].node;
    }
    size_t labels = 0;
    if (total && !label_order(declared, total, declared, &labels)) {
        markdown_core_free((void *)declared);
        return false;
    }
    size_t count = 0;
    markdown_core_chunk last = {0}, label;
    for (size_t i = 0; i < labels; i++) {
        definition_label(declared[i], &label);
        if (count && label_compare(last.data, (size_t)last.len, label.data, (size_t)label.len) == 0) {
            continue;
        }
        declared[count++] = declared[i];
        last = label;
    }
    if (!count) {
        markdown_core_free((void *)declared);
        return true;
    }
    value->reference_targets = declared;
    value->reference_target_count = count;
    return true;
}

bool markdown_core_publish_tree(markdown_core_parser *parser) {
    markdown_core_revision *revision = parser->revision;
    markdown_core_node *root = parser->root, *previous = revision->previous;
    markdown_core_document_value *value = root->as.document;
    publish_walk walk;
    walk.stack = (publish_stack){parser, parser->walk_stack, 0, parser->walk_stack_size / sizeof(publish_frame)};
    memset(walk.tables, 0, sizeof(walk.tables));
    walk.next_id = revision->last_id;
    walk.matched = 0;
    walk.runs = (markdown_core_source_runs){0};
    walk.images = (content_images){0};
    walk.entered = NULL;
    markdown_core_place place;
    bool ok, same = false;
    if (previous) {
        walk.identity = (publish_identity){revision->edits, revision->edit_count, NULL, NULL, 0};
        ok = publish_prepare(&walk.identity, revision->node_count) && publish_matched(&walk, root, previous, &same);
    } else {
        unsigned slot = slot_of(root);
        ok = publish_node(&walk, root, slot, shape_runs(root, (relation_shape)(slot & SLOT_SHAPE)), 0, ++walk.next_id,
                          false, &place) &&
             publish_fresh(&walk, root, (relation_shape)(slot & SLOT_SHAPE), place.start, false);
    }
    if (ok && !same) {
        definition_table *references = &walk.tables[TABLE_REFERENCES];
        value->references = (markdown_core_definitions){0};
        ok = table_seal(&walk.tables[TABLE_FOOTNOTES], &parser->source_order, &value->footnotes) &&
             table_seal(&walk.tables[TABLE_SPECIMENS], &parser->source_order, &value->specimens) &&
             table_nodes(references, &parser->source_order, &value->references.nodes);
        value->references.count = ok ? references->count : 0;
        ok = ok && reference_targets(value, &walk.tables[TABLE_HEADINGS], &parser->source_order);
    }
    /* Nothing below can fail: the result is committed. */
    if (ok && previous) {
        publish_continue(parser, &walk.identity, same);
    }
    if (ok) {
        revision->node_count = (size_t)(walk.next_id - revision->last_id) + walk.matched;
        revision->last_id = walk.next_id;
    }
    for (size_t table = 0; table < TABLE_COUNT; table++) {
        markdown_core_free(walk.tables[table].values);
    }
    markdown_core_free(walk.runs.runs);
    markdown_core_free(walk.images.items);
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

const markdown_core_piece *markdown_core_node_pieces(const markdown_core_node *node, size_t *count) {
    *count = node->pieces ? node->pieces->count : 0;
    return node->pieces ? &node->pieces->items[0].piece : NULL;
}

const markdown_core_run *markdown_core_node_runs(const markdown_core_node *node, size_t *count) {
    const markdown_core_runs *runs = content_runs(node);
    *count = runs ? runs->count : 0;
    return runs ? &runs->items[0].run : NULL;
}

static size_t chain_length(const markdown_core_node *first) {
    size_t count = 0;
    for (; first; first = first->next) {
        count++;
    }
    return count;
}

void markdown_core_walk_begin(markdown_core_walk *walk, const markdown_core_node *root) {
    *walk = (markdown_core_walk){.root = root};
}

void markdown_core_walk_end(markdown_core_walk *walk) {
    markdown_core_free(walk->frames);
    markdown_core_free(walk->runs.runs);
    markdown_core_free(walk->ranges);
    *walk = (markdown_core_walk){.root = walk->root, .failed = walk->failed};
}

/* Where `node` is, given the offset its extent is relative to. */
static markdown_core_place walk_place(const markdown_core_node *node, uint32_t anchor) {
    markdown_core_extent extent = node->where.extent;
    markdown_core_place place;
    place.start = (uint32_t)((int64_t)anchor + extent.lead);
    place.end = place.start + extent.span;
    return place;
}

/* A node whose relations hold nothing takes no frame. An inline root's
 * frame reads its runs, which its first relation's nodes are placed by. */
static bool walk_push(markdown_core_walk *walk, const markdown_core_node *node, size_t level, uint32_t start,
                      bool content) {
    if (relations_empty(node)) {
        return true;
    }
    const markdown_core_runs *runs = content_runs(node);
    if (runs && !markdown_core_source_runs_read(&walk->runs, runs, start)) {
        walk->failed = true;
        return false;
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
    frame->root = runs != NULL;
    frame->content = content || frame->root;
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
        if (relation.group || relation.first != relation.end) {
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
        markdown_core_place place = walk_place(walk->root, 0);
        *item = (markdown_core_walk_item){walk->root, place, NULL, 0, 0, false};
        return walk_push(walk, walk->root, 0, place.start, false);
    }
    while (walk->count) {
        markdown_core_walk_frame *frame = &walk->frames[walk->count - 1];
        if (!frame->active) {
            if (!markdown_core_relations_next(&frame->cursor, &frame->relation)) {
                walk->count--;
                continue;
            }
            /* A root's first relation is its content, which runs from 0. */
            frame->anchor = frame->root ? 0 : frame->owner_start;
            frame->active = true;
            frame->group_pending = frame->relation.group != NULL;
            frame->next = frame->relation.first;
        }
        if (frame->group_pending) {
            frame->group_pending = false;
            *item = (markdown_core_walk_item){
                NULL, {0, 0}, frame->relation.group, markdown_core_relation_count(&frame->relation), frame->level + 1,
                false};
            walk->at_group = true;
            walk->owner = walk->count;
            return true;
        }
        if (frame->next != frame->relation.end) {
            markdown_core_node *node = (markdown_core_node *)frame->next;
            frame->next = node->next;
            markdown_core_place place = walk_place(node, frame->anchor);
            frame->anchor = place.end;
            size_t level = frame->level + (frame->relation.group ? 2 : 1);
            *item = (markdown_core_walk_item){node, place, NULL, 0, level, frame->content};
            walk->at_group = false;
            walk->owner = walk->count;
            return walk_push(walk, node, level, place.start, frame->content);
        }
        frame->active = false;
        /* A root's later relations are not its content. */
        if (frame->root) {
            frame->root = frame->content = false;
        }
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
    return owner->next != owner->relation.end || (!owner->relation.group && walk_more_after(owner));
}

static bool walk_reserve(markdown_core_walk *walk, size_t count) {
    markdown_core_place *ranges = markdown_core_reserve(walk->ranges, &walk->range_capacity, count, sizeof(*ranges));
    if (!ranges) {
        walk->failed = true;
        return false;
    }
    walk->ranges = ranges;
    return true;
}

bool markdown_core_walk_ranges(markdown_core_walk *walk, const markdown_core_walk_item *item,
                               const markdown_core_place **ranges, size_t *count) {
    const markdown_core_pieces *pieces = item->node->pieces;
    if (item->content) {
        size_t needed = markdown_core_source_runs_ranges(&walk->runs, item->place, walk->ranges, walk->range_capacity);
        if (needed > walk->range_capacity) {
            if (!walk_reserve(walk, needed)) {
                return false;
            }
            markdown_core_source_runs_ranges(&walk->runs, item->place, walk->ranges, walk->range_capacity);
        }
        *count = needed;
    } else if (pieces) {
        if (!walk_reserve(walk, pieces->count)) {
            return false;
        }
        int64_t at = item->place.start;
        for (uint32_t i = 0; i < pieces->count; i++) {
            const markdown_core_piece piece = pieces->items[i].piece;
            const uint32_t start = (uint32_t)(at + piece.lead);
            walk->ranges[i] = (markdown_core_place){start, start + piece.span};
            at = start + piece.span;
        }
        *count = pieces->count;
    } else {
        if (!walk_reserve(walk, 1)) {
            return false;
        }
        walk->ranges[0] = item->place;
        *count = 1;
    }
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

const markdown_core_node *markdown_core_node_get_first_child(const markdown_core_node *node) {
    return node->kind != MARKDOWN_CORE_NODE_DEFINITION ? node->first_child : NULL;
}

const markdown_core_node *markdown_core_node_get_next_sibling(const markdown_core_node *node) { return node->next; }

size_t markdown_core_node_child_count(const markdown_core_node *node) {
    return chain_length(markdown_core_node_get_first_child(node));
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

markdown_core_status markdown_core_node_definition_term(const markdown_core_node *node,
                                                        const markdown_core_node **term) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DEFINITION));
    *term = node->as.definition->term->first_child;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_node_definition_bodies(const markdown_core_node *node,
                                                          const markdown_core_definition_body **bodies) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DEFINITION));
    *bodies = (const markdown_core_definition_body *)node->first_child;
    return MARKDOWN_CORE_OK;
}

const markdown_core_definition_body *markdown_core_definition_body_next(const markdown_core_definition_body *body) {
    return (const markdown_core_definition_body *)((const markdown_core_node *)body)->next;
}

const markdown_core_node *markdown_core_definition_body_content(const markdown_core_definition_body *body) {
    return ((const markdown_core_node *)body)->first_child;
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

markdown_core_status markdown_core_node_callout_title(const markdown_core_node *node,
                                                      const markdown_core_node **title) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_CALLOUT));
    *title = node->as.callout->title ? node->as.callout->title->first_child : NULL;
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

markdown_core_status markdown_core_node_cite_citations(const markdown_core_node *node,
                                                       const markdown_core_node **citations) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_CITE));
    *citations = node->as.cite->citations;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_citation_referent(const markdown_core_node *citation,
                                                     markdown_core_referent *referent) {
    REQUIRE_KIND(citation, KIND_BIT(MARKDOWN_CORE_KIND_CITATION));
    *referent = citation_referent(citation);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_citation_prefix(const markdown_core_node *citation,
                                                   const markdown_core_node **prefix) {
    REQUIRE_KIND(citation, KIND_BIT(MARKDOWN_CORE_KIND_CITATION));
    *prefix = citation->as.citation->prefix ? citation->as.citation->prefix->first_child : NULL;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_citation_suffix(const markdown_core_node *citation,
                                                   const markdown_core_node **suffix) {
    REQUIRE_KIND(citation, KIND_BIT(MARKDOWN_CORE_KIND_CITATION));
    *suffix = citation->as.citation->suffix ? citation->as.citation->suffix->first_child : NULL;
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
static const markdown_core_node *definition_for(const markdown_core_node *const *labeled, size_t count,
                                                markdown_core_string label) {
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
    if (lo == count) {
        return NULL;
    }
    definition_label(labeled[lo], &candidate);
    return label_compare(candidate.data, (size_t)candidate.len, label.data, label.length) == 0 ? labeled[lo] : NULL;
}

const markdown_core_node *markdown_core_document_footnote_for(const markdown_core_document *document,
                                                              markdown_core_string label) {
    return definition_for(document_footnotes(document)->labeled, document_footnotes(document)->labeled_count, label);
}

const markdown_core_node *markdown_core_document_specimen_for(const markdown_core_document *document,
                                                              markdown_core_string label) {
    return definition_for(document_specimens(document)->labeled, document_specimens(document)->labeled_count, label);
}

markdown_core_status markdown_core_footnote_label(const markdown_core_node *footnote,
                                                  markdown_core_optional_string *label) {
    REQUIRE_KIND(footnote, KIND_BIT(MARKDOWN_CORE_KIND_FOOTNOTE));
    *label = optional_chunk_string(footnote->as.footnote->label);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_footnote_content(const markdown_core_node *footnote,
                                                    const markdown_core_node **content) {
    REQUIRE_KIND(footnote, KIND_BIT(MARKDOWN_CORE_KIND_FOOTNOTE));
    *content = footnote->first_child;
    return MARKDOWN_CORE_OK;
}

size_t markdown_core_document_reference_count(const markdown_core_document *document) {
    return document->root->as.document->references.count;
}

markdown_core_status markdown_core_document_reference_at(const markdown_core_document *document, size_t index,
                                                         const markdown_core_node **reference) {
    return definition_at(&document->root->as.document->references, index, reference);
}

const markdown_core_node *markdown_core_document_reference_for(const markdown_core_document *document,
                                                               markdown_core_string label) {
    const markdown_core_document_value *value = document->root->as.document;
    return definition_for(value->reference_targets, value->reference_target_count, label);
}

size_t markdown_core_document_reference_label_count(const markdown_core_document *document) {
    return document->root->as.document->reference_target_count;
}

markdown_core_status markdown_core_document_reference_label_at(const markdown_core_document *document, size_t index,
                                                               markdown_core_string *label,
                                                               const markdown_core_node **target) {
    const markdown_core_document_value *value = document->root->as.document;
    if (index >= value->reference_target_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    markdown_core_chunk chunk;
    definition_label(value->reference_targets[index], &chunk);
    *label = chunk_string(chunk);
    *target = value->reference_targets[index];
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

markdown_core_status markdown_core_specimen_content(const markdown_core_node *specimen,
                                                    const markdown_core_node **content) {
    REQUIRE_KIND(specimen, KIND_BIT(MARKDOWN_CORE_KIND_SPECIMEN));
    *content = specimen->first_child;
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
    /* `children` counts structural children: a cite's are its items and a
     * definition's its bodies. */
    size_t child_count = kind == MARKDOWN_CORE_KIND_CITE         ? chain_length(node->as.cite->citations)
                         : kind == MARKDOWN_CORE_KIND_DEFINITION ? chain_length(node->first_child)
                                                                 : markdown_core_node_child_count(node);
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
