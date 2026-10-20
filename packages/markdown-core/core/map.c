#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "alloc.h"
#include "map.h"

#define KEY_INDEX_MIN_CAPACITY 16

/* The key's bytes from `p`, eight or four of them, as one number in the
 * host's byte order: the hash of a key is the index's own, so it need not
 * agree between hosts. */
static inline uint64_t key_word(const unsigned char *p) {
    uint64_t word;
    memcpy(&word, p, sizeof(word));
    return word;
}

static inline uint64_t key_half(const unsigned char *p) {
    uint32_t half;
    memcpy(&half, p, sizeof(half));
    return half;
}

static inline uint64_t mix_word(uint64_t hash, uint64_t word) {
    hash = (hash ^ word) * UINT64_C(0x9e3779b97f4a7c15);
    return hash ^ hash >> 32;
}

/* The key's hash, eight bytes at a time and then the rest as one word, from
 * a start that holds the key's length; the bytes of the rest are each read,
 * in two overlapping halves for four to seven of them, so that keys of one
 * length mix different words. Never zero. */
static uint64_t hash_key(const unsigned char *key, bufsize_t key_len) {
    const size_t length = (size_t)key_len;
    uint64_t hash = UINT64_C(1469598103934665603) ^ length;
    size_t at = 0;
    for (; length - at >= 8; at += 8) {
        hash = mix_word(hash, key_word(key + at));
    }
    const size_t rest = length - at;
    if (rest >= 4) {
        hash = mix_word(hash, key_half(key + at) << 32 | key_half(key + length - 4));
    } else if (rest) {
        hash = mix_word(hash, (uint64_t)key[at] << 16 | (uint64_t)key[at + rest / 2] << 8 | key[length - 1]);
    }
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;
    return hash ? hash : 1;
}

static markdown_core_key_index_slot *find_key_slot(markdown_core_key_index_slot *slots, size_t capacity, uint64_t hash,
                                                   const unsigned char *key, bufsize_t key_len) {
    size_t position = (size_t)hash & (capacity - 1);
    size_t probe;
    for (probe = 0; probe < capacity; probe++) {
        markdown_core_key_index_slot *slot = &slots[position];
        if (!slot->key ||
            (slot->hash == hash && slot->key_len == key_len && memcmp(slot->key, key, (size_t)key_len) == 0)) {
            return slot;
        }
        position = (position + 1) & (capacity - 1);
    }
    return NULL;
}

/* The first vacant slot from `hash`'s home, for a key the table does not
 * hold. */
static markdown_core_key_index_slot *vacant_key_slot(markdown_core_key_index_slot *slots, size_t capacity,
                                                     uint64_t hash) {
    size_t position = (size_t)hash & (capacity - 1);
    while (slots[position].key) {
        position = (position + 1) & (capacity - 1);
    }
    return &slots[position];
}

/* The least capacity that holds `size` keys at most half full, or zero when
 * none can be had. */
static size_t S_capacity_for(size_t size) {
    size_t capacity = KEY_INDEX_MIN_CAPACITY;
    if (size > SIZE_MAX / 2) {
        return 0;
    }
    while (capacity < size * 2) {
        if (capacity > SIZE_MAX / 2) {
            return 0;
        }
        capacity *= 2;
    }
    return capacity <= SIZE_MAX / sizeof(markdown_core_key_index_slot) ? capacity : 0;
}

/* THE MOVE. The older table is emptied run by run: the cursor is a vacant
 * slot, and a move takes the runs after it whole, so every entry left keeps
 * its probe chain from its home, which no vacant slot breaks, and no run
 * reaches past the cursor. A removal from the older table shifts entries
 * within their run, so the cursor stays vacant.
 *
 * Each new key passes KEY_INDEX_MOVE_SLOTS slots of the older table, more to
 * end a run. The older table, of `c` slots, held at most `c / 2` keys when the
 * newer one, of at least `2c`, was had, and the newer one is half full only
 * after `c / 2` more keys: two slots each would empty the older table in
 * time, and four empty it in half the time, so a lookup reads two tables
 * for as short a stretch. */
