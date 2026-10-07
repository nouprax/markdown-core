#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "config.h"
#include "node.h"
#include "references.h"
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
    case MARKDOWN_CORE_NODE_DEFINITION:
        return child_type == MARKDOWN_CORE_NODE_DEFINITION_BODY;
    case MARKDOWN_CORE_NODE_LIST:
        return child_type == MARKDOWN_CORE_NODE_LIST_ITEM;
    case MARKDOWN_CORE_NODE_CITE:
        return child_type == MARKDOWN_CORE_NODE_CITATION;

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

#define MARKDOWN_CORE_NODE_SLAB_BYTES ((size_t)64 * 1024)

static markdown_core_node_slot *S_slot_of(markdown_core_node *node) {
    return (markdown_core_node_slot *)((unsigned char *)node - offsetof(markdown_core_node_slot, node));
}

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
    markdown_core_slab_pool_dispose(&pool->members);
    markdown_core_bytes_pool_dispose(&pool->bytes);
}

void markdown_core_node_pool_bytes_free(markdown_core_node_pool *pool, void *storage) {
    markdown_core_bytes_release(pool ? &pool->bytes : NULL, storage);
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
    node->refs = 1;
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

/* THE BYTE STRINGS A NODE'S RECORD HOLDS, by kind, into `strings`; their
 * count. A resource holds its own (markdown_core_resource_new). */
#define NODE_STRING_LIMIT 3
static int node_strings(markdown_core_node *node, markdown_core_chunk *strings[NODE_STRING_LIMIT]) {
    switch (node->kind) {
    case MARKDOWN_CORE_NODE_CALLOUT:
        strings[0] = &node->as.callout->variant.value;
        return 1;
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        strings[0] = &node->as.list->task_marker.value;
        return 1;
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        strings[0] = &node->as.code->info.value;
        strings[1] = &node->as.code->literal;
        return 2;
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        strings[0] = node->as.literal;
        return 1;
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        strings[0] = &node->as.html_block->literal;
        return 1;
    case MARKDOWN_CORE_NODE_CROSS_LINK:
    case MARKDOWN_CORE_NODE_CROSS_EMBEDDED: {
        markdown_core_cross_reference *cross = markdown_core_node_cross_reference(node);
        strings[0] = &cross->path;
        strings[1] = &cross->anchor.value;
        strings[2] = &cross->label.value;
        return 3;
    }
    case MARKDOWN_CORE_NODE_CITATION:
        /* The note and the affixes are fields, which the release drops;
         * only the referent's bytes are the arm's. */
        strings[0] = &node->as.citation->value;
        return 1;
    case MARKDOWN_CORE_NODE_SPECIMEN:
        strings[0] = &node->as.specimen->label.value;
        return 1;
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        strings[0] = &node->as.footnote->label.value;
        return 1;
    default:
        return 0;
    }
}

/* A string a parse built may view the bytes of the input it read: a
 * content buffer, or a line. A node outlives that input once a later
 * revision shares it, so what a completed node reads is its own: a viewed
 * string becomes a copy, and an empty view the empty literal; an absent one
 * stays absent. False when the
 * copy could not be allocated, the view left as it was. */
static bool hold_string(markdown_core_chunk *string) {
    if (string->alloc) {
        return true;
    }
    if (!string->len) {
        if (string->data) {
            string->data = (unsigned char *)"";
        }
        return true;
    }
    return markdown_core_chunk_to_cstr(string) != NULL;
}

bool markdown_core_node_hold_strings(markdown_core_node *node) {
    markdown_core_chunk *strings[NODE_STRING_LIMIT];
    const int count = node_strings(node, strings);
    for (int i = 0; i < count; i++) {
        if (!hold_string(strings[i])) {
            return false;
        }
    }
    return true;
}

static void free_node_as(markdown_core_node_pool *pool, markdown_core_node *node) {
    markdown_core_slab_pool *resources = pool ? &pool->resources : NULL;
    markdown_core_chunk *strings[NODE_STRING_LIMIT];
    const int count = node_strings(node, strings);
    for (int i = 0; i < count; i++) {
        markdown_core_chunk_free(strings[i]);
    }
    switch (node->kind) {
    case MARKDOWN_CORE_NODE_METADATA:
        markdown_core_metadata_fields_free(node->as.metadata);
        break;
    case MARKDOWN_CORE_NODE_DOCUMENT:
        markdown_core_free((void *)node->as.document->footnotes.nodes);
        markdown_core_free((void *)node->as.document->footnotes.labeled);
        markdown_core_free((void *)node->as.document->specimens.nodes);
        markdown_core_free((void *)node->as.document->specimens.labeled);
        markdown_core_free((void *)node->as.document->references.nodes);
        markdown_core_free((void *)node->as.document->references.labeled);
        markdown_core_free((void *)node->as.document->reference_targets);
        break;
    case MARKDOWN_CORE_NODE_HEADING:
        if (node->as.heading->label.len) {
            markdown_core_node_pool_bytes_free(pool, node->as.heading->label.data);
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

/* THE NODE-VALUED FIELDS OF A NODE'S RECORD, each slot visited whether or
 * not it holds a node. An element's own are visited by its
 * `visit_owned_subtrees_func`; `markdown_core_node_visit_fields` visits both. */
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

int markdown_core_node_visit_fields(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor,
                                    void *context) {
    return S_visit_record_fields(node, visitor, context) &&
           (!node->element || !node->element->visit_owned_subtrees_func ||
            node->element->visit_owned_subtrees_func(node->element, node, visitor, context));
}

/* THE PENDING LISTS OF A RELEASE. A node whose last reference was dropped is
 * linked through its id, and a stem through its first word, neither of which
 * anything reads again. */
typedef struct {
    markdown_core_node *nodes;
    markdown_core_stem *stems;
} S_release_lists;

static void S_drop_node(S_release_lists *lists, markdown_core_node *node) {
    if (node && --node->refs == 0) {
        memcpy(&node->id, &lists->nodes, sizeof(lists->nodes));
        lists->nodes = node;
    }
}

/* The first word of a stem, its count of references and of nodes, holds a
 * pointer once it is released. */
typedef char S_stem_link_fits[sizeof(markdown_core_stem *) <= offsetof(markdown_core_stem, height) ? 1 : -1];

/* The link a released stem holds in its first word. */
static markdown_core_stem *S_stem_link(const markdown_core_stem *stem) {
    markdown_core_stem *next;
    memcpy(&next, (const void *)stem, sizeof(next));
    return next;
}

static void S_link_stem(S_release_lists *lists, markdown_core_stem *stem) {
    memcpy((void *)stem, &lists->stems, sizeof(lists->stems));
    lists->stems = stem;
}

static void S_drop_stem(S_release_lists *lists, markdown_core_stem *stem) {
    if (stem && --stem->refs == 0) {
        S_link_stem(lists, stem);
    }
}

static int S_drop_field(markdown_core_node **slot, void *context) {
    S_drop_node(context, *slot);
    *slot = NULL;
    return 1;
}

/* Releases the node's own storage: everything but its children and fields,
 * which the caller dropped. */
static void S_release_value(markdown_core_node_pool *pool, markdown_core_node *e) {
    /* Almost no node owns an attribute value or a content buffer: the test
     * each releaser makes first -- its own predicate, defined once beside it
     * -- is made here, so a node that owns neither pays the compares and no
     * call. */
    if (markdown_core_attributes_owns(&e->attributes)) {
        markdown_core_attributes_free(&e->attributes);
    }
    if (markdown_core_strbuf_owns(&e->content)) {
        markdown_core_strbuf_free(&e->content);
    }
    if (e->runs) {
        markdown_core_node_pool_bytes_free(pool, e->runs);
    }
    if (e->opaque && e->element && e->element->opaque_free_func) {
        e->element->opaque_free_func(e->element, e);
    }
    free_node_as(pool, e);
}

static size_t S_release(markdown_core_node_pool *pool, S_release_lists *lists) {
    size_t released = 0;
    while (lists->nodes || lists->stems) {
        if (lists->stems) {
            markdown_core_stem *stem = lists->stems;
            lists->stems = S_stem_link(stem);
            for (uint8_t i = 0; i < stem->width; i++) {
                if (stem->height) {
                    S_drop_stem(lists, stem->entries[i].stem);
                } else {
                    S_drop_node(lists, stem->entries[i].node);
                }
            }
            markdown_core_node_pool_bytes_free(pool, stem);
            continue;
        }
        markdown_core_node *node = lists->nodes;
        memcpy(&lists->nodes, &node->id, sizeof(lists->nodes));
        released++;
        markdown_core_node_visit_fields(node, S_drop_field, lists);
        S_drop_stem(lists, node->children);
        node->children = NULL;
        S_release_value(pool, node);
        S_slot_release(pool, node);
    }
    return released;
}

size_t markdown_core_node_pool_release(markdown_core_node_pool *pool, markdown_core_node *node) {
    S_release_lists lists = {NULL, NULL};
    S_drop_node(&lists, node);
    return S_release(pool, &lists);
}

size_t markdown_core_node_release(markdown_core_node *node) { return markdown_core_node_pool_release(NULL, node); }

void markdown_core_node_free(markdown_core_node *node) { (void)markdown_core_node_release(node); }

/* THE CHILDREN TREE. */

static markdown_core_stem *S_stem_new(markdown_core_node_pool *pool, uint8_t height, size_t width) {
    markdown_core_stem *stem = markdown_core_node_pool_bytes(pool, offsetof(markdown_core_stem, entries) +
                                                                       width * sizeof(markdown_core_stem_entry));
    if (stem) {
        stem->refs = 1;
        stem->count = 0;
        stem->height = height;
        stem->width = (uint8_t)width;
    }
    return stem;
}

/* `count` entries, split as evenly as stems of at most the width allow, so a
 * level of `count` entries has the fewest stems and every stem of it holds
 * within one entry of every other. */
static size_t S_stem_level_width(size_t count) {
    return (count + MARKDOWN_CORE_STEM_WIDTH - 1) / MARKDOWN_CORE_STEM_WIDTH;
}

markdown_core_stem *markdown_core_stem_make(markdown_core_node_pool *pool, markdown_core_node *const *nodes,
                                            size_t count, bool *failed) {
    *failed = false;
    if (!count) {
        return NULL;
    }
    /* The levels are built bottom up, each level's stems written over the
     * front of one array: a level never has more stems than entries below. */
    size_t stems = S_stem_level_width(count);
    markdown_core_stem **level = markdown_core_alloc(stems, sizeof(*level));
    if (!level) {
        *failed = true;
        return NULL;
    }
    size_t built = 0;
    for (size_t i = 0, from = 0; i < stems; i++) {
        size_t width = (count - from) / (stems - i);
        markdown_core_stem *leaf = S_stem_new(pool, 0, width);
        if (!leaf) {
            goto failed;
        }
        for (size_t j = 0; j < width; j++) {
            leaf->entries[j].node = nodes[from + j];
        }
        leaf->count = (uint32_t)width;
        level[built++] = leaf;
        from += width;
    }
    for (uint8_t height = 1; built > 1; height++) {
        size_t above = S_stem_level_width(built);
        size_t made = 0;
        for (size_t i = 0, from = 0; i < above; i++) {
            size_t width = (built - from) / (above - i);
            markdown_core_stem *stem = S_stem_new(pool, height, width);
            if (!stem) {
                /* The stems not yet taken into this level are released
                 * below, after the ones it made. */
                for (size_t j = from; j < built; j++) {
                    level[made + j - from] = level[j];
                }
                built = made + built - from;
                goto failed;
            }
            for (size_t j = 0; j < width; j++) {
                stem->entries[j].stem = level[from + j];
                stem->count += level[from + j]->count;
            }
            level[made++] = stem;
            from += width;
        }
        built = made;
    }
    markdown_core_stem *root = level[0];
    markdown_core_free(level);
    return root;

failed:
    /* Nothing was taken: the stems built so far go, and the nodes stay the
     * caller's. */
    {
        S_release_lists lists = {NULL, NULL};
        for (size_t i = 0; i < built; i++) {
            S_link_stem(&lists, level[i]);
        }
        while (lists.stems) {
            markdown_core_stem *stem = lists.stems;
            lists.stems = S_stem_link(stem);
            for (uint8_t j = 0; stem->height && j < stem->width; j++) {
                S_link_stem(&lists, stem->entries[j].stem);
            }
            markdown_core_node_pool_bytes_free(pool, stem);
        }
    }
    markdown_core_free(level);
    *failed = true;
    return NULL;
}

markdown_core_node *markdown_core_stem_at(const markdown_core_stem *stem, size_t index) {
    while (stem->height) {
        uint8_t i = 0;
        while (index >= stem->entries[i].stem->count) {
            index -= stem->entries[i].stem->count;
            i++;
        }
        stem = stem->entries[i].stem;
    }
    return stem->entries[index].node;
}

markdown_core_node *markdown_core_stem_put(markdown_core_stem *stem, size_t index, markdown_core_node *node) {
    assert(stem->refs == 1);
    while (stem->height) {
        uint8_t i = 0;
        while (index >= stem->entries[i].stem->count) {
            index -= stem->entries[i].stem->count;
            i++;
        }
        stem = stem->entries[i].stem;
        assert(stem->refs == 1);
    }
    markdown_core_node *held = stem->entries[index].node;
    stem->entries[index].node = node;
    return held;
}

void markdown_core_stem_walk_begin(markdown_core_stem_walk *walk, const markdown_core_stem *stem, size_t index,
                                   size_t count) {
    walk->left = stem ? count : 0;
    if (!walk->left) {
        return;
    }
    for (;;) {
        uint8_t level = stem->height;
        walk->path[level] = stem;
        if (!level) {
            walk->at[0] = (uint8_t)index;
            return;
        }
        uint8_t i = 0;
        while (index >= stem->entries[i].stem->count) {
            index -= stem->entries[i].stem->count;
            i++;
        }
        walk->at[level] = i;
        stem = stem->entries[i].stem;
    }
}

markdown_core_node *markdown_core_stem_walk_next(markdown_core_stem_walk *walk) {
    if (!walk->left) {
        return NULL;
    }
    markdown_core_node *node = walk->path[0]->entries[walk->at[0]].node;
    if (--walk->left) {
        /* Up to the lowest stem with an entry after the path's, then down
         * its first entries to a leaf. */
        uint8_t level = 0;
        while (walk->at[level] + 1 == walk->path[level]->width) {
            level++;
        }
        walk->at[level]++;
        while (level) {
            const markdown_core_stem *below = walk->path[level]->entries[walk->at[level]].stem;
            level--;
            walk->path[level] = below;
            walk->at[level] = 0;
        }
    }
    return node;
}

/* THE BUILDERS. */

#define MARKDOWN_CORE_MEMBER_SLAB_BYTES ((size_t)16 * 1024)

markdown_core_member *markdown_core_member_new(markdown_core_node_pool *pool, markdown_core_node *node, bool held) {
    markdown_core_member *member =
        markdown_core_slab_take(pool ? &pool->members : NULL, sizeof(*member), MARKDOWN_CORE_MEMBER_SLAB_BYTES);
    if (member) {
        *member = (markdown_core_member){.node = node, .held = held};
    }
    return member;
}

void markdown_core_member_attach(markdown_core_member *owner, markdown_core_member *child,
                                 markdown_core_member *before) {
    assert(owner && child && owner != child);
    assert(!child->owner && !child->prev && !child->next);
    assert(!before || before->owner == owner);
    /* Built-in containment is pure and shares its rules with checked
     * construction. Dynamic policies were decided before; never replay them. */
    assert((owner->node->element && owner->node->element->can_contain_func) ||
           markdown_core_node_can_contain_builtin(owner->node, (markdown_core_node_type)child->node->kind));
    markdown_core_member *previous = before ? before->prev : owner->last;
    child->owner = owner;
    child->inner = owner->inner;
    child->prev = previous;
    child->next = before;
    if (previous) {
        previous->next = child;
    } else {
        owner->first = child;
    }
    if (before) {
        before->prev = child;
    } else {
        owner->last = child;
    }
}

void markdown_core_member_attach_field(markdown_core_member *owner, markdown_core_member *field) {
    assert(!field->owner && !field->next);
    field->owner = owner;
    field->field = true;
    field->inner = owner->inner;
    markdown_core_member **at = &owner->fields;
    while (*at) {
        at = &(*at)->next;
    }
    *at = field;
}

void markdown_core_member_unlink(markdown_core_member *member) {
    markdown_core_member *owner = member->owner;
    if (member->field) {
        markdown_core_member **at = &owner->fields;
        while (*at != member) {
            at = &(*at)->next;
        }
        *at = member->next;
        member->field = false;
    } else {
        if (member->prev) {
            member->prev->next = member->next;
        } else if (owner) {
            owner->first = member->next;
        }
        if (member->next) {
            member->next->prev = member->prev;
        } else if (owner) {
            owner->last = member->prev;
        }
    }
    member->owner = member->prev = member->next = NULL;
}

static void S_member_free(markdown_core_node_pool *pool, markdown_core_member *member) {
    markdown_core_slab_release(pool ? &pool->members : NULL, member);
}

bool markdown_core_member_freeze(markdown_core_node_pool *pool, markdown_core_member *member) {
    size_t count = 0;
    for (const markdown_core_member *child = member->first; child; child = child->next) {
        count += child->held;
    }
    markdown_core_node *node = member->node;
    assert(!node->children || !count);
    if (!count) {
        return true;
    }
    markdown_core_node *small[MARKDOWN_CORE_STEM_WIDTH];
    markdown_core_node **nodes = count <= MARKDOWN_CORE_STEM_WIDTH ? small : markdown_core_alloc(count, sizeof(*nodes));
    if (!nodes) {
        return false;
    }
    size_t i = 0;
    for (const markdown_core_member *child = member->first; child; child = child->next) {
        if (child->held) {
            nodes[i++] = child->node;
        }
    }
    bool failed;
    node->children = markdown_core_stem_make(pool, nodes, count, &failed);
    if (nodes != small) {
        markdown_core_free(nodes);
    }
    if (failed) {
        return false;
    }
    for (markdown_core_member *child = member->first; child; child = child->next) {
        child->held = false;
    }
    return true;
}

void markdown_core_member_release(markdown_core_node_pool *pool, markdown_core_member *member) {
    assert(!member->owner);
    /* A pending list threaded through `next`: each member taken from it puts
     * its children and its field roots on it before it goes. */
    member->next = NULL;
    markdown_core_member *pending = member;
    while (pending) {
        markdown_core_member *taken = pending;
        pending = taken->next;
        if (taken->first) {
            taken->last->next = pending;
            pending = taken->first;
        }
        if (taken->fields) {
            markdown_core_member *tail = taken->fields;
            while (tail->next) {
                tail = tail->next;
            }
            tail->next = pending;
            pending = taken->fields;
        }
        if (taken->held) {
            markdown_core_node_pool_release(pool, taken->node);
        }
        S_member_free(pool, taken);
    }
}

markdown_core_node_set_kind_result markdown_core_node_set_kind(markdown_core_node *node, markdown_core_node *owner,
                                                               markdown_core_node_type kind) {
    markdown_core_node_type initial_kind = (markdown_core_node_type)node->kind;
    if (kind == initial_kind) {
        return MARKDOWN_CORE_NODE_SET_KIND_OK;
    }
    if (!owner || !markdown_core_node_can_contain_type(owner, kind)) {
        return MARKDOWN_CORE_NODE_SET_KIND_REJECTED;
    }

    /* Reserve any external replacement before releasing anything. A record
     * that fits already has storage in the node's slot; initializing it after
     * the old fields are destroyed cannot fail. Both cases keep the node's
     * address, and an allocation refusal preserves all old owned values. */
    size_t size = S_node_payload_size(kind);
    void *allocation = size > MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES ? markdown_core_alloc(1, size) : NULL;
    if (size > MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES && !allocation) {
        return MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED;
    }
    /* Kind conversion keeps the element's fields; the record's are dropped. */
    S_release_lists lists = {NULL, NULL};
    S_visit_record_fields(node, S_drop_field, &lists);
    S_release(NULL, &lists);
    free_node_as(NULL, node);
    node->as.data = allocation ? allocation : size ? S_slot_of(node)->record : NULL;
    node->node_data_allocation = allocation;
    if (!allocation && size) {
        memset(node->as.data, 0, size);
    }
    S_init_node_as(kind, &node->as);
    node->kind = (uint16_t)kind;
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

const char *markdown_core_node_get_string_content(markdown_core_node *node) { return (char *)node->content.ptr; }

int markdown_core_node_set_string_content(markdown_core_node *node, const char *content) {
    markdown_core_strbuf_sets(&node->content, content);
    return true;
}

#define MARKDOWN_CORE_RESOURCE_SLAB_BYTES ((size_t)8 * 1024)

markdown_core_resource *markdown_core_resource_new(markdown_core_node_pool *pool, markdown_core_chunk url,
                                                   markdown_core_optional_chunk title) {
    markdown_core_resource *resource = (markdown_core_resource *)markdown_core_slab_take(
        pool ? &pool->resources : NULL, sizeof(markdown_core_resource), MARKDOWN_CORE_RESOURCE_SLAB_BYTES);
    if (!resource) {
        return NULL;
    }
    /* The resource holds what it reads (hold_string). */
    markdown_core_chunk held = url;
    markdown_core_chunk held_title = title.value;
    if (!hold_string(&held) || !hold_string(&held_title)) {
        if (held.alloc && !url.alloc) {
            markdown_core_chunk_free(&held);
        }
        markdown_core_slab_release(pool ? &pool->resources : NULL, resource);
        return NULL;
    }
    memset(resource, 0, sizeof(*resource));
    resource->url = held;
    resource->title = title;
    resource->title.value = held_title;
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

static void S_print_error(FILE *out, markdown_core_node *node, const char *elem) {
    if (out == NULL) {
        return;
    }
    fprintf(out, "Invalid '%s' in node type %s (id %llu)\n", elem, markdown_core_node_get_type_string(node),
            (unsigned long long)node->id);
}

/* The errors of a node's stem: a stem that holds no entry or no reference,
 * one whose entries are not all one height below it, and one that miscounts
 * the nodes under it. The walk's path is no deeper than the stem's height. */
static int S_check_stem(const markdown_core_node *owner, const markdown_core_stem *stem, FILE *out) {
    const markdown_core_stem *path[MARKDOWN_CORE_STEM_HEIGHT + 1];
    uint8_t at[MARKDOWN_CORE_STEM_HEIGHT + 1];
    int errors = 0;
    if (stem->height >= MARKDOWN_CORE_STEM_HEIGHT) {
        S_print_error(out, (markdown_core_node *)owner, "stem height");
        return 1;
    }
    size_t depth = 0;
    path[0] = stem;
    at[0] = 0;
    for (;;) {
        const markdown_core_stem *top = path[depth];
        if (at[depth] == 0) {
            size_t count = 0;
            if (!top->width || !top->refs) {
                S_print_error(out, (markdown_core_node *)owner, "stem");
                return errors + 1;
            }
            for (uint8_t i = 0; i < top->width; i++) {
                if (top->height) {
                    if (top->entries[i].stem->height + 1 != top->height) {
                        S_print_error(out, (markdown_core_node *)owner, "stem balance");
                        return errors + 1;
                    }
                    count += top->entries[i].stem->count;
                } else {
                    count++;
                }
            }
            if (count != top->count) {
                S_print_error(out, (markdown_core_node *)owner, "stem count");
                errors++;
            }
        }
        if (!top->height || at[depth] == top->width) {
            if (!depth) {
                return errors;
            }
            depth--;
            continue;
        }
        path[depth + 1] = top->entries[at[depth]++].stem;
        at[++depth] = 0;
    }
}

/* THE WORK STACK of the self-check: the nodes it has still to check. */
typedef struct {
    markdown_core_node **nodes;
    size_t count, capacity;
} S_check_stack;

static bool S_check_push(S_check_stack *stack, markdown_core_node *node) {
    if (stack->count == stack->capacity) {
        size_t grown = stack->capacity * 2;
        markdown_core_node **more = markdown_core_realloc(stack->nodes, grown * sizeof(*more));
        if (!more) {
            return false;
        }
        stack->nodes = more;
        stack->capacity = grown;
    }
    stack->nodes[stack->count++] = node;
    return true;
}

static int S_check_field(markdown_core_node **root_slot, void *context) {
    return !*root_slot || S_check_push(context, *root_slot);
}

/* THE STRUCTURAL SELF-CHECK of a tree: every stem sound and every node held,
 * through its children and its node-valued fields alike. The count of faults
 * it found, or -1 when its work stack could not be allocated. */
int markdown_core_node_check(markdown_core_node *node, FILE *out) {
    int errors = 0;
    if (!node) {
        return 0;
    }
    S_check_stack stack = {markdown_core_alloc(64, sizeof(markdown_core_node *)), 0, 64};
    if (!stack.nodes || !S_check_push(&stack, node)) {
        markdown_core_free(stack.nodes);
        return -1;
    }
    while (stack.count) {
        markdown_core_node *current = stack.nodes[--stack.count];
        if (!current->refs) {
            S_print_error(out, current, "refs");
            errors++;
        }
        if (!markdown_core_node_visit_fields(current, S_check_field, &stack)) {
            markdown_core_free(stack.nodes);
            return -1;
        }
        if (!current->children) {
            continue;
        }
        errors += S_check_stem(current, current->children, out);
        markdown_core_stem_walk walk;
        markdown_core_stem_walk_begin(&walk, current->children, 0, current->children->count);
        for (markdown_core_node *child; (child = markdown_core_stem_walk_next(&walk));) {
            if (!S_check_push(&stack, child)) {
                markdown_core_free(stack.nodes);
                return -1;
            }
        }
    }
    markdown_core_free(stack.nodes);
    return errors;
}

bool markdown_core_node_kind_set_intersects(const markdown_core_node_kind_set *a,
                                            const markdown_core_node_kind_set *b) {
    return (a->blocks & b->blocks) != 0 || (a->inlines & b->inlines) != 0;
}
