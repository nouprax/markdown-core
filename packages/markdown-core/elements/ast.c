#include <assert.h>
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

static uint64_t definition_key(const void *entry) { return ((const markdown_core_definition_entry *)entry)->start; }

static bool table_add(markdown_core_definition_table *table, const markdown_core_node *node, uint32_t start) {
    if (table->count == table->capacity) {
        size_t capacity = table->capacity ? table->capacity * 2 : 8;
        if (capacity > SIZE_MAX / sizeof(*table->values)) {
            return false;
        }
        markdown_core_definition_entry *values = markdown_core_realloc(table->values, capacity * sizeof(*values));
        if (!values) {
            return false;
        }
        table->values = values;
        table->capacity = capacity;
    }
    table->values[table->count++] = (markdown_core_definition_entry){start, node};
    return true;
}

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
    const size_t bytes = length < 8 ? length : 8;
    uint64_t head = 0;
    for (size_t i = 0; i < bytes; i++) {
        head = head << 8 | label[i];
    }
    /* The padding: a shift by 64 is undefined, and no bytes is a zero head. */
    return (label_entry){bytes ? head << 8 * (8 - bytes) : 0, label, length, node};
}

/* The labels of two entries with equal heads in byte order: a label no
 * longer than eight bytes is all in its head, so it is a prefix of the other
 * label, and the shorter is first. */
static int label_tie_compare(const label_entry *a, const label_entry *b) {
    if (a->length <= 8 || b->length <= 8) {
        return (a->length > b->length) - (a->length < b->length);
    }
    return label_compare(a->label + 8, a->length - 8, b->label + 8, b->length - 8);
}

static inline bool label_before(const label_entry *a, const label_entry *b) {
    return a->head != b->head ? a->head < b->head : label_tie_compare(a, b) < 0;
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

/* Whether two entries hold the same label. */
static inline bool label_same(const label_entry *a, const label_entry *b) {
    return a->head == b->head && label_tie_compare(a, b) == 0;
}

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

/* The table's nodes in source order, as node handles the document borrows,
 * into `*nodes`. */
static bool table_nodes(markdown_core_parser *parser, markdown_core_definition_table *table,
                        const markdown_core_node ***nodes) {
    *nodes = NULL;
    if (!table->count) {
        return true;
    }
    if (!markdown_core_order_source_entries(&parser->source_order, table->values, table->count, sizeof(*table->values),
                                            definition_key)) {
        return false;
    }
    /* The handles take the entries' place: each is smaller than an entry, so
     * the one written lands on entries already read. Byte copies, as the
     * storage changes what it holds. The document owns it from here. */
    unsigned char *storage = (unsigned char *)table->values;
    for (size_t i = 0; i < table->count; i++) {
        const markdown_core_node *node;
        memcpy(&node,
               storage + i * sizeof(markdown_core_definition_entry) + offsetof(markdown_core_definition_entry, node),
               sizeof(node));
        memcpy(storage + i * sizeof(node), &node, sizeof(node));
    }
    *nodes = (const markdown_core_node **)storage;
    table->values = NULL;
    table->capacity = 0;
    return true;
}

/* The table in source order, and its labeled definitions in label order, as
 * node handles the document borrows. */
static bool table_seal(markdown_core_parser *parser, markdown_core_definition_table *table,
                       markdown_core_definitions *out) {
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
            entries[labels++] = label_entry_of(label.data, (size_t)label.len, nodes[i]);
        }
    }
    const label_entry *sorted = label_order(entries, labels, table->count);
    for (size_t i = 0; i < labels; i++) {
        labeled[i] = sorted[i].node;
    }
    markdown_core_free(entries);
    *out = (markdown_core_definitions){nodes, table->count, labeled, labels};
    return true;
}

/* A relation of the `count` nodes of `stem` from `index` on. */
static bool relation_run(markdown_core_relation *relation, const char *group, markdown_core_field name,
                         const markdown_core_stem *stem, size_t index, size_t count) {
    *relation = (markdown_core_relation){group, stem, index, count, NULL, name, 0, false};
    return true;
}

/* A relation of all the nodes of `stem`. */
static bool relation_stem(markdown_core_relation *relation, const char *group, markdown_core_field name,
                          const markdown_core_stem *stem) {
    return relation_run(relation, group, name, stem, 0, markdown_core_stem_count(stem));
}

static bool relation_one(markdown_core_relation *relation, markdown_core_field name, const markdown_core_node *node) {
    *relation = (markdown_core_relation){NULL, NULL, 0, 1, node, name, 0, true};
    return true;
}

void markdown_core_relation_walk_begin(markdown_core_relation_walk *walk, const markdown_core_relation *relation) {
    walk->node = relation->node;
    markdown_core_stem_walk_begin(&walk->stem, relation->stem, relation->index, relation->node ? 0 : relation->count);
}

const markdown_core_node *markdown_core_relation_walk_next(markdown_core_relation_walk *walk) {
    const markdown_core_node *node = walk->node;
    if (node) {
        walk->node = NULL;
        return node;
    }
    return markdown_core_stem_walk_next(&walk->stem);
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
typedef char
    shapes_fit_their_slot[SHAPE_DEFINITION <= SLOT_SHAPE && SLOT_LOOKUP(MARKDOWN_CORE_TABLE_COUNT) <= 0xff ? 1 : -1];

static const uint8_t kind_slots[SHAPE_SLOTS] = {
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DOCUMENT)] = SHAPE_DOCUMENT,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_TABLE)] = SHAPE_TABLE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DIRECTIVE)] = SHAPE_DIRECTIVE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK)] = SHAPE_DIRECTIVE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_CALLOUT)] = SHAPE_CALLOUT,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_CITE)] = SHAPE_CITE,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_CITATION)] = SHAPE_CITATION,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_DEFINITION)] = SHAPE_DEFINITION,
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_FOOTNOTE)] = SHAPE_CHILDREN | SLOT_LOOKUP(MARKDOWN_CORE_TABLE_FOOTNOTES),
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_SPECIMEN)] = SHAPE_CHILDREN | SLOT_LOOKUP(MARKDOWN_CORE_TABLE_SPECIMENS),
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_REFERENCE)] = SHAPE_CHILDREN | SLOT_LOOKUP(MARKDOWN_CORE_TABLE_REFERENCES),
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_HEADING)] = SHAPE_CHILDREN | SLOT_LOOKUP(MARKDOWN_CORE_TABLE_HEADINGS),
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
    return shape == SHAPE_CHILDREN ? !node->children : fields_empty(node, shape);
}

