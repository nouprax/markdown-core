#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AN OPEN-ADDRESSED INDEX FROM BYTE KEYS TO A WORD AND A 32-BIT NUMBER, by
 * linear probing. It borrows its keys: a key's bytes stay where the caller
 * keeps them for as long as the key is in the index, and the caller may
 * point an entry at other bytes that hold the same key. The number takes the
 * room the slot's alignment leaves after the key's length. A vacant entry
 * holds zero in both.
 *
 * IT GROWS A SLICE AT A TIME. A new key that would fill the table past half
 * takes a table of twice the slots, which that key and every later one
 * enter; the entries already held stay in the older table, and each later
 * new key first moves a slice of them over (map.c, THE MOVE). A key is in
 * one table, and a lookup reads the newer, then the older. The table it
 * grows to is made vacant a slice at a time too, by the new keys before it
 * (map.c, THE NEXT TABLE). So no insertion copies or clears a table whole. */
typedef struct markdown_core_key_index_slot {
    uint64_t hash;
    const unsigned char *key;
    bufsize_t key_len;
    uint32_t extra;
    size_t value;
} markdown_core_key_index_slot;

typedef struct markdown_core_key_index {
    markdown_core_key_index_slot *slots;
    size_t capacity;
    /* The keys of both tables. */
    size_t size;
    /* The table the entries are leaving, or NULL; its entries, and the
     * vacant slot of it the next move walks down from. */
    markdown_core_key_index_slot *older;
    size_t older_capacity;
    size_t left;
    size_t cursor;
    /* The table the index grows to, of twice the slots, or NULL, and its
     * slots from the front that are vacant. */
    markdown_core_key_index_slot *next;
    size_t vacant;
} markdown_core_key_index;

int markdown_core_key_index_init(markdown_core_key_index *index, size_t expected_size);
/* Make room for `size` keys in all, so that taking that many grows the index
 * no further: a table that holds them at most half full, to which the
 * entries held move as later keys enter. While entries are moving it makes
 * none, and the keys taken grow the index as they need. Like any new keys,
 * the keys taken from three eighths full on make the table the index grows
 * to vacant ahead (map.c, THE NEXT TABLE). Zero means allocation failure. */
int markdown_core_key_index_reserve(markdown_core_key_index *index, size_t size);
/* markdown_core_key_index_free for an index that has a table. */
void markdown_core_key_index_free_tables(markdown_core_key_index *index);
/* Gives back the index's tables. An index that has none holds nothing. */
static inline void markdown_core_key_index_free(markdown_core_key_index *index) {
    if (index->slots) {
        markdown_core_key_index_free_tables(index);
    }
}
/* Find an occupied or vacant entry, growing only for a new key. NULL means
 * allocation failure. The entry is borrowed until the next insertion; a
 * vacant entry must be committed before another index operation. */
markdown_core_key_index_slot *markdown_core_key_index_entry(markdown_core_key_index *index, const unsigned char *key,
                                                            bufsize_t key_len);
void markdown_core_key_index_commit(markdown_core_key_index *index, markdown_core_key_index_slot *entry,
                                    const unsigned char *key);
/* The occupied entry for `key`, or NULL; never grows the index. */
markdown_core_key_index_slot *markdown_core_key_index_find(const markdown_core_key_index *index,
                                                           const unsigned char *key, bufsize_t key_len);
/* Remove an occupied entry. Every other entry stays findable; entries the
 * index holds may move, so no entry pointer survives the call. */
void markdown_core_key_index_remove(markdown_core_key_index *index, markdown_core_key_index_slot *entry);

/* A KEYED ENTRY: a byte key, a 32-bit number and a pointer. Keys are in
 * byte order, a shorter key first on a common prefix. The first eight bytes,
 * big-endian and padded with zero bytes, are the `head`, which orders two
 * keys whose heads differ; equal heads leave it to the lengths, when either
 * key is all in its head, or to the bytes past the eighth. A key of at most
 * eight bytes is all in its head, so its bytes are never read: a number is
 * the key of its big-endian bytes (markdown_core_key_number). */
