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
    capacity = index->capacity * 2;
    if (capacity > SIZE_MAX / sizeof(*slots)) {
        return 0;
    }
    slots = (markdown_core_key_index_slot *)index->mem->calloc(capacity, sizeof(*slots));
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
            index->mem->free(slots);
            return 0;
        }
        *destination = *source;
    }
    index->mem->free(index->slots);
    index->slots = slots;
    index->capacity = capacity;
    return 1;
}

int markdown_core_key_index_init(markdown_core_key_index *index, markdown_core_mem *mem, size_t expected_size) {
    size_t capacity = KEY_INDEX_MIN_CAPACITY;
    memset(index, 0, sizeof(*index));
    index->mem = mem;
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
    index->slots = (markdown_core_key_index_slot *)mem->calloc(capacity, sizeof(*index->slots));
    if (!index->slots) {
        return 0;
    }
    index->capacity = capacity;
    return 1;
}

void markdown_core_key_index_free(markdown_core_key_index *index) {
    if (index->slots) {
        index->mem->free(index->slots);
    }
    memset(index, 0, sizeof(*index));
}

int markdown_core_key_index_insert(markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len,
                                   void *value, int replace, void **existing) {
    uint64_t hash = hash_key(key, key_len);
    markdown_core_key_index_slot *slot;
    if (existing) {
        *existing = NULL;
    }
    if (!index || !index->slots || !index->capacity) {
        return 0;
    }
    slot = find_key_slot(index->slots, index->capacity, hash, key, key_len);
    if (!slot) {
        return 0;
    }
    if (slot->key) {
        if (existing) {
            *existing = slot->value;
        }
        if (replace) {
            slot->value = value;
        }
        return 1;
    }
    if (index->size + 1 > index->capacity / 2) {
        if (!grow_key_index(index)) {
            return 0;
        }
        slot = find_key_slot(index->slots, index->capacity, hash, key, key_len);
        if (!slot) {
            return 0;
        }
    }
    slot->hash = hash;
    slot->key = key;
    slot->key_len = key_len;
    slot->value = value;
    index->size++;
    return 1;
}

void *markdown_core_key_index_lookup(const markdown_core_key_index *index, const unsigned char *key,
                                     bufsize_t key_len) {
    uint64_t hash;
    size_t position;
    size_t probe;
    if (!index || !index->slots || !index->capacity) {
        return NULL;
    }
    hash = hash_key(key, key_len);
    position = (size_t)hash & (index->capacity - 1);
    for (probe = 0; probe < index->capacity; probe++) {
        const markdown_core_key_index_slot *slot = &index->slots[position];
        if (!slot->key) {
            return NULL;
        }
        if (slot->hash == hash && slot->key_len == key_len && memcmp(slot->key, key, (size_t)key_len) == 0) {
            return slot->value;
        }
        position = (position + 1) & (index->capacity - 1);
    }
    return NULL;
}

// normalize map label:  collapse internal whitespace to single space,
// remove leading/trailing whitespace, case fold
// Return NULL if the label is actually empty (i.e. composed solely from
// whitespace)
unsigned char *normalize_map_label(markdown_core_mem *mem, markdown_core_chunk *ref, int *lost) {
    markdown_core_strbuf normalized = MARKDOWN_CORE_BUF_INIT(mem);
    unsigned char *result;

    if (ref == NULL) {
        return NULL;
    }

    if (ref->len == 0) {
        return NULL;
    }

    markdown_core_utf8proc_case_fold(&normalized, ref->data, ref->len);
    markdown_core_strbuf_trim(&normalized);
    markdown_core_strbuf_normalize_whitespace(&normalized);

    result = markdown_core_strbuf_detach(&normalized);
    /* NULL distinguishes allocation loss from a legitimately empty label. */
    if (!result) {
        if (lost) {
            *lost = 1;
        }
        return NULL;
    }

    if (result[0] == '\0') {
        mem->free(result);
        return NULL;
    }

    return result;
}

static int index_map(markdown_core_map *map) {
    markdown_core_map_record *record;
    if (!markdown_core_key_index_init(&map->index, map->mem, map->size)) {
        return 0;
    }
    /* Records are linked newest-first. Replacing while traversing therefore
     * leaves the oldest (first source) definition in each slot. */
    for (record = map->records; record; record = record->next) {
        if (!markdown_core_key_index_insert(&map->index, record->label, (bufsize_t)strlen((char *)record->label),
                                            record, 1, NULL)) {
            markdown_core_key_index_free(&map->index);
            return 0;
        }
    }
    map->size = map->index.size;
    map->prepared = 1;
    return 1;
}

markdown_core_map_record *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label) {
    markdown_core_map_record *record = NULL;
    unsigned char *norm;

    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH) {
        return NULL;
    }

    if (map == NULL || !map->size || map->oom) {
        return NULL;
    }

    {
        int lost = 0;
        norm = normalize_map_label(map->mem, label, &lost);
        if (norm == NULL) {
            if (lost) {
                map->oom = 1;
            }
            return NULL;
        }
    }

    if (!map->prepared && !index_map(map)) {
        map->oom = 1;
        map->mem->free(norm);
        return NULL;
    }

    record =
        (markdown_core_map_record *)markdown_core_key_index_lookup(&map->index, norm, (bufsize_t)strlen((char *)norm));
    map->mem->free(norm);

    return record;
}

void markdown_core_map_free(markdown_core_map *map) {
    markdown_core_map_record *record;

    if (map == NULL) {
        return;
    }

    record = map->records;
    while (record) {
        markdown_core_map_record *next = record->next;
        map->mem->free(record->label);
        map->mem->free(record);
        record = next;
    }

    markdown_core_key_index_free(&map->index);
    map->mem->free(map);
}

markdown_core_map *markdown_core_map_new(markdown_core_mem *mem) {
    markdown_core_map *map = (markdown_core_map *)mem->calloc(1, sizeof(markdown_core_map));
    if (!map) {
        return NULL;
    }
    map->mem = mem;
    return map;
}
