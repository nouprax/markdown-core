#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "config.h"
#include "node.h"
#include "references.h"
#include "element.h"
#include "../elements/markdown-core-elements.h"

static void S_node_unlink(markdown_core_node *node);

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

static bool S_can_contain(markdown_core_node *node, markdown_core_node *child) {
    if (node == NULL || child == NULL) {
        return false;
    }
    /* Arbitrary reparenting must reject cycles. Parser construction instead
     * proves containment and transfers a disjoint subtree through
     * attach_validated. */
    {
        markdown_core_node *cur = node;
        do {
            if (cur == child) {
                return false;
            }
            cur = cur->parent;
        } while (cur != NULL);
    }

    return markdown_core_node_can_contain_type(node, (markdown_core_node_type)child->kind);
}

/* THE CELL (see the pool in node.h). A C99 union aligns the node and the
 * record space after it for ordinary scalar fields; the header before them is
 * padded to the same alignment, so the node sits at one fixed offset in every
 * cell whichever storage the cell came from. This is storage layout, not a
 * field format: nothing reads a cell through the node. */
typedef union {
    markdown_core_node_slab *slab;
    long double alignment;
    int64_t integer_alignment;
} markdown_core_node_cell_header;

typedef union {
    markdown_core_node node;
    long double alignment;
    int64_t integer_alignment;
} markdown_core_node_allocation;

/* Room for a kind's record inside the cell. The bound is a property of the
 * cell, not of any kind: a record that fits is placed here, and one that does
 * not is owned through `node_data_allocation` exactly as a replacement record
 * is, so the release path has one rule for both. Sixty-four bytes hold every
 * record but the metadata fields and a cross transclusion's, which are one
 * per document and rare. */
#define MARKDOWN_CORE_NODE_CELL_RECORD_BYTES 64

typedef struct {
    markdown_core_node_cell_header header;
    markdown_core_node_allocation node;
    unsigned char record[MARKDOWN_CORE_NODE_CELL_RECORD_BYTES];
} markdown_core_node_cell;

/* A slab: its hold count, then its cells at the same alignment. A slab is
 * freed by whoever drops its last hold, which may be a cell released long
 * after the parse that took it. */
struct markdown_core_node_slab {
    union {
        size_t holds;
        long double alignment;
    } head;
};

#define MARKDOWN_CORE_NODE_SLAB_BYTES ((size_t)64 * 1024)
#define MARKDOWN_CORE_NODE_SLAB_CELLS                                                                                  \
    ((MARKDOWN_CORE_NODE_SLAB_BYTES - sizeof(markdown_core_node_slab)) / sizeof(markdown_core_node_cell))

static markdown_core_node_cell *S_slab_cell(markdown_core_node_slab *slab, size_t index) {
    return (markdown_core_node_cell *)((unsigned char *)slab + sizeof(*slab)) + index;
}

static markdown_core_node_cell *S_cell_of(markdown_core_node *node) {
    return (markdown_core_node_cell *)((unsigned char *)node - offsetof(markdown_core_node_cell, node));
}

static void S_slab_drop(markdown_core_node_slab *slab) {
    if (slab && --slab->head.holds == 0) {
        markdown_core_free(slab);
    }
}

/* A zeroed cell, or NULL. The pool's released cells come first, then the
 * current slab, then a new one; the pool holds the slab it takes from, so a
 * release that empties it cannot free it out from under the pool. A NULL
 * pool takes one cell from the allocator, with no slab.
 *
 * A slab is taken uninitialised and each cell is zeroed as it is handed out,
 * whether it is fresh or reused: one path, and the bytes are cleared right
 * before they are written rather than a slab ahead. Clearing whole slabs
 * through `calloc` cost more than the allocations it replaced. */
