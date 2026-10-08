#include <assert.h>
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

/* THE ROSTERS (registry.h): AVL trees, each node with the size of its
 * subtree. An AVL tree of fewer than 2^64 nodes is at most 92 high, so a
 * path down one fits ROSTER_HEIGHT slots. */
#define ROSTER_HEIGHT 96

static inline uint8_t roster_height(const markdown_core_roster *roster) { return roster ? roster->height : 0; }

static void roster_sum(markdown_core_roster *roster) {
    const uint8_t left = roster_height(roster->left), right = roster_height(roster->right);
    roster->height = (uint8_t)(1 + (left > right ? left : right));
    roster->count = 1 + markdown_core_roster_count(roster->left) + markdown_core_roster_count(roster->right);
}

markdown_core_roster *markdown_core_roster_retain(const markdown_core_roster *roster) {
    markdown_core_roster *held = (markdown_core_roster *)roster;
    if (held) {
        held->refs++;
    }
    return held;
}

void markdown_core_roster_release(markdown_core_roster *roster) {
    if (!roster || --roster->refs) {
        return;
    }
    /* The nodes only this roster held go, a left child turned up above its
     * parent at a time, so no stack is needed; a child something else holds
     * loses this reference and stays. */
    markdown_core_roster *at = roster;
    while (at) {
        markdown_core_roster *left = at->left;
        if (left && left->refs > 1) {
            left->refs--;
            at->left = left = NULL;
        }
        if (left) {
            at->left = left->right;
            left->right = at;
            at = left;
            continue;
        }
        markdown_core_roster *right = at->right;
        markdown_core_free(at);
        at = right && --right->refs == 0 ? right : NULL;
    }
}

const markdown_core_node *markdown_core_roster_at(const markdown_core_roster *roster, size_t index) {
    for (;;) {
        const size_t left = markdown_core_roster_count(roster->left);
        if (index == left) {
            return roster->node;
        }
        if (index < left) {
            roster = roster->left;
        } else {
            index -= left + 1;
            roster = roster->right;
        }
    }
}

size_t markdown_core_roster_rank(const markdown_core_roster *roster,
                                 bool (*before)(const markdown_core_roster *entry, const void *context),
                                 const void *context) {
    size_t rank = 0;
    while (roster) {
        if (before(roster, context)) {
            rank += markdown_core_roster_count(roster->left) + 1;
            roster = roster->right;
        } else {
            roster = roster->left;
        }
    }
    return rank;
}

/* The node at `*slot` is the edited roster's own: a copy takes the place of
 * one something else holds too. False when the copy could not be had. */
static bool roster_own(markdown_core_roster **slot) {
    markdown_core_roster *held = *slot;
    if (held->refs == 1) {
        return true;
    }
    markdown_core_roster *copy = markdown_core_alloc(1, sizeof(*copy));
    if (!copy) {
        return false;
    }
    *copy = *held;
    copy->refs = 1;
    markdown_core_roster_retain(copy->left);
    markdown_core_roster_retain(copy->right);
    held->refs--;
    *slot = copy;
    return true;
}

static void roster_rotate_right(markdown_core_roster **slot) {
    markdown_core_roster *at = *slot, *left = at->left;
    at->left = left->right;
    left->right = at;
    roster_sum(at);
    roster_sum(left);
    *slot = left;
}

static void roster_rotate_left(markdown_core_roster **slot) {
    markdown_core_roster *at = *slot, *right = at->right;
    at->right = right->left;
    right->left = at;
    roster_sum(at);
    roster_sum(right);
    *slot = right;
}

/* The node at `*slot`, whose subtrees are balanced and within two of each
 * other's height, is balanced again. A rotation changes only nodes the
 * edited roster owns; when one could not be owned, the node keeps its shape
 * and its sums, and the answer is false. */