/* The same question for a kind that owns fields besides its children. */
static bool fields_empty(const markdown_core_node *node, relation_shape shape) {
    switch (shape) {
    case SHAPE_CHILDREN:
    case SHAPE_CITE:
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

/* The field a kind's children are: a DefinitionList's definitions and a
 * TableRow's cells, and every other kind's content. */
static inline markdown_core_field children_field(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_DEFINITION_LIST ? MARKDOWN_CORE_FIELD_DEFINITIONS
           : node->kind == MARKDOWN_CORE_NODE_TABLE_ROW     ? MARKDOWN_CORE_FIELD_CELLS
                                                            : MARKDOWN_CORE_FIELD_CONTENT;
}

void markdown_core_relations_begin(markdown_core_relation_cursor *cursor, const markdown_core_node *owner) {
    *cursor = (markdown_core_relation_cursor){owner, (uint8_t)shape_of(owner), 0, 0};
}

/* Each kind's relations in canonical field order. An optional field that is
 * absent yields no relation; a group the dump always draws yields one even
 * when it is empty. `more` says whether stepping again may find another. */
static inline bool relations_next(markdown_core_relation_cursor *cursor, markdown_core_relation *relation, bool *more) {
    static const char *const table_groups[] = {"TableHead", "TableBody", "TableFoot"};
    static const markdown_core_field table_fields[] = {MARKDOWN_CORE_FIELD_HEAD, MARKDOWN_CORE_FIELD_CONTENT,
                                                       MARKDOWN_CORE_FIELD_FOOT};
    const markdown_core_node *node = cursor->owner;
    int step = cursor->step++;
    /* An absent optional field steps on to the next relation at once. */
    switch ((relation_shape)cursor->shape) {
    case SHAPE_DOCUMENT:
        if (step == 0) {
            if (node->as.document->metadata) {
                *more = true;
                return relation_one(relation, MARKDOWN_CORE_FIELD_METADATA, node->as.document->metadata);
            }
            step = cursor->step++;
        }
        *more = false;
        return step == 1 && relation_stem(relation, NULL, MARKDOWN_CORE_FIELD_CONTENT, node->children);
    case SHAPE_TABLE: {
        const markdown_core_table *table = node->opaque;
        if (step == 0) {
            if (table->caption) {
                *more = true;
                return relation_one(relation, MARKDOWN_CORE_FIELD_CAPTION, table->caption);
            }
            step = cursor->step++;
        }
        if (step > 3) {
            return false;
        }
        size_t count = step == 1 ? table->head_count : step == 2 ? table->content_count : table->foot_count;
        size_t left = markdown_core_stem_count(node->children) - cursor->next;
        count = count < left ? count : left;
        relation_run(relation, table_groups[step - 1], table_fields[step - 1], node->children, cursor->next, count);
        cursor->next += count;
        *more = step < 3;
        return true;
    }
    case SHAPE_DIRECTIVE:
        if (step == 0) {
            const markdown_core_node *label = markdown_core_directive_label(node);
            if (label) {
                *more = true;
                return relation_one(relation, MARKDOWN_CORE_FIELD_LABEL, label);
            }
            step = cursor->step++;
        }
        *more = false;
        return step == 1 && relation_stem(relation, NULL, MARKDOWN_CORE_FIELD_CONTENT, node->children);
    case SHAPE_CALLOUT:
        if (step == 0) {
            if (node->as.callout->title) {
                *more = true;
                return relation_stem(relation, "Title", MARKDOWN_CORE_FIELD_TITLE, node->as.callout->title->children);
            }
            step = cursor->step++;
        }
        *more = false;
        return step == 1 && relation_stem(relation, NULL, MARKDOWN_CORE_FIELD_CONTENT, node->children);
    case SHAPE_CITE:
        *more = false;
        return step == 0 && relation_stem(relation, NULL, MARKDOWN_CORE_FIELD_CITATIONS, node->children);
    case SHAPE_CITATION: {
        const markdown_core_citation_item *citation = node->as.citation;
        *more = true;
        if (step == 0) {
            if (citation->note) {
                return relation_one(relation, MARKDOWN_CORE_FIELD_NOTE, citation->note);
            }
            step = cursor->step++;
        }
        if (step == 1) {
            return relation_stem(relation, "CitationPrefix", MARKDOWN_CORE_FIELD_PREFIX,
                                 citation->prefix ? citation->prefix->children : NULL);
        }
        *more = false;
        return step == 2 && relation_stem(relation, "CitationSuffix", MARKDOWN_CORE_FIELD_SUFFIX,
                                          citation->suffix ? citation->suffix->children : NULL);
    }
    case SHAPE_DEFINITION: {
        const size_t bodies = markdown_core_stem_count(node->children);
        if (step == 0) {
            *more = bodies > 0;
            return relation_stem(relation, "DefinitionTerm", MARKDOWN_CORE_FIELD_TERM,
                                 node->as.definition->term->children);
        }
        if (cursor->next == bodies) {
            return false;
        }
        const markdown_core_node *body = markdown_core_stem_at(node->children, cursor->next++);
        *more = cursor->next < bodies;
        relation_stem(relation, "DefinitionBody", MARKDOWN_CORE_FIELD_CONTENT, body->children);
        relation->list = (uint32_t)(step - 1);
        return true;
    }
    case SHAPE_CHILDREN:
        *more = false;
        return step == 0 && relation_stem(relation, NULL, children_field(node), node->children);
    }
    return false;
}

bool markdown_core_relations_next(markdown_core_relation_cursor *cursor, markdown_core_relation *relation) {
    bool more;
    return relations_next(cursor, relation, &more);
}

/* WHERE A NODE'S RUNS ARE HELD: by the node itself, or by the private node
 * a callout's title or a definition's term hangs from, whose runs are the
 * callout's or the definition's; NULL for a callout without a title. */
static inline markdown_core_runs **shape_runs_at(const markdown_core_node *node, relation_shape shape) {
    switch (shape) {
    case SHAPE_CALLOUT:
        return node->as.callout->title ? &node->as.callout->title->runs : NULL;
    case SHAPE_DEFINITION:
        return &node->as.definition->term->runs;
    default:
        return (markdown_core_runs **)&node->runs;
    }
}

static inline markdown_core_runs *shape_runs(const markdown_core_node *node, relation_shape shape) {
    markdown_core_runs **runs = shape_runs_at(node, shape);
    return runs ? *runs : NULL;
}

/* THE RUNS OF A NODE'S INLINE CONTENT, which is its first relation when it
 * is an inline root's: its runs when they read content, and NULL otherwise.
 * Its first relation's places are offsets in that content, and every place
 * below them is too. Roots never nest. */
static markdown_core_runs *content_runs(const markdown_core_node *node) {
    markdown_core_runs *runs = shape_runs(node, shape_of(node));
    return runs && runs->decoded ? runs : NULL;
}

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
        const uint32_t start = (uint32_t)(at + run.source.lead);
        table->runs[i] = (markdown_core_source_run){content, run.decoded, start, start + run.source.span};
        content += run.decoded;
        at = start + run.source.span;
    }
    table->count = runs->count;
    return true;
}

/* The content run content offset `offset` is in: the last that starts at or
 * before it and reads content, or the first run. */
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
    while (lo > 0 && !table->runs[lo].decoded) {
        lo--;
    }
    return lo;
}

/* Whether the run reads each content byte from one source byte. */
static inline bool source_run_copied(const markdown_core_source_run *run) {
    return run->end - run->start == run->decoded;
}

/* Where content offset `offset` is read from: its source byte, the start of
 * the run that reads it whole, or, past the content, where the content
 * ends. */
static uint32_t source_run_place(const markdown_core_source_runs *table, uint32_t offset) {
    const markdown_core_source_run *run = &table->runs[source_run_at(table, offset)];
    if (offset >= run->content + run->decoded) {
        return run->end;
    }
    return source_run_copied(run) ? run->start + (offset - run->content) : run->start;
}

/* Where the content byte before `offset` is read to: past its source byte,
 * or the end of the run that reads it whole. */
static uint32_t source_run_end(const markdown_core_source_runs *table, uint32_t offset) {
    const markdown_core_source_run *run = &table->runs[source_run_at(table, offset - 1)];
    if (offset - 1 >= run->content + run->decoded || !source_run_copied(run)) {
        return run->end;
    }
    return run->start + (offset - run->content);
}

markdown_core_place markdown_core_source_runs_window(const markdown_core_source_runs *table,
                                                     markdown_core_place place) {
    const uint32_t start = source_run_place(table, place.start);
    return (markdown_core_place){start, place.end > place.start ? source_run_end(table, place.end) : start};
}

size_t markdown_core_source_runs_ranges(const markdown_core_source_runs *table, markdown_core_place window,
                                        markdown_core_place *ranges, size_t capacity) {
    if (window.end <= window.start) {
        if (capacity) {
            ranges[0] = window;
        }
        return 1;
    }
    /* The first run that ends past the window's start; the gaps from there
     * that lie in the window cut it. */
    size_t lo = 0, hi = table->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (table->runs[mid].end <= window.start) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    size_t count = 0;
    uint32_t from = window.start;
    for (size_t i = lo; i + 1 < table->count && table->runs[i].end < window.end; i++) {
        const uint32_t gap = table->runs[i].end, past = table->runs[i + 1].start;
        if (past <= gap) {
            continue;
        }
        if (gap > from) {
            if (count < capacity) {
                ranges[count] = (markdown_core_place){from, gap};
            }
            count++;
        }
        from = past > from ? past : from;
    }
    if (from < window.end) {
        if (count < capacity) {
            ranges[count] = (markdown_core_place){from, window.end};
        }
        count++;
    }
    return count;
}

/* A node's runs as they are published (node.h): each run that decodes
 * nothing within its place, those that touch joined, and those at either end
 * without a gap beside them dropped, the rest measured from the
 * end of the run before, or from the node's start. A node that has no runs
 * left has none. */