static markdown_core_node_cell *S_cell_take(markdown_core_node_pool *pool) {
    markdown_core_node_cell *cell;
    if (!pool) {
        return (markdown_core_node_cell *)markdown_core_alloc(1, sizeof(*cell));
    }
    if (pool->released) {
        markdown_core_node *node = pool->released;
        pool->released = node->next;
        cell = S_cell_of(node);
    } else {
        if (!pool->current || pool->taken == MARKDOWN_CORE_NODE_SLAB_CELLS) {
            markdown_core_node_slab *slab =
                (markdown_core_node_slab *)markdown_core_realloc(NULL, MARKDOWN_CORE_NODE_SLAB_BYTES);
            if (!slab) {
                return NULL;
            }
            slab->head.holds = 1;
            S_slab_drop(pool->current);
            pool->current = slab;
            pool->taken = 0;
        }
        cell = S_slab_cell(pool->current, pool->taken++);
        cell->header.slab = pool->current;
        pool->current->head.holds++;
    }
    memset(&cell->node, 0, sizeof(*cell) - offsetof(markdown_core_node_cell, node));
    return cell;
}

/* The node's storage, after its contents are released. Into a pool, a slab
 * cell is kept for reuse and keeps its hold; otherwise it drops the hold, and
 * a cell with no slab is the allocator's. */
static void S_cell_release(markdown_core_node_pool *pool, markdown_core_node *node) {
    markdown_core_node_cell *cell = S_cell_of(node);
    if (!cell->header.slab) {
        markdown_core_free(cell);
    } else if (pool) {
        node->next = pool->released;
        pool->released = node;
    } else {
        S_slab_drop(cell->header.slab);
    }
}

void markdown_core_node_pool_dispose(markdown_core_node_pool *pool) {
    while (pool->released) {
        markdown_core_node *node = pool->released;
        pool->released = node->next;
        S_slab_drop(S_cell_of(node)->header.slab);
    }
    S_slab_drop(pool->current);
    pool->current = NULL;
    pool->taken = 0;
}

/* RECORD SIZE IS A PROPERTY OF THE KIND, so it is an array index.
 *
 * This was a 24-branch switch on the kind, compiled as a decision tree and
 * entered once per node -- 26.4% of `markdown_core_node_new_with_ext` on the
 * same-job corpus, at about 891,000 nodes. The kind already indexes two other
 * tables this way (`markdown_core_block_structure` and its inline twin, in
 * core/element.h), and the encoding is what makes that work: the low bits of
 * a kind are a dense ordinal within its class, and the class is one bit test.
 *
 * A kind absent from its table gets 0, which is what the switch's `default`
 * gave it: no record, and `as.data` left NULL. The element types numbered in
 * elements/markdown-core-elements.h are exactly those -- their payload comes
 * from `opaque_alloc_func`, not from here. */
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
};

static const size_t S_inline_payload_size[MARKDOWN_CORE_NODE_KIND_COUNT] = {
    [MARKDOWN_CORE_NODE_TEXT & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_chunk),
    [MARKDOWN_CORE_NODE_CODE & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_chunk),
    [MARKDOWN_CORE_NODE_HTML & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_chunk),
    [MARKDOWN_CORE_NODE_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_link),
    [MARKDOWN_CORE_NODE_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_link),
    [MARKDOWN_CORE_NODE_CITE & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_cite),
    [MARKDOWN_CORE_NODE_COMMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_chunk),
    [MARKDOWN_CORE_NODE_CITATION & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_citation_item),
    [MARKDOWN_CORE_NODE_CROSS_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_cross_reference),
    [MARKDOWN_CORE_NODE_CROSS_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = sizeof(markdown_core_cross_embedded),
};

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
        as->list->list_type = MARKDOWN_CORE_BULLET_LIST;
        as->list->variant.kind = MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL;
        break;
    default:
        break;
    }
}

