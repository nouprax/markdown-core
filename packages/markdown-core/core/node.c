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

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) int S_visit_fields(markdown_core_node *node,
                                                                          markdown_core_owned_subtree_visitor visitor,
                                                                          void *context) {
    return S_visit_record_fields(node, visitor, context) &&
           markdown_core_visit_element_subtrees(node, visitor, context);
}

/* A CANDIDATE LETS GO of the content buffer it borrows
 * (markdown_core_node_candidate) as its release begins, which then gives
 * back what the candidate still holds as it does for any node: exactly what
 * it owns. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) void S_drop_borrowed(markdown_core_node *candidate) {
    markdown_core_strbuf_init_zeroed(&candidate->content);
}

/* Releases the nodes on `stack` and everything they hold. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_free_stack(markdown_core_node_pool *pool,
                                                                         markdown_core_node *stack) {
    while (stack) {
        markdown_core_node *e = stack;
        if (e->flags & MARKDOWN_CORE_NODE__BORROWS_CONTENT) {
            S_drop_borrowed(e);
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

/* SWAPPING TWO NODES moves every byte of their slots, so each record in a
 * slot moves with the node it belongs to, and each node's view of it is
 * pointed at the slot the node now lies in. The slots exchange their bytes
 * a block at a time, each block read from both before either is written,
 * the last block what is left of the slot. Every block has a size known
 * when the code is made, so each copy is a few moves, not a call. */
static inline void S_record_follow(markdown_core_node *node) {
    if (node->as.data && !node->node_data_allocation) {
        node->as.data = S_slot_of(node)->record;
    }
}

enum {
    S_SWAP_BLOCK = 96,
    S_SWAP_LAST = sizeof(markdown_core_node_slot) % S_SWAP_BLOCK,
};

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_swap_block(unsigned char *x, unsigned char *y,
                                                                         size_t size) {
    unsigned char held[S_SWAP_BLOCK], taken[S_SWAP_BLOCK];
    memcpy(held, x, size);
    memcpy(taken, y, size);
    memcpy(x, taken, size);
    memcpy(y, held, size);
}

void markdown_core_node_swap(markdown_core_node *a, markdown_core_node *b) {
    unsigned char *x = (unsigned char *)S_slot_of(a), *y = (unsigned char *)S_slot_of(b);
    size_t at = 0;
    for (; at + S_SWAP_BLOCK <= sizeof(markdown_core_node_slot); at += S_SWAP_BLOCK) {
        S_swap_block(x + at, y + at, S_SWAP_BLOCK);
    }
    if (S_SWAP_LAST) {
        S_swap_block(x + at, y + at, S_SWAP_LAST);
    }
    S_record_follow(a);
    S_record_follow(b);
}

/* WHAT A VIEW OR A CANDIDATE HOLDS OF ITS ORIGINAL'S. A view VIEWS each part
 * its original owns whose holder marks what it owns -- a chunk's `alloc`, an
 * attribute value's block and anchor, the element payload's own marks: it
 * holds the part and owns none of it, so releasing the view frees none of
 * it, and it writes a part of its own only where it lets the viewed one go
 * (markdown_core_chunk_view, markdown_core_attributes_view, the element's `view`).
 * It copies what its kind owns with no mark: the labels a record holds in the
 * pool's storage, each at most a label's length, and its places, which its
 * completion replaces. A candidate copies what it viewed. Once a part cannot
 * be had, every later part is left owned by nobody, so releasing the view or
 * the candidate gives back exactly what it took. */
typedef struct {
    markdown_core_node_pool *pool;
    bool failed;
} S_copy_state;

/* The parts of the record the kind owns, as its `free` releases them,
 * viewed and its labels copied (markdown_core_record_ops, `view`). A view is
 * of a block a snapshot lends (markdown_core_node_view), and a candidate of
 * an inline root (markdown_core_node_candidate) -- a block's own, or a field
 * root: a paragraph, a caption or a directive's label, whose record owns
 * nothing --: of no kind made whole as it settles -- a Reference -- or held
 * only by the document, which holds its metadata committed once its prefix
 * is read, and of no kind whose record owns an inline's storage. */
static void S_view_node_as(S_copy_state *state, markdown_core_node *copy, const markdown_core_node *node) {
    const markdown_core_record_ops *record = S_record_of(copy);
    if (record->view && !record->view(state->pool, copy, node, state->failed)) {
        state->failed = true;
    }
}

