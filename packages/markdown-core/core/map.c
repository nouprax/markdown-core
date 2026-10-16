#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "alloc.h"
#include "map.h"

#define KEY_INDEX_MIN_CAPACITY 16

static uint64_t hash_key(const unsigned char *key, bufsize_t key_len) {
    uint64_t hash = UINT64_C(1469598103934665603);
    bufsize_t i;
    for (i = 0; i < key_len; i++) {
        hash ^= key[i];
        hash *= UINT64_C(1099511628211);
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

static int grow_key_index(markdown_core_key_index *index) {
    markdown_core_key_index_slot *slots;
    size_t capacity;
    size_t i;
    if (index->capacity > SIZE_MAX / 2) {
        return 0;
    }
    capacity = index->capacity ? index->capacity * 2 : KEY_INDEX_MIN_CAPACITY;
    if (capacity > SIZE_MAX / sizeof(*slots)) {
        return 0;
    }
    slots = (markdown_core_key_index_slot *)markdown_core_alloc(capacity, sizeof(*slots));
    if (!slots) {
        return 0;
    }
    for (i = 0; i < index->capacity; i++) {
        markdown_core_key_index_slot *source = &index->slots[i];
        markdown_core_key_index_slot *destination;
        if (!source->key) {
            continue;
        }
        destination = find_key_slot(slots, capacity, source->hash, source->key, source->key_len);
        if (!destination) {
            markdown_core_free(slots);
            return 0;
        }
        *destination = *source;
    }
    markdown_core_free(index->slots);
    index->slots = slots;
    index->capacity = capacity;
    return 1;
}

int markdown_core_key_index_init(markdown_core_key_index *index, size_t expected_size) {
    size_t capacity = KEY_INDEX_MIN_CAPACITY;
    memset(index, 0, sizeof(*index));
    if (!expected_size) {
        return 1;
    }
    if (expected_size > SIZE_MAX / 2) {
        return 0;
    }
    while (capacity < expected_size * 2) {
        if (capacity > SIZE_MAX / 2) {
            return 0;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*index->slots)) {
        return 0;
    }
    index->slots = (markdown_core_key_index_slot *)markdown_core_alloc(capacity, sizeof(*index->slots));
    if (!index->slots) {
        return 0;
    }
    index->capacity = capacity;
    return 1;
}

void markdown_core_key_index_free(markdown_core_key_index *index) {
    if (index->slots) {
        markdown_core_free(index->slots);
    }
    memset(index, 0, sizeof(*index));
}

markdown_core_key_index_slot *markdown_core_key_index_entry(markdown_core_key_index *index, const unsigned char *key,
                                                            bufsize_t key_len) {
    uint64_t hash = hash_key(key, key_len);
    if (!index->capacity && !grow_key_index(index)) {
        return NULL;
    }
    markdown_core_key_index_slot *slot = find_key_slot(index->slots, index->capacity, hash, key, key_len);
    if (!slot || slot->key) {
        return slot;
    }
    if (index->size + 1 > index->capacity / 2) {
        if (!grow_key_index(index)) {
            return NULL;
        }
        slot = find_key_slot(index->slots, index->capacity, hash, key, key_len);
        if (!slot) {
            return NULL;
        }
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
    markdown_core_key_index_slot *slot =
        find_key_slot(index->slots, index->capacity, hash_key(key, key_len), key, key_len);
    return slot && slot->key ? slot : NULL;
}

/* REMOVAL KEEPS EVERY PROBE CHAIN WHOLE. An occupied run is what a lookup
 * walks, so the hole a removal leaves is filled by the next entry of the run
 * that may stand there -- one whose home is not between the hole and itself
 * -- and the hole moves to where that entry was, until the run ends. No
 * entry is ever marked deleted, so the table is exactly the one its live
 * keys would have built. */
void markdown_core_key_index_remove(markdown_core_key_index *index, markdown_core_key_index_slot *slot) {
    const size_t mask = index->capacity - 1;
    size_t hole = (size_t)(slot - index->slots);
    for (size_t next = (hole + 1) & mask; index->slots[next].key; next = (next + 1) & mask) {
        size_t home = (size_t)index->slots[next].hash & mask;
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            index->slots[hole] = index->slots[next];
            hole = next;
        }
    }
    index->slots[hole] = (markdown_core_key_index_slot){0};
    index->size--;
}