markdown_core_node *markdown_core_node_pool_new(markdown_core_node_pool *pool, markdown_core_node_type type,
                                                const markdown_core_element *element) {
    /* Construction gives the node and its record one cell, when the record
     * fits; the cell is zeroed, so every field of `content` but `ptr` already
     * holds what an init would write. */
    size_t payload_size = S_node_payload_size(type);
    markdown_core_node_cell *cell = S_cell_take(pool);
    if (!cell) {
        return NULL;
    }
    markdown_core_node *node = &cell->node.node;
    if (payload_size > MARKDOWN_CORE_NODE_CELL_RECORD_BYTES) {
        node->node_data_allocation = markdown_core_alloc(1, payload_size);
        if (!node->node_data_allocation) {
            S_cell_release(pool, node);
            return NULL;
        }
        node->as.data = node->node_data_allocation;
    } else {
        node->as.data = payload_size ? cell->record : NULL;
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

static void free_node_as(markdown_core_node *node) {
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
        /* The affix chains are freed by the walk in `S_free_nodes`, spliced
         * in beside the children; only the referent's bytes are the arm's. */
        markdown_core_chunk_free(&node->as.citation->value);
        break;
    case MARKDOWN_CORE_NODE_SPECIMEN:
        markdown_core_optional_chunk_free(&node->as.specimen->id);
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        markdown_core_chunk_free(&node->as.footnote->id);
        break;
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_EMBEDDED:
        /* One holder fewer; a resource shared with other occurrences, or
         * still held by the reference map, stays. */
        markdown_core_resource_release(node->as.link->resource);
        node->as.link->resource = NULL;
        break;
    default:
        break;
    }
    /* Free only the record this node owns separately: a replacement a kind
     * change installed, or an initial record too large for the cell. Pointer
     * equality cannot establish ownership: an allocator may place a
     * replacement right after a cell. Almost no node owns one, so the release
     * is entered only when there is one. */
    if (node->node_data_allocation) {
        markdown_core_free(node->node_data_allocation);
        node->node_data_allocation = NULL;
    }
    node->as.data = NULL;
}

// Free a markdown_core_node list and any children.
/* Splices `first`'s sibling chain into the free walk right after `e`, so the
 * walk frees it as it frees children: without recursion. */
static void S_splice_after(markdown_core_node *e, markdown_core_node *first) {
    markdown_core_node *last;
    if (first == NULL) {
        return;
    }
    last = first;
    while (last->next != NULL) {
        last = last->next;
    }
    last->next = e->next;
    e->next = first;
}

static int S_release_owned_subtree(markdown_core_node **slot, void *context) {
    S_splice_after(context, *slot);
    *slot = NULL;
    return 1;
}

/* The node-valued fields join the same iterative free walk as content.
 * Kind conversion uses a separate walk so its siblings remain untouched. */
static void S_splice_owned_fields(markdown_core_node *owner, markdown_core_node *after) {
    switch (owner->kind) {
    case MARKDOWN_CORE_NODE_DEFINITION:
        S_splice_after(after, owner->as.definition->term);
        break;
    case MARKDOWN_CORE_NODE_CALLOUT:
        S_splice_after(after, owner->as.callout->title);
        break;
    case MARKDOWN_CORE_NODE_CITE:
        S_splice_after(after, owner->as.cite->citations);
        break;
    case MARKDOWN_CORE_NODE_CITATION:
        S_splice_after(after, owner->as.citation->suffix);
        S_splice_after(after, owner->as.citation->prefix);
        break;
    case MARKDOWN_CORE_NODE_DOCUMENT:
        S_splice_after(after, owner->as.document->metadata);
        S_splice_after(after, owner->as.document->footnotes);
        S_splice_after(after, owner->as.document->specimens);
        break;
    default:
        break;
    }
}

static size_t S_free_nodes(markdown_core_node_pool *pool, markdown_core_node *e) {
    markdown_core_node *next;
    size_t released = 0;
    while (e != NULL) {
        released++;
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

        if (e->user_data && e->user_data_free_func) {
            e->user_data_free_func(e->user_data);
        }

        if (e->element && e->element->visit_owned_subtrees_func) {
            e->element->visit_owned_subtrees_func(e->element, e, S_release_owned_subtree, e);
        }
        if (e->opaque && e->element && e->element->opaque_free_func) {
            e->element->opaque_free_func(e->element, e);
        }

        S_splice_owned_fields(e, e);
        free_node_as(e);

        if (e->last_child) {
            // Splice children into list
            e->last_child->next = e->next;
            e->next = e->first_child;
        }
        next = e->next;
        S_cell_release(pool, e);
        e = next;
    }
    return released;
}

size_t markdown_core_node_pool_release(markdown_core_node_pool *pool, markdown_core_node *node) {
    S_node_unlink(node);
    node->next = NULL;
    return S_free_nodes(pool, node);
}

size_t markdown_core_node_release(markdown_core_node *node) { return markdown_core_node_pool_release(NULL, node); }

void markdown_core_node_free(markdown_core_node *node) { (void)markdown_core_node_release(node); }

markdown_core_node_type markdown_core_node_get_type(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NODE_NONE;
    } else {
        return (markdown_core_node_type)node->kind;
    }
}