typedef struct markdown_core_key_entry {
    uint64_t head;
    const unsigned char *key;
    bufsize_t key_len;
    uint32_t extra;
    const void *value;
} markdown_core_key_entry;

static inline uint64_t markdown_core_key_head(const unsigned char *key, bufsize_t key_len) {
    const bufsize_t bytes = key_len < 8 ? key_len : 8;
    uint64_t head = 0;
    for (bufsize_t i = 0; i < bytes; i++) {
        head = head << 8 | key[i];
    }
    /* The padding: a shift by 64 is undefined, and no bytes is a zero head. */
    return bytes ? head << 8 * (8 - bytes) : 0;
}

/* The entry of key `number`, its four big-endian bytes. */
static inline markdown_core_key_entry markdown_core_key_number(uint32_t number) {
    const markdown_core_key_entry entry = {(uint64_t)number << 32, NULL, 4, number, NULL};
    return entry;
}

/* The order of two keys with equal heads. */
static inline int markdown_core_key_tie_order(const markdown_core_key_entry *a, const markdown_core_key_entry *b) {
    if (a->key_len <= 8 || b->key_len <= 8) {
        return (a->key_len > b->key_len) - (a->key_len < b->key_len);
    }
    const bufsize_t common = (a->key_len < b->key_len ? a->key_len : b->key_len) - 8;
    const int order = memcmp(a->key + 8, b->key + 8, (size_t)common);
    return order ? order : (a->key_len > b->key_len) - (a->key_len < b->key_len);
}

/* Whether `a`'s key sorts before `b`'s, and whether the two keys are the
 * same. */
static inline bool markdown_core_key_before(const markdown_core_key_entry *a, const markdown_core_key_entry *b) {
    return a->head != b->head ? a->head < b->head : markdown_core_key_tie_order(a, b) < 0;
}

static inline bool markdown_core_key_same(const markdown_core_key_entry *a, const markdown_core_key_entry *b) {
    return a->head == b->head && markdown_core_key_tie_order(a, b) == 0;
}

/* AN ORDERED INDEX OF KEYED ENTRIES, one per key: a B+ tree whose inner
 * nodes count the entries below each child, so that the entry of a key and
 * its rank, the number of keys before it, the entry of a rank, and an
 * insertion each take time logarithmic in the entries. Its leaves are linked
 * in key order for a walk. It borrows its keys as the key index does. */
typedef struct markdown_core_key_tree {
    void *root;
    size_t count;
    /* The levels of inner nodes above the leaves. */
    uint32_t levels;
} markdown_core_key_tree;

/* A walk of a tree's entries in key order, from where a seek left it. */
typedef struct markdown_core_key_tree_cursor {
    struct markdown_core_key_tree_leaf *leaf;
    uint32_t index;
} markdown_core_key_tree_cursor;

void markdown_core_key_tree_free(markdown_core_key_tree *tree);
/* The entry of `key`'s key, or NULL, with the rank of that key into
 * `*rank` when `rank` is not NULL. */
markdown_core_key_entry *markdown_core_key_tree_find(const markdown_core_key_tree *tree,
                                                     const markdown_core_key_entry *key, size_t *rank);
/* The entry of `key`'s key, made with `key`'s key, a zero number and a NULL
 * pointer when the tree had none, which `*made` says. NULL, with the tree as
 * it was, when storage could not be had. The entry is borrowed until the
 * next insertion. */
markdown_core_key_entry *markdown_core_key_tree_entry(markdown_core_key_tree *tree, const markdown_core_key_entry *key,
                                                      bool *made);
/* The entry of rank `rank`, below the tree's count. */
markdown_core_key_entry *markdown_core_key_tree_at(const markdown_core_key_tree *tree, size_t rank);
/* Sets `*cursor` at the first entry whose key is not before `key`'s. */
void markdown_core_key_tree_seek(const markdown_core_key_tree *tree, const markdown_core_key_entry *key,
                                 markdown_core_key_tree_cursor *cursor);
/* The cursor's entry, the cursor stepped past it; NULL past the last. */
markdown_core_key_entry *markdown_core_key_tree_next(markdown_core_key_tree_cursor *cursor);

#ifdef __cplusplus
}
#endif

#endif