static bool roster_balance(markdown_core_roster **slot) {
    markdown_core_roster *at = *slot;
    const int balance = (int)roster_height(at->left) - (int)roster_height(at->right);
    if (balance > 1) {
        if (!roster_own(&at->left) ||
            (roster_height(at->left->left) < roster_height(at->left->right) && !roster_own(&at->left->right))) {
            roster_sum(at);
            return false;
        }
        if (roster_height(at->left->left) < roster_height(at->left->right)) {
            roster_rotate_left(&at->left);
        }
        roster_rotate_right(slot);
    } else if (balance < -1) {
        if (!roster_own(&at->right) ||
            (roster_height(at->right->right) < roster_height(at->right->left) && !roster_own(&at->right->left))) {
            roster_sum(at);
            return false;
        }
        if (roster_height(at->right->right) < roster_height(at->right->left)) {
            roster_rotate_right(&at->right);
        }
        roster_rotate_left(slot);
    } else {
        roster_sum(at);
    }
    return true;
}

/* The nodes on the path, from the deepest up, take their sums and are
 * balanced again. */
static bool roster_rise(markdown_core_roster **path[], size_t depth) {
    bool ok = true;
    while (depth) {
        ok &= roster_balance(path[--depth]);
    }
    return ok;
}

/* The path from `*root` down to the node at `index`, owned: its slots in
 * `path`, the node's last. Its depth, or 0 when a node could not be owned. */
static size_t roster_descend(markdown_core_roster **root, size_t index, markdown_core_roster **path[]) {
    size_t depth = 0;
    markdown_core_roster **slot = root;
    for (;;) {
        if (!roster_own(slot)) {
            return 0;
        }
        path[depth++] = slot;
        markdown_core_roster *at = *slot;
        const size_t left = markdown_core_roster_count(at->left);
        if (index == left) {
            return depth;
        }
        if (index < left) {
            slot = &at->left;
        } else {
            index -= left + 1;
            slot = &at->right;
        }
    }
}

/* `node` and `order` take place `index` of the roster at `*root`. */
static bool roster_insert(markdown_core_roster **root, size_t index, const markdown_core_node *node,
                          const markdown_core_order *order) {
    markdown_core_roster **path[ROSTER_HEIGHT];
    size_t depth = 0;
    markdown_core_roster **slot = root;
    while (*slot) {
        if (!roster_own(slot)) {
            return false;
        }
        path[depth++] = slot;
        markdown_core_roster *at = *slot;
        const size_t left = markdown_core_roster_count(at->left);
        if (index <= left) {
            slot = &at->left;
        } else {
            index -= left + 1;
            slot = &at->right;
        }
    }
    markdown_core_roster *leaf = markdown_core_alloc(1, sizeof(*leaf));
    if (!leaf) {
        return false;
    }
    *leaf = (markdown_core_roster){1, 1, 1, NULL, NULL, node, order};
    *slot = leaf;
    return roster_rise(path, depth);
}

/* The node at `index` leaves the roster at `*root`. */
static bool roster_remove(markdown_core_roster **root, size_t index) {
    markdown_core_roster **path[ROSTER_HEIGHT];
    size_t depth = roster_descend(root, index, path);
    if (!depth) {
        return false;
    }
    markdown_core_roster *at = *path[depth - 1];
    if (at->left && at->right) {
        /* It takes what the first node after it holds, which goes in its
         * stead. */
        markdown_core_roster **next = &at->right;
        for (;;) {
            if (!roster_own(next)) {
                return false;
            }
            if (!(*next)->left) {
                break;
            }
            path[depth++] = next;
            next = &(*next)->left;
        }
        markdown_core_roster *after = *next;
        at->node = after->node;
        at->order = after->order;
        *next = after->right;
        markdown_core_free(after);
    } else {
        *path[--depth] = at->left ? at->left : at->right;
        markdown_core_free(at);
    }
    return roster_rise(path, depth);
}