markdown_core_node_set_kind_result markdown_core_node_set_kind(markdown_core_node *node, markdown_core_node_type kind) {
    markdown_core_node_type initial_kind = (markdown_core_node_type)node->kind;
    if (kind == initial_kind) {
        return MARKDOWN_CORE_NODE_SET_KIND_OK;
    }
    /* Conversion preserves every tree edge, so it cannot introduce a cycle. */
    if (!node->parent || !markdown_core_node_can_contain_type(node->parent, kind)) {
        return MARKDOWN_CORE_NODE_SET_KIND_REJECTED;
    }

    /* Allocate before releasing anything. A failed conversion preserves the
     * old kind, data, and owned subtrees, with stable node identity. */
    size_t size = S_node_payload_size(kind);
    markdown_core_node_data replacement = {.data = size ? markdown_core_alloc(1, size) : NULL};
    if (size && !replacement.data) {
        return MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED;
    }
    S_init_node_as(kind, &replacement);
    markdown_core_node fields = {0};
    S_splice_owned_fields(node, &fields);
    S_free_nodes(NULL, fields.next);
    free_node_as(node);
    node->as = replacement;
    node->node_data_allocation = replacement.data;
    node->kind = (uint16_t)kind;
    return MARKDOWN_CORE_NODE_SET_KIND_OK;
}

