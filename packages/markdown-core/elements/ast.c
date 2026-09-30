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

/* A parse error means there is no document and carries no source scope. Error
 * values are immutable process-lifetime sentinels. Reporting allocation
 * failure must itself allocate nothing; otherwise the consumer can receive
 * neither a document nor the error that explains its absence. */
struct markdown_core_error {
    markdown_core_error_code code;
    const char *message;
};

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

static const markdown_core_error ERROR_DOCUMENT_ALLOCATION = {MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                                                              "could not allocate document"};
static const markdown_core_error ERROR_PARSE_ALLOCATION = {MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                                                           "the parse could not complete an allocation"};
static const markdown_core_error ERROR_DUMP_ALLOCATION = {MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                                                          "could not produce canonical AST dump"};

static void set_error(markdown_core_error **error, const markdown_core_error *value) {
    *error = (markdown_core_error *)(uintptr_t)value;
}

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
    [SHAPE_INDEX(MARKDOWN_CORE_NODE_CITE)] = SHAPE_CITE,
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

/* Gives `node` its id and its extent against `anchor`, and records it in its
 * lookup table when its slot says the tables find it. Returns where it ended. */
static inline bool publish_node(markdown_core_node *node, unsigned slot, uint32_t anchor, uint64_t *next_id,
                                definition_table *footnotes, definition_table *specimens, markdown_core_place *place) {
    *place = node->where.place;
    node->id = ++*next_id;
    node->where.extent =
        (markdown_core_extent){(int32_t)((int64_t)place->start - (int64_t)anchor), place->end - place->start};
    if (!(slot & SLOT_LOOKUP)) {
        return true;
    }
    return table_add(node->kind == MARKDOWN_CORE_NODE_FOOTNOTE ? footnotes : specimens, node, place->start);
}

/* Nodes are published in canonical walk order: a node, then the nodes of its
 * relations in order, with one frame per node whose relations are open. A
 * node's place becomes its extent as it is published, so each frame keeps
 * the offsets its later nodes are relative to. */
bool markdown_core_publish_tree(markdown_core_parser *parser) {
    markdown_core_node *root = parser->root;
    markdown_core_source_order *order = &parser->source_order;
    markdown_core_document_value *value = root->as.document;
    publish_stack stack = {parser, parser->walk_stack, 0, parser->walk_stack_size / sizeof(publish_frame)};
    definition_table footnotes = {0}, specimens = {0};
    uint64_t next_id = 0;
    markdown_core_place place;
    publish_relation hand = {NULL, NULL, 0}, first;
    publish_frame *frame = NULL;
    markdown_core_relation_cursor cursor;
    bool more;
    bool ok = publish_node(root, slot_of(root), 0, &next_id, &footnotes, &specimens, &place);
    if (ok && publish_first(root, shape_of(root), &cursor, &first, &more)) {
        ok = stack.capacity || publish_grow(&stack);
        if (ok) {
            frame = &stack.frames[stack.count++];
            *frame = (publish_frame){.cursor = cursor, .start = place.start, .more = more};
            hand = (publish_relation){first.item, first.end, place.start};
        }
    }
    while (ok && stack.count) {
        if (hand.item == hand.end) {
            markdown_core_relation relation;
            if (frame->more && relations_next(&frame->cursor, &relation, &frame->more)) {
                hand = (publish_relation){relation.first, relation.end, frame->start};
            } else if (--stack.count) {
                frame--;
                hand = (publish_relation){frame->item, frame->end, frame->anchor};
            }
            continue;
        }
        markdown_core_node *node = (markdown_core_node *)hand.item;
        hand.item = node->next;
        unsigned slot = slot_of(node);
        ok = publish_node(node, slot, hand.anchor, &next_id, &footnotes, &specimens, &place);
        hand.anchor = place.end;
        if (!ok || !publish_first(node, (relation_shape)(slot & SLOT_SHAPE), &cursor, &first, &more)) {
            continue;
        }
        /* A frame with nothing left gives its slot to the node's own; any
         * other waits with the relation in hand. */
        if (hand.item != hand.end || frame->more) {
            frame->item = hand.item;
            frame->end = hand.end;
            frame->anchor = hand.anchor;
            if (stack.count == stack.capacity) {
                ok = publish_grow(&stack);
                if (!ok) {
                    continue;
                }
                frame = &stack.frames[stack.count - 1];
            }
            frame++;
            stack.count++;
        }
        frame->start = place.start;
        frame->more = more;
        if (more) {
            frame->cursor = cursor;
        }
        hand = (publish_relation){first.item, first.end, place.start};
    }
    ok = ok && table_seal(&footnotes, order, &value->footnotes) && table_seal(&specimens, order, &value->specimens);
    markdown_core_free(footnotes.values);
    markdown_core_free(specimens.values);
    return ok;
}

