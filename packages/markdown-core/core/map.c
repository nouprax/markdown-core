#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "alloc.h"
#include "map.h"

/* The measure's tables (measure.h), defined here alone. */
#include "measure_tables.inc"

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
uint64_t markdown_core_key_hash(const unsigned char *key, bufsize_t key_len) {
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

/* The slot of the key `probe` stands for, which `holds` tells an occupied
 * slot of the same hash and length holds, or the vacant slot its run ends
 * at; NULL when the table is full. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_key_index_slot *find_slot(markdown_core_key_index_slot *slots, size_t capacity, uint64_t hash,
                                            bufsize_t key_len, markdown_core_key_holds holds, const void *probe) {
    size_t position = (size_t)hash & (capacity - 1);
    for (size_t step = 0; step < capacity; step++) {
        markdown_core_key_index_slot *slot = &slots[position];
        if (!slot->key || (slot->hash == hash && slot->key_len == key_len && holds(probe, slot))) {
            return slot;
        }
        position = (position + 1) & (capacity - 1);
    }
    return NULL;
}

/* A byte key: its bytes, the probe of the byte lookups. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_same(const void *probe,
                                                                   const markdown_core_key_index_slot *slot) {
    return memcmp(slot->key, probe, (size_t)slot->key_len) == 0;
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

/* Move every entry to a table of `capacity` slots, more than the index has. */
static int S_rehash(markdown_core_key_index *index, size_t capacity) {
    markdown_core_key_index_slot *slots;
    size_t i;
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
        /* Keys are distinct: each takes the first vacant slot of its run. */
        size_t position = (size_t)source->hash & (capacity - 1);
        while (slots[position].key) {
            position = (position + 1) & (capacity - 1);
        }
        destination = &slots[position];
        *destination = *source;
    }
    markdown_core_free(index->slots);
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
    return capacity <= index->capacity || S_rehash(index, capacity);
}

void markdown_core_key_index_free(markdown_core_key_index *index) {
    if (index->slots) {
        markdown_core_free(index->slots);
    }
    memset(index, 0, sizeof(*index));
}

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_key_index_slot *S_entry(markdown_core_key_index *index, uint64_t hash, bufsize_t key_len,
                                          markdown_core_key_holds holds, const void *probe) {
    if (!index->capacity && !markdown_core_key_index_reserve(index, 1)) {
        return NULL;
    }
    markdown_core_key_index_slot *slot = find_slot(index->slots, index->capacity, hash, key_len, holds, probe);
    if (!slot || slot->key) {
        return slot;
    }
    if (index->size + 1 > index->capacity / 2) {
        if (!markdown_core_key_index_reserve(index, index->size + 1)) {
            return NULL;
        }
        slot = find_slot(index->slots, index->capacity, hash, key_len, holds, probe);
        if (!slot) {
            return NULL;
        }
    }
    slot->hash = hash;
    slot->key_len = key_len;
    return slot;
}

static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_key_index_slot *S_find(const markdown_core_key_index *index, uint64_t hash, bufsize_t key_len,
                                         markdown_core_key_holds holds, const void *probe) {
    if (!index->capacity) {
        return NULL;
    }
    markdown_core_key_index_slot *slot = find_slot(index->slots, index->capacity, hash, key_len, holds, probe);
    return slot && slot->key ? slot : NULL;
}

markdown_core_key_index_slot *markdown_core_key_index_entry(markdown_core_key_index *index, const unsigned char *key,
                                                            bufsize_t key_len) {
    return S_entry(index, markdown_core_key_hash(key, key_len), key_len, S_same, key);
}

markdown_core_key_index_slot *markdown_core_key_index_entry_hashed(markdown_core_key_index *index, uint64_t hash,
                                                                   const unsigned char *key, bufsize_t key_len) {
    return S_entry(index, hash, key_len, S_same, key);
}

markdown_core_key_index_slot *markdown_core_key_index_place(markdown_core_key_index *index, uint64_t hash,
                                                            bufsize_t key_len, markdown_core_key_holds holds,
                                                            const void *probe) {
    return S_entry(index, hash, key_len, holds, probe);
}

void markdown_core_key_index_commit(markdown_core_key_index *index, markdown_core_key_index_slot *entry,
                                    const unsigned char *key) {
    assert(!entry->key && key);
    entry->key = key;
    index->size++;
}

markdown_core_key_index_slot *markdown_core_key_index_find(const markdown_core_key_index *index,
                                                           const unsigned char *key, bufsize_t key_len) {
    return S_find(index, markdown_core_key_hash(key, key_len), key_len, S_same, key);
}

markdown_core_key_index_slot *markdown_core_key_index_find_hashed(const markdown_core_key_index *index, uint64_t hash,
                                                                  const unsigned char *key, bufsize_t key_len) {
    return S_find(index, hash, key_len, S_same, key);
}

markdown_core_key_index_slot *markdown_core_key_index_seek(const markdown_core_key_index *index, uint64_t hash,
                                                           bufsize_t key_len, markdown_core_key_holds holds,
                                                           const void *probe) {
    return S_find(index, hash, key_len, holds, probe);
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