#define KEY_INDEX_MOVE_SLOTS 4

static void S_drop_older(markdown_core_key_index *index) {
    markdown_core_free(index->older);
    index->older = NULL;
    index->older_capacity = 0;
    index->cursor = 0;
}

/* Move the runs after the cursor to the newer table. A run is cleared as it
 * goes, whole before any lookup reads the older table again. */
static void S_move_runs(markdown_core_key_index *index) {
    markdown_core_key_index_slot *older = index->older;
    const size_t mask = index->older_capacity - 1;
    size_t at = index->cursor;
    for (size_t passed = 0; passed < KEY_INDEX_MOVE_SLOTS && index->left;) {
        at = (at + 1) & mask;
        passed++;
        for (; older[at].key; at = (at + 1) & mask, passed++) {
            *vacant_key_slot(index->slots, index->capacity, older[at].hash) = older[at];
            older[at] = (markdown_core_key_index_slot){0};
            index->left--;
        }
    }
    if (index->left) {
        index->cursor = at;
    } else {
        S_drop_older(index);
    }
}

/* THE NEXT TABLE. The table of twice the slots that the index grows to is
 * made vacant before any key enters it, from the front, a slice for each new
 * key: from three eighths full on, each new key clears KEY_INDEX_CLEAR_SLOTS
 * of its slots, so that at half full, an eighth of the slots in keys later,
 * all of its twice as many slots are vacant. By three eighths full the older
 * table is empty (THE MOVE: the keys from a quarter full, where the table
 * was had, pass every slot of the older table), so the index holds at most
 * two tables. */
#define KEY_INDEX_CLEAR_SLOTS 16

/* Make the next table's first `vacant` slots vacant, taking it first. */
static int S_clear_next(markdown_core_key_index *index, size_t vacant) {
    if (!index->next) {
        if (index->capacity > SIZE_MAX / 2 / sizeof(*index->next) ||
            !(index->next = (markdown_core_key_index_slot *)markdown_core_realloc(NULL, 2 * index->capacity *
                                                                                            sizeof(*index->next)))) {
            return 0;
        }
        index->vacant = 0;
    }
    if (vacant > index->vacant) {
        memset(index->next + index->vacant, 0, (vacant - index->vacant) * sizeof(*index->next));
        index->vacant = vacant;
    }
    return 1;
}

/* Take a table of `capacity` slots, more than the index has, for the keys to
 * come: the next table when it has twice the slots, with the rest of it made
 * vacant. The entries the index holds become the older table's, from which
 * later keys move them; an index with none drops its table. */
static int S_extend(markdown_core_key_index *index, size_t capacity) {
    assert(!index->older);
    markdown_core_key_index_slot *slots;
    if (capacity == 2 * index->capacity) {
        if (!S_clear_next(index, capacity)) {
            return 0;
        }
        slots = index->next;
    } else {
        if (!(slots = (markdown_core_key_index_slot *)markdown_core_alloc(capacity, sizeof(*slots)))) {
            return 0;
        }
        markdown_core_free(index->next);
    }
    index->next = NULL;
    index->vacant = 0;
    if (index->size) {
        index->older = index->slots;
        index->older_capacity = index->capacity;
        index->left = index->size;
        /* A table at most half full has a vacant slot. */
        size_t at = 0;
        while (index->older[at].key) {
            at++;
        }
        index->cursor = at;
    } else if (index->slots) {
        markdown_core_free(index->slots);
    }
    index->slots = slots;
    index->capacity = capacity;
    return 1;
}

int markdown_core_key_index_init(markdown_core_key_index *index, size_t expected_size) {
    memset(index, 0, sizeof(*index));
    return markdown_core_key_index_reserve(index, expected_size);
}