/* `node` and `order` take the place of the roster's node at `index`. */
static bool roster_put(markdown_core_roster **root, size_t index, const markdown_core_node *node,
                       const markdown_core_order *order) {
    markdown_core_roster **path[ROSTER_HEIGHT];
    const size_t depth = roster_descend(root, index, path);
    if (!depth) {
        return false;
    }
    (*path[depth - 1])->node = node;
    (*path[depth - 1])->order = order;
    return true;
}

/* The roster that lists `node` in tree order, or -1. */
static int roster_of(const markdown_core_node *node) {
    switch (node->kind) {
    case MARKDOWN_CORE_NODE_FOOTNOTE:
        return MARKDOWN_CORE_ROSTER_FOOTNOTES;
    case MARKDOWN_CORE_NODE_SPECIMEN:
        return MARKDOWN_CORE_ROSTER_SPECIMENS;
    case MARKDOWN_CORE_NODE_REFERENCE:
        return MARKDOWN_CORE_ROSTER_REFERENCES;
    default:
        return -1;
    }
}

static bool roster_before_label(const markdown_core_roster *entry, const void *context) {
    return entry->order->label < *(const uint64_t *)context;
}

/* Where `order`'s node lies in its tree order roster, or is to. */
static size_t roster_place(const markdown_core_roster *roster, const markdown_core_order *order) {
    return markdown_core_roster_rank(roster, roster_before_label, &order->label);
}

/* `order`, just placed, lists its node in its roster. */
static bool roster_enter(markdown_core_registry *registry, const markdown_core_order *order) {
    const int kind = roster_of(order->node);
    return kind < 0 ||
           roster_insert(&registry->rosters[kind], roster_place(registry->rosters[kind], order), order->node, order);
}