static void publish_runs(markdown_core_node_pool *pool, markdown_core_runs **at, markdown_core_place place) {
    markdown_core_runs *runs = *at;
    markdown_core_run_where *items = runs->items;
    uint32_t count = 0;
    for (uint32_t i = 0; i < runs->count; i++) {
        markdown_core_run_place run = items[i].place;
        if (!run.decoded) {
            run.start = run.start > place.start ? run.start : place.start;
            run.end = run.end < place.end ? run.end : place.end;
            if (run.start >= run.end) {
                continue;
            }
            if (count && !items[count - 1].place.decoded && items[count - 1].place.end == run.start) {
                items[count - 1].place.end = run.end;
                continue;
            }
        }
        items[count++].place = run;
    }
    uint32_t first = 0;
    while (first < count && !items[first].place.decoded &&
           (first + 1 == count || items[first].place.end == items[first + 1].place.start)) {
        first++;
    }
    while (count > first && !items[count - 1].place.decoded &&
           (count - 1 == first || items[count - 2].place.end == items[count - 1].place.start)) {
        count--;
    }
    if (first == count) {
        markdown_core_node_pool_bytes_free(pool, runs);
        *at = NULL;
        return;
    }
    uint32_t anchor = place.start;
    for (uint32_t i = first; i < count; i++) {
        const markdown_core_run_place run = items[i].place;
        items[i - first].run =
            (markdown_core_run){{(int32_t)((int64_t)run.start - anchor), run.end - run.start}, run.decoded};
        anchor = run.end;
    }
    runs->count = count - first;
}

/* Where `node`, whose extent is measured from `anchor`, lies: in the content
 * of the root whose runs the walk holds when it is in one. */
static inline markdown_core_place publish_place(const markdown_core_node *node, uint32_t anchor) {
    const uint32_t start = (uint32_t)((int64_t)anchor + node->where.extent.lead);
    return (markdown_core_place){start, start + node->where.extent.span};
}

static inline markdown_core_node_kind public_kind(const markdown_core_node *node);

/* COMPLETION (docs/plans/2026-09-29-incremental-parsing.md, 5.8, 5.9). A
 * node is complete when it is made: as it closes, or, for the nodes of an
 * inline root's content, as the root's completion leaves them. Completing a
 * node numbers each node it holds that is not numbered yet, in canonical
 * field order -- a node that gains one later, as a table gains a trailing
 * caption, completes again. Numbering a node passes the reuse cursor over
 * its range, collecting the old nodes it may continue. A node continues the first of
 * those old nodes of its kind, which gives it its id, or none, and takes the
 * next id; it decides as soon as that cannot change (number_id), at the latest
 * as it settles, once it waits on nothing, its kind and range final. Equal to
 * the old node it continues, it is that node. A descendant that asks which
 * old node a node continues has it decided then. */

/* The node a node's inline content is parsed into: the node itself, or the
 * private node its title or term hangs from, which holds the runs that read
 * it (shape_runs_at); NULL for a callout without a title. */
static markdown_core_node *content_holder(markdown_core_node *node, relation_shape shape) {
    switch (shape) {
    case SHAPE_CALLOUT:
        return node->as.callout->title;
    case SHAPE_DEFINITION:
        return node->as.definition->term;
    default:
        return node;
    }
}

/* Reads runs that still hold absolute source ranges (node.h) into `table`,
 * whose storage it reuses. False when it could not grow. */
static bool source_runs_read_places(markdown_core_source_runs *table, const markdown_core_runs *runs) {
    markdown_core_source_run *grown =
        markdown_core_reserve(table->runs, &table->capacity, runs->count, sizeof(*table->runs));
    if (!grown) {
        return false;
    }
    table->runs = grown;
    uint32_t content = 0;
    for (uint32_t i = 0; i < runs->count; i++) {
        const markdown_core_run_place run = runs->items[i].place;
        table->runs[i] = (markdown_core_source_run){content, run.decoded, run.start, run.end};
        content += run.decoded;
    }
    table->count = runs->count;
    return true;
}

/* The runs of the root being completed, still in absolute offsets, in the
 * publication's `runs`; NULL when it has none, or, with `*failed`, when they
 * could not be read. */
static const markdown_core_source_runs *completing_runs(markdown_core_parser *parser,
                                                        markdown_core_publication *publication, bool *failed) {
    const markdown_core_inline_root *root = parser->completing;
    const markdown_core_runs *runs = shape_runs(root->node, shape_of(root->node));
    *failed = false;
    if (!runs || !runs->count) {
        return NULL;
    }
    if (publication->runs_root != root) {
        if (!source_runs_read_places(&publication->runs, runs)) {
            *failed = true;
            return NULL;
        }
        publication->runs_root = root;
    }
    return &publication->runs;
}

/* Where a definition at content offset `offset` of the root being completed
 * was written. False when the root's runs could not be read. */
static bool completing_source(markdown_core_parser *parser, markdown_core_publication *publication, uint32_t offset,
                              uint32_t *source) {
    bool failed;
    const markdown_core_source_runs *runs = completing_runs(parser, publication, &failed);
    *source = runs ? source_run_place(runs, offset) : parser->completing->place.start;
    return !failed;
}

/* THE REUSE CURSOR (5.9). Every member that is numbered asks for the old node
 * its node continues. The root continues the previous root. Within the
 * relation of an owner that continues an old node, an old node's anchor is
 * the first byte of its range that survived the edits, and a node continues
 * the earliest old sibling of its kind whose anchor's image its range holds.
 * Both relations are in source order, so each owner keeps one cursor into its
 * old node's relation, moving forward as the members it holds ask in order:
 * a member that asks first has every member before it ask. The old nodes the
 * cursor passes are passed for good, since every later member starts where
 * the one asking ends. A group of the owner's -- a title, a term, a citation's
 * affixes, a definition's bodies -- continues the old owner's group in the
 * same field, or at the same place among its bodies.
 *
 * A member asks before its range is known when a member it holds asks: it
 * then searches only the old nodes whose images lie before where the one
 * below ends, `reach`, since no later one can hold the old node below's
 * owner. It finds the same old node the whole range would, and asks again as
 * far as it needs. */

static bool content_image_add(markdown_core_publication *publication, markdown_core_content_image image) {
    markdown_core_content_image *grown = markdown_core_reserve(publication->images, &publication->image_capacity,
                                                               publication->image_count + 1, sizeof(*grown));
    if (!grown) {
        return false;
    }
    publication->images = grown;
    publication->images[publication->image_count++] = image;
    return true;
}

/* Reads the images of the content of the old root whose runs are `runs`,
 * measured from `origin`, against the new root's runs `fresh`. The old runs,
 * the edits and the new runs all increase in source, so one pass over each
 * pairs them. False when the table could not grow. */