int markdown_core_key_index_reserve(markdown_core_key_index *index, size_t size) {
    if (!size) {
        return 1;
    }
    const size_t capacity = S_capacity_for(size);
    if (!capacity) {
        return 0;
    }
    return capacity <= index->capacity || index->older || S_extend(index, capacity);
}

void markdown_core_key_index_free_tables(markdown_core_key_index *index) {
    markdown_core_free(index->slots);
    markdown_core_free(index->older);
    markdown_core_free(index->next);
    memset(index, 0, sizeof(*index));
}

markdown_core_key_index_slot *markdown_core_key_index_entry(markdown_core_key_index *index, const unsigned char *key,
                                                            bufsize_t key_len) {
    uint64_t hash = hash_key(key, key_len);
    if (!index->capacity && !markdown_core_key_index_reserve(index, 1)) {
        return NULL;
    }
    markdown_core_key_index_slot *slot = find_key_slot(index->slots, index->capacity, hash, key, key_len);
    if (!slot || slot->key) {
        return slot;
    }
    if (index->older) {
        markdown_core_key_index_slot *held = find_key_slot(index->older, index->older_capacity, hash, key, key_len);
        if (held && held->key) {
            return held;
        }
    }
    const size_t size = index->size + 1;
    if (size > index->capacity / 2) {
        const size_t capacity = S_capacity_for(size);
        if (!capacity || !S_extend(index, capacity)) {
            return NULL;
        }
    }
    const size_t clear_from = index->capacity / 8 * 3;
    if (size > clear_from) {
        const size_t vacant = (size - clear_from) * KEY_INDEX_CLEAR_SLOTS;
        if (!S_clear_next(index, vacant < 2 * index->capacity ? vacant : 2 * index->capacity)) {
            return NULL;
        }
    }
    if (index->older) {
        S_move_runs(index);
        slot = vacant_key_slot(index->slots, index->capacity, hash);
    }
    slot->hash = hash;
    slot->key_len = key_len;
    return slot;
}

void markdown_core_key_index_commit(markdown_core_key_index *index, markdown_core_key_index_slot *entry,
                                    const unsigned char *key) {
    assert(!entry->key && key);
    entry->key = key;
    index->size++;
}

markdown_core_key_index_slot *markdown_core_key_index_find(const markdown_core_key_index *index,
                                                           const unsigned char *key, bufsize_t key_len) {
    if (!index->capacity) {
        return NULL;
    }
    const uint64_t hash = hash_key(key, key_len);
    markdown_core_key_index_slot *slot = find_key_slot(index->slots, index->capacity, hash, key, key_len);
    if (slot && !slot->key && index->older) {
        slot = find_key_slot(index->older, index->older_capacity, hash, key, key_len);
    }
    return slot && slot->key ? slot : NULL;
}

/* REMOVAL KEEPS EVERY PROBE CHAIN WHOLE. An occupied run is what a lookup
 * walks, so the hole a removal leaves is filled by the next entry of the run
 * that may stand there -- one whose home is not between the hole and itself
 * -- and the hole moves to where that entry was, until the run ends. No
 * entry is ever marked deleted, so the table is exactly the one its live
 * keys would have built. */
static void S_close_hole(markdown_core_key_index_slot *slots, size_t capacity, size_t hole) {
    const size_t mask = capacity - 1;
    for (size_t next = (hole + 1) & mask; slots[next].key; next = (next + 1) & mask) {
        size_t home = (size_t)slots[next].hash & mask;
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            slots[hole] = slots[next];
            hole = next;
        }
    }
    slots[hole] = (markdown_core_key_index_slot){0};
}

void markdown_core_key_index_remove(markdown_core_key_index *index, markdown_core_key_index_slot *slot) {
    index->size--;
    if (index->older &&
        (uintptr_t)slot - (uintptr_t)index->older < index->older_capacity * sizeof(markdown_core_key_index_slot)) {
        S_close_hole(index->older, index->older_capacity, (size_t)(slot - index->older));
        if (!--index->left) {
            S_drop_older(index);
        }
        return;
    }
    S_close_hole(index->slots, index->capacity, (size_t)(slot - index->slots));
}

