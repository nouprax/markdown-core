#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "config.h"
#include "node.h"
#include "element.h"

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
    case MARKDOWN_CORE_NODE_CITE:
        return child_type == MARKDOWN_CORE_NODE_CITATION;
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

#ifndef NDEBUG
bool markdown_core_node_may_attach(const markdown_core_node *parent, markdown_core_node_type child_type) {
    return (parent->element && parent->element->can_contain_func) ||
           markdown_core_node_can_contain_builtin(parent, child_type);
}
#endif

bool markdown_core_node_can_contain_type(markdown_core_node *node, markdown_core_node_type child_type) {
    if (node->element && node->element->can_contain_func) {
        if (!S_child_kind_allowed(child_type)) {
            return false;
        }
        return node->element->can_contain_func(node->element, node, child_type) != 0;
    }
    return markdown_core_node_can_contain_builtin(node, child_type);
}

#define S_slot_of markdown_core_node_slot_of

/* Uninitialized slot storage, or NULL (see slab.h). The constructor
 * initializes the node and its active record after taking the slot. Spare
 * record capacity is storage, not an object to initialize. */
static markdown_core_node_slot *S_slot_take(markdown_core_node_pool *pool) {
    return (markdown_core_node_slot *)markdown_core_slab_take(
        pool ? &pool->nodes : NULL, sizeof(markdown_core_node_slot), MARKDOWN_CORE_NODE_SLAB_BYTES);
}

/* The node's storage, after its contents are released. */
static void S_slot_release(markdown_core_node_pool *pool, markdown_core_node *node) {
    markdown_core_slab_release(pool ? &pool->nodes : NULL, S_slot_of(node));
}

void markdown_core_node_pool_dispose(markdown_core_node_pool *pool) {
    markdown_core_slab_pool_dispose(&pool->nodes);
    markdown_core_slab_pool_dispose(&pool->resources);
    markdown_core_bytes_pool_dispose(&pool->bytes);
}

void markdown_core_node_pool_bytes_free(markdown_core_node_pool *pool, void *storage) {
    markdown_core_bytes_release(pool ? &pool->bytes : NULL, storage);
}

/* Storage of `bytes` a node owns, from `pool`, or a slot of its own from the
 * allocator when there is no pool: either is given back by
 * markdown_core_node_pool_bytes_free. */
