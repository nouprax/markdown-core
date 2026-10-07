#include "alloc.h"
#include "map.h"
#include "utf8.h"
#include "parser.h"

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

// normalize map label:  collapse internal whitespace to single space,
// remove leading/trailing whitespace, case fold
// Return 0 if the label is actually empty (i.e. composed solely from
// whitespace)
int normalize_map_label_into(markdown_core_strbuf *normalized, const markdown_core_chunk *ref) {
    markdown_core_strbuf_clear(normalized);
    if (!ref || !ref->len) {
        return 0;
    }
    markdown_core_utf8proc_normalize_label(normalized, ref->data, ref->len);
    return normalized->size && !normalized->oom;
}

unsigned char *normalize_map_label(markdown_core_chunk *ref, int *lost) {
    markdown_core_strbuf normalized = MARKDOWN_CORE_BUF_INIT();
    if (!normalize_map_label_into(&normalized, ref)) {
        if (normalized.oom && lost) {
            *lost = 1;
        }
        markdown_core_strbuf_free(&normalized);
        return NULL;
    }
    return markdown_core_strbuf_detach(&normalized);
}
