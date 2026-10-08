#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "config.h"
#include "node.h"
#include "registry.h"
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
    markdown_core_registry_dispose(&pool->registry);
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
 * string becomes a copy in the pool's storage, and an empty view the empty
 * literal; an absent one stays absent. False when the copy could not be
 * allocated, the view left as it was. */
static bool hold_string(markdown_core_node_pool *pool, markdown_core_chunk *string) {
    if (string->alloc) {
        return true;
    }
    if (!string->len) {
        if (string->data) {
            string->data = (unsigned char *)"";
        }
        return true;
    }
    unsigned char *bytes = markdown_core_node_pool_bytes(pool, (size_t)string->len + 1);
    if (!bytes) {
        return false;
    }
    memcpy(bytes, string->data, (size_t)string->len);
    bytes[string->len] = '\0';
    string->data = bytes;
    string->alloc = 1;
    return true;
}

bool markdown_core_node_hold_strings(markdown_core_node_pool *pool, markdown_core_node *node) {
    markdown_core_chunk *strings[NODE_STRING_LIMIT];
    const int count = node_strings(node, strings);
    for (int i = 0; i < count; i++) {
        if (!hold_string(pool, strings[i])) {
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
        if (strings[i]->alloc) {
            markdown_core_node_pool_bytes_free(pool, strings[i]->data);
        }
        *strings[i] = (markdown_core_chunk){0};
    }
    switch (node->kind) {
    case MARKDOWN_CORE_NODE_METADATA:
        markdown_core_metadata_fields_free(node->as.metadata);
        break;
    case MARKDOWN_CORE_NODE_DOCUMENT:
        for (size_t i = 0; i < MARKDOWN_CORE_ROSTER_COUNT; i++) {
            markdown_core_roster_release(node->as.document->rosters[i]);
        }
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
    if (e->facts) {
        markdown_core_registry_unlink(e);
    }
    markdown_core_order_free(e->order);
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
    if (e->lines) {
        markdown_core_node_pool_bytes_free(pool, e->lines);
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

/* A COPY'S OWN BYTES. Each makes the copy's member, which views the bytes of
 * the node it copies, its own; when it cannot, it leaves the member owning
 * nothing, so releasing the copy frees nothing of the node's, and fails. */

/* A label in storage of the pool's (markdown_core_node_pool_bytes),
 * NUL-terminated. */
static bool S_copy_label(markdown_core_node_pool *pool, markdown_core_chunk *label) {
    if (!label->len) {
        return true;
    }
    unsigned char *bytes = markdown_core_node_pool_bytes(pool, (size_t)label->len + 1);
    if (!bytes) {
        *label = (markdown_core_chunk){0};
        return false;
    }
    memcpy(bytes, label->data, (size_t)label->len);
    bytes[label->len] = '\0';
    label->data = bytes;
    return true;
}

/* A resource, which the copy holds its own of. */
static bool S_copy_resource(markdown_core_node_pool *pool, markdown_core_resource **resource) {
    if (!*resource) {
        return true;
    }
    const markdown_core_resource *from = *resource;
    markdown_core_optional_chunk title = from->title;
    title.value.alloc = 0;
    markdown_core_chunk url = from->url;
    url.alloc = 0;
    *resource = markdown_core_resource_new(pool, url, title);
    return *resource != NULL;
}

static int S_retain_field(markdown_core_node **slot, void *context) {
    (void)context;
    if (*slot) {
        markdown_core_node_retain(*slot);
    }
    return 1;
}

markdown_core_node *markdown_core_node_copy(markdown_core_node_pool *pool, const markdown_core_node *node) {
    assert(node->kind != MARKDOWN_CORE_NODE_DOCUMENT && node->kind != MARKDOWN_CORE_NODE_METADATA);
    markdown_core_node *copy = markdown_core_node_pool_new(pool, (markdown_core_node_type)node->kind, node->element);
    if (!copy) {
        return NULL;
    }
    copy->flags = node->flags;
    copy->id = node->id;
    copy->entry = node->entry;
    copy->reach = node->reach;
    copy->where = node->where;
    copy->internal_offset = node->internal_offset;
    copy->children = markdown_core_stem_retain(node->children);
    /* It takes the node's order, with its facts, as it takes its place
     * (markdown_core_registry_move). */
    copy->first = node->first;
    const size_t payload = S_node_payload_size((markdown_core_node_type)node->kind);
    if (payload) {
        memcpy(copy->as.data, node->as.data, payload);
    }
    bool ok = true;
    markdown_core_chunk *strings[NODE_STRING_LIMIT];
    const int count = node_strings(copy, strings);
    for (int i = 0; i < count; i++) {
        ok &= markdown_core_chunk_own(strings[i]);
    }
    switch (copy->kind) {
    case MARKDOWN_CORE_NODE_HEADING:
        ok &= S_copy_label(pool, &copy->as.heading->label);
        break;
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_EMBEDDED:
        ok &= S_copy_resource(pool, &copy->as.link->resource);
        ok &= S_copy_label(pool, &copy->as.link->label);
        break;
    case MARKDOWN_CORE_NODE_REFERENCE:
        ok &= S_copy_resource(pool, &copy->as.reference->resource);
        ok &= S_copy_label(pool, &copy->as.reference->label);
        break;
    default:
        break;
    }
    if (copy->element && copy->element->opaque_copy_func) {
        ok &= copy->element->opaque_copy_func(copy->element, node, copy);
    }
    markdown_core_node_visit_fields(copy, S_retain_field, NULL);
    ok &= markdown_core_attributes_copy(&copy->attributes, &node->attributes);
    if (node->content.size) {
        markdown_core_strbuf_set(&copy->content, node->content.ptr, node->content.size);
        ok &= !copy->content.oom;
    }
    if (node->runs) {
        const size_t bytes = markdown_core_runs_size(node->runs->capacity, node->runs->pieces);
        copy->runs = markdown_core_node_pool_bytes(pool, bytes);
        if (copy->runs) {
            memcpy(copy->runs, node->runs, bytes);
        }
        ok &= copy->runs != NULL;
    }
    if (node->lines) {
        const size_t bytes = offsetof(markdown_core_lines, items) + node->lines->capacity * sizeof(markdown_core_line);
        copy->lines = markdown_core_node_pool_bytes(pool, bytes);
        if (copy->lines) {
            memcpy(copy->lines, node->lines, bytes);
        }
        ok &= copy->lines != NULL;
    }
    if (!ok) {
        markdown_core_node_pool_release(pool, copy);
        return NULL;
    }
    return copy;
}

/* THE CHILDREN TREE. */

static markdown_core_stem *S_stem_new(markdown_core_node_pool *pool, uint8_t height, size_t width) {
    markdown_core_stem *stem = markdown_core_node_pool_bytes(pool, offsetof(markdown_core_stem, entries) +
                                                                       width * sizeof(markdown_core_stem_entry));
    if (stem) {
        stem->refs = 1;
        stem->count = 0;
        stem->height = height;
        stem->width = (uint8_t)width;
        stem->marks = 0;
        stem->summary = 0;
        stem->length = stem->far = 0;
        stem->first = NULL;
    }
    return stem;
}

/* Sets what `stem` measures (MARKDOWN_CORE_STEM_FRESH) from its entries. A
 * leaf keeps whether it is fresh; a higher stem is fresh when a stem it
 * holds is. */
static void S_stem_measure(markdown_core_stem *stem) {
    uint8_t marks = stem->height ? 0 : stem->marks & MARKDOWN_CORE_STEM_FRESH;
    int64_t length = 0, far = INT64_MIN;
    markdown_core_order *first = NULL;
    for (uint8_t i = 0; i < stem->width; i++) {
        const markdown_core_stem_entry entry = stem->entries[i];
        int64_t end, reach;
        if (!first) {
            first = stem->height ? entry.stem->first : entry.node->first;
        }
        if (stem->height) {
            marks |= entry.stem->marks;
            end = length + entry.stem->length;
            reach = entry.stem->far;
        } else {
            const markdown_core_node *node = entry.node;
            marks |= (node->flags & MARKDOWN_CORE_NODE__GROUP ? MARKDOWN_CORE_STEM_GROUP : 0) |
                     (node->flags & MARKDOWN_CORE_NODE__HOLDS_NEXT ? 0 : MARKDOWN_CORE_STEM_FREE);
            end = length + node->where.extent.lead + (int64_t)node->where.extent.span;
            reach = node->reach;
        }
        length = end;
        if (end + reach > far) {
            far = end + reach;
        }
    }
    stem->marks = marks;
    stem->length = length;
    stem->far = far - length;
    stem->first = first;
}

/* Sets the count and the `summary` word of `stem` from its entries. */
static void S_stem_count(markdown_core_stem *stem, const markdown_core_stem_summary *summary) {
    stem->count = 0;
    for (uint8_t i = 0; i < stem->width; i++) {
        const markdown_core_stem_entry entry = stem->entries[i];
        stem->count += stem->height ? entry.stem->count : 1;
        if (summary) {
            const uint64_t word = stem->height ? entry.stem->summary : summary->of(entry.node);
            stem->summary = i ? summary->combine(stem->summary, word) : word;
        }
    }
}

/* Sets the count and the `summary` word of `stem` from its entries, and
 * what it measures. */
static void S_stem_sum(markdown_core_stem *stem, const markdown_core_stem_summary *summary) {
    S_stem_count(stem, summary);
    S_stem_measure(stem);
}

void markdown_core_stem_measure(markdown_core_stem *stem) {
    if (!stem || !(stem->marks & MARKDOWN_CORE_STEM_FRESH)) {
        return;
    }
    /* Bottom up over the fresh stems: each is measured once the fresh ones
     * it holds are. */
    markdown_core_stem *path[MARKDOWN_CORE_STEM_HEIGHT];
    uint8_t at[MARKDOWN_CORE_STEM_HEIGHT];
    size_t depth = 0;
    path[0] = stem;
    at[0] = 0;
    for (;;) {
        markdown_core_stem *top = path[depth];
        if (top->height && at[depth] < top->width) {
            markdown_core_stem *below = top->entries[at[depth]++].stem;
            if (below->marks & MARKDOWN_CORE_STEM_FRESH) {
                path[++depth] = below;
                at[depth] = 0;
            }
            continue;
        }
        top->marks &= (uint8_t)~MARKDOWN_CORE_STEM_FRESH;
        S_stem_measure(top);
        if (!depth--) {
            return;
        }
    }
}

/* `count` entries, split as evenly as stems of at most the width allow, so a
 * level of `count` entries has the fewest stems and every stem of it holds
 * within one entry of every other. */
static size_t S_stem_level_width(size_t count) {
    return (count + MARKDOWN_CORE_STEM_WIDTH - 1) / MARKDOWN_CORE_STEM_WIDTH;
}

markdown_core_stem *markdown_core_stem_make(markdown_core_node_pool *pool, markdown_core_node *const *nodes,
                                            size_t count, const markdown_core_stem_summary *summary, bool *failed) {
    *failed = false;
    if (!count) {
        return NULL;
    }
    /* Level h holds `entries[h]` entries in `stems[h]` stems; the top level
     * has one. The levels are built bottom up as the nodes come, each with
     * one open stem, which closes at `width[h]` entries: the `made[h]`th stem of the
     * level takes an even share of the entries `from[h]` on, so every stem
     * of a level holds within one entry of every other. */
    size_t entries[MARKDOWN_CORE_STEM_HEIGHT], stems[MARKDOWN_CORE_STEM_HEIGHT];
    size_t made[MARKDOWN_CORE_STEM_HEIGHT], from[MARKDOWN_CORE_STEM_HEIGHT];
    markdown_core_stem *open[MARKDOWN_CORE_STEM_HEIGHT];
    uint8_t width[MARKDOWN_CORE_STEM_HEIGHT];
    uint8_t top = 0;
    entries[0] = count;
    while ((stems[top] = S_stem_level_width(entries[top])) > 1) {
        entries[top + 1] = stems[top];
        top++;
    }
    for (uint8_t h = 0; h <= top; h++) {
        made[h] = from[h] = 0;
        open[h] = NULL;
    }
    /* A stem closed but not yet held by the level above. */
    markdown_core_stem *loose = NULL;
    for (size_t i = 0; i < count; i++) {
        markdown_core_stem_entry entry = {.node = nodes[i]};
        for (uint8_t h = 0;; h++) {
            markdown_core_stem *stem = open[h];
            if (!stem) {
                width[h] = (uint8_t)((entries[h] - from[h]) / (stems[h] - made[h]));
                stem = open[h] = S_stem_new(pool, h, width[h]);
                if (!stem) {
                    loose = h ? entry.stem : NULL;
                    goto failed;
                }
                /* Its entries are measured once their owner numbers the
                 * nodes under them. */
                stem->marks = MARKDOWN_CORE_STEM_FRESH;
                stem->width = 0;
                from[h] += width[h];
                made[h]++;
            }
            stem->entries[stem->width++] = entry;
            if (stem->width < width[h]) {
                break;
            }
            open[h] = NULL;
            S_stem_count(stem, summary);
            if (h == top) {
                return stem;
            }
            entry = (markdown_core_stem_entry){.stem = stem};
        }
    }
    assert(false);

failed:
    /* Nothing was taken: the stems built so far go, and the nodes stay the
     * caller's. An open stem holds the entries filled so far. */
    {
        S_release_lists lists = {NULL, NULL};
        for (uint8_t h = 0; h <= top; h++) {
            if (open[h]) {
                S_link_stem(&lists, open[h]);
            }
        }
        if (loose) {
            S_link_stem(&lists, loose);
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

static markdown_core_stem *S_stem_hold(markdown_core_node_pool *pool, uint8_t height,
                                       const markdown_core_stem_entry *entries, size_t width,
                                       const markdown_core_stem_summary *summary, uint8_t fresh);

markdown_core_stem *markdown_core_stem_replace(markdown_core_node_pool *pool, const markdown_core_stem *stem,
                                               size_t index, markdown_core_node *node,
                                               const markdown_core_stem_summary *summary, bool *failed) {
    const markdown_core_stem *path[MARKDOWN_CORE_STEM_HEIGHT];
    uint8_t at[MARKDOWN_CORE_STEM_HEIGHT];
    size_t depth = 0;
    *failed = false;
    for (;;) {
        uint8_t i = 0;
        if (stem->height) {
            while (index >= stem->entries[i].stem->count) {
                index -= stem->entries[i].stem->count;
                i++;
            }
        } else {
            i = (uint8_t)index;
        }
        path[depth] = stem;
        at[depth++] = i;
        if (!stem->height) {
            break;
        }
        stem = stem->entries[i].stem;
    }
    /* Up the path, each stem is a new one holding what the old one held but
     * the entry below, which is the new one under it. */
    markdown_core_stem *below = NULL;
    while (depth--) {
        const markdown_core_stem *old = path[depth];
        markdown_core_stem *copy =
            S_stem_hold(pool, old->height, old->entries, old->width, NULL, old->marks & MARKDOWN_CORE_STEM_FRESH);
        if (!copy) {
            if (below) {
                markdown_core_node_retain(node);
                markdown_core_stem_release(pool, below);
            }
            *failed = true;
            return NULL;
        }
        markdown_core_stem_entry *entry = &copy->entries[at[depth]];
        if (old->height) {
            entry->stem->refs--;
            entry->stem = below;
        } else {
            entry->node->refs--;
            entry->node = node;
        }
        S_stem_sum(copy, summary);
        below = copy;
    }
    return below;
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

void markdown_core_stem_release(markdown_core_node_pool *pool, markdown_core_stem *stem) {
    S_release_lists lists = {NULL, NULL};
    S_drop_stem(&lists, stem);
    (void)S_release(pool, &lists);
}

/* A stem of `height` holding the `width` entries at `entries`, each of which
 * it takes a new reference to, FRESH as `fresh` says when it holds nodes;
 * NULL when it could not be allocated. */
static markdown_core_stem *S_stem_hold(markdown_core_node_pool *pool, uint8_t height,
                                       const markdown_core_stem_entry *entries, size_t width,
                                       const markdown_core_stem_summary *summary, uint8_t fresh) {
    markdown_core_stem *stem = S_stem_new(pool, height, width);
    if (!stem) {
        return NULL;
    }
    stem->marks = height ? 0 : fresh;
    for (size_t i = 0; i < width; i++) {
        stem->entries[i] = entries[i];
        if (height) {
            entries[i].stem->refs++;
        } else {
            entries[i].node->refs++;
        }
    }
    S_stem_sum(stem, summary);
    return stem;
}

/* The `width` entries at `entries`, all of one stem's height, as one stem or,
 * past the width, two of at least the fill each: `out[0]` and `out[1]`, new
 * references, and how many. 0 when a stem could not be allocated. */
static size_t S_stem_pack(markdown_core_node_pool *pool, uint8_t height, const markdown_core_stem_entry *entries,
                          size_t width, const markdown_core_stem_summary *summary, uint8_t fresh,
                          markdown_core_stem *out[2]) {
    if (width <= MARKDOWN_CORE_STEM_WIDTH) {
        return (out[0] = S_stem_hold(pool, height, entries, width, summary, fresh)) != NULL;
    }
    const size_t half = width / 2;
    if (!(out[0] = S_stem_hold(pool, height, entries, half, summary, fresh))) {
        return 0;
    }
    if (!(out[1] = S_stem_hold(pool, height, entries + half, width - half, summary, fresh))) {
        markdown_core_stem_release(pool, out[0]);
        return 0;
    }
    return 2;
}

/* Two stems of one height side by side as one stem or two, as S_stem_pack
 * gives them; two that each hold the fill already stay as they are. */
static size_t S_stem_pair(markdown_core_node_pool *pool, markdown_core_stem *front, markdown_core_stem *back,
                          const markdown_core_stem_summary *summary, markdown_core_stem *out[2]) {
    const size_t width = (size_t)front->width + back->width;
    if (width > MARKDOWN_CORE_STEM_WIDTH && front->width >= MARKDOWN_CORE_STEM_FILL &&
        back->width >= MARKDOWN_CORE_STEM_FILL) {
        out[0] = markdown_core_stem_retain(front);
        out[1] = markdown_core_stem_retain(back);
        return 2;
    }
    markdown_core_stem_entry entries[2 * MARKDOWN_CORE_STEM_WIDTH];
    memcpy(entries, front->entries, front->width * sizeof(*entries));
    memcpy(entries + front->width, back->entries, back->width * sizeof(*entries));
    return S_stem_pack(pool, front->height, entries, width, summary,
                       (front->marks | back->marks) & MARKDOWN_CORE_STEM_FRESH, out);
}

markdown_core_stem *markdown_core_stem_join(markdown_core_node_pool *pool, markdown_core_stem *front,
                                            markdown_core_stem *back, const markdown_core_stem_summary *summary,
                                            bool *failed) {
    *failed = false;
    if (!front || !back) {
        return front ? front : back;
    }
    /* The shorter tree joins the taller one's outermost stem of its own
     * height on the side it lies; each stem of that side above is then copied
     * with its outermost entry replaced by what that made, one stem or two. */
    const bool down_front = front->height >= back->height;
    markdown_core_stem *tall = down_front ? front : back, *short_tree = down_front ? back : front;
    const markdown_core_stem *spine[MARKDOWN_CORE_STEM_HEIGHT];
    spine[tall->height] = tall;
    for (uint8_t level = tall->height; level > short_tree->height; level--) {
        const markdown_core_stem *above = spine[level];
        spine[level - 1] = above->entries[down_front ? above->width - 1 : 0].stem;
    }
    markdown_core_stem *out[2];
    markdown_core_stem *meet = (markdown_core_stem *)spine[short_tree->height];
    size_t made = down_front ? S_stem_pair(pool, meet, short_tree, summary, out)
                             : S_stem_pair(pool, short_tree, meet, summary, out);
    for (uint8_t level = short_tree->height + 1; made && level <= tall->height; level++) {
        const markdown_core_stem *above = spine[level];
        markdown_core_stem_entry entries[MARKDOWN_CORE_STEM_WIDTH + 1];
        const size_t kept = above->width - 1u;
        size_t width = 0;
        if (down_front) {
            memcpy(entries, above->entries, kept * sizeof(*entries));
            width = kept;
        }
        for (size_t i = 0; i < made; i++) {
            entries[width++].stem = out[i];
        }
        if (!down_front) {
            memcpy(entries + width, above->entries + 1, kept * sizeof(*entries));
            width += kept;
        }
        markdown_core_stem *packed[2];
        const size_t packs = S_stem_pack(pool, level, entries, width, summary, 0, packed);
        for (size_t i = 0; i < made; i++) {
            markdown_core_stem_release(pool, out[i]);
        }
        made = packs;
        memcpy(out, packed, sizeof(out));
    }
    if (made == 2) {
        markdown_core_stem *root = NULL;
        if (tall->height + 1 < MARKDOWN_CORE_STEM_HEIGHT) {
            const markdown_core_stem_entry entries[2] = {{.stem = out[0]}, {.stem = out[1]}};
            root = S_stem_hold(pool, (uint8_t)(tall->height + 1), entries, 2, summary, 0);
        }
        markdown_core_stem_release(pool, out[0]);
        markdown_core_stem_release(pool, out[1]);
        made = root != NULL;
        out[0] = root;
    }
    if (!made) {
        *failed = true;
        return NULL;
    }
    markdown_core_stem_release(pool, front);
    markdown_core_stem_release(pool, back);
    return out[0];
}

/* A STEM'S NODES FROM `index`, `count` of them, as runs of entries of the
 * stems on the way: a walk in order that hands `visit` each run of one
 * stem's entries that lie wholly among them, the most it can at once -- a
 * whole stem, the entries of a stem between the two it goes down into, or
 * the nodes of a leaf it cuts. A stem the nodes cover in part has at most
 * two entries covered in part, the first and the last, so the walk visits
 * at most two runs at each height, and keeps at most three items due at
 * each. */
typedef struct {
    const markdown_core_stem *stem;
    /* Where the stem's nodes begin, or, for a run of its entries, SIZE_MAX. */
    size_t start;
    uint8_t from, to;
} S_stem_due;

typedef bool (*S_stem_visit)(void *context, const markdown_core_stem *stem, size_t from, size_t to);

static bool S_stem_cover(const markdown_core_stem *stem, size_t index, size_t count, S_stem_visit visit,
                         void *context) {
    S_stem_due due[3 * MARKDOWN_CORE_STEM_HEIGHT + 1];
    size_t depth = 0;
    const size_t end = index + count;
    due[depth++] = (S_stem_due){stem, 0, 0, 0};
    while (depth) {
        const S_stem_due top = due[--depth];
        if (top.start == SIZE_MAX) {
            if (!visit(context, top.stem, top.from, top.to)) {
                return false;
            }
            continue;
        }
        const size_t top_end = top.start + top.stem->count;
        if (top.start >= index && top_end <= end) {
            if (!visit(context, top.stem, 0, top.stem->width)) {
                return false;
            }
            continue;
        }
        if (!top.stem->height) {
            const size_t from = index > top.start ? index - top.start : 0;
            const size_t to = (end < top_end ? end : top_end) - top.start;
            if (!visit(context, top.stem, from, to)) {
                return false;
            }
            continue;
        }
        /* The entries it covers: those wholly covered are one run, between
         * the first and the last, which it goes down into when they are
         * covered in part. */
        size_t at = top.start;
        uint8_t first = 0;
        while (at + top.stem->entries[first].stem->count <= index) {
            at += top.stem->entries[first++].stem->count;
        }
        const size_t first_start = at;
        uint8_t last = first;
        size_t last_start = at;
        while (last + 1 < top.stem->width && at + top.stem->entries[last].stem->count < end) {
            at += top.stem->entries[last++].stem->count;
            last_start = at;
        }
        const bool first_whole = first_start >= index;
        const bool last_whole = last_start + top.stem->entries[last].stem->count <= end;
        const uint8_t from = (uint8_t)(first + !first_whole), to = (uint8_t)(last + last_whole);
        if (!last_whole && (last != first || first_whole)) {
            due[depth++] = (S_stem_due){top.stem->entries[last].stem, last_start, 0, 0};
        }
        if (from < to) {
            due[depth++] = (S_stem_due){top.stem, SIZE_MAX, from, to};
        }
        if (!first_whole) {
            due[depth++] = (S_stem_due){top.stem->entries[first].stem, first_start, 0, 0};
        }
    }
    return true;
}

/* A slice's runs as trees: a run of one entry is the stem it holds, and
 * any other is a stem of its stem's height holding it. */
typedef struct {
    markdown_core_node_pool *pool;
    const markdown_core_stem_summary *summary;
    markdown_core_stem *pieces[2 * MARKDOWN_CORE_STEM_HEIGHT + 1];
    size_t count;
} S_stem_cut;

static bool S_stem_cut_visit(void *context, const markdown_core_stem *stem, size_t from, size_t to) {
    S_stem_cut *cut = context;
    markdown_core_stem *piece = from == 0 && to == stem->width ? markdown_core_stem_retain((markdown_core_stem *)stem)
                                : stem->height && to - from == 1
                                    ? markdown_core_stem_retain(stem->entries[from].stem)
                                    : S_stem_hold(cut->pool, stem->height, stem->entries + from, to - from,
                                                  cut->summary, stem->marks & MARKDOWN_CORE_STEM_FRESH);
    if (!piece) {
        return false;
    }
    cut->pieces[cut->count++] = piece;
    return true;
}

/* The pieces from `first` to `last`, all of them, joined into `*joined`
 * one at a time, the shorter into the taller, from the end that is
 * `forward` -- each join meets its piece on its own height, so the joins
 * cost what the heights climb. Each piece is taken or released. */
static bool S_stem_fold(S_stem_cut *cut, size_t first, size_t last, bool forward, markdown_core_stem **joined) {
    bool ok = true;
    for (size_t i = first; i <= last; i++) {
        markdown_core_stem *piece = cut->pieces[forward ? i : first + last - i];
        if (!ok) {
            markdown_core_stem_release(cut->pool, piece);
            continue;
        }
        bool failed;
        markdown_core_stem *next = forward ? markdown_core_stem_join(cut->pool, *joined, piece, cut->summary, &failed)
                                           : markdown_core_stem_join(cut->pool, piece, *joined, cut->summary, &failed);
        if (failed) {
            markdown_core_stem_release(cut->pool, piece);
            ok = false;
            continue;
        }
        *joined = next;
    }
    return ok;
}

markdown_core_stem *markdown_core_stem_slice(markdown_core_node_pool *pool, const markdown_core_stem *stem,
                                             size_t index, size_t count, const markdown_core_stem_summary *summary,
                                             bool *failed) {
    *failed = false;
    if (!count) {
        return NULL;
    }
    S_stem_cut cut = {pool, summary, {NULL}, 0};
    if (!S_stem_cover(stem, index, count, S_stem_cut_visit, &cut)) {
        for (size_t i = 0; i < cut.count; i++) {
            markdown_core_stem_release(pool, cut.pieces[i]);
        }
        *failed = true;
        return NULL;
    }
    /* The pieces climb to the tallest and descend after it: those before it
     * join from the first, those after it from the last. */
    size_t peak = 0;
    for (size_t i = 1; i < cut.count; i++) {
        peak = cut.pieces[i]->height > cut.pieces[peak]->height ? i : peak;
    }
    markdown_core_stem *front = NULL, *back = NULL;
    const bool front_ok = S_stem_fold(&cut, 0, peak, true, &front);
    const bool back_ok = peak + 1 >= cut.count || S_stem_fold(&cut, peak + 1, cut.count - 1, false, &back);
    markdown_core_stem *joined =
        front_ok && back_ok ? markdown_core_stem_join(pool, front, back, summary, failed) : NULL;
    if (!joined) {
        markdown_core_stem_release(pool, front);
        markdown_core_stem_release(pool, back);
        *failed = true;
    }
    return joined;
}

typedef struct {
    const markdown_core_stem_summary *summary;
    uint64_t word;
    bool any;
} S_stem_total;

static void S_stem_total_add(S_stem_total *total, uint64_t word) {
    total->word = total->any ? total->summary->combine(total->word, word) : word;
    total->any = true;
}

static bool S_stem_total_visit(void *context, const markdown_core_stem *stem, size_t from, size_t to) {
    S_stem_total *total = context;
    for (size_t i = from; i < to; i++) {
        S_stem_total_add(total,
                         stem->height ? stem->entries[i].stem->summary : total->summary->of(stem->entries[i].node));
    }
    return true;
}

uint64_t markdown_core_stem_run_summary(const markdown_core_stem *stem, size_t index, size_t count,
                                        const markdown_core_stem_summary *summary) {
    S_stem_total total = {summary, 0, false};
    (void)S_stem_cover(stem, index, count, S_stem_total_visit, &total);
    return total.word;
}

static bool S_stem_length_visit(void *context, const markdown_core_stem *stem, size_t from, size_t to) {
    int64_t *length = context;
    if (from == 0 && to == stem->width) {
        *length += stem->length;
        return true;
    }
    for (size_t i = from; i < to; i++) {
        if (stem->height) {
            *length += stem->entries[i].stem->length;
        } else {
            const markdown_core_node *node = stem->entries[i].node;
            *length += node->where.extent.lead + (int64_t)node->where.extent.span;
        }
    }
    return true;
}

int64_t markdown_core_stem_length(const markdown_core_stem *stem, size_t index, size_t count) {
    int64_t length = 0;
    if (count) {
        (void)S_stem_cover(stem, index, count, S_stem_length_visit, &length);
    }
    return length;
}

/* The path from `stem` down to the leaf that holds the node at `index`:
 * `at[level]` is the entry of `path[level]` it goes through, and the result
 * the index of the first node under that entry of the leaf, which is to
 * say `index` less the entry's place in its leaf. */
static size_t S_stem_path(const markdown_core_stem *stem, size_t index, const markdown_core_stem **path, uint8_t *at) {
    size_t first = 0;
    for (;;) {
        const uint8_t level = stem->height;
        path[level] = stem;
        if (!level) {
            at[0] = (uint8_t)(index - first);
            return first;
        }
        uint8_t i = 0;
        while (index - first >= stem->entries[i].stem->count) {
            first += stem->entries[i].stem->count;
            i++;
        }
        at[level] = i;
        stem = stem->entries[i].stem;
    }
}

size_t markdown_core_stem_meet(const markdown_core_stem *stem, size_t index, int64_t *anchor, int64_t edge) {
    const size_t count = markdown_core_stem_count(stem);
    if (index >= count) {
        return count;
    }
    const markdown_core_stem *path[MARKDOWN_CORE_STEM_HEIGHT];
    uint8_t at[MARKDOWN_CORE_STEM_HEIGHT];
    size_t position = S_stem_path(stem, index, path, at) + at[0];
    uint8_t level = 0;
    /* Forward from the node at `index`: a leaf's nodes one by one, and above
     * them the stems after the path whole, unless one holds what is sought,
     * which the walk then goes down into. */
    for (;;) {
        const markdown_core_stem *top = path[level];
        for (; at[level] < top->width; at[level]++) {
            if (!level) {
                const markdown_core_node *node = top->entries[at[0]].node;
                const int64_t end = *anchor + node->where.extent.lead + (int64_t)node->where.extent.span;
                if ((node->flags & MARKDOWN_CORE_NODE__GROUP) || end + node->reach >= edge) {
                    return position;
                }
                *anchor = end;
                position++;
                continue;
            }
            const markdown_core_stem *below = top->entries[at[level]].stem;
            if ((below->marks & MARKDOWN_CORE_STEM_GROUP) || *anchor + below->length + below->far >= edge) {
                break;
            }
            *anchor += below->length;
            position += below->count;
        }
        if (level && at[level] < top->width) {
            level--;
            path[level] = top->entries[at[level + 1]].stem;
            at[level] = 0;
            continue;
        }
        if (level == stem->height) {
            return count;
        }
        level++;
        at[level]++;
    }
}

size_t markdown_core_stem_last_free(const markdown_core_stem *stem, size_t index, size_t end) {
    if (end <= index) {
        return SIZE_MAX;
    }
    const markdown_core_stem *path[MARKDOWN_CORE_STEM_HEIGHT];
    uint8_t at[MARKDOWN_CORE_STEM_HEIGHT];
    /* `position` is one past the last node under the entry the walk is at. */
    size_t position = end;
    (void)S_stem_path(stem, end - 1, path, at);
    uint8_t level = 0;
    /* Back from the node before `end`, as markdown_core_stem_meet goes
     * forward. The entries are walked from `at[level]` down; one past the
     * first means the stem is done. */
    int at_level = at[0];
    for (;;) {
        const markdown_core_stem *top = path[level];
        for (; at_level >= 0 && position > index; at_level--) {
            if (!level) {
                if (!(top->entries[at_level].node->flags & MARKDOWN_CORE_NODE__HOLDS_NEXT)) {
                    return position - 1;
                }
                position--;
                continue;
            }
            const markdown_core_stem *below = top->entries[at_level].stem;
            if (below->marks & MARKDOWN_CORE_STEM_FREE) {
                break;
            }
            position -= below->count;
        }
        if (position <= index) {
            return SIZE_MAX;
        }
        if (level && at_level >= 0) {
            at[level] = (uint8_t)at_level;
            level--;
            path[level] = top->entries[at_level].stem;
            at_level = path[level]->width - 1;
            continue;
        }
        if (level == stem->height) {
            return SIZE_MAX;
        }
        level++;
        at_level = (int)at[level] - 1;
    }
}

static inline bool S_order_at_or_before(const markdown_core_order *order, uint64_t label) {
    return order && order->label <= label;
}

size_t markdown_core_stem_find(const markdown_core_stem *stem, uint64_t label) {
    if (!stem || !S_order_at_or_before(stem->first, label)) {
        return SIZE_MAX;
    }
    /* Down the entries whose first order is the last at or before `label`:
     * the first orders grow along the stem as the labels do. */
    size_t position = 0;
    for (;;) {
        uint8_t chosen = 0;
        size_t before = 0, chosen_before = 0;
        for (uint8_t i = 0; i < stem->width; i++) {
            const markdown_core_order *first =
                stem->height ? stem->entries[i].stem->first : stem->entries[i].node->first;
            if (S_order_at_or_before(first, label)) {
                chosen = i;
                chosen_before = before;
            }
            before += stem->height ? stem->entries[i].stem->count : 1;
        }
        position += chosen_before;
        if (!stem->height) {
            return position;
        }
        stem = stem->entries[chosen].stem;
    }
}

/* Whether a definition has a label, and the label in `label`. */
bool markdown_core_definition_label(const markdown_core_node *node, markdown_core_chunk *label) {
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

/* THE BUILDERS. */

bool markdown_core_member_admits(const markdown_core_member *owner, const markdown_core_member *child) {
    return (owner->node->element && owner->node->element->can_contain_func) ||
           markdown_core_node_can_contain_builtin(owner->node, (markdown_core_node_type)child->node->kind);
}

void markdown_core_member_attach_field(markdown_core_member *owner, markdown_core_member *field) {
    assert(!field->owner && !field->next);
    field->owner = owner;
    field->field = true;
    markdown_core_member_inherit(owner, field);
    markdown_core_member **at = &owner->fields;
    while (*at) {
        at = &(*at)->next;
    }
    *at = field;
}

static void S_member_free(markdown_core_node_pool *pool, markdown_core_member *member) {
    markdown_core_slab_release(pool ? &pool->members : NULL, member);
}

bool markdown_core_member_freeze(markdown_core_node_pool *pool, markdown_core_member *member,
                                 const markdown_core_stem_summary *summary) {
    if (!member->first) {
        return true;
    }
    /* The most nodes held between two runs. */
    size_t count = 0, most = 0, runs = 0;
    for (const markdown_core_member *child = member->first; child; child = child->next) {
        count += child->held;
        if (child->run) {
            runs++;
            count = 0;
        }
        most = count > most ? count : most;
    }
    markdown_core_node *node = member->node;
    assert(!node->children || (!most && !runs));
    if (!most && !runs) {
        return true;
    }
    markdown_core_node *small[MARKDOWN_CORE_STEM_WIDTH];
    markdown_core_node **nodes = most <= MARKDOWN_CORE_STEM_WIDTH ? small : markdown_core_alloc(most, sizeof(*nodes));
    if (!nodes) {
        return false;
    }
    /* The nodes held between two runs make a stem, and the stems and the
     * runs join in order. `done` is the first member whose node or run the
     * joined stem does not hold yet. */
    markdown_core_stem *joined = NULL;
    const markdown_core_member *done = member->first;
    bool failed = false;
    count = 0;
    for (const markdown_core_member *child = member->first;; child = child->next) {
        if (child && child->held) {
            nodes[count++] = child->node;
            continue;
        }
        if (count) {
            markdown_core_stem *made = markdown_core_stem_make(pool, nodes, count, summary, &failed);
            markdown_core_stem *both = failed ? NULL : markdown_core_stem_join(pool, joined, made, summary, &failed);
            if (failed) {
                if (made) {
                    for (size_t i = 0; i < count; i++) {
                        markdown_core_node_retain(nodes[i]);
                    }
                    markdown_core_stem_release(pool, made);
                }
                break;
            }
            joined = both;
            done = child;
            count = 0;
        }
        if (!child) {
            break;
        }
        if (child->run) {
            markdown_core_stem *run = markdown_core_stem_retain(child->run);
            markdown_core_stem *both = markdown_core_stem_join(pool, joined, run, summary, &failed);
            if (failed) {
                markdown_core_stem_release(pool, run);
                break;
            }
            joined = both;
            done = child->next;
        }
    }
    if (nodes != small) {
        markdown_core_free(nodes);
    }
    if (failed) {
        /* The members keep their nodes: the joined stem gives back the
         * references it took from them. */
        for (const markdown_core_member *child = member->first; child != done; child = child->next) {
            if (child->held) {
                markdown_core_node_retain(child->node);
            }
        }
        markdown_core_stem_release(pool, joined);
        return false;
    }
    node->children = joined;
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
        if (taken->run) {
            markdown_core_stem_release(pool, taken->run);
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
    if (!hold_string(pool, &held) || !hold_string(pool, &held_title)) {
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
 * one below the root that holds less than the fill, one whose entries are
 * not all one height below it, and one that miscounts the nodes under it.
 * The walk's path is no deeper than the stem's height. */
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
            if (depth && top->width < MARKDOWN_CORE_STEM_FILL) {
                S_print_error(out, (markdown_core_node *)owner, "stem fill");
                errors++;
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