static void *S_bytes_take(markdown_core_node_pool *pool, size_t bytes) {
    if (pool) {
        return markdown_core_node_pool_bytes(pool, bytes);
    }
    markdown_core_bytes_header *header = markdown_core_realloc(NULL, sizeof(*header) + bytes);
    if (!header) {
        return NULL;
    }
    header->owner.slab = NULL;
    return header + 1;
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
    [MARKDOWN_CORE_NODE_REFERENCE & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_reference_value),
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
    [MARKDOWN_CORE_NODE_REFERENCE & MARKDOWN_CORE_NODE_VALUE_MASK] = "reference",
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
    /* Only the active record is an object. Clear it together with the
     * scaffold and the node, including any alignment gap, whether the slot is
     * fresh or reused. An external record is initialized by its own
     * allocation below. */
    size_t payload_size = S_node_payload_size(type);
    markdown_core_node_slot *slot = S_slot_take(pool);
    if (!slot) {
        return NULL;
    }
    size_t inline_size = payload_size <= MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES ? payload_size : 0;
    memset(slot, 0, offsetof(markdown_core_node_slot, record) + inline_size);
    markdown_core_node *node = &slot->node.node;
    if (payload_size > MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES) {
        node->node_data_allocation = S_bytes_take(pool, payload_size);
        if (!node->node_data_allocation) {
            S_slot_release(pool, node);
            return NULL;
        }
        memset(node->node_data_allocation, 0, payload_size);
        node->as.data = node->node_data_allocation;
    } else {
        node->as.data = payload_size ? slot->record : NULL;
    }
    markdown_core_strbuf_init_zeroed(&node->content);
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

/* A document's definition tables, but a snapshot's, which it borrows (ast.c,
 * A SNAPSHOT'S TABLES). Out of line, so that the free of every other kind
 * keeps its own registers. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void free_document_tables(markdown_core_document_value *document) {
    if (document->committed) {
        return;
    }
    markdown_core_definitions *const tables[] = {&document->footnotes, &document->specimens, &document->references};
    for (size_t i = 0; i < sizeof(tables) / sizeof(*tables); i++) {
        markdown_core_free((void *)tables[i]->all.nodes);
        markdown_core_free(tables[i]->all.places);
        markdown_core_free((void *)tables[i]->labeled);
    }
    markdown_core_free((void *)document->reference_targets.nodes);
    markdown_core_free(document->reference_targets.places);
}

static void free_node_as(markdown_core_node_pool *pool, markdown_core_node *node) {
    markdown_core_slab_pool *resources = pool ? &pool->resources : NULL;
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
        /* The note and the affixes are freed by the walk in `S_free_nodes`,
         * pushed beside the children; only the referent's bytes are the
         * arm's. */
        markdown_core_chunk_free(&node->as.citation->value);
        break;

    case MARKDOWN_CORE_NODE_DOCUMENT:
        free_document_tables(node->as.document);
        break;
    case MARKDOWN_CORE_NODE_HEADING:
        if (node->as.heading->label.len) {
            markdown_core_node_pool_bytes_free(pool, node->as.heading->label.data);
        }
        break;
    case MARKDOWN_CORE_NODE_SPECIMEN:
        markdown_core_optional_chunk_free(&node->as.specimen->label);
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        if (node->as.footnote->label.has_value) {
            markdown_core_node_pool_bytes_free(pool, node->as.footnote->label.value.data);
        }
        break;
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_EMBEDDED:
        markdown_core_resource_free(resources, node->as.link->resource);
        node->as.link->resource = NULL;
        if (node->as.link->label.len) {
            markdown_core_node_pool_bytes_free(pool, node->as.link->label.data);
        }
        break;
    case MARKDOWN_CORE_NODE_REFERENCE:
        markdown_core_resource_free(resources, node->as.reference->resource);
        node->as.reference->resource = NULL;
        if (node->as.reference->label.len) {
            markdown_core_node_pool_bytes_free(pool, node->as.reference->label.data);
        }
        break;
    default:
        break;
    }
    /* Give back only a record too large for the slot, whether construction,
     * a copy or a kind change installed it, as storage the node owns.
     * Pointer equality cannot establish ownership: such storage may lie
     * right after a slot. Almost no node owns one, so the release is entered
     * only when there is one. */
    if (node->node_data_allocation) {
        markdown_core_node_pool_bytes_free(pool, node->node_data_allocation);
        node->node_data_allocation = NULL;
    }
    node->as.data = NULL;
}

/* THE RELEASE WALK'S STACK is linked through the nodes' own scaffolds
 * (node.h): a node is pushed when the node holding it is released, so the
 * walk takes no storage and no C frame per level. */
static inline void S_push_release(markdown_core_node **stack, markdown_core_node *node) {
    markdown_core_scaffold_of(node)->next = *stack;
    *stack = node;
}

static int S_release_owned_subtree(markdown_core_node **slot, void *context) {
    if (*slot) {
        S_push_release(context, *slot);
        *slot = NULL;
    }
    return 1;
}