static bool content_images_read(const markdown_core_parser *parser, markdown_core_publication *publication,
                                const markdown_core_runs *runs, uint32_t origin,
                                const markdown_core_source_runs *fresh) {
    const markdown_core_revision *revision = parser->revision;
    const markdown_core_byte_edit *edits = revision->edits;
    publication->image_count = publication->hint = 0;
    if (!runs || !runs->count || !fresh) {
        return true;
    }
    int64_t at = origin;
    uint32_t content = 0;
    size_t edit = markdown_core_parser_edit_after(parser, (uint32_t)(at + runs->items[0].run.source.lead)), next = 0;
    for (uint32_t i = 0; i < runs->count; i++) {
        const markdown_core_run run = runs->items[i].run;
        const uint32_t start = (uint32_t)(at + run.source.lead), end = start + run.source.span, from = content;
        const bool copied = run.source.span == run.decoded;
        at = end;
        content += run.decoded;
        if (!run.decoded) {
            continue;
        }
        /* Each stretch of the run's source that no edit replaced, and the
         * parts of its image the new runs read. */
        for (uint32_t x = start; x < end;) {
            while (edit < revision->edit_count && edits[edit].end <= x) {
                edit++;
            }
            if (edit < revision->edit_count && edits[edit].start <= x) {
                x = (uint32_t)edits[edit].end;
                continue;
            }
            const uint32_t stop =
                edit < revision->edit_count && edits[edit].start < end ? (uint32_t)edits[edit].start : end;
            const int64_t shift = parser->edit_shift[edit];
            const uint32_t low = (uint32_t)(x + shift), high = (uint32_t)(stop + shift);
            while (next < fresh->count && fresh->runs[next].end <= low) {
                next++;
            }
            for (size_t j = next; j < fresh->count && fresh->runs[j].start < high; j++) {
                const markdown_core_source_run *read = &fresh->runs[j];
                if (!read->decoded) {
                    continue;
                }
                const uint32_t first = low > read->start ? low : read->start;
                const uint32_t last = high < read->end ? high : read->end;
                if (first >= last) {
                    continue;
                }
                const bool follows = source_run_copied(read);
                const uint32_t image = follows ? read->content + (first - read->start) : read->content;
                if (!copied) {
                    if (!content_image_add(publication, (markdown_core_content_image){from, content, image, false})) {
                        return false;
                    }
                    x = end;
                    break;
                }
                const uint32_t old_first = from + (uint32_t)(first - shift - start);
                if (!content_image_add(publication, (markdown_core_content_image){old_first, old_first + (last - first),
                                                                                  image, follows})) {
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

/* The images of the old content of the root being completed, read when an
 * old node in it first asks. False when they could not be read. */
static bool images_ready(markdown_core_parser *parser, markdown_core_publication *publication) {
    const markdown_core_inline_root *root = parser->completing;
    if (publication->images_root == root) {
        return true;
    }
    const markdown_core_member *member = root->member;
    bool failed;
    const markdown_core_source_runs *fresh = completing_runs(parser, publication, &failed);
    if (failed || !content_images_read(parser, publication, shape_runs(member->old, shape_of(member->old)),
                                       member->old_start, fresh)) {
        return false;
    }
    publication->images_root = root;
    return true;
}

/* The image of an old content range [start, end): the content offset at
 * which the new root reads the first source byte that the old root read for
 * the range, that no edit replaced, and that the new root reads too (5.2).
 * False when there is none. Members ask in content order, so the lookup
 * steps on from where the last one ended. */
static bool content_anchor(markdown_core_publication *publication, uint32_t start, uint32_t end, uint32_t *image) {
    const markdown_core_content_image *items = publication->images;
    size_t i = publication->hint;
    if (i && items[i - 1].to > start) {
        size_t lo = 0, hi = i;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (items[mid].to <= start) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        i = lo;
    }
    while (i < publication->image_count && items[i].to <= start) {
        i++;
    }
    publication->hint = i;
    if (i == publication->image_count || items[i].from >= end) {
        return false;
    }
    *image = items[i].at + (items[i].copied && start > items[i].from ? start - items[i].from : 0);
    return true;
}

/* What completing one node shares with the nodes it holds. */
typedef struct {
    markdown_core_parser *parser;
    markdown_core_publication *publication;
    void (*observe)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node *);
    const markdown_core_element_instance *observer;
} complete_context;

/* The relation of `owner`'s that the member at `index` among the members it
 * holds as children is in: a table's rows are in its head, body or foot by
 * their place, and every other kind's children are its one children
 * relation. */
static void child_relation(const markdown_core_node *owner, uint32_t index, markdown_core_field *name) {
    switch (shape_of(owner)) {
    case SHAPE_TABLE: {
        const markdown_core_table *table = owner->opaque;
        *name = index < table->head_count                          ? MARKDOWN_CORE_FIELD_HEAD
                : index < table->head_count + table->content_count ? MARKDOWN_CORE_FIELD_CONTENT
                                                                   : MARKDOWN_CORE_FIELD_FOOT;
        return;
    }
    case SHAPE_CITE:
        *name = MARKDOWN_CORE_FIELD_CITATIONS;
        return;
    default:
        *name = children_field(owner);
        return;
    }
}

/* Points `owner`'s cursor at its old node's relation `name`: the relation's
 * first old node is measured from the old node's start, or from 0 when the
 * relation is the content of the root being completed. */
static void pair_open(const markdown_core_parser *parser, markdown_core_member *owner, markdown_core_field name) {
    const markdown_core_inline_root *root = parser->completing;
    owner->paired = true;
    owner->pair_name = (uint32_t)name;
    owner->pair_stem = NULL;
    owner->pair_next = owner->pair_end = 0;
    owner->pair_anchor = root && root->builder == owner ? 0 : owner->old_start;
    markdown_core_relation_cursor cursor;
    markdown_core_relation relation;
    bool more;
    markdown_core_relations_begin(&cursor, owner->old);
    while (relations_next(&cursor, &relation, &more)) {
        if (!relation.field && relation.name == name) {
            owner->pair_stem = relation.stem;
            owner->pair_next = relation.index;
            owner->pair_end = relation.index + relation.count;
            return;
        }
    }
}

/* Where an old node beside `member`, starting at `start` and ending at `end`
 * in its old coordinates, lies now: the image of its anchor, in the content
 * of the root being completed when `member`'s owner is inner, else in the
 * source. False when no byte of it survives, or, with `*failed`, when the
 * images could not be read. */
static bool old_image(markdown_core_parser *parser, markdown_core_publication *publication,
                      const markdown_core_member *member, uint32_t start, uint32_t end, uint32_t *image, bool *failed) {
    if (!member->owner->inner) {
        return markdown_core_parser_source_anchor(parser, start, end, image);
    }
    if (!images_ready(parser, publication)) {
        *failed = true;
        return false;
    }
    return content_anchor(publication, start, end, image);
}

/* Whether `member`'s range is final: numbered, or a closed block's. */
static inline bool range_final(const markdown_core_member *member) {
    return member->numbered || !(member->node->flags & MARKDOWN_CORE_NODE__OPEN);
}

/* `member`, a child of `owner`'s, passes `owner`'s cursor up to `reach`: it
 * passes every old node whose image lies before `reach`, and holds those
 * whose image lies in its range as its candidates, in their order. A child
 * passes its whole range before the next one asks, as the old nodes are in
 * its order. False when an allocation failed. */
static bool pair_search(markdown_core_parser *parser, markdown_core_publication *publication,
                        markdown_core_member *owner, markdown_core_member *member, uint32_t reach) {
    markdown_core_field name;
    child_relation(owner->node, member->index, &name);
    if (!owner->paired || owner->pair_name != (uint32_t)name) {
        pair_open(parser, owner, name);
    }
    const uint32_t from = markdown_core_member_place(member).start;
    bool failed = false;
    while (owner->pair_next < owner->pair_end) {
        const markdown_core_node *old = markdown_core_stem_at(owner->pair_stem, owner->pair_next);
        const uint32_t start = (uint32_t)((int64_t)owner->pair_anchor + old->where.extent.lead);
        const uint32_t end = start + old->where.extent.span;
        uint32_t image;
        const bool anchored = old_image(parser, publication, member, start, end, &image, &failed);
        if (failed) {
            return false;
        }
        if (anchored && image >= reach) {
            break;
        }
        owner->pair_next++;
        owner->pair_anchor = end;
        if (anchored && image >= from) {
            markdown_core_candidate *candidates =
                markdown_core_reserve(publication->candidates, &publication->candidate_capacity,
                                      publication->candidate_count + 1, sizeof(*candidates));
            if (!candidates) {
                return false;
            }
            publication->candidates = candidates;
            candidates[publication->candidate_count++] = (markdown_core_candidate){old, start, 0};
            const uint32_t taken = (uint32_t)publication->candidate_count;
            if (member->last_candidate) {
                candidates[member->last_candidate - 1].next = taken;
            } else {
                member->candidates = taken;
            }
            member->last_candidate = taken;
        }
    }
    member->passed = reach;
    return true;
}

/* Whether `member`'s range is final and `reach` passes all of it. */
static bool passed_whole(const markdown_core_member *member, uint32_t reach) {
    return range_final(member) && reach >= markdown_core_member_place(member).end;
}

/* `member`, a child of a node that continues an old node, continues its first
 * candidate of its kind. With none, it continues nothing once `whole`, its
 * whole range passed; until then it is undecided. Its kind is final when its
 * subtree is complete, so a child no request decides earlier decides when it
 * settles. */
static void choose(const markdown_core_publication *publication, markdown_core_member *member, bool whole) {
    const markdown_core_node_kind kind = public_kind(member->node);
    for (uint32_t at = member->candidates; at; at = publication->candidates[at - 1].next) {
        const markdown_core_candidate *candidate = &publication->candidates[at - 1];
        if (public_kind(candidate->old) == kind) {
            member->old = candidate->old;
            member->old_start = candidate->start;
            member->decided = true;
            return;
        }
    }
    member->decided = whole;
}

/* The field slot `node` fills among `owner`'s fields, counted in canonical
 * order; the slot of that place among another node's of the kind. */
typedef struct {
    const markdown_core_node *node;
    size_t place, at;
    markdown_core_node **slot;
} field_find;

static int field_find_visit(markdown_core_node **slot, void *context) {
    field_find *find = context;
    if (find->node ? *slot == find->node : find->at == find->place) {
        find->slot = slot;
        return 0;
    }
    find->at++;
    return 1;
}

static markdown_core_node **field_slot(markdown_core_node *owner, const markdown_core_node *node, size_t *place) {
    field_find find = {node, 0, 0, NULL};
    markdown_core_node_visit_fields(owner, field_find_visit, &find);
    *place = find.at;
    return find.slot;
}

static markdown_core_node *field_at(const markdown_core_node *owner, size_t place) {
    field_find find = {NULL, place, 0, NULL};
    markdown_core_node_visit_fields((markdown_core_node *)owner, field_find_visit, &find);
    return find.slot ? *find.slot : NULL;
}

/* `member`, a field of `owner`'s, continues the old owner's field in the same
 * place: a group the old group, and a node of its own the old node when it is
 * of its kind and the image of its anchor lies in its range, up to
 * `reach`. */
static bool field_search(markdown_core_parser *parser, markdown_core_publication *publication,
                         markdown_core_member *owner, markdown_core_member *member, uint32_t reach) {
    size_t place;
    field_slot(owner->node, member->node, &place);
    const markdown_core_node *old = field_at(owner->old, place);
    if (!old || (member->node->flags & MARKDOWN_CORE_NODE__GROUP)) {
        member->old = old;
        member->old_start = owner->old_start;
        member->decided = true;
        return true;
    }
    const uint32_t start = (uint32_t)((int64_t)owner->old_start + old->where.extent.lead);
    uint32_t image;
    bool failed = false;
    const bool anchored =
        old_image(parser, publication, member, start, start + old->where.extent.span, &image, &failed);
    if (failed) {
        return false;
    }
    if (anchored && image >= reach && !passed_whole(member, reach)) {
        member->passed = reach;
        return true;
    }
    if (anchored && image >= markdown_core_member_place(member).start && image < reach &&
        public_kind(old) == public_kind(member->node)) {
        member->old = old;
        member->old_start = start;
    }
    member->decided = true;
    return true;
}

/* `member` takes its place among the members `owner` holds as children that
 * have asked, after every member before it has. */
static void ask_in_order(markdown_core_member *owner, markdown_core_member *member) {
    markdown_core_member *first = member;
    while (first->prev && !first->prev->asked) {
        first = first->prev;
    }
    for (markdown_core_member *at = first;; at = at->next) {
        at->asked = true;
        at->index = owner->asks++;
        if (at == member) {
            return;
        }
    }
}

/* `member`, a child of its owner's, passes its owner's cursor over its whole
 * range: as the next child asks, or as it settles, whichever comes first. A
 * definition body holds no cursor, and a child of an owner that continues
 * nothing has none to pass. */
static bool pass_range(markdown_core_parser *parser, markdown_core_publication *publication,
                       markdown_core_member *member) {
    markdown_core_member *owner = member->owner;
    const uint32_t end = markdown_core_member_place(member).end;
    if ((member->node->flags & MARKDOWN_CORE_NODE__GROUP) || !owner->decided || !owner->old || member->passed >= end) {
        return true;
    }
    return pair_search(parser, publication, owner, member, end);
}

/* `member`, a child of an owner that continues an old node, asks the
 * owner's cursor: the children before it that have not asked ask first,
 * each passing its whole range, after the child that asked last has passed
 * its own, so the cursor passes the old nodes in their order. A child that
 * holds no cursor of its own -- a definition body -- passes nothing; any
 * other passes its range up to `reach`. */
static bool pair_ask(markdown_core_parser *parser, markdown_core_publication *publication, markdown_core_member *member,
                     uint32_t reach) {
    markdown_core_member *owner = member->owner;
    if (!member->asked) {
        markdown_core_member *first = member;
        while (first->prev && !first->prev->asked) {
            first = first->prev;
        }
        ask_in_order(owner, member);
        for (markdown_core_member *at = first->prev ? first->prev : first; at != member; at = at->next) {
            if (!pass_range(parser, publication, at)) {
                return false;
            }
        }
    }
    return (member->node->flags & MARKDOWN_CORE_NODE__GROUP) || member->passed >= reach ||
           pair_search(parser, publication, owner, member, reach);
}

/* One step of a search: `member`'s owner has searched as far as `reach`. An
 * owner that continues nothing as far as `reach` has no old node in `member`'s
 * range, so `member` continues nothing once its whole range is passed. */
static bool search_step(markdown_core_parser *parser, markdown_core_publication *publication,
                        markdown_core_member *member, uint32_t reach) {
    markdown_core_member *owner = member->owner;
    if (!owner->decided || !owner->old) {
        member->decided = owner->decided || passed_whole(member, reach);
        return true;
    }
    if (member->field) {
        return field_search(parser, publication, owner, member, reach);
    }
    if (!pair_ask(parser, publication, member, reach)) {
        return false;
    }
    if (member->node->flags & MARKDOWN_CORE_NODE__GROUP) {
        const markdown_core_stem *bodies = owner->old->children;
        member->old =
            member->index < markdown_core_stem_count(bodies) ? markdown_core_stem_at(bodies, member->index) : NULL;
        member->old_start = owner->old_start;
        member->decided = true;
        return true;
    }
    choose(publication, member, passed_whole(member, member->passed));
    return true;
}

/* `reach`, or the end of `member`'s range when that comes first; an open
 * block's range has no end yet. */
static uint32_t within(const markdown_core_member *member, uint32_t reach) {
    const uint32_t end = markdown_core_member_place(member).end;
    return !range_final(member) || reach < end ? reach : end;
}

/* `member` searches for the old node it continues as far as `reach`, its
 * undecided owners first, from the highest. A request from the content of an
 * inline root reaches the whole range of each member it climbs through
 * outside that content, whose range is in the source. False when an
 * allocation failed. */
static bool search(markdown_core_parser *parser, markdown_core_publication *publication, markdown_core_member *member,
                   uint32_t reach) {
    size_t count = 0;
    /* No member's range reaches past its end: a descendant's request reaches
     * as far as the member it asks for. */
    for (markdown_core_member *at = member; !at->decided; at = at->owner) {
        markdown_core_member **climb =
            markdown_core_reserve(publication->climb, &publication->climb_capacity, count + 1, sizeof(*climb));
        if (!climb) {
            return false;
        }
        publication->climb = climb;
        climb[count++] = at;
    }
    while (count) {
        markdown_core_member *at = publication->climb[--count];
        const bool across = at->owner->inner != member->owner->inner;
        if (!search_step(parser, publication, at, across ? markdown_core_member_place(at).end : within(at, reach))) {
            return false;
        }
    }
    return true;
}

/* `member` takes the id of the old node it continues, or the next id. */
static void identify(markdown_core_parser *parser, markdown_core_member *member) {
    member->identified = true;
    if (!(member->node->flags & MARKDOWN_CORE_NODE__GROUP)) {
        member->node->id = member->old ? member->old->id : ++parser->last_id;
    }
}

/* `member`'s node is numbered. It decides, and takes its id, as soon as that
 * cannot change: now when the nearest of its owners that decided -- the
 * document, above members not attached yet -- continues nothing, as then it
 * and every owner on the way continue nothing whatever they become, or when
 * it waits on nothing, its kind and range final; otherwise as it settles,
 * when they are. False when an allocation failed. */
static bool number_id(markdown_core_parser *parser, markdown_core_publication *publication,
                      markdown_core_member *member) {
    markdown_core_member *above = member->owner;
    while (above && !above->decided) {
        above = above->owner;
    }
    /* Members not attached yet lie under the document all the same. */
    const markdown_core_member *decided = above ? above : parser->root;
    if (!decided->old) {
        for (markdown_core_member *at = member; at && at != above; at = at->owner) {
            at->decided = true;
        }
    } else if (member->waits) {
        return true;
    }
    if (!search(parser, publication, member, member->place.end)) {
        return false;
    }
    assert(member->decided);
    identify(parser, member);
    return true;
}

/* Numbers `member`'s node, which `owner` holds, lies at its place, is
 * measured from `anchor` and fills `slot` of its relation: it holds its
 * strings and keeps its place, and takes its extent and its runs when they
 * are not an inline root's; a node holding
 * inline content outside an inline root's content waits on the parser's list
 * of inline roots, a field's as a field; a definition records where it was
 * written; the observer sees it. It settles now when it waits on nothing.
 * Returns where the item ends, or false when an allocation failed. */
static bool complete_number(const complete_context *context, markdown_core_member *member, uint32_t anchor,
                            uint32_t slot, bool field, uint32_t *end) {
    markdown_core_parser *parser = context->parser;
    markdown_core_node *item = member->node;
    const markdown_core_place place = item->where.place;
    const unsigned slots = slot_of(item);
    const relation_shape shape = (relation_shape)(slots & SLOT_SHAPE);
    if (!markdown_core_node_hold_strings(item)) {
        return false;
    }
    member->place = place;
    item->where.extent =
        (markdown_core_extent){(int32_t)((int64_t)place.start - (int64_t)anchor), place.end - place.start};
    /* Its reach is measured past its end from here (5.1). */
    item->reach = item->reach > place.end ? item->reach - place.end : 0;
    *end = place.end;
    markdown_core_node *holder = parser->completing ? NULL : content_holder(item, shape);
    if (holder && markdown_core_parser_contains_inlines(parser, holder)) {
        if (!markdown_core_parser_hold_inline_root(parser, member, holder, place, field || holder != item)) {
            return false;
        }
    } else {
        markdown_core_runs **runs = shape_runs_at(item, shape);
        if (runs && *runs) {
            publish_runs(parser->pool, runs, place);
        }
    }
    member->source = place.start;
    if (SLOT_TABLE(slots) && parser->completing &&
        !completing_source(parser, context->publication, place.start, &member->source)) {
        return false;
    }
    if (context->observe) {
        context->observe(context->observer, parser, item);
    }
    member->numbered = true;
    member->slot = slot;
    if (!number_id(parser, context->publication, member)) {
        return false;
    }
    if (member->waits) {
        member->counted = true;
        member->owner->waits++;
    } else {
        markdown_core_settle_member(parser, context->publication, member);
    }
    return !parser->error;
}

/* The member of `owner`'s that builds the group whose stem is `stem`: a field
 * root, or a child, whose members are a relation's. NULL when none does. */
static markdown_core_member *group_member(markdown_core_member *owner, const markdown_core_stem *stem) {
    for (markdown_core_member *field = owner->fields; field; field = field->next) {
        if (field->node->children == stem) {
            return field;
        }
    }
    for (markdown_core_member *child = owner->first; child; child = child->next) {
        if (child->node->children == stem) {
            return child;
        }
    }
    return NULL;
}

/* The member of `owner`'s field root `node`, made when the field was built
 * without one, as a block's fields are. NULL when it could not be made. */
static markdown_core_member *field_member(markdown_core_parser *parser, markdown_core_member *owner,
                                          markdown_core_node *node) {
    for (markdown_core_member *field = owner->fields; field; field = field->next) {
        if (field->node == node) {
            return field;
        }
    }
    return markdown_core_parser_attach_field(parser, owner, node);
}

/* A group whose relation `owner` numbered settles with it, or waits for the
 * members it holds that wait. */
static void group_numbered(markdown_core_parser *parser, markdown_core_publication *publication,
                           markdown_core_member *group) {
    group->numbered = true;
    if (group->waits) {
        if (!group->counted) {
            group->counted = true;
            group->owner->waits++;
        }
    } else {
        markdown_core_settle_member(parser, publication, group);
    }
}

/* Numbers what `member`'s node, which starts at `start`, holds and has not
 * numbered yet, relation by relation: each node is measured from the end of
 * the one before it in its relation, or from where the relation is measured
 * -- the owner's start, or 0 for the content of the inline root being
 * completed, which is its first relation. The members of a relation's nodes
 * are walked with them: the node's children's, or those of the group that
 * holds the relation, and a field's own. */
static bool complete_relations(const complete_context *context, markdown_core_member *member, uint32_t start) {
    markdown_core_node *node = member->node;
    const markdown_core_inline_root *completing = context->parser->completing;
    markdown_core_relation_cursor cursor;
    markdown_core_relation relation;
    markdown_core_relation_walk nodes;
    markdown_core_member *child = member->first;
    bool more, content = completing && completing->node == node;
    markdown_core_relations_begin(&cursor, node);
    while (relations_next(&cursor, &relation, &more)) {
        uint32_t anchor = content ? 0 : start;
        content = false;
        if (relation.field) {
            markdown_core_node *item = (markdown_core_node *)relation.node;
            if (!item->id) {
                markdown_core_member *field = field_member(context->parser, member, item);
                if (!field || (!field->numbered && !complete_number(context, field, anchor, 0, true, &anchor))) {
                    return false;
                }
            }
            continue;
        }
        const bool own = relation.stem == node->children;
        markdown_core_member *group = own || !relation.count ? NULL : group_member(member, relation.stem);
        markdown_core_member *at = own ? child : group ? group->first : NULL;
        markdown_core_relation_walk_begin(&nodes, &relation);
        uint32_t slot = (uint32_t)relation.index;
        for (markdown_core_node *item; (item = (markdown_core_node *)markdown_core_relation_walk_next(&nodes));
             slot++) {
            if (item->id || (at && at->node == item && at->numbered)) {
                anchor = publish_place(item, anchor).end;
                if (at && at->node == item) {
                    at = at->next;
                }
                continue;
            }
            assert(at && at->node == item);
            markdown_core_member *next = at->next;
            if (!complete_number(context, at, anchor, slot, false, &anchor)) {
                return false;
            }
            at = next;
        }
        if (own) {
            child = at;
        } else if (group && !group->numbered) {
            group_numbered(context->parser, context->publication, group);
        }
    }
    return true;
}

bool markdown_core_complete_node(markdown_core_parser *parser, markdown_core_publication *publication,
                                 markdown_core_member *member, uint32_t start,
                                 void (*observe)(const markdown_core_element_instance *, markdown_core_parser *,
                                                 markdown_core_node *),
                                 const markdown_core_element_instance *observer) {
    markdown_core_node *node = member->node;
    if (node->flags & MARKDOWN_CORE_NODE__GROUP) {
        return true;
    }
    const complete_context context = {parser, publication, observe, observer};
    const relation_shape shape = shape_of(node);
    if (!complete_relations(&context, member, start)) {
        return false;
    }
    /* The root being completed reads its runs last: definitions in its
     * content read them as they were. */
    const markdown_core_inline_root *completing = parser->completing;
    if (completing && completing->node == node) {
        markdown_core_runs **runs = shape_runs_at(node, shape);
        if (runs && *runs) {
            publish_runs(parser->pool, runs, completing->place);
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
        if (x.source.lead != y.source.lead || x.source.span != y.source.span || x.decoded != y.decoded) {
            return false;
        }
    }
    return true;
}

static bool scalars_equal(const markdown_core_node *a, const markdown_core_node *b);

/* Whether every relation of `node` holds the very nodes the same relation of
 * `old` holds, and they have the same relations. */
static bool relations_same(const markdown_core_node *node, const markdown_core_node *old) {
    markdown_core_relation_cursor cursor, old_cursor;
    markdown_core_relation relation, old_relation;
    markdown_core_relation_walk nodes, old_nodes;
    bool more, old_more;
    markdown_core_relations_begin(&cursor, node);
    markdown_core_relations_begin(&old_cursor, old);
    for (;;) {
        const bool has = relations_next(&cursor, &relation, &more);
        if (has != relations_next(&old_cursor, &old_relation, &old_more)) {
            return false;
        }
        if (!has) {
            return true;
        }
        if (relation.name != old_relation.name || relation.list != old_relation.list ||
            relation.field != old_relation.field || relation.count != old_relation.count) {
            return false;
        }
        markdown_core_relation_walk_begin(&nodes, &relation);
        markdown_core_relation_walk_begin(&old_nodes, &old_relation);
        for (const markdown_core_node *item; (item = markdown_core_relation_walk_next(&nodes));) {
            if (item != markdown_core_relation_walk_next(&old_nodes)) {
                return false;
            }
        }
    }
}

/* Whether `node` equals `old` as a value: its kind, extent, runs and
 * scalars, and its relations holding the same nodes. */
static bool node_same(const markdown_core_node *node, const markdown_core_node *old) {
    return public_kind(node) == public_kind(old) && node->where.extent.lead == old->where.extent.lead &&
           node->where.extent.span == old->where.extent.span &&
           runs_equal(shape_runs(node, shape_of(node)), shape_runs(old, shape_of(old))) && scalars_equal(node, old) &&
           relations_same(node, old);
}

/* `old` takes the place of `member`'s node in its owner, or as the document,
 * and the node goes when the parse does. False when it could not be kept
 * for that. */
static bool settle_old(markdown_core_parser *parser, markdown_core_publication *publication,
                       markdown_core_member *member, const markdown_core_node *old) {
    markdown_core_node **replaced = markdown_core_reserve(publication->replaced, &publication->replaced_capacity,
                                                          publication->replaced_count + 1, sizeof(*replaced));
    if (!replaced) {
        return false;
    }
    publication->replaced = replaced;
    markdown_core_node *node = member->node, *kept = markdown_core_node_retain((markdown_core_node *)old);
    markdown_core_member *owner = member->owner;
    /* The old node is the record of this parse now (5.1): its entry, its
     * reach and its flags are the new node's. */
    kept->entry = node->entry;
    kept->reach = node->reach;
    kept->flags = node->flags;
    /* And it declares what the new node declared: an equal node declares
     * the same, and these facts are this parse's (5.7). */
    markdown_core_registry_move(node, kept);
    if (!owner) {
        member->node = kept;
    } else if (member->field) {
        size_t place;
        *field_slot(owner->node, node, &place) = kept;
    } else {
        markdown_core_node *held = markdown_core_stem_put(owner->node->children, member->slot, kept);
        assert(held == node);
        (void)held;
    }
    (void)parser;
    publication->replaced[publication->replaced_count++] = node;
    return true;
}

void markdown_core_settle_member(markdown_core_parser *parser, markdown_core_publication *publication,
                                 markdown_core_member *member) {
    for (;;) {
        markdown_core_node *node = member->node;
        /* Settled, its kind and its range are final: the search decides it,
         * unless it decided already. */
        if (!search(parser, publication, member, member->place.end)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
        assert(member->decided);
        if (!member->identified) {
            identify(parser, member);
        }
        if (!(node->flags & MARKDOWN_CORE_NODE__GROUP)) {
            if (member->old && node_same(node, member->old)) {
                if (!settle_old(parser, publication, member, member->old)) {
                    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                    return;
                }
                node = member->node == node ? (markdown_core_node *)member->old : member->node;
            }
            const unsigned slots = slot_of(node);
            if (SLOT_TABLE(slots) && !table_add(&publication->tables[SLOT_TABLE(slots) - 1], node, member->source)) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                return;
            }
        }
        markdown_core_member *owner = member->owner;
        if (!owner) {
            return;
        }
        /* Its siblings no longer see it, so it passes its whole range now. */
        if (!member->field && !pass_range(parser, publication, member)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
        const bool counted = member->counted;
        markdown_core_parser_release_member(parser, member);
        if (!counted || --owner->waits || !owner->numbered) {
            return;
        }
        member = owner;
    }
}

void markdown_core_publication_dispose(markdown_core_publication *publication, markdown_core_node_pool *pool) {
    for (size_t table = 0; table < MARKDOWN_CORE_TABLE_COUNT; table++) {
        markdown_core_free(publication->tables[table].values);
    }
    markdown_core_free(publication->runs.runs);
    markdown_core_free(publication->images);
    markdown_core_free(publication->climb);
    markdown_core_free(publication->candidates);
    for (size_t i = 0; i < publication->replaced_count; i++) {
        markdown_core_node_pool_release(pool, publication->replaced[i]);
    }
    markdown_core_free(publication->replaced);
    *publication = (markdown_core_publication){0};
}

/* Takes `node` into the `*taken` entries when its label is not empty. */
static inline void label_take(label_entry *entries, size_t *taken, markdown_core_chunk label,
                              const markdown_core_node *node) {
    if (label.len > 0) {
        entries[(*taken)++] = label_entry_of(label.data, (size_t)label.len, node);
    }
}

/* The node each label a reference occurrence can name resolves to, in label
 * order: the first Reference declaring it, or, when none does, the first
 * Heading whose text declares it. The References in source order, then the
 * Headings in source order, sorted by label keeping that order among equal
 * labels, put each label's target first among its own. */
static bool reference_targets(markdown_core_parser *parser, markdown_core_document_value *value,
                              markdown_core_definition_table *headings) {
    value->reference_targets = NULL;
    value->reference_target_count = 0;
    if (headings->count && !markdown_core_order_source_entries(&parser->source_order, headings->values, headings->count,
                                                               sizeof(*headings->values), definition_key)) {
        return false;
    }
    const size_t total = value->references.count + headings->count;
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
    for (size_t i = 0; i < value->references.count; i++) {
        const markdown_core_node *node = value->references.nodes[i];
        label_take(entries, &labels, node->as.reference->label, node);
    }
    for (size_t i = 0; i < headings->count; i++) {
        const markdown_core_node *node = headings->values[i].node;
        label_take(entries, &labels, node->as.heading->label, node);
    }
    const label_entry *sorted = label_order(entries, labels, total);
    size_t count = 0;
    for (size_t i = 0; i < labels; i++) {
        if (!i || !label_same(&sorted[i], &sorted[i - 1])) {
            declared[count++] = sorted[i].node;
        }
    }
    markdown_core_free(entries);
    if (!count) {
        markdown_core_free((void *)declared);
        return true;
    }
    value->reference_targets = declared;
    value->reference_target_count = count;
    return true;
}

bool markdown_core_publication_take(markdown_core_publication *publication, const markdown_core_node *node,
                                    uint32_t start, void (*visit)(void *, const markdown_core_node *, uint32_t),
                                    void *context) {
    markdown_core_walk walk;
    markdown_core_walk_item item;
    markdown_core_walk_begin_at(&walk, node, (uint32_t)((int64_t)start - node->where.extent.lead));
    bool ok = true;
    while (ok && markdown_core_walk_next(&walk, &item)) {
        if (!item.node) {
            continue;
        }
        const uint32_t source = item.content ? source_run_place(&walk.runs, item.place.start) : item.place.start;
        const unsigned slots = slot_of(item.node);
        if (SLOT_TABLE(slots)) {
            ok = table_add(&publication->tables[SLOT_TABLE(slots) - 1], item.node, source);
        }
        visit(context, item.node, source);
    }
    ok = ok && !walk.failed;
    markdown_core_walk_end(&walk);
    return ok;
}

bool markdown_core_publish_tree(markdown_core_parser *parser, markdown_core_publication *publication) {
    markdown_core_revision *revision = parser->revision;
    markdown_core_member *member = parser->root;
    markdown_core_node *root = member->node;
    markdown_core_document_value *value = root->as.document;
    /* The document completes last, and numbers itself. */
    const markdown_core_place place = root->where.place;
    root->where.extent = (markdown_core_extent){(int32_t)place.start, place.end - place.start};
    if (root->runs) {
        publish_runs(parser->pool, (markdown_core_runs **)&root->runs, place);
    }
    markdown_core_definition_table *tables = publication->tables;
    markdown_core_definition_table *references = &tables[MARKDOWN_CORE_TABLE_REFERENCES];
    value->references = (markdown_core_definitions){0};
    bool ok = table_seal(parser, &tables[MARKDOWN_CORE_TABLE_FOOTNOTES], &value->footnotes) &&
              table_seal(parser, &tables[MARKDOWN_CORE_TABLE_SPECIMENS], &value->specimens) &&
              table_nodes(parser, references, &value->references.nodes);
    value->references.count = ok ? references->count : 0;
    ok = ok && reference_targets(parser, value, &tables[MARKDOWN_CORE_TABLE_HEADINGS]);
    if (!ok) {
        return false;
    }
    assert(!member->waits);
    member->place = place;
    member->numbered = true;
    if (!number_id(parser, publication, member)) {
        return false;
    }
    markdown_core_settle_member(parser, publication, member);
    if (parser->error) {
        return false;
    }
    revision->last_id = parser->last_id;
    return true;
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
    const markdown_core_runs *runs = shape_runs(node, shape_of(node));
    *count = runs ? runs->count : 0;
    return runs ? &runs->items[0].run : NULL;
}

void markdown_core_walk_begin(markdown_core_walk *walk, const markdown_core_node *root) {
    *walk = (markdown_core_walk){.root = root};
}

void markdown_core_walk_begin_at(markdown_core_walk *walk, const markdown_core_node *root, uint32_t anchor) {
    *walk = (markdown_core_walk){.root = root, .anchor = anchor};
}

void markdown_core_walk_end(markdown_core_walk *walk) {
    markdown_core_free(walk->frames);
    markdown_core_free(walk->runs.runs);
    markdown_core_free(walk->own.runs);
    markdown_core_free(walk->ranges);
    *walk = (markdown_core_walk){.root = walk->root, .anchor = walk->anchor, .failed = walk->failed};
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
        markdown_core_place place = walk_place(walk->root, walk->anchor);
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
            markdown_core_relation_walk_begin(&frame->nodes, &frame->relation);
        }
        if (frame->group_pending) {
            frame->group_pending = false;
            *item = (markdown_core_walk_item){
                NULL, {0, 0}, frame->relation.group, frame->relation.count, frame->level + 1, false};
            walk->at_group = true;
            walk->owner = walk->count;
            return true;
        }
        const markdown_core_node *node = markdown_core_relation_walk_next(&frame->nodes);
        if (node) {
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
    return markdown_core_relation_walk_more(&owner->nodes) || (!owner->relation.group && walk_more_after(owner));
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
    /* An inline node's window is the source its content range was read
     * from, cut by its root's runs; a block's is its range, cut by its own. */
    const markdown_core_source_runs *table = &walk->runs;
    markdown_core_place window = item->place;
    if (item->content) {
        window = markdown_core_source_runs_window(table, item->place);
    } else {
        const markdown_core_runs *runs = shape_runs(item->node, shape_of(item->node));
        walk->own.count = 0;
        if (runs && !markdown_core_source_runs_read(&walk->own, runs, item->place.start)) {
            walk->failed = true;
            return false;
        }
        table = &walk->own;
    }
    size_t needed = markdown_core_source_runs_ranges(table, window, walk->ranges, walk->range_capacity);
    if (needed > walk->range_capacity) {
        if (!walk_reserve(walk, needed)) {
            return false;
        }
        markdown_core_source_runs_ranges(table, window, walk->ranges, walk->range_capacity);
    }
    *count = needed;
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

/* A TREE CURSOR (markdown_core.h): its path from the start node, one frame
 * per node on it. Each frame below the top holds where its node's children
 * are read: the relation in hand, the walk over the rest of its nodes and
 * the cursor over the later relations. */
typedef struct {
    const markdown_core_node *node;
    markdown_core_relation_cursor relations;
    markdown_core_relation relation;
    markdown_core_relation_walk nodes;
} cursor_frame;

struct markdown_core_cursor {
    cursor_frame *frames;
    size_t count, capacity;
};

markdown_core_status markdown_core_cursor_open(const markdown_core_node *node, markdown_core_cursor **cursor) {
    markdown_core_cursor *made = markdown_core_alloc(1, sizeof(*made));
    if (!made || !(made->frames = markdown_core_alloc(8, sizeof(*made->frames)))) {
        markdown_core_free(made);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    made->capacity = 8;
    markdown_core_cursor_reset(made, node);
    *cursor = made;
    return MARKDOWN_CORE_OK;
}

void markdown_core_cursor_free(markdown_core_cursor *cursor) {
    if (cursor) {
        markdown_core_free(cursor->frames);
        markdown_core_free(cursor);
    }
}

void markdown_core_cursor_reset(markdown_core_cursor *cursor, const markdown_core_node *node) {
    cursor->frames[0] = (cursor_frame){.node = node};
    cursor->count = 1;
}

const markdown_core_node *markdown_core_cursor_node(const markdown_core_cursor *cursor) {
    return cursor->frames[cursor->count - 1].node;
}

markdown_core_field markdown_core_cursor_field(const markdown_core_cursor *cursor) {
    return cursor->count > 1 ? cursor->frames[cursor->count - 2].relation.name : (markdown_core_field)0;
}

size_t markdown_core_cursor_list(const markdown_core_cursor *cursor) {
    return cursor->count > 1 ? cursor->frames[cursor->count - 2].relation.list : 0;
}

size_t markdown_core_cursor_depth(const markdown_core_cursor *cursor) { return cursor->count - 1; }

/* The first node of the next relation of `frame` that holds one, or NULL. */
static const markdown_core_node *cursor_relation(cursor_frame *frame) {
    while (markdown_core_relations_next(&frame->relations, &frame->relation)) {
        if (frame->relation.count) {
            markdown_core_relation_walk_begin(&frame->nodes, &frame->relation);
            return markdown_core_relation_walk_next(&frame->nodes);
        }
    }
    return NULL;
}

markdown_core_status markdown_core_cursor_child(markdown_core_cursor *cursor, bool *moved) {
    *moved = false;
    if (cursor->count == cursor->capacity) {
        cursor_frame *frames =
            markdown_core_reserve(cursor->frames, &cursor->capacity, cursor->count + 1, sizeof(*frames));
        if (!frames) {
            return MARKDOWN_CORE_ALLOCATION_FAILED;
        }
        cursor->frames = frames;
    }
    cursor_frame *frame = &cursor->frames[cursor->count - 1];
    markdown_core_relations_begin(&frame->relations, frame->node);
    const markdown_core_node *child = cursor_relation(frame);
    if (child) {
        cursor->frames[cursor->count++] = (cursor_frame){.node = child};
        *moved = true;
    }
    return MARKDOWN_CORE_OK;
}

bool markdown_core_cursor_next(markdown_core_cursor *cursor) {
    if (cursor->count < 2) {
        return false;
    }
    cursor_frame *owner = &cursor->frames[cursor->count - 2], *frame = &cursor->frames[cursor->count - 1];
    const markdown_core_node *next = markdown_core_relation_walk_next(&owner->nodes);
    if (!next) {
        /* The relation in hand ends here; the owner's later ones are read on
         * a copy, so a cursor at the last child stays where it is. */
        cursor_frame rest = *owner;
        if (!(next = cursor_relation(&rest))) {
            return false;
        }
        *owner = rest;
    }
    *frame = (cursor_frame){.node = next};
    return true;
}

bool markdown_core_cursor_parent(markdown_core_cursor *cursor) {
    if (cursor->count < 2) {
        return false;
    }
    cursor->count--;
    return true;
}

size_t markdown_core_node_child_count(const markdown_core_node *node) {
    return node->kind != MARKDOWN_CORE_NODE_DEFINITION && node->kind != MARKDOWN_CORE_NODE_CITE
               ? markdown_core_stem_count(node->children)
               : 0;
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

static inline bool is_link(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_LINK || node->kind == MARKDOWN_CORE_NODE_EMBEDDED;
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

markdown_core_status markdown_core_node_definition_bodies(const markdown_core_node *node, size_t *count) {
    REQUIRE_KIND(node, KIND_BIT(MARKDOWN_CORE_KIND_DEFINITION));
    *count = markdown_core_stem_count(node->children);
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

markdown_core_status markdown_core_citation_referent(const markdown_core_node *citation,
                                                     markdown_core_referent *referent) {
    REQUIRE_KIND(citation, KIND_BIT(MARKDOWN_CORE_KIND_CITATION));
    *referent = citation_referent(citation);
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
    size_t child_count = kind == MARKDOWN_CORE_KIND_DEFINITION || kind == MARKDOWN_CORE_KIND_CITE
                             ? markdown_core_stem_count(node->children)
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
