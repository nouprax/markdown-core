#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "config.h"
#include "node.h"
#include "claim.h"
#include "element.h"

/* Whether `kind` may be a child at all: a ROOT kind (markdown_core_hold) is
 * an owned root or field, never an ordinary child edge, even under a dynamic
 * policy. Both decision paths share this structural boundary. */
static inline bool S_child_kind_allowed(markdown_core_node_type kind) {
    return !(markdown_core_holds_for_kind(kind)->flags & MARKDOWN_CORE_HOLD_ROOT);
}

bool markdown_core_node_can_contain_builtin(const markdown_core_node *node, markdown_core_node_type child_type) {
    const unsigned ordinal = (unsigned)child_type & MARKDOWN_CORE_NODE_VALUE_MASK;
    if (ordinal >= MARKDOWN_CORE_NODE_KIND_COUNT) {
        return false;
    }

    const markdown_core_node_type *kinds = markdown_core_node_contain_kinds(node);
    if (kinds) {
        const markdown_core_node_type *kind = kinds;
        while (*kind && *kind != node->kind) {
            kind++;
        }
        if (!*kind) {
            return false;
        }
    }

    /* The parent's holds, which its kind's schema declares. */
    const markdown_core_hold *hold = markdown_core_holds_for_kind((markdown_core_node_type)node->kind);
    if ((hold->flags & MARKDOWN_CORE_HOLD_OWNED) && !kinds) {
        return false;
    }
    const uint32_t held = MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type)  ? hold->inlines
                          : MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type) ? hold->blocks
                                                                        : 0;
    return (held >> ordinal) & 1u;
}

#ifndef NDEBUG
bool markdown_core_node_may_attach(const markdown_core_node *parent, markdown_core_node_type child_type) {
    return markdown_core_node_allows(parent) || markdown_core_node_can_contain_builtin(parent, child_type);
}
#endif