/* THE NODE-VALUED FIELDS OF A NODE'S RECORD, each slot visited whether or
 * not it holds a node. An element's own are visited by its
 * `visit_owned_subtrees_func`; `S_visit_fields` visits both. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) int S_visit_record_fields(
    markdown_core_node *node, markdown_core_owned_subtree_visitor visitor, void *context) {
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

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) int S_visit_fields(markdown_core_node *node,
                                                                          markdown_core_owned_subtree_visitor visitor,
                                                                          void *context) {
    return S_visit_record_fields(node, visitor, context) &&
           (!node->element || !node->element->visit_owned_subtrees_func ||
            node->element->visit_owned_subtrees_func(node->element, node, visitor, context));
}

/* A CANDIDATE LETS GO of the content buffer it borrows
 * (markdown_core_node_candidate) as its release begins, which then gives
 * back what the candidate still holds as it does for any node: exactly what
 * it owns. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_let_go(markdown_core_node *candidate) {
    markdown_core_strbuf_init_zeroed(&candidate->content);
}

/* Releases the nodes on `stack` and everything they hold. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_free_stack(markdown_core_node_pool *pool,
                                                                         markdown_core_node *stack) {
    while (stack) {
        markdown_core_node *e = stack;
        if (e->flags & MARKDOWN_CORE_NODE__BORROWS_CONTENT) {
            S_let_go(e);
        }
        /* Almost no node owns an attribute value or a content buffer: the
         * test each releaser makes first -- its own predicate, defined once
         * beside it -- is made here, so a node that owns neither pays the
         * compares and no call. */
        if (markdown_core_attributes_owns(&e->attributes)) {
            markdown_core_attributes_free(&e->attributes);
        }
        if (markdown_core_strbuf_owns(&e->content)) {
            markdown_core_strbuf_free(&e->content);
        }
        if (e->source.runs) {
            markdown_core_node_pool_bytes_free(pool, e->source.runs);
        }

        /* The node-valued fields join the same iterative free walk as
         * content: they are pushed onto the rest of the stack, which `e`'s
         * scaffold links until `e` leaves it. */
        S_visit_fields(e, S_release_owned_subtree, &markdown_core_scaffold_of(e)->next);
        stack = markdown_core_scaffold_next(e);
        if (e->opaque && e->element && e->element->opaque_free_func) {
            e->element->opaque_free_func(e->element, e);
        }
        free_node_as(pool, e);

        /* Its children: those of its array, and those an inline parse still
         * links through its scaffold. */
        for (uint32_t i = 0; i < e->child_count; i++) {
            S_push_release(&stack, e->children[i]);
        }
        if (e->children) {
            markdown_core_node_pool_bytes_free(pool, e->children);
        }
        for (markdown_core_node *child = markdown_core_scaffold_first(e); child;) {
            markdown_core_node *next = markdown_core_scaffold_next(child);
            S_push_release(&stack, child);
            child = next;
        }
        S_slot_release(pool, e);
    }
}

static void S_free_nodes(markdown_core_node_pool *pool, markdown_core_node *root) {
    markdown_core_scaffold_of(root)->next = NULL;
    S_free_stack(pool, root);
}

void markdown_core_node_pool_release(markdown_core_node_pool *pool, markdown_core_node *node) {
    if (MARKDOWN_CORE_NODE_TYPE_INLINE_P((markdown_core_node_type)node->kind)) {
        markdown_core_scaffold_unlink(node);
    }
    S_free_nodes(pool, node);
}

void markdown_core_node_free(markdown_core_node *node) { markdown_core_node_pool_release(NULL, node); }

/* SWAPPING TWO NODES moves every byte of their slots, so each record in a
 * slot moves with the node it belongs to, and each node's view of it is
 * pointed at the slot the node now lies in. The slots exchange their bytes
 * a block at a time, each block read from both before either is written,
 * the last block what is left of the slot. */
static inline void S_record_follow(markdown_core_node *node) {
    if (node->as.data && !node->node_data_allocation) {
        node->as.data = S_slot_of(node)->record;
    }
}

enum { S_SWAP_BLOCK = 96 };

void markdown_core_node_swap(markdown_core_node *a, markdown_core_node *b) {
    unsigned char *x = (unsigned char *)S_slot_of(a), *y = (unsigned char *)S_slot_of(b);
    for (size_t at = 0; at < sizeof(markdown_core_node_slot); at += S_SWAP_BLOCK) {
        const size_t left = sizeof(markdown_core_node_slot) - at, size = left < S_SWAP_BLOCK ? left : S_SWAP_BLOCK;
        unsigned char held[S_SWAP_BLOCK], taken[S_SWAP_BLOCK];
        memcpy(held, x + at, size);
        memcpy(taken, y + at, size);
        memcpy(x + at, taken, size);
        memcpy(y + at, held, size);
    }
    S_record_follow(a);
    S_record_follow(b);
}