/* The strings the record of `copy` still views of `node`'s record, copied. */
static void S_own_node_as(S_copy_state *state, markdown_core_node *copy, const markdown_core_node *node) {
    const markdown_core_record_ops *record = S_record_of(copy);
    if (!state->failed && record->own && !record->own(copy, node)) {
        state->failed = true;
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
    if (copy->opaque && !S_payload_of(copy)->view(state->pool, copy, node, state->failed)) {
        copy->opaque = NULL;
        state->failed = true;
    }
    S_view_node_as(state, copy, node);
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
    pair = (S_field_pair){pool, view->opaque, node->opaque};
    markdown_core_visit_element_subtrees(view, S_let_go_field, &pair);
}

/* A visit of a view's fields that hands each it took since to a taker. */
typedef struct {
    markdown_core_owned_subtree_visitor take;
    void *context;
    const unsigned char *storage, *original;
} S_field_taker;

static int S_take_field(markdown_core_node **slot, void *context) {
    const S_field_taker *taker = context;
    if (*slot &&
        (!taker->original ||
         *slot != *(markdown_core_node *const *)(taker->original + ((const unsigned char *)slot - taker->storage)))) {
        return taker->take(slot, taker->context);
    }
    return 1;
}

void markdown_core_node_view_taken(markdown_core_node *view, const markdown_core_node *node,
                                   markdown_core_owned_subtree_visitor take, void *context) {
    S_field_taker taker = {take, context, view->as.data, view->kind == node->kind ? node->as.data : NULL};
    S_visit_record_fields(view, S_take_field, &taker);
    taker.storage = view->opaque;
    taker.original = view->element == node->element ? node->opaque : NULL;
    markdown_core_visit_element_subtrees(view, S_take_field, &taker);
}

static int S_field_other(markdown_core_node **slot, void *context) { return *slot != context; }

bool markdown_core_node_holds_field(const markdown_core_node *node, const markdown_core_node *field) {
    markdown_core_node *const holder = (markdown_core_node *)node;
    return !S_visit_fields(holder, S_field_other, (void *)field);
}

/* The strings of `node`'s record that `view`'s still reads, the view's
 * (markdown_core_record_ops, `adopt`). */
static void S_adopt_node_as(markdown_core_node *view, markdown_core_node *node) {
    const markdown_core_record_ops *record = S_record_of(view);
    if (record->adopt) {
        record->adopt(view, node);
    }
}

void markdown_core_node_view_adopt(markdown_core_node_pool *pool, markdown_core_node *view, markdown_core_node *node) {
    const bool kind = view->kind == node->kind;
    markdown_core_attributes_adopt(&view->attributes, &node->attributes);
    if (kind) {
        S_adopt_node_as(view, node);
    }
    const bool element = view->element == node->element && view->opaque && node->opaque;
    if (element) {
        S_payload_of(view)->adopt(view, node);
    }
    node->children = NULL;
    node->child_count = 0;
    markdown_core_strbuf_init_zeroed(&node->content);
    S_field_pair pair = {pool, node->as.data, kind ? view->as.data : NULL};
    S_visit_record_fields(node, S_let_go_field, &pair);
    pair = (S_field_pair){pool, node->opaque, element ? view->opaque : NULL};
    markdown_core_visit_element_subtrees(node, S_let_go_field, &pair);
}

markdown_core_node *markdown_core_node_view(markdown_core_node_pool *pool, const markdown_core_node *node) {
    markdown_core_node *view = S_copy_slot(pool, node);
    if (!view) {
        return NULL;
    }
    S_copy_state state = {pool, false};
    view->source.places = NULL;
    view->fields = NULL;
    view->flags &= (markdown_core_node_internal_flags)~MARKDOWN_CORE_NODE__PUBLISHED;
    S_view_owned(&state, view, node);
    if (node->fields && !state.failed) {
        const size_t bytes = sizeof(*node->fields) + node->fields->count * sizeof(node->fields->items[0]);
        view->fields = S_bytes_take(pool, bytes);
        if (view->fields) {
            memcpy(view->fields, node->fields, bytes);
        } else {
            state.failed = true;
        }
    }
    if (!(node->flags & MARKDOWN_CORE_NODE__PUBLISHED) && node->source.places && !state.failed) {
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
    copy->fields = NULL;
    copy->flags &= (markdown_core_node_internal_flags)~MARKDOWN_CORE_NODE__PUBLISHED;
    copy->children = NULL;
    copy->child_count = 0;
    if (markdown_core_strbuf_owns(&root->content)) {
        copy->flags |= MARKDOWN_CORE_NODE__BORROWS_CONTENT;
    }
    S_copy_state state = {pool, false};
    /* The fields the block phase wrote, an attribute tail's and the
     * constructs its lines gave up, without those its completion gave up,
     * which its inlines parsed again give up anew. */
    if (root->fields && root->fields->count) {
        const size_t bytes = sizeof(*root->fields) + root->fields->count * sizeof(root->fields->items[0]);
        copy->fields = S_bytes_take(pool, bytes);
        if (copy->fields) {
            memcpy(copy->fields, root->fields, bytes);
            markdown_core_node_drop_opens(copy, false, 0, 0);
        } else {
            state.failed = true;
        }
    }
    markdown_core_attributes_view(&copy->attributes);
    S_view_node_as(&state, copy, root);
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
     * its new kind an id of its own. A snapshot's view of a block it lent is
     * a node the snapshot made once its kind changes (blocks.c, THE
     * LENDING). */
    node->id = 0;
    /* The element flags were the old kind's element's (node.h), which no
     * longer owns the node. */
    node->flags &=
        (markdown_core_node_internal_flags)((MARKDOWN_CORE_NODE__ELEMENT_FIRST - 1) & ~MARKDOWN_CORE_NODE__LENT);
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
