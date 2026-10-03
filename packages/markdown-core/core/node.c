#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "config.h"
#include "node.h"
#include "facts.h"
#include "element.h"
#include "iterator.h"

/* These kinds are owned roots/fields, never ordinary child edges, even under
 * a dynamic policy. Both decision paths share this structural boundary. */
static inline bool S_child_kind_allowed(markdown_core_node_type kind) {
    return kind != MARKDOWN_CORE_NODE_DOCUMENT && kind != MARKDOWN_CORE_NODE_TABLE_CAPTION &&
           kind != MARKDOWN_CORE_NODE_METADATA;
}

bool markdown_core_node_can_contain_builtin(const markdown_core_node *node, markdown_core_node_type child_type) {
    if (!S_child_kind_allowed(child_type)) {
        return false;
    }

    if (node->element && node->element->containment_kinds) {
        const markdown_core_node_type *kind = node->element->containment_kinds;
        while (*kind && *kind != node->kind) {
            kind++;
        }
        if (!*kind) {
            return false;
        }
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_DOCUMENT:
    case MARKDOWN_CORE_NODE_CALLOUT:
    case MARKDOWN_CORE_NODE_SPECIMEN:
    case MARKDOWN_CORE_NODE_DEFINITION_BODY:
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type) && child_type != MARKDOWN_CORE_NODE_LIST_ITEM &&
               child_type != MARKDOWN_CORE_NODE_DEFINITION && child_type != MARKDOWN_CORE_NODE_DEFINITION_BODY;

    case MARKDOWN_CORE_NODE_FOOTNOTE:
        return (MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type) && child_type != MARKDOWN_CORE_NODE_LIST_ITEM &&
                child_type != MARKDOWN_CORE_NODE_DEFINITION && child_type != MARKDOWN_CORE_NODE_DEFINITION_BODY) ||
               MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);

    case MARKDOWN_CORE_NODE_DEFINITION_LIST:
        return child_type == MARKDOWN_CORE_NODE_DEFINITION;
    case MARKDOWN_CORE_NODE_DEFINITION:
        return child_type == MARKDOWN_CORE_NODE_DEFINITION_BODY;
    case MARKDOWN_CORE_NODE_LIST:
        return child_type == MARKDOWN_CORE_NODE_LIST_ITEM;

    case MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK:
        return node->element && node->element->containment_kinds && MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type) &&
               child_type != MARKDOWN_CORE_NODE_LIST_ITEM && child_type != MARKDOWN_CORE_NODE_DEFINITION &&
               child_type != MARKDOWN_CORE_NODE_DEFINITION_BODY;
    case MARKDOWN_CORE_NODE_TABLE:
        return node->element && node->element->containment_kinds && child_type == MARKDOWN_CORE_NODE_TABLE_ROW;
    case MARKDOWN_CORE_NODE_TABLE_ROW:
        return node->element && node->element->containment_kinds && child_type == MARKDOWN_CORE_NODE_TABLE_CELL;
    case MARKDOWN_CORE_NODE_TABLE_CELL:
        return node->element && node->element->containment_kinds &&
               (MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type) || MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type));
    case MARKDOWN_CORE_NODE_DIRECTIVE_LABEL:
        return node->element && node->element->containment_kinds && MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type) &&
               child_type != MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
    case MARKDOWN_CORE_NODE_STRIKETHROUGH:
        return node->element && node->element->containment_kinds && MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);
    case MARKDOWN_CORE_NODE_PARAGRAPH:
    case MARKDOWN_CORE_NODE_TABLE_CAPTION:
    case MARKDOWN_CORE_NODE_HEADING:
    case MARKDOWN_CORE_NODE_EMPHASIS:
    case MARKDOWN_CORE_NODE_STRONG:
    case MARKDOWN_CORE_NODE_MARK:
    case MARKDOWN_CORE_NODE_INSERTION:
    case MARKDOWN_CORE_NODE_SPAN:
    case MARKDOWN_CORE_NODE_SUPERSCRIPT:
    case MARKDOWN_CORE_NODE_SUBSCRIPT:
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_EMBEDDED:
        return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);

    default:
        break;
    }

    return false;
}

bool markdown_core_node_can_contain_type(markdown_core_node *node, markdown_core_node_type child_type) {
    if (node->element && node->element->can_contain_func) {
        if (!S_child_kind_allowed(child_type)) {
            return false;
        }
        return node->element->can_contain_func(node->element, node, child_type) != 0;
    }
    return markdown_core_node_can_contain_builtin(node, child_type);
}

/* A NODE'S SLOT STORAGE (slab.h): the node, then room for its kind's record.
 * A C99 union aligns the node and the record space after it for ordinary
 * scalar fields, as the slot header before them is; the node sits at one
 * fixed offset in every slot whichever storage the slot came from. This is
 * storage layout, not a field format: nothing reads a slot through the node. */
typedef union {
    markdown_core_node node;
    long double alignment;
    int64_t integer_alignment;
} markdown_core_node_allocation;