/* WHAT A VIEW OR A CANDIDATE HOLDS OF ITS ORIGINAL'S. A view VIEWS each part
 * its original owns whose holder marks what it owns -- a chunk's `alloc`, an
 * attribute value's block and anchor, the element payload's own marks: it
 * holds the part and owns none of it, so releasing the view frees none of
 * it, and it writes a part of its own only where it lets the viewed one go
 * (markdown_core_chunk_view, markdown_core_attributes_view, opaque_view_func).
 * It copies what its kind owns with no mark: the labels a record holds in the
 * pool's storage, each at most a label's length, and its places, which its
 * completion replaces. A candidate copies what it viewed. Once a part cannot
 * be had, every later part is left owned by nobody, so releasing the view or
 * the candidate gives back exactly what it took. */
typedef struct {
    markdown_core_node_pool *pool;
    bool failed;
} S_copy_state;

/* The bytes `view` still reads of what `chunk` owns, copied. */
static void S_chunk_own(S_copy_state *state, markdown_core_chunk *view, const markdown_core_chunk *chunk) {
    if (!state->failed && !markdown_core_chunk_own(view, chunk)) {
        state->failed = true;
    }
}

/* A label in storage of the pool's, `length` bytes and a NUL, copied: false
 * when it could not be. */
static bool S_label_own(S_copy_state *state, markdown_core_chunk *label) {
    if (state->failed) {
        return false;
    }
    unsigned char *bytes = markdown_core_node_pool_bytes(state->pool, (size_t)label->len + 1);
    if (!bytes) {
        state->failed = true;
        return false;
    }
    memcpy(bytes, label->data, (size_t)label->len + 1);
    label->data = bytes;
    return true;
}

/* The parts of the record the kind owns, as free_node_as releases them: its
 * strings viewed and its labels copied. A view is of a block a snapshot
 * lends (markdown_core_node_view), and a candidate of an inline root
 * (markdown_core_node_candidate) -- a block's own, or a field root: a
 * paragraph, a caption or a directive's label, whose record owns nothing --:
 * of no kind made whole as it settles -- a Reference -- or held only by the
 * document, which holds its metadata committed once its prefix is read, and
 * of no kind whose record owns an inline's storage. */
static void S_view_node_as(S_copy_state *state, markdown_core_node *copy) {
    switch (copy->kind) {
    case MARKDOWN_CORE_NODE_CALLOUT:
        markdown_core_chunk_view(&copy->as.callout->variant.value);
        break;
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        markdown_core_chunk_view(&copy->as.list->task_marker.value);
        break;
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        markdown_core_chunk_view(&copy->as.code->info.value);
        markdown_core_chunk_view(&copy->as.code->literal);
        break;
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        markdown_core_chunk_view(copy->as.literal);
        break;
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        markdown_core_chunk_view(&copy->as.html_block->literal);
        break;
    case MARKDOWN_CORE_NODE_DOCUMENT:
        /* Its lookup tables are made as it is published, which no block the
         * block phase writes has been. */
        break;
    case MARKDOWN_CORE_NODE_HEADING:
        if (copy->as.heading->label.len && !S_label_own(state, &copy->as.heading->label)) {
            copy->as.heading->label = (markdown_core_chunk){0};
        }
        break;
    case MARKDOWN_CORE_NODE_SPECIMEN:
        markdown_core_chunk_view(&copy->as.specimen->label.value);
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        if (copy->as.footnote->label.has_value && !S_label_own(state, &copy->as.footnote->label.value)) {
            copy->as.footnote->label = markdown_core_optional_chunk_absent();
        }
        break;
    default:
        assert(copy->kind != MARKDOWN_CORE_NODE_METADATA && copy->kind != MARKDOWN_CORE_NODE_REFERENCE &&
               copy->kind != MARKDOWN_CORE_NODE_TEXT && copy->kind != MARKDOWN_CORE_NODE_HTML &&
               copy->kind != MARKDOWN_CORE_NODE_CODE && copy->kind != MARKDOWN_CORE_NODE_COMMENT &&
               copy->kind != MARKDOWN_CORE_NODE_CROSS_LINK && copy->kind != MARKDOWN_CORE_NODE_CROSS_EMBEDDED &&
               copy->kind != MARKDOWN_CORE_NODE_CITATION && copy->kind != MARKDOWN_CORE_NODE_LINK &&
               copy->kind != MARKDOWN_CORE_NODE_EMBEDDED);
        break;
    }
}