/* `order`, still placed, takes its node out of its roster. */
static bool roster_leave(markdown_core_registry *registry, const markdown_core_order *order) {
    const int kind = roster_of(order->node);
    if (kind < 0) {
        return true;
    }
    const size_t index = roster_place(registry->rosters[kind], order);
    assert(index < markdown_core_roster_count(registry->rosters[kind]));
    return roster_remove(&registry->rosters[kind], index);
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

static void order_unlink(markdown_core_order *order) {
    order->prev->next = order->next;
    order->next->prev = order->prev;
    order->prev = order->next = NULL;
}

void markdown_core_order_free(markdown_core_order *order) {
    if (!order) {
        return;
    }
    if (markdown_core_order_joined(order)) {
        order_unlink(order);
    }
    markdown_core_free(order);
}

bool markdown_core_order_unjoin(markdown_core_registry *registry, markdown_core_order *order) {
    const bool ok = roster_leave(registry, order);
    order_unlink(order);
    return ok;
}

bool markdown_core_order_leave(markdown_core_registry *registry, markdown_core_order *order) {
    const bool ok = !order || !markdown_core_order_joined(order) || markdown_core_order_unjoin(registry, order);
    markdown_core_free(order);
    return ok;
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

bool markdown_core_order_join(markdown_core_registry *registry, markdown_core_order *first,
                              markdown_core_order *after) {
    markdown_core_order *const head = &registry->orders;
    if (!head->next) {
        head->next = head->prev = head;
    }
    for (markdown_core_order *order = first, *next; order; order = next) {
        next = order->next;
        markdown_core_order *const at = order->before ? order->before->prev : after;
        assert(at);
        order->before = NULL;
        order_place(registry, order, at);
        after = order;
        if (!roster_enter(registry, order)) {
            return false;
        }
    }
    return true;
}

void markdown_core_registry_dispose(markdown_core_registry *registry) {
    for (size_t i = 0; i < MARKDOWN_CORE_ROSTER_COUNT; i++) {
        markdown_core_roster_release(registry->rosters[i]);
    }
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

bool markdown_core_registry_move(markdown_core_registry *registry, markdown_core_node *from, markdown_core_node *to) {
    markdown_core_registry_unlink(to);
    to->facts = from->facts;
    from->facts = NULL;
    for (markdown_core_fact *fact = to->facts; fact; fact = fact->sibling) {
        fact->node = to;
        /* The node its key resolves to may be this one. */
        key_mark(fact->key);
    }
    to->first = from->first;
    if (to->order == from->order) {
        return true;
    }
    if (!markdown_core_order_leave(registry, to->order)) {
        to->order = NULL;
        return false;
    }
    markdown_core_order *order = to->order = from->order;
    from->order = NULL;
    if (!order) {
        return true;
    }
    order->node = to;
    const int kind = roster_of(to);
    return !markdown_core_order_joined(order) || kind < 0 ||
           roster_put(&registry->rosters[kind], roster_place(registry->rosters[kind], order), to, order);
}

/* A key's label against the label of the node an entry lists: in byte
 * order, the shorter first on a common prefix. */
static bool label_before(const markdown_core_roster *entry, const void *context) {
    const markdown_core_key *key = context;
    markdown_core_chunk label;
    markdown_core_definition_label(entry->node, &label);
    const size_t length = (size_t)label.len, common = length < key->length ? length : key->length;
    const int order = common ? memcmp(label.data, key->label, common) : 0;
    return order < 0 || (order == 0 && length < key->length);
}

/* Whether `fact` resolves its key before `winner`: a Reference before a
 * heading's target, and then the first in tree order. */
static bool fact_wins(const markdown_core_fact *fact, const markdown_core_fact *winner) {
    if (!winner) {
        return true;
    }
    const bool heading = fact->node->kind == MARKDOWN_CORE_NODE_HEADING;
    const bool winner_heading = winner->node->kind == MARKDOWN_CORE_NODE_HEADING;
    if (heading != winner_heading) {
        return winner_heading;
    }
    return fact->node->order->label < winner->node->order->label;
}

bool markdown_core_registry_resolve(markdown_core_registry *registry) {
    static const int labels[] = {
        [MARKDOWN_CORE_KEY_REFERENCE] = MARKDOWN_CORE_ROSTER_REFERENCE_LABELS,
        [MARKDOWN_CORE_KEY_FOOTNOTE] = MARKDOWN_CORE_ROSTER_FOOTNOTE_LABELS,
        [MARKDOWN_CORE_KEY_SPECIMEN] = MARKDOWN_CORE_ROSTER_SPECIMEN_LABELS,
        [MARKDOWN_CORE_KEY_FAMILY] = -1,
    };
    for (const markdown_core_key *key = registry->marked; key; key = key->marked_next) {
        if (labels[key->group] < 0) {
            continue;
        }
        const markdown_core_fact *winner = NULL;
        for (const markdown_core_fact *fact = key->facts; fact; fact = fact->next) {
            if (fact->role == MARKDOWN_CORE_FACT_DECLARE && fact_wins(fact, winner)) {
                winner = fact;
            }
        }
        markdown_core_roster **roster = &registry->rosters[labels[key->group]];
        const size_t index = markdown_core_roster_rank(*roster, label_before, key);
        bool listed = false;
        if (index < markdown_core_roster_count(*roster)) {
            markdown_core_chunk label;
            markdown_core_definition_label(markdown_core_roster_at(*roster, index), &label);
            listed = (size_t)label.len == key->length && !memcmp(label.data, key->label, key->length);
        }
        if (winner && listed && markdown_core_roster_at(*roster, index) == winner->node) {
            continue;
        }
        const bool ok = winner ? listed ? roster_put(roster, index, winner->node, NULL)
                                        : roster_insert(roster, index, winner->node, NULL)
                               : !listed || roster_remove(roster, index);
        if (!ok) {
            return false;
        }
    }
    return true;
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

bool markdown_core_registry_retire(markdown_core_registry *registry, markdown_core_node *root) {
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
        if (!markdown_core_order_leave(registry, node->order)) {
            stack.failed = true;
        }
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