/* Room for a kind's record inside the slot. The bound is a property of the
 * slot, not of any kind: a record that fits is placed here, and one that does
 * not is owned through `node_data_allocation`. Construction and conversion
 * use this same capacity and ownership rule. Sixty-four bytes hold every
 * record but the metadata fields and a cross transclusion's, which are one
 * per document and rare. */
#define MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES 64

typedef struct {
    markdown_core_node_allocation node;
    unsigned char record[MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES];
} markdown_core_node_slot;

static markdown_core_node_slot *S_slot_of(markdown_core_node *node) {
    return (markdown_core_node_slot *)((unsigned char *)node - offsetof(markdown_core_node_slot, node));
}

/* Uninitialized slot storage, or NULL (see slab.h). The constructor
 * initializes the node and its active record after taking the slot. Spare
 * record capacity is storage, not an object to initialize. */
static markdown_core_node_slot *S_slot_take(markdown_core_node_pool *pool) {
    return (markdown_core_node_slot *)markdown_core_slab_take(pool ? &pool->slabs : NULL, pool ? &pool->nodes : NULL,
                                                              sizeof(markdown_core_node_slot));
}

/* The node's storage, after its contents are released. */
static void S_slot_release(markdown_core_node_pool *pool, markdown_core_node *node) {
    markdown_core_slab_release(pool ? &pool->nodes : NULL, S_slot_of(node));
}

void markdown_core_node_pool_dispose(markdown_core_node_pool *pool) {
    markdown_core_slab_pool_dispose(&pool->nodes);
    markdown_core_slab_pool_dispose(&pool->resources);
    markdown_core_slab_pool_dispose(&pool->runs);
    markdown_core_slab_pool_dispose(&pool->bytes);
    markdown_core_slabs_dispose(&pool->slabs);
}

/* RECORD SIZE IS A PROPERTY OF THE KIND, so it is an array index.
 *
 * This was a 24-branch switch on the kind, compiled as a decision tree and
 * entered once per node -- 26.4% of `markdown_core_node_new_with_ext` on the
 * same-job corpus, at about 891,000 nodes. The kind already indexes two other
 * tables this way (`markdown_core_block_structure` and its inline twin, in
 * core/element.h), and the encoding is what makes that work: the low bits of
 * a kind are a dense ordinal within its class, and the class is one bit test.
 * The tables, and the type-string tables beside them, are generated from
 * packages/markdown-core/node-types.json. */
/* BEGIN GENERATED by scripts/tooling/generate-node-kinds.mjs; edit the node-kind schemas instead. */
/* A type without a record is absent and reads as 0: no inline record, and
 * `as.data` left NULL. An element-owned payload comes from `opaque_alloc_func`. */
static const size_t S_block_payload_size[MARKDOWN_CORE_NODE_KIND_COUNT] = {
    [MARKDOWN_CORE_NODE_DOCUMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_document_value),
    [MARKDOWN_CORE_NODE_CALLOUT & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_callout),
    [MARKDOWN_CORE_NODE_LIST & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_list),
    [MARKDOWN_CORE_NODE_LIST_ITEM & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_list),
    [MARKDOWN_CORE_NODE_CODE_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_code),
    [MARKDOWN_CORE_NODE_HTML_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_html_block),
    [MARKDOWN_CORE_NODE_HEADING & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_heading),
    [MARKDOWN_CORE_NODE_FOOTNOTE & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_footnote_value),
    [MARKDOWN_CORE_NODE_TABLE_CELL & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_table_cell),
    [MARKDOWN_CORE_NODE_COMMENT_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_chunk),
    [MARKDOWN_CORE_NODE_SPECIMEN & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_specimen_value),
    [MARKDOWN_CORE_NODE_DEFINITION & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_definition),
    [MARKDOWN_CORE_NODE_DEFINITION_BODY & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_definition_body_value),
    [MARKDOWN_CORE_NODE_METADATA & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_metadata_fields),
    [MARKDOWN_CORE_NODE_FACT & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_fact_place),
};

static const size_t S_inline_payload_size[MARKDOWN_CORE_NODE_KIND_COUNT] = {
    [MARKDOWN_CORE_NODE_TEXT & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_chunk),
    [MARKDOWN_CORE_NODE_CODE & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_chunk),
    [MARKDOWN_CORE_NODE_HTML & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_chunk),
    [MARKDOWN_CORE_NODE_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_link),
    [MARKDOWN_CORE_NODE_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_link),
    [MARKDOWN_CORE_NODE_COMMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_chunk),
    [MARKDOWN_CORE_NODE_CITATION & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_citation_item),
    [MARKDOWN_CORE_NODE_CROSS_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_cross_reference),
    [MARKDOWN_CORE_NODE_CROSS_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_cross_embedded),
};