/* The strings the record of `copy` still views of `node`'s record, copied. */
static void S_own_node_as(S_copy_state *state, markdown_core_node *copy, const markdown_core_node *node) {
    switch (copy->kind) {
    case MARKDOWN_CORE_NODE_CALLOUT:
        S_chunk_own(state, &copy->as.callout->variant.value, &node->as.callout->variant.value);
        break;
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        S_chunk_own(state, &copy->as.list->task_marker.value, &node->as.list->task_marker.value);
        break;
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        S_chunk_own(state, &copy->as.code->info.value, &node->as.code->info.value);
        S_chunk_own(state, &copy->as.code->literal, &node->as.code->literal);
        break;
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        S_chunk_own(state, copy->as.literal, node->as.literal);
        break;
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        S_chunk_own(state, &copy->as.html_block->literal, &node->as.html_block->literal);
        break;
    case MARKDOWN_CORE_NODE_SPECIMEN:
        S_chunk_own(state, &copy->as.specimen->label.value, &node->as.specimen->label.value);
        break;
    default:
        break;
    }
}

/* A NODE'S SLOT, COPIED: a node in a slot of `pool` holding `node`'s state,
 * its record in storage of its own, and its scaffold empty. Everything else
 * it holds is still `node`'s, which the caller views, copies or lets go
 * before it can be released. NULL, having taken nothing, when storage
 * could not be had. */
static markdown_core_node *S_copy_slot(markdown_core_node_pool *pool, const markdown_core_node *node) {
    markdown_core_node_slot *slot = S_slot_take(pool);
    if (!slot) {
        return NULL;
    }
    memcpy(slot, S_slot_of(node), sizeof(*slot));
    memset(&slot->scaffold, 0, sizeof(slot->scaffold));
    markdown_core_node *copy = &slot->node.node;
    if (node->node_data_allocation) {
        const size_t size = S_node_payload_size((markdown_core_node_type)node->kind);
        copy->node_data_allocation = S_bytes_take(pool, size);
        if (!copy->node_data_allocation) {
            S_slot_release(pool, copy);
            return NULL;
        }
        memcpy(copy->node_data_allocation, node->node_data_allocation, size);
        copy->as.data = copy->node_data_allocation;
    } else {
        S_record_follow(copy);
    }
    return copy;
}

/* The parts a view holds of `node`'s: its attributes, its element's payload
 * and what its record owns, viewed, and its labels copied. */
static void S_view_owned(S_copy_state *state, markdown_core_node *copy, const markdown_core_node *node) {
    markdown_core_attributes_view(&copy->attributes);
    if (copy->opaque) {
        if (state->failed || !copy->element->opaque_view_func(copy->element, copy, node)) {
            copy->opaque = NULL;
            state->failed = true;
        }
    }
    S_view_node_as(state, copy);
}

/* A visit of a view's fields that lets each go, the field node of the node
 * it views in the same place released when it is another and `pool` is
 * given: fields of one kind sit at one offset from the storage that holds
 * them, the record or the element's payload. */
typedef struct {
    markdown_core_node_pool *pool;
    const unsigned char *storage, *original;
} S_field_pair;