const char *markdown_core_node_get_type_string(markdown_core_node *node) {
    if (node == NULL) {
        return "NONE";
    }

    if (node->element && node->element->get_type_string_func) {
        return node->element->get_type_string_func(node->element, node);
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_NONE:
        return "none";
    case MARKDOWN_CORE_NODE_DOCUMENT:
        return "document";
    case MARKDOWN_CORE_NODE_CALLOUT:
        return "callout";
    case MARKDOWN_CORE_NODE_DEFINITION_LIST:
        return "definition_list";
    case MARKDOWN_CORE_NODE_DEFINITION:
        return "definition";
    case MARKDOWN_CORE_NODE_DEFINITION_BODY:
        return "definition_body";
    case MARKDOWN_CORE_NODE_METADATA:
        return "metadata";
    case MARKDOWN_CORE_NODE_TABLE_CAPTION:
        return "table_caption";
    case MARKDOWN_CORE_NODE_LIST:
        return "list";
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return "list_item";
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return "code_block";
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        return "html_block";
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        return "comment_block";
    case MARKDOWN_CORE_NODE_PARAGRAPH:
        return "paragraph";
    case MARKDOWN_CORE_NODE_HEADING:
        return "heading";
    case MARKDOWN_CORE_NODE_THEMATIC_BREAK:
        return "thematic_break";
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        return "footnote";
    case MARKDOWN_CORE_NODE_SPECIMEN:
        return "specimen";
    case MARKDOWN_CORE_NODE_TEXT:
        return "text";
    case MARKDOWN_CORE_NODE_SOFT_BREAK:
        return "soft_break";
    case MARKDOWN_CORE_NODE_LINE_BREAK:
        return "line_break";
    case MARKDOWN_CORE_NODE_CODE:
        return "code";
    case MARKDOWN_CORE_NODE_HTML:
        return "html";
    case MARKDOWN_CORE_NODE_COMMENT:
        return "comment";
    case MARKDOWN_CORE_NODE_EMPHASIS:
        return "emphasis";
    case MARKDOWN_CORE_NODE_STRONG:
        return "strong";
    case MARKDOWN_CORE_NODE_MARK:
        return "mark";
    case MARKDOWN_CORE_NODE_INSERTION:
        return "insertion";
    case MARKDOWN_CORE_NODE_SPAN:
        return "span";
    case MARKDOWN_CORE_NODE_SUPERSCRIPT:
        return "superscript";
    case MARKDOWN_CORE_NODE_SUBSCRIPT:
        return "subscript";
    case MARKDOWN_CORE_NODE_LINK:
        return "link";
    case MARKDOWN_CORE_NODE_EMBEDDED:
        return "embedded";
    case MARKDOWN_CORE_NODE_CITE:
        return "cite";
    case MARKDOWN_CORE_NODE_CITATION:
        return "citation";
    }

    return "<unknown>";
}

markdown_core_node *markdown_core_node_next(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->next;
    }
}

markdown_core_node *markdown_core_node_previous(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->prev;
    }
}

markdown_core_node *markdown_core_node_parent(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->parent;
    }
}

markdown_core_node *markdown_core_node_first_child(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->first_child;
    }
}

markdown_core_node *markdown_core_node_last_child(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->last_child;
    }
}

void *markdown_core_node_get_user_data(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->user_data;
    }
}

int markdown_core_node_set_user_data(markdown_core_node *node, void *user_data) {
    if (node == NULL) {
        return 0;
    }
    node->user_data = user_data;
    return 1;
}

int markdown_core_node_set_user_data_free_func(markdown_core_node *node, markdown_core_free_func free_func) {
    if (node == NULL) {
        return 0;
    }
    node->user_data_free_func = free_func;
    return 1;
}

const char *markdown_core_node_get_literal(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        return markdown_core_chunk_to_cstr(&node->as.html_block->literal);
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        return markdown_core_chunk_to_cstr(node->as.literal);

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return markdown_core_chunk_to_cstr(&node->as.code->literal);

    default:
        break;
    }

    return NULL;
}

int markdown_core_node_set_literal(markdown_core_node *node, const char *content) {
    if (node == NULL) {
        return 0;
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        return markdown_core_chunk_set_cstr(&node->as.html_block->literal, content);
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        return markdown_core_chunk_set_cstr(node->as.literal, content);

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return markdown_core_chunk_set_cstr(&node->as.code->literal, content);

    default:
        break;
    }

    return 0;
}

const char *markdown_core_node_get_string_content(markdown_core_node *node) { return (char *)node->content.ptr; }

int markdown_core_node_set_string_content(markdown_core_node *node, const char *content) {
    markdown_core_strbuf_sets(&node->content, content);
    return true;
}

int markdown_core_node_get_heading_level(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_HEADING:
        return node->as.heading->level;

    default:
        break;
    }

    return 0;
}

int markdown_core_node_set_heading_level(markdown_core_node *node, int level) {
    if (node == NULL || level < 1 || level > 6) {
        return 0;
    }

    switch (node->kind) {
    case MARKDOWN_CORE_NODE_HEADING:
        node->as.heading->level = level;
        return 1;

    default:
        break;
    }

    return 0;
}