/* THE ORDERED INDEX. Every node but the root holds at least half the fanout
 * (no entry is ever removed), so a tree of entries a size_t counts has fewer
 * inner levels than KEY_TREE_LEVELS. */
#define KEY_TREE_FANOUT 16
#define KEY_TREE_LEVELS 24

struct markdown_core_key_tree_leaf {
    uint32_t count;
    struct markdown_core_key_tree_leaf *next;
    markdown_core_key_entry entries[KEY_TREE_FANOUT];
};

typedef struct markdown_core_key_tree_leaf S_tree_leaf;

/* An inner node: its children, the entries below each, and the first key
 * below each, whose head, bytes and length are those of an entry's; the
 * first child's key is never read. */
typedef struct {
    uint32_t count;
    void *children[KEY_TREE_FANOUT];
    size_t sizes[KEY_TREE_FANOUT];
    markdown_core_key_entry keys[KEY_TREE_FANOUT];
} S_tree_inner;

/* The child of `inner` whose subtree holds `key`'s place: the last whose
 * first key is not after it. */
static uint32_t S_tree_child(const S_tree_inner *inner, const markdown_core_key_entry *key) {
    uint32_t low = 1, top = inner->count;
    while (low < top) {
        const uint32_t middle = low + (top - low) / 2;
        if (!markdown_core_key_before(key, &inner->keys[middle])) {
            low = middle + 1;
        } else {
            top = middle;
        }
    }
    return low - 1;
}

/* The first entry of `leaf` whose key is not before `key`'s. */
static uint32_t S_tree_place(const S_tree_leaf *leaf, const markdown_core_key_entry *key) {
    uint32_t low = 0, top = leaf->count;
    while (low < top) {
        const uint32_t middle = low + (top - low) / 2;
        if (markdown_core_key_before(&leaf->entries[middle], key)) {
            low = middle + 1;
        } else {
            top = middle;
        }
    }
    return low;
}

/* The entries below a node `levels` above the leaves. */
static size_t S_tree_size(const void *node, uint32_t levels) {
    if (!levels) {
        return ((const S_tree_leaf *)node)->count;
    }
    const S_tree_inner *inner = node;
    size_t size = 0;
    for (uint32_t i = 0; i < inner->count; i++) {
        size += inner->sizes[i];
    }
    return size;
}

void markdown_core_key_tree_free(markdown_core_key_tree *tree) {
    if (tree->root && !tree->levels) {
        markdown_core_free(tree->root);
    } else if (tree->root) {
        S_tree_inner *stack[KEY_TREE_LEVELS];
        uint32_t next[KEY_TREE_LEVELS];
        uint32_t top = 0;
        stack[0] = tree->root;
        next[0] = 0;
        for (;;) {
            S_tree_inner *inner = stack[top];
            if (next[top] == inner->count) {
                markdown_core_free(inner);
                if (!top) {
                    break;
                }
                top--;
                continue;
            }
            void *child = inner->children[next[top]++];
            if (top + 1 == tree->levels) {
                markdown_core_free(child);
            } else {
                stack[++top] = child;
                next[top] = 0;
            }
        }
    }
    memset(tree, 0, sizeof(*tree));
}

markdown_core_key_entry *markdown_core_key_tree_find(const markdown_core_key_tree *tree,
                                                     const markdown_core_key_entry *key, size_t *rank) {
    size_t before = 0;
    const void *node = tree->root;
    if (!node) {
        if (rank) {
            *rank = 0;
        }
        return NULL;
    }
    for (uint32_t level = tree->levels; level; level--) {
        const S_tree_inner *inner = node;
        const uint32_t child = S_tree_child(inner, key);
        for (uint32_t i = 0; i < child; i++) {
            before += inner->sizes[i];
        }
        node = inner->children[child];
    }
    S_tree_leaf *leaf = (S_tree_leaf *)node;
    const uint32_t place = S_tree_place(leaf, key);
    if (rank) {
        *rank = before + place;
    }
    return place < leaf->count && markdown_core_key_same(&leaf->entries[place], key) ? &leaf->entries[place] : NULL;
}