static int S_let_go_field(markdown_core_node **slot, void *context) {
    const S_field_pair *pair = context;
    if (*slot && pair->pool &&
        (!pair->original ||
         *slot != *(markdown_core_node *const *)(pair->original + ((const unsigned char *)slot - pair->storage)))) {
        markdown_core_node_pool_release(pair->pool, *slot);
    }
    *slot = NULL;
    return 1;
}

void markdown_core_node_view_let_go(markdown_core_node_pool *pool, markdown_core_node *view,
                                    const markdown_core_node *node) {
    view->children = NULL;
    view->child_count = 0;
    markdown_core_strbuf_init_zeroed(&view->content);
    S_field_pair pair = {pool, view->as.data, node->as.data};
    S_visit_record_fields(view, S_let_go_field, &pair);
    if (view->element && view->element->visit_owned_subtrees_func) {
        pair = (S_field_pair){pool, view->opaque, node->opaque};
        view->element->visit_owned_subtrees_func(view->element, view, S_let_go_field, &pair);
    }
}

markdown_core_node *markdown_core_node_view(markdown_core_node_pool *pool, const markdown_core_node *node) {
    markdown_core_node *view = S_copy_slot(pool, node);
    if (!view) {
        return NULL;
    }
    S_copy_state state = {pool, false};
    view->source.places = NULL;
    view->flags &= (markdown_core_node_internal_flags)~MARKDOWN_CORE_NODE__PUBLISHED_OPEN;
    S_view_owned(&state, view, node);
    if (!(node->flags & MARKDOWN_CORE_NODE__PUBLISHED_OPEN) && node->source.places && !state.failed) {
        const size_t bytes =
            sizeof(markdown_core_places) + node->source.places->count * sizeof(markdown_core_source_place);
        view->source.places = markdown_core_node_pool_bytes(pool, bytes);
        if (view->source.places) {
            memcpy(view->source.places, node->source.places, bytes);
        } else {
            state.failed = true;
        }
    }
    if (state.failed) {
        markdown_core_node_view_let_go(NULL, view, node);
        markdown_core_node_pool_release(pool, view);
        return NULL;
    }
    return view;
}

markdown_core_node *markdown_core_node_candidate(markdown_core_node_pool *pool, const markdown_core_node *root) {
    assert(!root->opaque);
    markdown_core_node *copy = S_copy_slot(pool, root);
    if (!copy) {
        return NULL;
    }
    copy->id = 0;
    copy->place = (markdown_core_place){0, 0};
    copy->source.places = NULL;
    copy->children = NULL;
    copy->child_count = 0;
    if (markdown_core_strbuf_owns(&root->content)) {
        copy->flags |= MARKDOWN_CORE_NODE__BORROWS_CONTENT;
    }
    S_copy_state state = {pool, false};
    markdown_core_attributes_view(&copy->attributes);
    S_view_node_as(&state, copy);
    if (!state.failed && !markdown_core_attributes_own(&copy->attributes, &root->attributes)) {
        state.failed = true;
    }
    S_own_node_as(&state, copy, root);
    if (state.failed) {
        markdown_core_node_pool_release(pool, copy);
        return NULL;
    }
    return copy;
}

void markdown_core_node_take_borrowed(markdown_core_node *taker, markdown_core_node *lender) {
    const markdown_core_node_internal_flags borrowed = taker->flags & MARKDOWN_CORE_NODE__BORROWS_CONTENT;
    taker->flags &= (markdown_core_node_internal_flags)~borrowed;
    lender->flags |= borrowed;
}