/* THE ONE PARSE TRANSACTION. Every caller runs it over the whole dialect:
 * the public entry supplies the default allocator, and the allocation-failure
 * tests supply an injected one. Nothing else builds a parser, so there is
 * exactly one language and no way to parse a part of it. */
markdown_core_document *markdown_core_document_parse_in(const uint8_t *source, size_t length,
                                                        markdown_core_text_unit unit, markdown_core_error **error) {
    markdown_core_document *document;

    *error = NULL;
    document = (markdown_core_document *)markdown_core_alloc(1, sizeof(*document));
    if (!document) {
        set_error(error, &ERROR_DOCUMENT_ALLOCATION);
        return NULL;
    }
    document->unit = unit;
    document->root = markdown_core_parse_document_with_setup((const char *)source, length, NULL, NULL);
    if (!document->root) {
        markdown_core_free(document);
        set_error(error, &ERROR_PARSE_ALLOCATION);
        return NULL;
    }
    return document;
}

markdown_core_document *markdown_core_document_parse(const uint8_t *source, size_t length,
                                                     markdown_core_error **error) {
    return markdown_core_document_parse_in(source, length, MARKDOWN_CORE_TEXT_UNIT_UTF8, error);
}

void markdown_core_document_free(markdown_core_document *document) {
    markdown_core_node_free(document->root);
    markdown_core_free(document);
}

markdown_core_text_unit markdown_core_document_unit(const markdown_core_document *document) { return document->unit; }

const markdown_core_node *markdown_core_document_root(const markdown_core_document *document) { return document->root; }

markdown_core_error_code markdown_core_error_get_code(const markdown_core_error *error) { return error->code; }

markdown_core_string markdown_core_error_get_message(const markdown_core_error *error) {
    return (markdown_core_string){(const uint8_t *)error->message, strlen(error->message)};
}

void markdown_core_error_free(markdown_core_error *error) { (void)error; }

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

markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node) {
    unsigned index = (unsigned)node->kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    return ((unsigned)node->kind & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_INLINE
               ? S_inline_kind[index]
               : S_block_kind[index];
}

const char *markdown_core_node_kind_name(markdown_core_node_kind kind) { return S_kind_name[kind]; }

uint64_t markdown_core_node_id(const markdown_core_node *node) { return node->id; }

markdown_core_extent markdown_core_node_extent(const markdown_core_node *node) { return node->where.extent; }

static size_t chain_length(const markdown_core_node *first) {
    size_t count = 0;
    for (; first; first = first->next) {
        count++;
    }
    return count;
}

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
            frame->next = frame->relation.first;
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
        if (frame->next != frame->relation.end) {
            markdown_core_node *node = (markdown_core_node *)frame->next;
            frame->next = node->next;
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
    return owner->next != owner->relation.end || (!owner->relation.group && walk_more_after(owner));
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

bool markdown_core_tree_scope(const markdown_core_node *root, const markdown_core_node *node, const uint8_t *source,
                              size_t length, markdown_core_text_unit unit, markdown_core_scope *scope) {
    markdown_core_place place = {0};
    source_lines lines;
    if (!tree_place(root, node, &place) || !source_lines_read(&lines, source, length)) {
        return false;
    }
    *scope = source_scope(&lines, source, place, unit);
    markdown_core_free(lines.starts);
    return true;
}

bool markdown_core_document_scope(const markdown_core_document *document, const markdown_core_node *node,
                                  const uint8_t *source, size_t length, markdown_core_scope *scope) {
    return markdown_core_tree_scope(document->root, node, source, length, document->unit, scope);
}

const markdown_core_node *markdown_core_document_node_at(const markdown_core_document *document,
                                                         markdown_core_position position, const uint8_t *source,
                                                         size_t length) {
    source_lines lines;
    if (!source_lines_read(&lines, source, length)) {
        return NULL;
    }
    size_t line = (size_t)position.line - 1;
    bool valid = line < lines.count;
    size_t offset = valid ? lines.starts[line] : 0;
    size_t end = valid && line + 1 < lines.count ? lines.starts[line + 1] : length;
    /* Step over the line's scalars up to the column; the position must land
     * on a byte of the line, at a scalar boundary. */
    int64_t column = 1;
    while (valid && column < position.column) {
        if (offset >= end) {
            valid = false;
            break;
        }
        size_t next = offset + 1;
        while (next < end && (source[next] & 0xC0) == 0x80) {
            next++;
        }
        column += (int64_t)source_columns(source, offset, next, document->unit);
        offset = next;
    }
    valid = valid && column == position.column;
    markdown_core_free(lines.starts);
    if (!valid || offset >= end) {
        return NULL;
    }
    markdown_core_walk walk;
    markdown_core_walk_item item;
    const markdown_core_node *found = NULL;
    markdown_core_walk_begin(&walk, document->root);
    while (markdown_core_walk_next(&walk, &item)) {
        if (item.node && item.place.start <= offset && offset < item.place.end) {
            found = item.node;
        }
    }
    bool failed = walk.failed;
    markdown_core_walk_end(&walk);
    return failed ? NULL : found;
}

const markdown_core_node *markdown_core_node_get_first_child(const markdown_core_node *node) {
    return node->kind != MARKDOWN_CORE_NODE_DEFINITION ? node->first_child : NULL;
}

const markdown_core_node *markdown_core_node_get_next_sibling(const markdown_core_node *node) { return node->next; }

size_t markdown_core_node_child_count(const markdown_core_node *node) {
    return chain_length(markdown_core_node_get_first_child(node));
}

int32_t markdown_core_node_heading_level(const markdown_core_node *node) { return node->as.heading->level; }

void markdown_core_node_list_properties(const markdown_core_node *node, markdown_core_list_flavor *flavor,
                                        markdown_core_optional_i64 *start, markdown_core_ordered_list_variant *variant,
                                        markdown_core_ordered_list_delimiter *delimiter, bool *tight) {
    *flavor = node->as.list->flavor;
    start->has_value = *flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED;
    start->value = node->as.list->start;
    *variant = node->as.list->variant;
    *delimiter = node->as.list->delimiter;
    *tight = node->as.list->tight;
}

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

markdown_core_optional_string markdown_core_node_list_item_marker(const markdown_core_node *node) {
    return optional_chunk_string(node->as.list->task_marker);
}

void markdown_core_node_code_block_properties(const markdown_core_node *node, markdown_core_optional_string *info,
                                              markdown_core_optional_string *language, markdown_core_string *literal,
                                              bool *fenced, bool *closed) {
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

markdown_core_string markdown_core_node_literal(const markdown_core_node *node) {
    return chunk_string(node->kind == MARKDOWN_CORE_NODE_HTML_BLOCK ? node->as.html_block->literal : *node->as.literal);
}

void markdown_core_node_formula_properties(const markdown_core_node *node, markdown_core_placement *mode,
                                           markdown_core_string *literal) {
    *mode = markdown_core_elements_get_formula_mode((markdown_core_node *)node) == MARKDOWN_CORE_FORMULA_MODE_EMBEDDED
                ? MARKDOWN_CORE_PLACEMENT_EMBEDDED
                : MARKDOWN_CORE_PLACEMENT_STANDALONE;
    *literal = cstr_string(markdown_core_elements_get_formula_literal((markdown_core_node *)node));
}

void markdown_core_node_table_properties(const markdown_core_node *node, size_t *column_count, size_t *head_count,
                                         size_t *content_count, size_t *foot_count) {
    const markdown_core_table *table = node->opaque;
    *column_count = table->column_count;
    *head_count = table->head_count;
    *content_count = table->content_count;
    *foot_count = table->foot_count;
}

markdown_core_table_column markdown_core_node_table_column_at(const markdown_core_node *node, size_t index) {
    return ((const markdown_core_table *)node->opaque)->columns[index];
}

void markdown_core_node_table_cell_spans(const markdown_core_node *node, int64_t *rowspan, int64_t *colspan) {
    *rowspan = node->as.table_cell->rowspan;
    *colspan = node->as.table_cell->colspan;
}

const markdown_core_node *markdown_core_node_table_caption(const markdown_core_node *node) {
    return ((const markdown_core_table *)node->opaque)->caption;
}

markdown_core_optional_string markdown_core_node_directive_properties(const markdown_core_node *node) {
    const char *value = markdown_core_elements_get_directive_name((markdown_core_node *)node);
    return value ? (markdown_core_optional_string){true, cstr_string(value)} : (markdown_core_optional_string){0};
}

bool markdown_core_node_definition_compact(const markdown_core_node *node) { return node->as.definition->compact; }
const markdown_core_node *markdown_core_node_definition_term(const markdown_core_node *node) {
    return node->as.definition->term->first_child;
}
const markdown_core_definition_body *markdown_core_node_definition_bodies(const markdown_core_node *node) {
    return (const markdown_core_definition_body *)node->first_child;
}
const markdown_core_definition_body *markdown_core_definition_body_next(const markdown_core_definition_body *body) {
    return (const markdown_core_definition_body *)((const markdown_core_node *)body)->next;
}
const markdown_core_node *markdown_core_definition_body_content(const markdown_core_definition_body *body) {
    return ((const markdown_core_node *)body)->first_child;
}

static bool is_link(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_LINK || node->kind == MARKDOWN_CORE_NODE_EMBEDDED;
}

const markdown_core_attribute_value *markdown_core_node_primary_attributes(const markdown_core_node *node) {
    return &node->attributes;
}
const markdown_core_attribute_value *markdown_core_node_inherited_attributes(const markdown_core_node *node) {
    return &node->as.link->resource->attributes;
}
markdown_core_optional_string markdown_core_attribute_value_anchor(const markdown_core_attribute_value *attributes) {
    return (markdown_core_optional_string){attributes->anchor.len > 0, chunk_string(attributes->anchor)};
}
size_t markdown_core_attribute_value_class_count(const markdown_core_attribute_value *attributes) {
    return attributes->class_count;
}
markdown_core_string markdown_core_attribute_value_class_at(const markdown_core_attribute_value *attributes,
                                                            size_t index) {
    return chunk_string(attributes->classes[index]);
}
size_t markdown_core_attribute_value_record_count(const markdown_core_attribute_value *attributes) {
    return attributes->record_count;
}
void markdown_core_attribute_value_record_at(const markdown_core_attribute_value *attributes, size_t index,
                                             markdown_core_string *name, markdown_core_string *value) {
    *name = chunk_string(attributes->records[index].name);
    *value = chunk_string(attributes->records[index].value);
}

/* A node's attributes merge its own over the ones its resource contributes:
 * a link or image reads through a resource, and no other kind has one, so
 * its inherited counts are zero. */
static size_t inherited_class_count(const markdown_core_node *node) {
    return is_link(node) ? node->as.link->resource->attributes.class_count : 0;
}
static size_t inherited_record_count(const markdown_core_node *node) {
    return is_link(node) ? node->as.link->resource->attributes.record_count : 0;
}
markdown_core_optional_string markdown_core_node_anchor(const markdown_core_node *node) {
    const markdown_core_chunk *anchor = markdown_core_node_anchor_chunk(node);
    return (markdown_core_optional_string){anchor->len > 0, chunk_string(*anchor)};
}
size_t markdown_core_node_attribute_class_count(const markdown_core_node *node) {
    return inherited_class_count(node) + node->attributes.class_count;
}
markdown_core_string markdown_core_node_attribute_class_at(const markdown_core_node *node, size_t index) {
    size_t count = inherited_class_count(node);
    return index < count ? markdown_core_attribute_value_class_at(markdown_core_node_inherited_attributes(node), index)
                         : markdown_core_attribute_value_class_at(&node->attributes, index - count);
}
size_t markdown_core_node_attribute_record_count(const markdown_core_node *node) {
    return inherited_record_count(node) + node->attributes.record_count;
}
void markdown_core_node_attribute_record_at(const markdown_core_node *node, size_t index, markdown_core_string *name,
                                            markdown_core_string *value) {
    size_t count = inherited_record_count(node);
    if (index < count) {
        markdown_core_attribute_value_record_at(markdown_core_node_inherited_attributes(node), index, name, value);
    } else {
        markdown_core_attribute_value_record_at(&node->attributes, index - count, name, value);
    }
}
const markdown_core_dimensions *markdown_core_node_dimensions(const markdown_core_node *node) {
    const markdown_core_optional_dimensions *dimensions =
        node->kind == MARKDOWN_CORE_NODE_EMBEDDED ? &node->as.link->dimensions : &node->as.cross_embedded->dimensions;
    return dimensions->has_value ? &dimensions->value : NULL;
}
const markdown_core_node *markdown_core_node_document_metadata(const markdown_core_node *node) {
    return node->as.document->metadata;
}

/* A metadata field the source wrote, or NULL: an absent field has no kind. */
static const markdown_core_metadata_value *metadata_field(const markdown_core_metadata_value *value) {
    return value->kind ? value : NULL;
}
const markdown_core_metadata_value *markdown_core_metadata_name(const markdown_core_node *metadata) {
    return metadata_field(&metadata->as.metadata->name);
}
const markdown_core_metadata_value *markdown_core_metadata_title(const markdown_core_node *metadata) {
    return metadata_field(&metadata->as.metadata->title);
}
const markdown_core_metadata_value *markdown_core_metadata_subtitle(const markdown_core_node *metadata) {
    return metadata_field(&metadata->as.metadata->subtitle);
}
const markdown_core_metadata_value *markdown_core_metadata_time(const markdown_core_node *metadata) {
    return metadata_field(&metadata->as.metadata->time);
}
const markdown_core_metadata_value *markdown_core_metadata_date(const markdown_core_node *metadata) {
    return metadata_field(&metadata->as.metadata->date);
}
const markdown_core_metadata_value *markdown_core_metadata_authors(const markdown_core_node *metadata) {
    return metadata_field(&metadata->as.metadata->authors);
}
const markdown_core_metadata_value *markdown_core_metadata_keywords(const markdown_core_node *metadata) {
    return metadata_field(&metadata->as.metadata->keywords);
}
const markdown_core_metadata_value *markdown_core_metadata_abstract(const markdown_core_node *metadata) {
    return metadata_field(&metadata->as.metadata->abstract);
}
const markdown_core_metadata_value *markdown_core_metadata_state(const markdown_core_node *metadata) {
    return metadata_field(&metadata->as.metadata->state);
}
const markdown_core_metadata_value *markdown_core_metadata_comment(const markdown_core_node *metadata) {
    return metadata_field(&metadata->as.metadata->comment);
}
markdown_core_metadata_value_kind markdown_core_metadata_value_get_kind(const markdown_core_metadata_value *value) {
    return value->kind;
}
markdown_core_metadata_scalar markdown_core_metadata_value_scalar(const markdown_core_metadata_value *value) {
    return value->as.scalar;
}
size_t markdown_core_metadata_value_item_count(const markdown_core_metadata_value *value) {
    return value->as.list.count;
}
markdown_core_metadata_list_item markdown_core_metadata_value_item_at(const markdown_core_metadata_value *value,
                                                                      size_t index) {
    return value->as.list.items[index];
}

const markdown_core_node *markdown_core_node_directive_label(const markdown_core_node *node) {
    return markdown_core_directive_label(node);
}

void markdown_core_node_callout_properties(const markdown_core_node *node, markdown_core_optional_string *variant,
                                           markdown_core_optional_bool *collapsed) {
    *variant = optional_chunk_string(node->as.callout->variant);
    *collapsed = node->as.callout->collapsed;
}

const markdown_core_node *markdown_core_node_callout_title(const markdown_core_node *node) {
    return node->as.callout->title ? node->as.callout->title->first_child : NULL;
}

markdown_core_destination markdown_core_node_destination(const markdown_core_node *node) {
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

markdown_core_optional_string markdown_core_node_cross_label(const markdown_core_node *node) {
    return optional_chunk_string(markdown_core_node_cross_reference(node)->label);
}

markdown_core_optional_string markdown_core_node_title(const markdown_core_node *node) {
    return optional_chunk_string(node->as.link->resource->title);
}

const markdown_core_resource *markdown_core_node_resource(const markdown_core_node *node) {
    return node->as.link->resource;
}

const markdown_core_node *markdown_core_node_cite_citations(const markdown_core_node *node) {
    return node->as.cite->citations;
}

markdown_core_referent markdown_core_citation_referent(const markdown_core_node *citation) {
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

const markdown_core_node *markdown_core_citation_prefix(const markdown_core_node *citation) {
    return citation->as.citation->prefix ? citation->as.citation->prefix->first_child : NULL;
}

const markdown_core_node *markdown_core_citation_suffix(const markdown_core_node *citation) {
    return citation->as.citation->suffix ? citation->as.citation->suffix->first_child : NULL;
}

/* The definition tables publishing recorded in the document's root. */
static const markdown_core_definitions *document_footnotes(const markdown_core_document *document) {
    return &document->root->as.document->footnotes;
}

static const markdown_core_definitions *document_specimens(const markdown_core_document *document) {
    return &document->root->as.document->specimens;
}

size_t markdown_core_document_footnote_count(const markdown_core_document *document) {
    return document_footnotes(document)->count;
}

const markdown_core_node *markdown_core_document_footnote_at(const markdown_core_document *document, size_t index) {
    return document_footnotes(document)->nodes[index];
}

size_t markdown_core_document_specimen_count(const markdown_core_document *document) {
    return document_specimens(document)->count;
}

const markdown_core_node *markdown_core_document_specimen_at(const markdown_core_document *document, size_t index) {
    return document_specimens(document)->nodes[index];
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

markdown_core_optional_string markdown_core_footnote_label(const markdown_core_node *footnote) {
    return optional_chunk_string(footnote->as.footnote->label);
}

const markdown_core_node *markdown_core_footnote_content(const markdown_core_node *footnote) {
    return footnote->first_child;
}

void markdown_core_specimen_properties(const markdown_core_node *specimen, markdown_core_optional_string *label,
                                       markdown_core_optional_i64 *start) {
    *label = optional_chunk_string(specimen->as.specimen->label);
    start->has_value = specimen->as.specimen->has_start;
    start->value = specimen->as.specimen->start;
}

const markdown_core_node *markdown_core_specimen_content(const markdown_core_node *specimen) {
    return specimen->first_child;
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
    size_t count, i;
    switch (kind) {
    case MARKDOWN_CORE_KIND_CITATION:
        buffer_cstr(buffer, " referent=");
        buffer_referent(buffer, markdown_core_citation_referent(node));
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE:
        buffer_cstr(buffer, " label=");
        buffer_optional_string(buffer, markdown_core_footnote_label(node));
        break;
    case MARKDOWN_CORE_KIND_SPECIMEN:
        markdown_core_specimen_properties(node, &oa, &start);
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
        dump_metadata_value(buffer, markdown_core_metadata_name(node));
        buffer_cstr(buffer, " title=");
        dump_metadata_value(buffer, markdown_core_metadata_title(node));
        buffer_cstr(buffer, " subtitle=");
        dump_metadata_value(buffer, markdown_core_metadata_subtitle(node));
        buffer_cstr(buffer, " time=");
        dump_metadata_value(buffer, markdown_core_metadata_time(node));
        buffer_cstr(buffer, " date=");
        dump_metadata_value(buffer, markdown_core_metadata_date(node));
        buffer_cstr(buffer, " authors=");
        dump_metadata_value(buffer, markdown_core_metadata_authors(node));
        buffer_cstr(buffer, " keywords=");
        dump_metadata_value(buffer, markdown_core_metadata_keywords(node));
        buffer_cstr(buffer, " abstract=");
        dump_metadata_value(buffer, markdown_core_metadata_abstract(node));
        buffer_cstr(buffer, " state=");
        dump_metadata_value(buffer, markdown_core_metadata_state(node));
        buffer_cstr(buffer, " comment=");
        dump_metadata_value(buffer, markdown_core_metadata_comment(node));
        break;
    case MARKDOWN_CORE_KIND_CALLOUT:
        markdown_core_node_callout_properties(node, &oa, &collapsed);
        buffer_cstr(buffer, " variant=");
        buffer_optional_string(buffer, oa);
        buffer_cstr(buffer, " collapsed=");
        buffer_optional_bool(buffer, collapsed);
        break;
    case MARKDOWN_CORE_KIND_DEFINITION:
        buffer_cstr(buffer, " compact=");
        buffer_cstr(buffer, markdown_core_node_definition_compact(node) ? "true" : "false");
        break;
    case MARKDOWN_CORE_KIND_HEADING:
        buffer_cstr(buffer, " level=");
        buffer_i64(buffer, markdown_core_node_heading_level(node));
        break;
    case MARKDOWN_CORE_KIND_LIST:
        markdown_core_node_list_properties(node, &flavor, &start, &variant, &delimiter, &x);
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
        buffer_optional_string(buffer, markdown_core_node_list_item_marker(node));
        break;
    case MARKDOWN_CORE_KIND_CODE_BLOCK:
        markdown_core_node_code_block_properties(node, &oa, &ob, &c, &x, &y);
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
        buffer_json_string(buffer, markdown_core_node_literal(node));
        break;
    case MARKDOWN_CORE_KIND_FORMULA:
        /* The only kind whose mode is a fact about the SOURCE: `$x$` is
         * embedded and `$$x$$` is standalone inside the same paragraph.  The
         * other five carried a mode that their kind already implied, and Q29
         * deleted all five at 15A.4. */
        markdown_core_node_formula_properties(node, &mode, &a);
        buffer_cstr(buffer, " mode=");
        buffer_cstr(buffer, mode_name(mode));
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
        markdown_core_node_formula_properties(node, &mode, &a);
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_TABLE: {
        size_t head, content, foot;
        markdown_core_node_table_properties(node, &count, &head, &content, &foot);
        buffer_cstr(buffer, " columns=[");
        for (i = 0; i < count; i++) {
            markdown_core_table_column column = markdown_core_node_table_column_at(node, i);
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
        int64_t rowspan, colspan;
        markdown_core_node_table_cell_spans(node, &rowspan, &colspan);
        buffer_cstr(buffer, " rowspan=");
        buffer_i64(buffer, rowspan);
        buffer_cstr(buffer, " colspan=");
        buffer_i64(buffer, colspan);
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE:
        buffer_cstr(buffer, " name=");
        buffer_optional_string(buffer, markdown_core_node_directive_properties(node));
        break;
    /* A DESTINATION IS REQUIRED (Q26): `dest=` is the tagged value and is
     * never `null`. `[a]()` used to print `destination=null`, which said the
     * author wrote no destination when the empty parentheses are the
     * destination they wrote; it is `dest=url("")` now. */
    case MARKDOWN_CORE_KIND_LINK:
        buffer_cstr(buffer, " dest=");
        buffer_destination(buffer, markdown_core_node_destination(node));
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, markdown_core_node_title(node));
        break;
    case MARKDOWN_CORE_KIND_CROSS_LINK:
        buffer_cstr(buffer, " dest=");
        buffer_destination(buffer, markdown_core_node_destination(node));
        buffer_cstr(buffer, " label=");
        buffer_optional_string(buffer, markdown_core_node_cross_label(node));
        break;
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED:
        buffer_cstr(buffer, " dest=");
        buffer_destination(buffer, markdown_core_node_destination(node));
        buffer_cstr(buffer, " label=");
        buffer_optional_string(buffer, markdown_core_node_cross_label(node));
        buffer_cstr(buffer, " dimensions=");
        buffer_dimensions(buffer, markdown_core_node_dimensions(node));
        break;
    case MARKDOWN_CORE_KIND_EMBEDDED:
        buffer_cstr(buffer, " dest=");
        buffer_destination(buffer, markdown_core_node_destination(node));
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, markdown_core_node_title(node));
        buffer_cstr(buffer, " dimensions=");
        buffer_dimensions(buffer, markdown_core_node_dimensions(node));
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
    if (markdown_core_metadata_value_get_kind(record) == MARKDOWN_CORE_METADATA_SCALAR) {
        markdown_core_metadata_scalar value = markdown_core_metadata_value_scalar(record);
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
        for (size_t i = 0; i < markdown_core_metadata_value_item_count(record); i++) {
            markdown_core_metadata_list_item item = markdown_core_metadata_value_item_at(record, i);
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
    size_t child_count = kind == MARKDOWN_CORE_KIND_CITE         ? chain_length(node->as.cite->citations)
                         : kind == MARKDOWN_CORE_KIND_DEFINITION ? chain_length(node->first_child)
                                                                 : markdown_core_node_child_count(node);
    dump_prefix(buffer, depth);
    buffer_cstr(buffer, markdown_core_node_kind_name(kind));
    buffer_cstr(buffer, " scope=");
    buffer_scope(buffer, place);
    buffer_cstr(buffer, " anchor=");
    buffer_optional_string(buffer, markdown_core_node_anchor(node));
    buffer_cstr(buffer, " attributes={");
    size_t classes = markdown_core_node_attribute_class_count(node);
    size_t records = markdown_core_node_attribute_record_count(node);
    for (size_t i = 0; i < classes; i++) {
        markdown_core_string value = markdown_core_node_attribute_class_at(node, i);
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
        markdown_core_node_attribute_record_at(node, i, &name, &value);
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

bool markdown_core_document_dump(const markdown_core_document *document, const markdown_core_node *node,
                                 const uint8_t *source, size_t source_length, uint8_t **output, size_t *length,
                                 markdown_core_error **error) {
    dump_buffer buffer = {0};
    markdown_core_place place = {0};
    source_lines lines;
    *error = NULL;
    if (!tree_place(document->root, node, &place) || !source_lines_read(&lines, source, source_length)) {
        set_error(error, &ERROR_DUMP_ALLOCATION);
        return false;
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
        set_error(error, &ERROR_DUMP_ALLOCATION);
        return false;
    }
    *output = buffer.data;
    *length = buffer.size;
    return true;
}

void markdown_core_dump_free(uint8_t *output) { free(output); }