markdown_core_list_type markdown_core_node_get_list_type(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NO_LIST;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list->list_type;
    } else {
        return MARKDOWN_CORE_NO_LIST;
    }
}

int markdown_core_node_set_list_type(markdown_core_node *node, markdown_core_list_type type) {
    if (!(type == MARKDOWN_CORE_BULLET_LIST || type == MARKDOWN_CORE_ORDERED_LIST)) {
        return 0;
    }

    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        node->as.list->list_type = type;
        return 1;
    } else {
        return 0;
    }
}

markdown_core_delim_type markdown_core_node_get_list_delim(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NO_DELIM;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        const markdown_core_ordered_list_delimiter value = node->as.list->delimiter;
        if (value.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD) {
            return MARKDOWN_CORE_PERIOD_DELIM;
        }
        if (value.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS && !value.closed) {
            return MARKDOWN_CORE_PAREN_DELIM;
        }
        return MARKDOWN_CORE_NO_DELIM;
    } else {
        return MARKDOWN_CORE_NO_DELIM;
    }
}

int markdown_core_node_set_list_delim(markdown_core_node *node, markdown_core_delim_type delim) {
    if (!(delim == MARKDOWN_CORE_PERIOD_DELIM || delim == MARKDOWN_CORE_PAREN_DELIM)) {
        return 0;
    }

    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        node->as.list->delimiter = (markdown_core_ordered_list_delimiter){
            delim == MARKDOWN_CORE_PERIOD_DELIM ? MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD
                                                : MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS,
            false};
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_list_start(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list->start;
    } else {
        return 0;
    }
}

int markdown_core_node_set_list_start(markdown_core_node *node, int start) {
    if (node == NULL || start < 0) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        node->as.list->start = start;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_list_tight(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list->tight;
    } else {
        return 0;
    }
}

int markdown_core_node_set_list_tight(markdown_core_node *node, int tight) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        node->as.list->tight = tight == 1;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_list_item_index(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
        return node->as.list->start;
    } else {
        return 0;
    }
}

int markdown_core_node_set_list_item_index(markdown_core_node *node, int idx) {
    if (node == NULL || idx < 0) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
        node->as.list->start = idx;
        return 1;
    } else {
        return 0;
    }
}

const char *markdown_core_node_get_fence_info(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        /* ABSENT IS NULL. `markdown_core_chunk_to_cstr` allocates a `""` for a
         * chunk with no data, which would answer "the source wrote an empty
         * info string" for a fence that wrote none (requirement 14). */
        if (!node->as.code->info.has_value) {
            return NULL;
        }
        return markdown_core_chunk_to_cstr(&node->as.code->info.value);
    } else {
        return NULL;
    }
}

int markdown_core_node_set_fence_info(markdown_core_node *node, const char *info) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        /* A NULL argument is ABSENCE and anything else is presence, including
         * `""`; the caller states which, and this is the write site. */
        if (!markdown_core_chunk_set_cstr(&node->as.code->info.value, info)) {
            return 0;
        }
        node->as.code->info.has_value = info != NULL;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_fence_closed(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        return node->as.code->fenced && node->as.code->fence_closed;
    } else {
        return 0;
    }
}

int markdown_core_node_get_fenced(markdown_core_node *node, int *length, int *offset, char *character) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        *length = node->as.code->fence_length;
        *offset = node->as.code->fence_offset;
        *character = node->as.code->fence_char;
        return node->as.code->fenced;
    } else {
        return 0;
    }
}

int markdown_core_node_set_fenced(markdown_core_node *node, int fenced, int length, int offset, char character) {
    if (node == NULL) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        node->as.code->fenced = (int8_t)fenced;
        node->as.code->fence_length = (uint8_t)length;
        node->as.code->fence_offset = (uint8_t)offset;
        node->as.code->fence_char = character;
        return 1;
    } else {
        return 0;
    }
}