markdown_core_node_set_kind_result markdown_core_node_set_kind(markdown_core_node *node, markdown_core_node *parent,
                                                               markdown_core_node_type kind) {
    markdown_core_node_type initial_kind = (markdown_core_node_type)node->kind;
    if (kind == initial_kind) {
        return MARKDOWN_CORE_NODE_SET_KIND_OK;
    }
    if (!parent || !markdown_core_node_can_contain_type(parent, kind)) {
        return MARKDOWN_CORE_NODE_SET_KIND_REJECTED;
    }

    /* Reserve any external replacement before releasing anything. A record
     * that fits already has storage in the node's slot; initializing it after
     * the old fields are destroyed cannot fail. Both cases keep the node's
     * address, and an allocation refusal preserves all old owned values. */
    size_t size = S_node_payload_size(kind);
    void *allocation = size > MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES ? S_bytes_take(NULL, size) : NULL;
    if (size > MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES && !allocation) {
        return MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED;
    }
    /* Kind conversion keeps the element's fields and the node's children;
     * the record's fields are freed by a walk of their own. */
    markdown_core_node *fields = NULL;
    S_visit_record_fields(node, S_release_owned_subtree, &fields);
    S_free_stack(NULL, fields);
    free_node_as(NULL, node);
    node->as.data = allocation ? allocation : size ? S_slot_of(node)->record : NULL;
    node->node_data_allocation = allocation;
    if (size) {
        memset(node->as.data, 0, size);
    }
    S_init_node_as(kind, &node->as);
    node->kind = (uint16_t)kind;
    /* An id names a node while its kind holds: a publish gives the node of
     * its new kind an id of its own. A snapshot's view of a block it lent is
     * a node the snapshot made once its kind changes (blocks.c, THE
     * LENDING). */
    node->id = 0;
    node->flags &= (markdown_core_node_internal_flags)~MARKDOWN_CORE_NODE__LENT;
    return MARKDOWN_CORE_NODE_SET_KIND_OK;
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

#define MARKDOWN_CORE_RESOURCE_SLAB_BYTES ((size_t)8 * 1024)

markdown_core_resource *markdown_core_resource_new(markdown_core_node_pool *pool, markdown_core_chunk url,
                                                   markdown_core_optional_chunk title) {
    markdown_core_resource *resource = (markdown_core_resource *)markdown_core_slab_take(
        pool ? &pool->resources : NULL, sizeof(markdown_core_resource), MARKDOWN_CORE_RESOURCE_SLAB_BYTES);
    if (!resource) {
        return NULL;
    }
    memset(resource, 0, sizeof(*resource));
    resource->url = url;
    resource->title = title;
    return resource;
}

void markdown_core_resource_free(markdown_core_slab_pool *resources, markdown_core_resource *resource) {
    if (!resource) {
        return;
    }
    markdown_core_chunk_free(&resource->url);
    markdown_core_optional_chunk_free(&resource->title);
    markdown_core_slab_release(resources, resource);
}

int markdown_core_node_set_element(markdown_core_node *node, const markdown_core_element *element) {
    if (node == NULL) {
        return 0;
    }
    node->element = element;
    return 1;
}

bool markdown_core_node_array_start(markdown_core_node_pool *pool, markdown_core_node ***items, uint32_t *count,
                                    markdown_core_node *node) {
    markdown_core_node **taken = S_bytes_take(pool, 2 * sizeof(*taken));
    if (!taken) {
        return false;
    }
    taken[0] = node;
    *items = taken;
    *count = 1;
    return true;
}

bool markdown_core_node_array_push_full(markdown_core_node_pool *pool, markdown_core_node ***items, uint32_t *count,
                                        markdown_core_node *node) {
    const uint32_t held = *count;
    if (held > UINT32_MAX / 2) {
        return false;
    }
    markdown_core_node **moved = S_bytes_take(pool, 2 * held * sizeof(*moved));
    if (!moved) {
        return false;
    }
    memcpy(moved, *items, held * sizeof(*moved));
    markdown_core_node_pool_bytes_free(pool, *items);
    moved[held] = node;
    *items = moved;
    *count = held + 1;
    return true;
}

bool markdown_core_node_kind_set_intersects(const markdown_core_node_kind_set *a,
                                            const markdown_core_node_kind_set *b) {
    return (a->blocks & b->blocks) != 0 || (a->inlines & b->inlines) != 0;
}