static const char *const S_block_type_string[MARKDOWN_CORE_NODE_KIND_COUNT] = {
    [MARKDOWN_CORE_NODE_DOCUMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = "document",
    [MARKDOWN_CORE_NODE_CALLOUT & MARKDOWN_CORE_NODE_VALUE_MASK] = "callout",
    [MARKDOWN_CORE_NODE_LIST & MARKDOWN_CORE_NODE_VALUE_MASK] = "list",
    [MARKDOWN_CORE_NODE_LIST_ITEM & MARKDOWN_CORE_NODE_VALUE_MASK] = "list_item",
    [MARKDOWN_CORE_NODE_CODE_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = "code_block",
    [MARKDOWN_CORE_NODE_HTML_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = "html_block",
    [MARKDOWN_CORE_NODE_PARAGRAPH & MARKDOWN_CORE_NODE_VALUE_MASK] = "paragraph",
    [MARKDOWN_CORE_NODE_HEADING & MARKDOWN_CORE_NODE_VALUE_MASK] = "heading",
    [MARKDOWN_CORE_NODE_THEMATIC_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = "thematic_break",
    [MARKDOWN_CORE_NODE_FOOTNOTE & MARKDOWN_CORE_NODE_VALUE_MASK] = "footnote",
    [MARKDOWN_CORE_NODE_TABLE & MARKDOWN_CORE_NODE_VALUE_MASK] = "table",
    [MARKDOWN_CORE_NODE_TABLE_ROW & MARKDOWN_CORE_NODE_VALUE_MASK] = "table_row",
    [MARKDOWN_CORE_NODE_TABLE_CELL & MARKDOWN_CORE_NODE_VALUE_MASK] = "table_cell",
    [MARKDOWN_CORE_NODE_FORMULA_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = "formula_block",
    [MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = "directive_block",
    [MARKDOWN_CORE_NODE_COMMENT_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = "comment_block",
    [MARKDOWN_CORE_NODE_SPECIMEN & MARKDOWN_CORE_NODE_VALUE_MASK] = "specimen",
    [MARKDOWN_CORE_NODE_DEFINITION_LIST & MARKDOWN_CORE_NODE_VALUE_MASK] = "definition_list",
    [MARKDOWN_CORE_NODE_DEFINITION & MARKDOWN_CORE_NODE_VALUE_MASK] = "definition",
    [MARKDOWN_CORE_NODE_DEFINITION_BODY & MARKDOWN_CORE_NODE_VALUE_MASK] = "definition_body",
    [MARKDOWN_CORE_NODE_TABLE_CAPTION & MARKDOWN_CORE_NODE_VALUE_MASK] = "table_caption",
    [MARKDOWN_CORE_NODE_METADATA & MARKDOWN_CORE_NODE_VALUE_MASK] = "metadata",
    [MARKDOWN_CORE_NODE_REGISTRY & MARKDOWN_CORE_NODE_VALUE_MASK] = "registry",
    [MARKDOWN_CORE_NODE_FACT & MARKDOWN_CORE_NODE_VALUE_MASK] = "fact",
};

static const char *const S_inline_type_string[MARKDOWN_CORE_NODE_KIND_COUNT] = {
    [MARKDOWN_CORE_NODE_TEXT & MARKDOWN_CORE_NODE_VALUE_MASK] = "text",
    [MARKDOWN_CORE_NODE_SOFT_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = "soft_break",
    [MARKDOWN_CORE_NODE_LINE_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = "line_break",
    [MARKDOWN_CORE_NODE_CODE & MARKDOWN_CORE_NODE_VALUE_MASK] = "code",
    [MARKDOWN_CORE_NODE_HTML & MARKDOWN_CORE_NODE_VALUE_MASK] = "html",
    [MARKDOWN_CORE_NODE_EMPHASIS & MARKDOWN_CORE_NODE_VALUE_MASK] = "emphasis",
    [MARKDOWN_CORE_NODE_STRONG & MARKDOWN_CORE_NODE_VALUE_MASK] = "strong",
    [MARKDOWN_CORE_NODE_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = "link",
    [MARKDOWN_CORE_NODE_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = "embedded",
    [MARKDOWN_CORE_NODE_CITE & MARKDOWN_CORE_NODE_VALUE_MASK] = "cite",
    [MARKDOWN_CORE_NODE_STRIKETHROUGH & MARKDOWN_CORE_NODE_VALUE_MASK] = "strikethrough",
    [MARKDOWN_CORE_NODE_FORMULA & MARKDOWN_CORE_NODE_VALUE_MASK] = "formula",
    [MARKDOWN_CORE_NODE_DIRECTIVE & MARKDOWN_CORE_NODE_VALUE_MASK] = "directive",
    [MARKDOWN_CORE_NODE_DIRECTIVE_LABEL & MARKDOWN_CORE_NODE_VALUE_MASK] = "directive_label",
    [MARKDOWN_CORE_NODE_COMMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = "comment",
    [MARKDOWN_CORE_NODE_CITATION & MARKDOWN_CORE_NODE_VALUE_MASK] = "citation",
    [MARKDOWN_CORE_NODE_CROSS_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = "cross_link",
    [MARKDOWN_CORE_NODE_MARK & MARKDOWN_CORE_NODE_VALUE_MASK] = "mark",
    [MARKDOWN_CORE_NODE_CROSS_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = "cross_embedded",
    [MARKDOWN_CORE_NODE_INSERTION & MARKDOWN_CORE_NODE_VALUE_MASK] = "insertion",
    [MARKDOWN_CORE_NODE_SPAN & MARKDOWN_CORE_NODE_VALUE_MASK] = "span",
    [MARKDOWN_CORE_NODE_SUPERSCRIPT & MARKDOWN_CORE_NODE_VALUE_MASK] = "superscript",
    [MARKDOWN_CORE_NODE_SUBSCRIPT & MARKDOWN_CORE_NODE_VALUE_MASK] = "subscript",
};
/* END GENERATED */

static size_t S_node_payload_size(markdown_core_node_type type) {
    unsigned index = (unsigned)type & MARKDOWN_CORE_NODE_VALUE_MASK;

    if (index >= MARKDOWN_CORE_NODE_KIND_COUNT) {
        return 0;
    }
    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(type) ? S_inline_payload_size[index] : S_block_payload_size[index];
}

/* Establish defaults over zero-initialized storage. */
static void S_init_node_as(markdown_core_node_type type, markdown_core_node_data *as) {
    switch ((uint16_t)type) {
    case MARKDOWN_CORE_NODE_HEADING:
        as->heading->level = 1;
        break;
    case MARKDOWN_CORE_NODE_LIST:
        as->list->flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET;
        as->list->variant.kind = MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL;
        break;
    default:
        break;
    }
}

markdown_core_node *markdown_core_node_pool_new(markdown_core_node_pool *pool, markdown_core_node_type type,
                                                const markdown_core_element *element) {
    /* Only the active record is an object. Clear it together with the node,
     * including any alignment gap, whether the slot is fresh or reused.
     * An external record is initialized by its own allocation below. */
    size_t payload_size = S_node_payload_size(type);
    markdown_core_node_slot *slot = S_slot_take(pool);
    if (!slot) {
        return NULL;
    }
    size_t inline_size = payload_size <= MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES ? payload_size : 0;
    memset(&slot->node, 0,
           offsetof(markdown_core_node_slot, record) - offsetof(markdown_core_node_slot, node) + inline_size);
    markdown_core_node *node = &slot->node.node;
    if (payload_size > MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES) {
        node->node_data_allocation = markdown_core_alloc(1, payload_size);
        if (!node->node_data_allocation) {
            S_slot_release(pool, node);
            return NULL;
        }
        node->as.data = node->node_data_allocation;
    } else {
        node->as.data = payload_size ? slot->record : NULL;
    }
    markdown_core_strbuf_init_zeroed(&node->content);
    node->hold.refs = 1;
    node->kind = (uint16_t)type;
    node->element = element;
    S_init_node_as(type, &node->as);

    if (node->element && node->element->opaque_alloc_func) {
        node->element->opaque_alloc_func(node->element, node);
    }

    return node;
}

markdown_core_node *markdown_core_node_new_with_ext(markdown_core_node_type type,
                                                    const markdown_core_element *element) {
    return markdown_core_node_pool_new(NULL, type, element);
}

markdown_core_node *markdown_core_node_new(markdown_core_node_type type) {
    return markdown_core_node_pool_new(NULL, type, NULL);
}

static void free_node_as(markdown_core_slab_pool *resources, markdown_core_node *node) {
    switch (node->kind) {
    case MARKDOWN_CORE_NODE_CALLOUT:
        markdown_core_optional_chunk_free(&node->as.callout->variant);
        break;
    case MARKDOWN_CORE_NODE_METADATA:
        markdown_core_metadata_fields_free(node->as.metadata);
        break;
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        markdown_core_optional_chunk_free(&node->as.list->task_marker);
        break;
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        markdown_core_optional_chunk_free(&node->as.code->info);
        markdown_core_chunk_free(&node->as.code->literal);
        break;
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        markdown_core_chunk_free(node->as.literal);
        break;
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        markdown_core_chunk_free(&node->as.html_block->literal);
        break;
    case MARKDOWN_CORE_NODE_CROSS_LINK:
    case MARKDOWN_CORE_NODE_CROSS_EMBEDDED: {
        markdown_core_cross_reference *cross = markdown_core_node_cross_reference(node);
        markdown_core_chunk_free(&cross->path);
        markdown_core_optional_chunk_free(&cross->anchor);
        markdown_core_optional_chunk_free(&cross->label);
        break;
    }
    case MARKDOWN_CORE_NODE_CITATION:
        /* The release drops the note and affix roots as fields; only the
         * referent's bytes are the arm's. */
        markdown_core_chunk_free(&node->as.citation->value);
        break;
    case MARKDOWN_CORE_NODE_SPECIMEN:
        markdown_core_optional_chunk_free(&node->as.specimen->label);
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        markdown_core_optional_chunk_free(&node->as.footnote->label);
        break;
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_EMBEDDED:
        /* One holder fewer; a resource shared with other occurrences, or
         * still held by the fact that declares it, stays. */
        markdown_core_resource_release(resources, node->as.link->resource);
        node->as.link->resource = NULL;
        break;
    default:
        break;
    }
    /* Free only a record too large for the slot, whether construction or a
     * kind change installed it. Pointer
     * equality cannot establish ownership: an allocator may place a
     * replacement right after a slot. Almost no node owns one, so the release
     * is entered only when there is one. */
    if (node->node_data_allocation) {
        markdown_core_free(node->node_data_allocation);
        node->node_data_allocation = NULL;
    }
    node->as.data = NULL;
}

/* THE RELEASE. A node or run whose last holder went is put on one of two
 * lists threaded through its hold word, and the lists are drained until both
 * are empty: a released node drops its hold on its children's run and on
 * every node-valued field, and a released run drops its hold on each entry.
 * Nothing recurses and nothing allocates, however deep or wide the tree. */
typedef struct {
    markdown_core_node *nodes;
    markdown_core_run *runs;
} S_released;

static inline void S_drop_node(S_released *list, markdown_core_node *node) {
    if (node && --node->hold.refs == 0) {
        node->hold.released = list->nodes;
        list->nodes = node;
    }
}

static inline void S_drop_run(S_released *list, markdown_core_run *run) {
    if (run && --run->hold.refs == 0) {
        run->hold.released = list->runs;
        list->runs = run;
    }
}

/* A fact whose last holder went gives back what it holds: its definition
 * goes on the list, and its resource and bytes go now. */
static void S_drop_fact(S_released *list, markdown_core_slab_pool *resources, markdown_core_fact *fact) {
    if (--fact->refs) {
        return;
    }
    S_drop_node(list, fact->node);
    markdown_core_resource_release(resources, fact->resource);
    markdown_core_chunk_free(&fact->anchor);
    markdown_core_optional_chunk_free(&fact->base);
    markdown_core_free(fact);
}

static int S_drop_field(markdown_core_node **slot, void *context) {
    S_drop_node(context, *slot);
    *slot = NULL;
    return 1;
}

/* THE NODE-VALUED FIELDS OF A NODE'S RECORD, each slot visited whether or
 * not it holds a node. An element's own are visited by its
 * `visit_owned_subtrees_func`; `S_visit_fields` visits both. */
static int S_visit_record_fields(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor, void *context) {
    switch (node->kind) {
    case MARKDOWN_CORE_NODE_DEFINITION:
        return visitor(&node->as.definition->term, context);
    case MARKDOWN_CORE_NODE_CALLOUT:
        return visitor(&node->as.callout->title, context);
    case MARKDOWN_CORE_NODE_CITATION:
        return visitor(&node->as.citation->note, context) && visitor(&node->as.citation->prefix, context) &&
               visitor(&node->as.citation->suffix, context);
    case MARKDOWN_CORE_NODE_DOCUMENT:
        return visitor(&node->as.document->metadata, context);
    default:
        return 1;
    }
}

static int S_visit_fields(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor, void *context) {
    return S_visit_record_fields(node, visitor, context) &&
           (!node->element || !node->element->visit_owned_subtrees_func ||
            node->element->visit_owned_subtrees_func(node->element, node, visitor, context));
}

static size_t S_drain(markdown_core_node_pool *pool, S_released *list) {
    markdown_core_slab_pool *resources = pool ? &pool->resources : NULL;
    size_t released = 0;
    for (;;) {
        if (list->runs) {
            markdown_core_run *run = list->runs;
            list->runs = run->hold.released;
            for (size_t i = 0; i < run->count; i++) {
                if (run->tier) {
                    S_drop_run(list, (markdown_core_run *)run->entries[i]);
                } else {
                    S_drop_node(list, (markdown_core_node *)run->entries[i]);
                }
            }
            markdown_core_run_free_slot(pool, run);
            continue;
        }
        markdown_core_node *e = list->nodes;
        if (!e) {
            return released;
        }
        list->nodes = e->hold.released;
        released++;
        /* Almost no node owns an attribute value or a content buffer: the
         * test each releaser makes first -- its own predicate, defined once
         * beside it -- is made here, so a node that owns neither pays the
         * compares and no call. */
        if (markdown_core_attributes_owns(&e->attributes)) {
            markdown_core_attributes_release(resources, &e->attributes);
        }
        if (markdown_core_strbuf_owns(&e->content)) {
            markdown_core_strbuf_free(&e->content);
        }
        S_visit_fields(e, S_drop_field, list);
        if (e->kind == MARKDOWN_CORE_NODE_FACT) {
            S_drop_fact(list, resources, e->as.fact_place->fact);
        }
        S_drop_run(list, e->children);
        if (e->bytes) {
            markdown_core_bytes_release(pool, e->bytes);
        }
        if (e->opaque && e->element && e->element->opaque_free_func) {
            e->element->opaque_free_func(e->element, e);
        }
        free_node_as(resources, e);
        S_slot_release(pool, e);
    }
}

size_t markdown_core_node_pool_release(markdown_core_node_pool *pool, markdown_core_node *node) {
    S_released list = {NULL, NULL};
    S_drop_node(&list, node);
    return S_drain(pool, &list);
}

size_t markdown_core_node_pool_release_children(markdown_core_node_pool *pool, markdown_core_run *run) {
    S_released list = {NULL, NULL};
    S_drop_run(&list, run);
    return S_drain(pool, &list);
}

void markdown_core_fact_release(markdown_core_node_pool *pool, markdown_core_fact *fact) {
    S_released list = {NULL, NULL};
    S_drop_fact(&list, pool ? &pool->resources : NULL, fact);
    S_drain(pool, &list);
}

size_t markdown_core_node_release(markdown_core_node *node) { return markdown_core_node_pool_release(NULL, node); }

void markdown_core_node_free(markdown_core_node *node) { (void)markdown_core_node_release(node); }

bool markdown_core_node_set_kind(markdown_core_node *node, markdown_core_node_type kind) {
    if (kind == (markdown_core_node_type)node->kind) {
        return true;
    }

    /* Reserve any external replacement before releasing anything. A record
     * that fits already has storage in the node's slot; initializing it after
     * the old fields are destroyed cannot fail. Both cases keep the node's
     * address, and an allocation refusal preserves all old owned values. */
    size_t size = S_node_payload_size(kind);
    void *allocation = size > MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES ? markdown_core_alloc(1, size) : NULL;
    if (size > MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES && !allocation) {
        return false;
    }
    /* Kind conversion keeps the element's fields; the record's are freed
     * by a separate walk so the node's siblings remain untouched. */
    S_released fields = {NULL, NULL};
    S_visit_record_fields(node, S_drop_field, &fields);
    S_drain(NULL, &fields);
    free_node_as(NULL, node);
    node->as.data = allocation ? allocation : size ? S_slot_of(node)->record : NULL;
    node->node_data_allocation = allocation;
    if (!allocation && size) {
        memset(node->as.data, 0, size);
    }
    S_init_node_as(kind, &node->as);
    node->kind = (uint16_t)kind;
    return true;
}

const char *markdown_core_node_get_type_string(markdown_core_node *node) {
    if (node == NULL) {
        return "NONE";
    }

    unsigned index = (unsigned)node->kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    const char *type_string = NULL;

    if (node->kind == MARKDOWN_CORE_NODE_NONE) {
        return "none";
    }
    if (index < MARKDOWN_CORE_NODE_KIND_COUNT) {
        switch ((unsigned)node->kind & MARKDOWN_CORE_NODE_TYPE_MASK) {
        case MARKDOWN_CORE_NODE_TYPE_BLOCK:
            type_string = S_block_type_string[index];
            break;
        case MARKDOWN_CORE_NODE_TYPE_INLINE:
            type_string = S_inline_type_string[index];
            break;
        default:
            break;
        }
    }
    return type_string ? type_string : "<unknown>";
}

const char *markdown_core_node_get_string_content(markdown_core_node *node) { return (char *)node->content.ptr; }

int markdown_core_node_set_string_content(markdown_core_node *node, const char *content) {
    markdown_core_strbuf_sets(&node->content, content);
    return true;
}

markdown_core_resource *markdown_core_resource_new(markdown_core_node_pool *pool, markdown_core_chunk url,
                                                   markdown_core_optional_chunk title) {
    markdown_core_resource *resource = (markdown_core_resource *)markdown_core_slab_take(
        pool ? &pool->slabs : NULL, pool ? &pool->resources : NULL, sizeof(markdown_core_resource));
    if (!resource) {
        return NULL;
    }
    memset(resource, 0, sizeof(*resource));
    resource->url = url;
    resource->title = title;
    resource->holders = 1;
    return resource;
}

void markdown_core_resource_retain(markdown_core_resource *resource) {
    if (resource) {
        resource->holders++;
    }
}

void markdown_core_resource_release(markdown_core_slab_pool *resources, markdown_core_resource *resource) {
    if (!resource) {
        return;
    }
    assert(resource->holders > 0);
    if (--resource->holders > 0) {
        return;
    }
    markdown_core_chunk_free(&resource->url);
    markdown_core_optional_chunk_free(&resource->title);
    markdown_core_attributes_release(resources, &resource->attributes);
    markdown_core_slab_release(resources, resource);
}

int markdown_core_node_set_element(markdown_core_node *node, const markdown_core_element *element) {
    if (node == NULL) {
        return 0;
    }
    node->element = element;
    return 1;
}

bool markdown_core_node_attach_validated(markdown_core_node_pool *pool, markdown_core_node *parent, size_t index,
                                         markdown_core_node *child) {
    assert(parent && child && parent != child);
    /* Built-in containment is pure and shares its rules with checked mutation.
     * Dynamic policies were decided before ownership moved; never replay them. */
    assert((parent->element && parent->element->can_contain_func) ||
           markdown_core_node_can_contain_builtin(parent, (markdown_core_node_type)child->kind));
    return markdown_core_children_insert(pool, &parent->children, index, child);
}

bool markdown_core_node_append_validated(markdown_core_node_pool *pool, markdown_core_node *parent,
                                         markdown_core_node *child) {
    assert(parent && child && parent != child);
    assert((parent->element && parent->element->can_contain_func) ||
           markdown_core_node_can_contain_builtin(parent, (markdown_core_node_type)child->kind));
    return markdown_core_children_append(pool, &parent->children, child);
}

bool markdown_core_node_insert_child(markdown_core_node_pool *pool, markdown_core_node *node, size_t index,
                                     markdown_core_node *child) {
    return markdown_core_node_can_contain_type(node, (markdown_core_node_type)child->kind) &&
           markdown_core_children_insert(pool, &node->children, index, child);
}

markdown_core_node *markdown_core_node_take_child(markdown_core_node_pool *pool, markdown_core_node *node,
                                                  size_t index) {
    markdown_core_node *child;
    return markdown_core_children_remove(pool, &node->children, index, &child) ? child : NULL;
}

int markdown_core_node_check(markdown_core_node *node, FILE *out) {
    markdown_core_iter_path path = {0};
    markdown_core_iter iter;
    int errors = 0;
    markdown_core_iter_init(&iter, &path, node);
    while (markdown_core_iter_step(&iter) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *cur = markdown_core_iter_node(&iter);
        if (iter.event != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        size_t broken = markdown_core_children_check(cur->children);
        if (broken && out) {
            fprintf(out, "Invalid children tree in node type %s (id %llu)\n", markdown_core_node_get_type_string(cur),
                    (unsigned long long)cur->id);
        }
        errors += broken != 0;
    }
    bool failed = iter.failed;
    markdown_core_iter_path_dispose(&path);
    return failed ? -1 : errors;
}

bool markdown_core_node_kind_set_intersects(const markdown_core_node_kind_set *a,
                                            const markdown_core_node_kind_set *b) {
    return (a->blocks & b->blocks) != 0 || (a->inlines & b->inlines) != 0;
}

/* The marks of one child (children.h). */
static inline unsigned S_child_marks(const markdown_core_node *node) {
    return ((node->flags & MARKDOWN_CORE_NODE__CHANGED) ? MARKDOWN_CORE_RUN_CHANGED : 0u) |
           ((node->flags & MARKDOWN_CORE_NODE__EXIT_FRAGILE) ? 0u : (unsigned)MARKDOWN_CORE_RUN_ENDS) |
           ((node->flags & MARKDOWN_CORE_NODE__CONTAINS_BLANK) ? (unsigned)MARKDOWN_CORE_RUN_CONTAINS_BLANK : 0u) |
           ((node->flags & MARKDOWN_CORE_NODE__AFTER_BLANK_END) ? (unsigned)MARKDOWN_CORE_RUN_AFTER_BLANK_END : 0u) |
           ((node->flags & MARKDOWN_CORE_NODE__AFTER_LOOSE_END) ? (unsigned)MARKDOWN_CORE_RUN_AFTER_LOOSE_END : 0u);
}

/* A tier-zero run's sums from its children's extents and reaches, and a run
 * above from its entries' sums: in both, a child's end and its reach are
 * measured from the run's start through the lengths before it. Its marks
 * are its entries' together, and its tally their sum. */
static void S_seal_run(markdown_core_run *run) {
    int64_t length = 0, furthest = 0;
    unsigned marks = 0;
    uint32_t tally = 0;
    for (size_t i = 0; i < run->count; i++) {
        int64_t end, reach;
        if (run->tier) {
            const markdown_core_run *entry = run->entries[i];
            end = length + entry->length;
            reach = entry->reach;
            marks |= entry->marks;
            tally += entry->tally;
        } else {
            const markdown_core_node *node = run->entries[i];
            end = length + node->where.extent.lead + node->where.extent.span;
            reach = node->reach;
            marks |= S_child_marks(node);
            tally += node->tally;
        }
        length = end;
        if (end + reach > furthest) {
            furthest = end + reach;
        }
    }
    run->length = length;
    run->reach = furthest > length ? (uint32_t)(furthest - length) : 0;
    run->marks = (uint8_t)marks;
    run->tally = tally;
    run->sealed = 1;
}

void markdown_core_children_seal(markdown_core_run *root) {
    if (!root || root->sealed) {
        return;
    }
    /* Down the unsealed runs, each sealed after the entries below it. */
    markdown_core_run *runs[MARKDOWN_CORE_RUN_TIERS];
    uint8_t at[MARKDOWN_CORE_RUN_TIERS];
    int tiers = 1;
    runs[0] = root;
    at[0] = 0;
    while (tiers) {
        markdown_core_run *run = runs[tiers - 1];
        if (run->tier) {
            while (at[tiers - 1] < run->count && ((markdown_core_run *)run->entries[at[tiers - 1]])->sealed) {
                at[tiers - 1]++;
            }
            if (at[tiers - 1] < run->count) {
                runs[tiers] = run->entries[at[tiers - 1]++];
                at[tiers] = 0;
                tiers++;
                continue;
            }
        }
        S_seal_run(run);
        tiers--;
    }
}

void markdown_core_children_reseal(markdown_core_run *root, size_t index) {
    for (markdown_core_run *run = root;;) {
        run->sealed = 0;
        if (!run->tier) {
            break;
        }
        run = run->entries[markdown_core_run_find(run, &index)];
    }
    markdown_core_children_seal(root);
}

bool markdown_core_children_find(const markdown_core_run *root, int64_t origin, int64_t offset, size_t *index,
                                 int64_t *lead) {
    if (!root) {
        return false;
    }
    const markdown_core_run *run = root;
    int64_t base = origin;
    size_t before = 0;
    while (run->tier) {
        size_t k = 0;
        for (; k < run->count; k++) {
            const markdown_core_run *entry = run->entries[k];
            if (base + entry->length > offset) {
                break;
            }
            base += entry->length;
            before += entry->total;
        }
        if (k == run->count) {
            return false;
        }
        run = run->entries[k];
    }
    for (size_t k = 0; k < run->count; k++) {
        const markdown_core_node *node = run->entries[k];
        int64_t end = base + node->where.extent.lead + node->where.extent.span;
        if (end > offset) {
            *index = before + k;
            *lead = base;
            return true;
        }
        base = end;
    }
    return false;
}

int64_t markdown_core_children_length_before(const markdown_core_run *root, size_t index) {
    int64_t length = 0;
    const markdown_core_run *run = root;
    while (run && run->tier) {
        size_t k = 0;
        for (const markdown_core_run *entry; (entry = run->entries[k])->total <= index; k++) {
            index -= entry->total;
            length += entry->length;
        }
        run = run->entries[k];
    }
    for (size_t k = 0; k < index; k++) {
        const markdown_core_node *node = run->entries[k];
        length += node->where.extent.lead + node->where.extent.span;
    }
    return length;
}

/* The sums of the children walked so far: their count, their bytes, the
 * furthest end of their reaches, from where the first lead starts, their
 * marks and their tallies' sum. */
typedef struct {
    size_t count;
    int64_t length, furthest;
    unsigned marks;
    uint32_t tally;
} take_sums;

static inline void take_add(take_sums *sums, size_t count, int64_t length, int64_t reach, unsigned marks,
                            uint32_t tally) {
    sums->count += count;
    sums->marks |= marks;
    sums->tally += tally;
    sums->length += length;
    if (sums->length + reach > sums->furthest) {
        sums->furthest = sums->length + reach;
    }
}

size_t markdown_core_children_take_run(const markdown_core_run *root, size_t first, size_t end,
                                       markdown_core_run_sums *sums) {
    markdown_core_children_cursor cursor;
    markdown_core_children_seek(&cursor, root, first);
    /* The walk takes each entry after `first` whole when no edit met a child
     * of it and it ends by `end`, and steps into it otherwise, up to the
     * first child an edit met or `end`.
     * The sums stand at the last child that can end the run, or before the
     * last whole entry that holds one, whose own last such child is found
     * after the walk. */
    take_sums walked = {0, 0, 0, 0, 0}, ends = {0, 0, 0, 0, 0};
    const markdown_core_run *within = NULL;
    int top = cursor.tiers - 1;
    while (top >= 0) {
        const markdown_core_run *run = cursor.runs[top];
        if (cursor.at[top] == run->count) {
            if (--top >= 0) {
                cursor.at[top]++;
            }
            continue;
        }
        if (!run->tier) {
            const markdown_core_node *node = run->entries[cursor.at[top]];
            if ((node->flags & MARKDOWN_CORE_NODE__CHANGED) || first + walked.count == end) {
                break;
            }
            take_add(&walked, 1, node->where.extent.lead + (int64_t)node->where.extent.span, node->reach,
                     S_child_marks(node), node->tally);
            if (!(node->flags & MARKDOWN_CORE_NODE__EXIT_FRAGILE)) {
                ends = walked;
                within = NULL;
            }
            cursor.at[top]++;
            continue;
        }
        const markdown_core_run *entry = run->entries[cursor.at[top]];
        if ((entry->marks & MARKDOWN_CORE_RUN_CHANGED) || first + walked.count + entry->total > end) {
            cursor.runs[++top] = entry;
            cursor.at[top] = 0;
            continue;
        }
        if (entry->marks & MARKDOWN_CORE_RUN_ENDS) {
            ends = walked;
            within = entry;
        }
        take_add(&walked, entry->total, entry->length, entry->reach, entry->marks, entry->tally);
        cursor.at[top]++;
    }
    /* Into the whole entry that holds the last child that can end the run,
     * down to that child: the entries before its last such entry whole. */
    while (within) {
        size_t last = within->count;
        while (last--) {
            unsigned found = within->tier ? ((const markdown_core_run *)within->entries[last])->marks
                                          : S_child_marks(within->entries[last]);
            if (found & MARKDOWN_CORE_RUN_ENDS) {
                break;
            }
        }
        for (size_t k = 0; k <= last; k++) {
            if (within->tier && k == last) {
                break;
            }
            if (within->tier) {
                const markdown_core_run *entry = within->entries[k];
                take_add(&ends, entry->total, entry->length, entry->reach, entry->marks, entry->tally);
            } else {
                const markdown_core_node *node = within->entries[k];
                take_add(&ends, 1, node->where.extent.lead + (int64_t)node->where.extent.span, node->reach,
                         S_child_marks(node), node->tally);
            }
        }
        within = within->tier ? within->entries[last] : NULL;
    }
    sums->length = ends.length;
    sums->reach = ends.furthest > ends.length ? (uint32_t)(ends.furthest - ends.length) : 0;
    sums->marks = ends.marks;
    sums->tally = ends.tally;
    return ends.count;
}