markdown_core_key_entry *markdown_core_key_tree_at(const markdown_core_key_tree *tree, size_t rank) {
    assert(rank < tree->count);
    const void *node = tree->root;
    for (uint32_t level = tree->levels; level; level--) {
        const S_tree_inner *inner = node;
        uint32_t child = 0;
        while (rank >= inner->sizes[child]) {
            rank -= inner->sizes[child++];
        }
        node = inner->children[child];
    }
    return &((S_tree_leaf *)node)->entries[rank];
}

void markdown_core_key_tree_seek(const markdown_core_key_tree *tree, const markdown_core_key_entry *key,
                                 markdown_core_key_tree_cursor *cursor) {
    const void *node = tree->root;
    if (!node) {
        *cursor = (markdown_core_key_tree_cursor){NULL, 0};
        return;
    }
    for (uint32_t level = tree->levels; level; level--) {
        const S_tree_inner *inner = node;
        node = inner->children[S_tree_child(inner, key)];
    }
    S_tree_leaf *leaf = (S_tree_leaf *)node;
    *cursor = (markdown_core_key_tree_cursor){leaf, S_tree_place(leaf, key)};
}

markdown_core_key_entry *markdown_core_key_tree_next(markdown_core_key_tree_cursor *cursor) {
    while (cursor->leaf && cursor->index == cursor->leaf->count) {
        cursor->leaf = cursor->leaf->next;
        cursor->index = 0;
    }
    return cursor->leaf ? &cursor->leaf->entries[cursor->index++] : NULL;
}

/* AN INSERTION splits each full node on its path, from the leaf up, and the
 * root too when every node on the path is full, which then gains a level
 * above it. The nodes the splits make are had first, so the tree is changed
 * only once nothing can fail. A split node keeps the first half of its
 * entries and one more than the fanout, and the new node after it takes the
 * rest. */