bool markdown_core_node_can_contain_type(markdown_core_node *node, markdown_core_node_type child_type) {
    const markdown_core_can_contain_func allows = markdown_core_node_allows(node);
    if (allows) {
        if (!S_child_kind_allowed(child_type)) {
            return false;
        }
        return allows(node->element, node, child_type) != 0;
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
    return pool ? markdown_core_node_pool_bytes(pool, bytes) : markdown_core_bytes_own(bytes);
}

/* A NODE'S RECORDS AND THEIR OPS: the kind's record (`node->as`), whose ops
 * the kind's schema names (element.h, markdown_core_record_for_kind), and
 * the payload of the node's element (`node->opaque`), whose ops are its
 * element's `payload`. Every walk below asks both through the one interface
 * (markdown_core_record_ops) and names no kind. */
static inline const markdown_core_record_ops *S_record_of(const markdown_core_node *node) {
    return markdown_core_record_for_kind((markdown_core_node_type)node->kind);
}

static inline const markdown_core_record_ops *S_payload_of(const markdown_core_node *node) {
    return node->element ? node->element->payload : NULL;
}

/* RECORD SIZE IS A PROPERTY OF THE KIND, so it is an array index: the kind
 * indexes its record's ops (the low bits of a kind are a dense ordinal within
 * its class, and the class is one bit test), which give it. A switch on the
 * kind here was 26.4% of `markdown_core_node_new_with_ext` on the same-job
 * corpus. A kind without a record reads as 0: no inline record, and
 * `as.data` left NULL. */
static size_t S_node_payload_size(markdown_core_node_type type) { return markdown_core_record_for_kind(type)->size; }

markdown_core_node *markdown_core_node_pool_new(markdown_core_node_pool *pool, markdown_core_node_type type,
                                                const markdown_core_element *element) {
    /* Only the active record is an object. Clear it together with the
     * scaffold and the node, including any alignment gap, whether the slot is
     * fresh or reused. An external record is initialized by its own
     * allocation below. */
    const markdown_core_record_ops *record = markdown_core_record_for_kind(type);
    const size_t payload_size = record->size;
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
    if (record->init) {
        record->init(node);
    }
    if (element && element->payload && element->payload->init) {
        element->payload->init(node);
    }

    return node;
}

markdown_core_node *markdown_core_node_new_with_ext(markdown_core_node_type type,
                                                    const markdown_core_element *element) {
    return markdown_core_node_pool_new(NULL, type, element);
}

markdown_core_written *markdown_core_node_field_room(markdown_core_node_pool *pool, markdown_core_node *node,
                                                     uint32_t count) {
    markdown_core_fields *fields = node->fields;
    const uint32_t held = fields ? fields->count : 0;
    const size_t needed = sizeof(*fields) + (size_t)(held + count) * sizeof(fields->items[0]);
    if (!fields || markdown_core_bytes_size(fields) < needed) {
        /* Room for four at first, then twice what it must hold: most nodes
         * that hold fields hold a few. */
        const uint32_t room = held + count <= 4 ? 4 : 2 * (held + count);
        markdown_core_fields *grown = S_bytes_take(pool, sizeof(*fields) + (size_t)room * sizeof(fields->items[0]));
        if (!grown) {
            return NULL;
        }
        grown->count = held;
        if (fields) {
            memcpy(grown->items, fields->items, held * sizeof(fields->items[0]));
            markdown_core_node_pool_bytes_free(pool, fields);
        }
        node->fields = fields = grown;
    }
    return fields->items + held;
}

bool markdown_core_node_add_field(markdown_core_node_pool *pool, markdown_core_node *node, uint8_t slot, uint8_t how,
                                  uint32_t start, uint32_t end, bool unique) {
    const bool anchor = how == MARKDOWN_CORE_FIELD_LINES || how == MARKDOWN_CORE_FIELD_FOUND;
    if (end <= start && !anchor) {
        return true;
    }
    markdown_core_fields *fields = node->fields;
    const markdown_core_written field = {
        .start = start, .end = anchor ? start : end, .slot = slot, .how = how, .raw = true};
    if (fields && unique) {
        for (uint32_t i = 0; i < fields->count; i++) {
            if (fields->items[i].slot == slot && fields->items[i].how == how) {
                fields->items[i] = field;
                return true;
            }
        }
    }
    markdown_core_written *room = markdown_core_node_field_room(pool, node, 1);
    if (!room) {
        return false;
    }
    *room = field;
    node->fields->count++;
    return true;
}

void markdown_core_node_drop_fields(markdown_core_node_pool *pool, markdown_core_node *node) {
    if (node->fields) {
        markdown_core_node_pool_bytes_free(pool, node->fields);
        node->fields = NULL;
    }
}

void markdown_core_node_drop_slots(markdown_core_node *node, uint32_t slots) {
    markdown_core_fields *fields = node->fields;
    uint32_t kept = 0;
    for (uint32_t i = 0; fields && i < fields->count; i++) {
        if (!(slots & (1u << fields->items[i].slot)) || fields->items[i].how != MARKDOWN_CORE_FIELD) {
            fields->items[kept++] = fields->items[i];
        }
    }
    if (fields) {
        fields->count = kept;
    }
}

void markdown_core_node_drop_opens(markdown_core_node *node, bool line, uint32_t raw, uint32_t published) {
    markdown_core_fields *fields = node->fields;
    uint32_t kept = 0;
    bool drop = false;
    for (uint32_t i = 0; fields && i < fields->count; i++) {
        const markdown_core_written *field = &fields->items[i];
        if (field->how == MARKDOWN_CORE_FIELD_OPENER) {
            drop = (bool)(field->slot & 1) == line && field->start >= (field->raw ? raw : published);
        } else if (field->how != MARKDOWN_CORE_FIELD_OPEN) {
            drop = false;
        }
        if (!drop) {
            fields->items[kept++] = *field;
        }
    }
    if (fields) {
        fields->count = kept;
    }
}

void markdown_core_node_keep_anchors(markdown_core_node *node, uint8_t how) {
    markdown_core_fields *fields = node->fields;
    uint32_t kept = 0;
    for (uint32_t i = 0; fields && i < fields->count; i++) {
        const uint8_t was = fields->items[i].how;
        if (was == MARKDOWN_CORE_FIELD_LINES || was == MARKDOWN_CORE_FIELD_FOUND) {
            fields->items[kept] = fields->items[i];
            fields->items[kept++].how = how;
        }
    }
    if (fields) {
        fields->count = kept;
    }
}

markdown_core_node *markdown_core_node_new(markdown_core_node_type type) {
    return markdown_core_node_pool_new(NULL, type, NULL);
}

static void free_node_as(markdown_core_node_pool *pool, markdown_core_node *node,
                         const markdown_core_record_ops *record) {
    if (record->free) {
        record->free(pool, node);
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

/* THE NODE-VALUED FIELDS OF A NODE'S RECORDS (markdown_core_record_ops,
 * `visit`): its kind record's, then its element payload's. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) int S_visit_record_fields(
    markdown_core_node *node, markdown_core_owned_subtree_visitor visitor, void *context) {
    const markdown_core_record_ops *record = S_record_of(node);
    return !record->visit || record->visit(node, visitor, context);
}

/* Releases the nodes on `stack` and everything they hold. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_free_stack(markdown_core_node_pool *pool,
                                                                         markdown_core_node *stack) {
    while (stack) {
        markdown_core_node *e = stack;
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
        if (e->fields) {
            markdown_core_node_pool_bytes_free(pool, e->fields);
        }

        /* The node-valued fields join the same iterative free walk as
         * content: they are pushed onto the rest of the stack, which `e`'s
         * scaffold links until `e` leaves it. */
        const markdown_core_record_ops *record = S_record_of(e);
        if (record->visit) {
            record->visit(e, S_release_owned_subtree, &markdown_core_scaffold_of(e)->next);
        }
        markdown_core_visit_element_subtrees(e, S_release_owned_subtree, &markdown_core_scaffold_of(e)->next);
        stack = markdown_core_scaffold_next(e);
        if (e->opaque) {
            const markdown_core_record_ops *payload = S_payload_of(e);
            if (payload && payload->free) {
                payload->free(pool, e);
            }
        }
        free_node_as(pool, e, record);

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

/* The record of an inline that views the content it was read from takes
 * its bytes (markdown_core_record_ops, `viewed`): every other inline record
 * owns its bytes, its labels in the pool's storage, an element's payload its
 * own. */
bool markdown_core_node_own_viewed(markdown_core_node *node) {
    const markdown_core_record_ops *record = S_record_of(node);
    return !record->viewed || record->viewed(node);
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
    free_node_as(NULL, node, S_record_of(node));
    node->as.data = allocation ? allocation : size ? S_slot_of(node)->record : NULL;
    node->node_data_allocation = allocation;
    if (size) {
        memset(node->as.data, 0, size);
    }
    node->kind = (uint16_t)kind;
    const markdown_core_record_ops *record = S_record_of(node);
    if (record->init) {
        record->init(node);
    }
    /* An id names a node while its kind holds: a publish gives the node of
     * its new kind an id of its own. */
    node->id = 0;
    /* The element flags were the old kind's element's (node.h), which no
     * longer owns the node. */
    node->flags &= (markdown_core_node_internal_flags)(MARKDOWN_CORE_NODE__ELEMENT_FIRST - 1);
    return MARKDOWN_CORE_NODE_SET_KIND_OK;
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
    if (markdown_core_bytes_size(*items) <= held * sizeof(**items)) {
        markdown_core_node **moved = S_bytes_take(pool, 2 * held * sizeof(*moved));
        if (!moved) {
            return false;
        }
        memcpy(moved, *items, held * sizeof(*moved));
        markdown_core_node_pool_bytes_free(pool, *items);
        *items = moved;
    }
    (*items)[held] = node;
    *count = held + 1;
    return true;
}

bool markdown_core_node_kind_set_intersects(const markdown_core_node_kind_set *a,
                                            const markdown_core_node_kind_set *b) {
    return (a->blocks & b->blocks) != 0 || (a->inlines & b->inlines) != 0;
}

void markdown_core_literal_halves(const markdown_core_node *node, const markdown_core_chunk *literal, bufsize_t start,
                                  const unsigned char **front, bufsize_t *front_size, const unsigned char **back,
                                  bufsize_t *back_size) {
    const unsigned char *data = markdown_core_claim_read((void *const *)&literal->data);
    if (data || !literal->len) {
        *front = data;
        *front_size = literal->len;
        *back = NULL;
        *back_size = 0;
        return;
    }
    markdown_core_strbuf_span(&node->content, start, start + literal->len, front, front_size, back, back_size);
}

/* A pending literal, joined: the first reader's join stands. */
bool markdown_core_literal_whole(const markdown_core_node *node, markdown_core_chunk *literal, bufsize_t start,
                                 const unsigned char **data) {
    const unsigned char *front, *back;
    bufsize_t front_size, back_size;
    markdown_core_literal_halves(node, literal, start, &front, &front_size, &back, &back_size);
    if (!back_size) {
        *data = front;
        return true;
    }
    unsigned char *joined = markdown_core_alloc((size_t)literal->len + 1, 1);
    if (!joined) {
        *data = NULL;
        return false;
    }
    if (front_size) {
        memcpy(joined, front, (size_t)front_size);
    }
    memcpy(joined + front_size, back, (size_t)back_size);
    joined[literal->len] = '\0';
    unsigned char *stands = markdown_core_claim((void **)&literal->data, joined);
    if (stands != joined) {
        markdown_core_free(joined);
    }
    *data = stands;
    return true;
}
