#include <string.h>

#include "alloc.h"
#include "node.h"
#include "registry.h"
#include "map.h"

#define REGISTRY_MIN_CAPACITY 16
/* Every role, to unlink_facts. */
#define FACT_ANY (-1)

static uint64_t key_hash(markdown_core_key_group group, const unsigned char *label, uint32_t length) {
    uint64_t hash = UINT64_C(1469598103934665603) ^ (uint64_t)group;
    for (uint32_t i = 0; i < length; i++) {
        hash ^= label[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    return hash;
}

void markdown_core_registry_begin(markdown_core_registry *registry) { registry->edit++; }

/* A key about to gain or lose a fact is marked for the edit, and remembers
 * whether it was defined before the edit first changed it. */
static void key_mark(markdown_core_key *key) {
    markdown_core_registry *registry = key->registry;
    if (key->marked == registry->edit) {
        return;
    }
    if (!key->marked) {
        key->marked_next = registry->marked;
        registry->marked = key;
    }
    key->marked = registry->edit;
    key->was = key->declared > 0;
}

/* THE ORDER LIST. Labels lie below ORDER_TOP; the list's head stands for
 * 0 before the first and ORDER_TOP after the last. */
#define ORDER_TOP (UINT64_C(1) << 62)

markdown_core_order *markdown_core_order_new(markdown_core_node *node) {
    markdown_core_order *order = markdown_core_alloc(1, sizeof(*order));
    if (order) {
        order->node = node;
    }
    return order;
}

void markdown_core_order_free(markdown_core_order *order) {
    if (!order) {
        return;
    }
    if (markdown_core_order_joined(order)) {
        order->prev->next = order->next;
        order->next->prev = order->prev;
    }
    markdown_core_free(order);
}

/* `order` joins the list right after `before`, with a label between theirs;
 * when there is none, the orders whose labels share the most leading bits
 * with `before`'s and are few enough for the room those bits leave -- at
 * most (10/7)^i of them for i free bits -- take labels spread evenly over
 * it. Each label spread pays for the insertions that filled its range, so
 * an insertion costs O(log n) amortized. */
static void order_place(markdown_core_registry *registry, markdown_core_order *order, markdown_core_order *before) {
    markdown_core_order *const head = &registry->orders;
    markdown_core_order *after = before->next;
    order->prev = before;
    order->next = after;
    before->next = order;
    after->prev = order;
    const uint64_t low = before == head ? 0 : before->label + 1;
    const uint64_t high = after == head ? ORDER_TOP : after->label;
    if (low < high) {
        order->label = low + (high - low) / 2;
        return;
    }
    /* The orders from `first` to `last` have labels in [base, base + 2^bits). */
    markdown_core_order *first = order, *last = order;
    size_t count = 1;
    double room = 1;
    for (unsigned bits = 1; bits <= 62; bits++) {
        room *= 10.0 / 7.0;
        const uint64_t base = before == head ? 0 : before->label & ~((UINT64_C(1) << bits) - 1);
        const uint64_t top = base + (UINT64_C(1) << bits);
        while (first->prev != head && first->prev->label >= base) {
            first = first->prev;
            count++;
        }
        while (last->next != head && last->next->label < top) {
            last = last->next;
            count++;
        }
        if ((double)count <= room || bits == 62) {
            const uint64_t step = (UINT64_C(1) << bits) / count;
            uint64_t label = base;
            for (markdown_core_order *at = first;; at = at->next) {
                at->label = label;
                label += step;
                if (at == last) {
                    return;
                }
            }
        }
    }
}

void markdown_core_order_join(markdown_core_registry *registry, markdown_core_order *first) {
    markdown_core_order *const head = &registry->orders;
    if (!head->next) {
        head->next = head->prev = head;
    }
    for (markdown_core_order *order = first, *next; order; order = next) {
        next = order->next;
        markdown_core_order *const before = order->before ? order->before : head;
        order->before = NULL;
        order_place(registry, order, before->prev);
    }
}

void markdown_core_registry_dispose(markdown_core_registry *registry) {
    /* The nodes outlive the registry: they hold no order now. */
    markdown_core_order *const head = &registry->orders;
    for (markdown_core_order *order = head->next, *next; order && order != head; order = next) {
        next = order->next;
        order->node->order = NULL;
        markdown_core_free(order);
    }
    for (size_t i = 0; i < registry->capacity; i++) {
        for (markdown_core_key *key = registry->buckets[i], *next; key; key = next) {
            next = key->chain;
            for (int list = 0; list < 2; list++) {
                for (markdown_core_fact *fact = list ? key->lookups : key->facts, *after; fact; fact = after) {
                    after = fact->next;
                    /* The node outlives the registry: it holds no fact now. */
                    fact->node->facts = NULL;
                    markdown_core_free(fact);
                }
            }
            markdown_core_free(key);
        }
    }
    if (registry->buckets) {
        markdown_core_free(registry->buckets);
    }
    if (registry->scratch.ptr) {
        markdown_core_strbuf_free(&registry->scratch);
    }
    *registry = (markdown_core_registry){0};
}

static bool registry_grow(markdown_core_registry *registry) {
    const size_t capacity = registry->capacity ? registry->capacity * 2 : REGISTRY_MIN_CAPACITY;
    markdown_core_key **buckets = markdown_core_alloc(capacity, sizeof(*buckets));
    if (!buckets) {
        return false;
    }
    for (size_t i = 0; i < registry->capacity; i++) {
        for (markdown_core_key *key = registry->buckets[i], *next; key; key = next) {
            next = key->chain;
            markdown_core_key **bucket = &buckets[key->hash & (capacity - 1)];
            key->chain = *bucket;
            *bucket = key;
        }
    }
    markdown_core_free(registry->buckets);
    registry->buckets = buckets;
    registry->capacity = capacity;
    return true;
}

/* The key (`group`, `label`), made when it is new; NULL when it could not
 * be. */
static markdown_core_key *key_find(markdown_core_registry *registry, markdown_core_key_group group,
                                   const unsigned char *label, uint32_t length) {
    const uint64_t hash = key_hash(group, label, length);
    if (registry->capacity) {
        for (markdown_core_key *key = registry->buckets[hash & (registry->capacity - 1)]; key; key = key->chain) {
            if (key->hash == hash && key->group == group && key->length == length &&
                !memcmp(key->label, label, length)) {
                return key;
            }
        }
    }
    if (registry->count + 1 > registry->capacity / 4 * 3 && !registry_grow(registry)) {
        return NULL;
    }
    markdown_core_key *key = markdown_core_alloc(1, sizeof(*key) + length + 1);
    if (!key) {
        return NULL;
    }
    key->registry = registry;
    key->hash = hash;
    key->group = (uint8_t)group;
    key->length = length;
    memcpy(key->label, label, length);
    key->label[length] = '\0';
    markdown_core_key **bucket = &registry->buckets[hash & (registry->capacity - 1)];
    key->chain = *bucket;
    *bucket = key;
    registry->count++;
    /* A key is marked from its making, so one no fact comes to hold goes as
     * the edit settles. */
    key_mark(key);
    return key;
}

static markdown_core_fact **key_list(markdown_core_key *key, uint8_t role) {
    return role == MARKDOWN_CORE_FACT_LOOKUP ? &key->lookups : &key->facts;
}

static void fact_join(markdown_core_fact *fact) {
    markdown_core_fact **list = key_list(fact->key, fact->role);
    fact->prev = NULL;
    fact->next = *list;
    if (*list) {
        (*list)->prev = fact;
    }
    *list = fact;
}

static void fact_leave(markdown_core_fact *fact) {
    markdown_core_key *key = fact->key;
    if (fact->prev) {
        fact->prev->next = fact->next;
    } else {
        *key_list(key, fact->role) = fact->next;
    }
    if (fact->next) {
        fact->next->prev = fact->prev;
    }
}

markdown_core_fact *markdown_core_registry_declare(markdown_core_registry *registry, markdown_core_node *node,
                                                   markdown_core_key_group group, markdown_core_fact_role role,
                                                   const unsigned char *label, uint32_t length,
                                                   const unsigned char *text, uint32_t text_length) {
    markdown_core_key *key = key_find(registry, group, label, length);
    markdown_core_fact *fact = key ? markdown_core_alloc(1, sizeof(*fact) + text_length) : NULL;
    if (!fact) {
        return NULL;
    }
    key_mark(key);
    *fact = (markdown_core_fact){.key = key,
                                 .node = node,
                                 .sibling = node->facts,
                                 .edit = registry->edit,
                                 .role = (uint8_t)role,
                                 .length = text_length};
    if (text_length) {
        memcpy(fact->text, text, text_length);
    }
    node->facts = fact;
    fact_join(fact);
    key->declared += role == MARKDOWN_CORE_FACT_DECLARE;
    return fact;
}

bool markdown_core_registry_ask(markdown_core_registry *registry, markdown_core_node *holder,
                                markdown_core_key_group group, const unsigned char *label, uint32_t length,
                                const markdown_core_key **asked, bool *failed) {
    markdown_core_key *key = key_find(registry, group, label, length);
    *asked = key;
    if (!key) {
        *failed = true;
        return false;
    }
    /* A root asks a key once: the question it asked last is this one. */
    markdown_core_fact *last = key->lookups;
    if (!last || last->node != holder || last->edit != registry->edit) {
        markdown_core_fact *fact = markdown_core_alloc(1, sizeof(*fact));
        if (!fact) {
            *failed = true;
            return false;
        }
        *fact = (markdown_core_fact){.key = key,
                                     .node = holder,
                                     .sibling = holder->facts,
                                     .edit = registry->edit,
                                     .role = MARKDOWN_CORE_FACT_LOOKUP};
        holder->facts = fact;
        fact_join(fact);
    }
    return markdown_core_key_defined(key);
}

const markdown_core_strbuf *markdown_core_registry_normalize(markdown_core_registry *registry,
                                                             const markdown_core_chunk *label, bool *failed) {
    markdown_core_strbuf *scratch = &registry->scratch;
    /* A registry starts zeroed, its scratch with it. */
    if (!scratch->ptr) {
        markdown_core_strbuf_init_zeroed(scratch);
    }
    if (!normalize_map_label_into(scratch, label)) {
        *failed = scratch->oom;
        return NULL;
    }
    return scratch;
}

/* The facts of `node` with `role`, or every fact when `role` is
 * FACT_ANY, leave their keys. */
static void unlink_facts(markdown_core_node *node, int role) {
    for (markdown_core_fact **at = &node->facts, *fact; (fact = *at);) {
        if (role != FACT_ANY && fact->role != role) {
            at = &fact->sibling;
            continue;
        }
        *at = fact->sibling;
        markdown_core_key *key = fact->key;
        key_mark(key);
        fact_leave(fact);
        key->declared -= fact->role == MARKDOWN_CORE_FACT_DECLARE;
        markdown_core_free(fact);
    }
}

void markdown_core_registry_unlink(markdown_core_node *node) { unlink_facts(node, FACT_ANY); }

void markdown_core_registry_unask(markdown_core_node *node) { unlink_facts(node, MARKDOWN_CORE_FACT_LOOKUP); }

void markdown_core_registry_move(markdown_core_node *from, markdown_core_node *to) {
    markdown_core_registry_unlink(to);
    to->facts = from->facts;
    from->facts = NULL;
    for (markdown_core_fact *fact = to->facts; fact; fact = fact->sibling) {
        fact->node = to;
    }
    if (to->order != from->order) {
        markdown_core_order_free(to->order);
        to->order = from->order;
        from->order = NULL;
        if (to->order) {
            to->order->node = to;
        }
    }
    to->first = from->first;
}

void markdown_core_registry_settle(markdown_core_registry *registry) {
    for (markdown_core_key *key = registry->marked, *next; key; key = next) {
        next = key->marked_next;
        key->marked = 0;
        key->marked_next = NULL;
        if (key->facts || key->lookups) {
            continue;
        }
        markdown_core_key **at = &registry->buckets[key->hash & (registry->capacity - 1)];
        while (*at != key) {
            at = &(*at)->chain;
        }
        *at = key->chain;
        registry->count--;
        markdown_core_free(key);
    }
    registry->marked = NULL;
}

/* What retiring has yet to visit: nodes, and stems of children. */
typedef struct {
    union {
        markdown_core_node *node;
        markdown_core_stem *stem;
    } as;
    bool stem;
} retire_item;

typedef struct {
    retire_item *items;
    size_t count, capacity;
    bool failed;
} retire_stack;

static void retire_push(retire_stack *stack, retire_item item) {
    if (stack->failed) {
        return;
    }
    if (stack->count == stack->capacity) {
        const size_t capacity = stack->capacity ? stack->capacity * 2 : 64;
        retire_item *items = markdown_core_realloc(stack->items, capacity * sizeof(*items));
        if (!items) {
            stack->failed = true;
            return;
        }
        stack->items = items;
        stack->capacity = capacity;
    }
    stack->items[stack->count++] = item;
}

static int retire_field(markdown_core_node **slot, void *context) {
    if (*slot) {
        retire_push(context, (retire_item){.as.node = *slot});
    }
    return 1;
}

bool markdown_core_registry_retire(markdown_core_node *root) {
    retire_stack stack = {0};
    retire_push(&stack, (retire_item){.as.node = root});
    while (stack.count && !stack.failed) {
        const retire_item item = stack.items[--stack.count];
        if (item.stem) {
            /* A stem the new tree holds too holds what the parse took. */
            markdown_core_stem *stem = item.as.stem;
            if (stem->refs > 1) {
                continue;
            }
            for (uint8_t i = 0; i < stem->width; i++) {
                retire_push(&stack, stem->height ? (retire_item){.as.stem = stem->entries[i].stem, .stem = true}
                                                 : (retire_item){.as.node = stem->entries[i].node});
            }
            continue;
        }
        markdown_core_node *node = item.as.node;
        /* A node the new tree holds too was taken, with all it holds. */
        if (node->refs > 1) {
            continue;
        }
        markdown_core_registry_unlink(node);
        markdown_core_order_free(node->order);
        node->order = NULL;
        markdown_core_node_visit_fields(node, retire_field, &stack);
        if (node->children) {
            retire_push(&stack, (retire_item){.as.stem = node->children, .stem = true});
        }
    }
    const bool ok = !stack.failed;
    markdown_core_free(stack.items);
    return ok;
}

markdown_core_chunk markdown_core_anchor_stem(const unsigned char *spelling, uint32_t length) {
    for (;;) {
        uint32_t at = length;
        while (at > 0 && spelling[at - 1] >= '0' && spelling[at - 1] <= '9') {
            at--;
        }
        if (at == length || at == 0 || spelling[at - 1] != '-') {
            return (markdown_core_chunk){(unsigned char *)spelling, (bufsize_t)length, 0};
        }
        length = at - 1;
    }
}