markdown_core_key_entry *markdown_core_key_tree_entry(markdown_core_key_tree *tree, const markdown_core_key_entry *key,
                                                      bool *made) {
    *made = false;
    if (!tree->root) {
        S_tree_leaf *leaf = markdown_core_alloc(1, sizeof(*leaf));
        if (!leaf) {
            return NULL;
        }
        tree->root = leaf;
    }
    S_tree_inner *path[KEY_TREE_LEVELS];
    uint32_t at[KEY_TREE_LEVELS];
    void *node = tree->root;
    for (uint32_t level = 0; level < tree->levels; level++) {
        path[level] = node;
        at[level] = S_tree_child(path[level], key);
        node = path[level]->children[at[level]];
    }
    S_tree_leaf *leaf = node;
    uint32_t place = S_tree_place(leaf, key);
    if (place < leaf->count && markdown_core_key_same(&leaf->entries[place], key)) {
        return &leaf->entries[place];
    }
    uint32_t splits = 0;
    if (leaf->count == KEY_TREE_FANOUT) {
        splits = 1;
        while (splits <= tree->levels && path[tree->levels - splits]->count == KEY_TREE_FANOUT) {
            splits++;
        }
    }
    const uint32_t rooted = splits > tree->levels;
    assert(tree->levels + rooted < KEY_TREE_LEVELS);
    void *room[KEY_TREE_LEVELS + 1];
    for (uint32_t i = 0; i < splits + rooted; i++) {
        room[i] = markdown_core_alloc(1, i ? sizeof(S_tree_inner) : sizeof(S_tree_leaf));
        if (!room[i]) {
            while (i) {
                markdown_core_free(room[--i]);
            }
            return NULL;
        }
    }
    const uint32_t half = (KEY_TREE_FANOUT + 1) / 2;
    markdown_core_key_entry *entry;
    /* The node a split below made, the first key below it and its entries;
     * NULL once a node took it without splitting. */
    void *made_node = NULL;
    markdown_core_key_entry made_key = {0};
    size_t made_size = 0;
    if (!splits) {
        memmove(&leaf->entries[place + 1], &leaf->entries[place], (leaf->count - place) * sizeof(*leaf->entries));
        leaf->count++;
        entry = &leaf->entries[place];
    } else {
        S_tree_leaf *right = room[0];
        const uint32_t from = place < half ? half - 1 : half;
        right->count = KEY_TREE_FANOUT - from;
        memcpy(right->entries, &leaf->entries[from], right->count * sizeof(*leaf->entries));
        leaf->count = from;
        S_tree_leaf *into = place < half ? leaf : right;
        const uint32_t into_place = place < half ? place : place - half;
        memmove(&into->entries[into_place + 1], &into->entries[into_place],
                (into->count - into_place) * sizeof(*into->entries));
        into->count++;
        entry = &into->entries[into_place];
        right->next = leaf->next;
        leaf->next = right;
        made_node = right;
        made_size = right->count;
    }
    *entry = (markdown_core_key_entry){key->head, key->key, key->key_len, 0, NULL};
    if (made_node) {
        made_key = ((S_tree_leaf *)made_node)->entries[0];
    }
    for (uint32_t level = tree->levels; level--;) {
        S_tree_inner *inner = path[level];
        const uint32_t child = at[level];
        if (!made_node) {
            inner->sizes[child]++;
            continue;
        }
        inner->sizes[child] = S_tree_size(inner->children[child], tree->levels - level - 1);
        const uint32_t slot = child + 1;
        if (inner->count < KEY_TREE_FANOUT) {
            const size_t moved = inner->count - slot;
            memmove(&inner->children[slot + 1], &inner->children[slot], moved * sizeof(*inner->children));
            memmove(&inner->sizes[slot + 1], &inner->sizes[slot], moved * sizeof(*inner->sizes));
            memmove(&inner->keys[slot + 1], &inner->keys[slot], moved * sizeof(*inner->keys));
            inner->children[slot] = made_node;
            inner->sizes[slot] = made_size;
            inner->keys[slot] = made_key;
            inner->count++;
            made_node = NULL;
            continue;
        }
        S_tree_inner *right = room[tree->levels - level];
        const uint32_t from = slot < half ? half - 1 : half;
        right->count = KEY_TREE_FANOUT - from;
        memcpy(right->children, &inner->children[from], right->count * sizeof(*inner->children));
        memcpy(right->sizes, &inner->sizes[from], right->count * sizeof(*inner->sizes));
        memcpy(right->keys, &inner->keys[from], right->count * sizeof(*inner->keys));
        inner->count = from;
        S_tree_inner *into = slot < half ? inner : right;
        const uint32_t into_slot = slot < half ? slot : slot - half;
        const size_t moved = into->count - into_slot;
        memmove(&into->children[into_slot + 1], &into->children[into_slot], moved * sizeof(*into->children));
        memmove(&into->sizes[into_slot + 1], &into->sizes[into_slot], moved * sizeof(*into->sizes));
        memmove(&into->keys[into_slot + 1], &into->keys[into_slot], moved * sizeof(*into->keys));
        into->children[into_slot] = made_node;
        into->sizes[into_slot] = made_size;
        into->keys[into_slot] = made_key;
        into->count++;
        made_node = right;
        made_key = right->keys[0];
        made_size = S_tree_size(right, tree->levels - level);
    }
    if (made_node) {
        S_tree_inner *root = room[splits];
        const uint32_t below = tree->levels;
        root->count = 2;
        root->children[0] = tree->root;
        root->sizes[0] = S_tree_size(tree->root, below);
        root->children[1] = made_node;
        root->sizes[1] = made_size;
        root->keys[1] = made_key;
        tree->root = root;
        tree->levels++;
    }
    tree->count++;
    *made = true;
    return entry;
}
