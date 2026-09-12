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

int markdown_core_key_index_insert(markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len,
                                   void *value, int replace, void **existing) {
    markdown_core_key_index_slot *slot = markdown_core_key_index_entry(index, key, key_len);
    if (existing) {
        *existing = slot && slot->key ? slot->value.pointer : NULL;
    }
    if (!slot) {
        return 0;
    }
    if (!slot->key) {
        markdown_core_key_index_commit(index, slot, key);
        slot->value.pointer = value;
    } else if (replace) {
        slot->value.pointer = value;
    }
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
            return slot->value.pointer;
        }
        position = (position + 1) & (index->capacity - 1);
    }
    return NULL;
}

// normalize map label:  collapse internal whitespace to single space,
// remove leading/trailing whitespace, case fold
// Return NULL if the label is actually empty (i.e. composed solely from
// whitespace)
int normalize_map_label_into(markdown_core_strbuf *normalized, markdown_core_chunk *ref) {
    markdown_core_strbuf_clear(normalized);
    if (!ref || !ref->len) {
        return 0;
    }
    markdown_core_utf8proc_case_fold(normalized, ref->data, ref->len);
    markdown_core_strbuf_trim(normalized);
    markdown_core_strbuf_normalize_whitespace(normalized);
    return normalized->size && !normalized->oom;
}

unsigned char *normalize_map_label(markdown_core_mem *mem, markdown_core_chunk *ref, int *lost) {
    markdown_core_strbuf normalized = MARKDOWN_CORE_BUF_INIT(mem);
    if (!normalize_map_label_into(&normalized, ref)) {
        if (normalized.oom && lost) {
            *lost = 1;
        }
        markdown_core_strbuf_free(&normalized);
        return NULL;
    }
    return markdown_core_strbuf_detach(&normalized);
}

static int index_map(markdown_core_map *map) {
    markdown_core_map_record *record;
    if (!markdown_core_key_index_init(&map->index, map->mem, map->size)) {
        return 0;
    }
    /* Construction order is independent of source order for mapped block
     * inputs. Explicit definitions precede implicit heading declarations;
     * within either class the first authored occurrence wins. */
    for (record = map->records; record; record = record->next) {
        bufsize_t length = (bufsize_t)strlen((char *)record->label);
        markdown_core_key_index_slot *slot = markdown_core_key_index_entry(&map->index, record->label, length);
        if (!slot) {
            markdown_core_key_index_free(&map->index);
            return 0;
        }
        markdown_core_map_record *existing = slot->key ? slot->value.pointer : NULL;
        if (!existing || (existing->implicit && !record->implicit) ||
            (existing->implicit == record->implicit && record->source_key <= existing->source_key)) {
            if (!existing) {
                markdown_core_key_index_commit(&map->index, slot, record->label);
            }
            slot->value.pointer = record;
        }
    }
    map->size = map->index.size;
    map->prepared = 1;
    return 1;
}

markdown_core_map_record *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label) {
    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH || !map || !map->size || map->oom) {
        return NULL;
    }
    if (!normalize_map_label_into(&map->label_buffer, label)) {
        map->oom = map->label_buffer.oom;
        return NULL;
    }
    if (!map->prepared && !index_map(map)) {
        map->oom = 1;
        return NULL;
    }
    return markdown_core_key_index_lookup(&map->index, map->label_buffer.ptr, map->label_buffer.size);
}

void markdown_core_map_free(markdown_core_map *map) {
    markdown_core_map_record *record;

    if (map == NULL) {
        return;
    }

    record = map->records;
    while (record) {
        markdown_core_map_record *next = record->next;
        /* The map's holder goes; a resource some node still reads through
         * stays with that node, which is how the tree outlives the parser. */
        markdown_core_resource_release(map->mem, record->resource);
        map->mem->free(record);
        record = next;
    }

    markdown_core_key_index_free(&map->index);
    markdown_core_strbuf_free(&map->label_buffer);
    map->mem->free(map);
}

markdown_core_map *markdown_core_map_new(markdown_core_mem *mem) {
    markdown_core_map *map = (markdown_core_map *)mem->calloc(1, sizeof(markdown_core_map));
    if (!map) {
        return NULL;
    }
    map->mem = mem;
    map->label_buffer = (markdown_core_strbuf)MARKDOWN_CORE_BUF_INIT(mem);
    return map;
}