markdown_core_resource *markdown_core_resource_new(markdown_core_chunk url, markdown_core_optional_chunk title) {
    markdown_core_resource *resource = (markdown_core_resource *)markdown_core_alloc(1, sizeof(*resource));
    if (!resource) {
        return NULL;
    }
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

void markdown_core_resource_release(markdown_core_resource *resource) {
    if (!resource) {
        return;
    }
    assert(resource->holders > 0);
    if (--resource->holders > 0) {
        return;
    }
    markdown_core_chunk_free(&resource->url);
    markdown_core_optional_chunk_free(&resource->title);
    markdown_core_attributes_free(&resource->attributes);
    markdown_core_free(resource);
}

int markdown_core_node_set_element(markdown_core_node *node, const markdown_core_element *element) {
    if (node == NULL) {
        return 0;
    }
    node->element = element;
    return 1;
}

int markdown_core_node_get_start_line(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->start_line;
}

int markdown_core_node_get_start_column(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->start_column;
}

int markdown_core_node_get_end_line(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->end_line;
}

int markdown_core_node_get_end_column(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->end_column;
}

// Unlink a node without adjusting its next, prev, and parent pointers.
static void S_node_unlink(markdown_core_node *node) {
    if (node == NULL) {
        return;
    }

    if (node->prev) {
        node->prev->next = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }

    // Adjust first_child and last_child of parent.
    markdown_core_node *parent = node->parent;
    if (parent) {
        if (parent->first_child == node) {
            parent->first_child = node->next;
        }
        if (parent->last_child == node) {
            parent->last_child = node->prev;
        }
    }
}

void markdown_core_node_unlink(markdown_core_node *node) {
    S_node_unlink(node);

    node->next = NULL;
    node->prev = NULL;
    node->parent = NULL;
}

/* Commit a validated, detached subtree. No callbacks or rejecting checks may
 * run here: public mutations have already detached the child from its owner. */
void markdown_core_node_attach_validated(markdown_core_node *parent, markdown_core_node *child,
                                         markdown_core_node *before) {
    assert(parent && child && parent != child);
    assert(!child->parent && !child->prev && !child->next);
    assert(!before || before->parent == parent);
    /* Built-in containment is pure and shares its rules with checked mutation.
     * Dynamic policies were decided before ownership moved; never replay them. */
    assert((parent->element && parent->element->can_contain_func) ||
           markdown_core_node_can_contain_builtin(parent, (markdown_core_node_type)child->kind));
    markdown_core_node *previous = before ? before->prev : parent->last_child;
    child->parent = parent;
    child->prev = previous;
    child->next = before;
    if (previous) {
        previous->next = child;
    } else {
        parent->first_child = child;
    }
    if (before) {
        before->prev = child;
    } else {
        parent->last_child = child;
    }
}

int markdown_core_node_insert_before(markdown_core_node *node, markdown_core_node *sibling) {
    if (!node || node == sibling || !S_can_contain(node->parent, sibling)) {
        return 0;
    }
    markdown_core_node_unlink(sibling);
    markdown_core_node_attach_validated(node->parent, sibling, node);
    return 1;
}

int markdown_core_node_insert_after(markdown_core_node *node, markdown_core_node *sibling) {
    if (!node || node == sibling || !S_can_contain(node->parent, sibling)) {
        return 0;
    }
    markdown_core_node_unlink(sibling);
    markdown_core_node_attach_validated(node->parent, sibling, node->next);
    return 1;
}

int markdown_core_node_replace(markdown_core_node *oldnode, markdown_core_node *newnode) {
    if (!markdown_core_node_insert_before(oldnode, newnode)) {
        return 0;
    }
    markdown_core_node_unlink(oldnode);
    return 1;
}

int markdown_core_node_prepend_child(markdown_core_node *node, markdown_core_node *child) {
    if (!S_can_contain(node, child)) {
        return 0;
    }
    markdown_core_node_unlink(child);
    markdown_core_node_attach_validated(node, child, node->first_child);
    return 1;
}

int markdown_core_node_append_child(markdown_core_node *node, markdown_core_node *child) {
    if (!S_can_contain(node, child)) {
        return 0;
    }
    markdown_core_node_unlink(child);
    markdown_core_node_attach_validated(node, child, NULL);
    return 1;
}

static void S_print_error(FILE *out, markdown_core_node *node, const char *elem) {
    if (out == NULL) {
        return;
    }
    fprintf(out, "Invalid '%s' in node type %s at %d:%d\n", elem, markdown_core_node_get_type_string(node),
            node->start_line, node->start_column);
}

int markdown_core_node_check(markdown_core_node *node, FILE *out) {
    markdown_core_node *cur;
    int errors = 0;

    if (!node) {
        return 0;
    }

    cur = node;
    for (;;) {
        if (cur->first_child) {
            if (cur->first_child->prev != NULL) {
                S_print_error(out, cur->first_child, "prev");
                cur->first_child->prev = NULL;
                ++errors;
            }
            if (cur->first_child->parent != cur) {
                S_print_error(out, cur->first_child, "parent");
                cur->first_child->parent = cur;
                ++errors;
            }
            cur = cur->first_child;
            continue;
        }

    next_sibling:
        if (cur == node) {
            break;
        }
        if (cur->next) {
            if (cur->next->prev != cur) {
                S_print_error(out, cur->next, "prev");
                cur->next->prev = cur;
                ++errors;
            }
            if (cur->next->parent != cur->parent) {
                S_print_error(out, cur->next, "parent");
                cur->next->parent = cur->parent;
                ++errors;
            }
            cur = cur->next;
            continue;
        }

        if (cur->parent->last_child != cur) {
            S_print_error(out, cur->parent, "last_child");
            cur->parent->last_child = cur;
            ++errors;
        }
        cur = cur->parent;
        goto next_sibling;
    }

    return errors;
}

const markdown_core_chunk *markdown_core_node_anchor_chunk(const markdown_core_node *node) {
    if (!node->attributes.anchor.len &&
        (node->kind == MARKDOWN_CORE_NODE_LINK || node->kind == MARKDOWN_CORE_NODE_EMBEDDED) &&
        node->as.link->resource) {
        return &node->as.link->resource->attributes.anchor;
    }
    return &node->attributes.anchor;
}

/* Document-owned definition values are independent roots, not child edges. */
int markdown_core_visit_block_subtrees_since(markdown_core_node *node,
                                             markdown_core_node *last[MARKDOWN_CORE_DOCUMENT_CHAINS],
                                             markdown_core_owned_subtree_visitor visitor, void *context, bool *found) {
    *found = false;
    if (node->kind != MARKDOWN_CORE_NODE_DOCUMENT) {
        return 1;
    }
    markdown_core_node **families[MARKDOWN_CORE_DOCUMENT_CHAINS] = {&node->as.document->footnotes,
                                                                    &node->as.document->specimens};
    for (size_t i = 0; i < MARKDOWN_CORE_DOCUMENT_CHAINS; i++) {
        for (markdown_core_node **slot = last[i] ? &last[i]->next : families[i]; *slot; slot = &(*slot)->next) {
            if (!visitor(slot, context)) {
                return 0;
            }
            last[i] = *slot;
            *found = true;
        }
    }
    return 1;
}

int markdown_core_visit_block_subtrees(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor,
                                       void *context) {
    markdown_core_node *last[MARKDOWN_CORE_DOCUMENT_CHAINS] = {NULL, NULL};
    bool found;
    return markdown_core_visit_block_subtrees_since(node, last, visitor, context, &found);
}

bool markdown_core_node_kind_set_intersects(const markdown_core_node_kind_set *a,
                                            const markdown_core_node_kind_set *b) {
    return (a->blocks & b->blocks) != 0 || (a->inlines & b->inlines) != 0;
}
