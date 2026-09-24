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

static int index_map(markdown_core_map *map) {
    markdown_core_map_record *record;
    if (!markdown_core_key_index_init(&map->index, map->size)) {
        return 0;
    }
    /* Construction order is independent of source order for mapped block
     * inputs. Explicit definitions precede implicit heading declarations;
     * within either class the first authored occurrence wins. */
    for (record = map->records; record; record = record->next) {
        markdown_core_key_index_slot *slot =
            markdown_core_key_index_entry(&map->index, record->label, record->label_len);
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

/* THE MAP'S RECORD STORAGE. A record lives exactly as long as its map -- none
 * is ever freed alone -- so records are carved one after another from blocks
 * the map owns, and the blocks go when the map does. A block starts small, so
 * a map that holds a few headings' records costs one small allocation, and
 * doubles up to a cap, so a document with many definitions pays for its
 * records in a few allocations rather than one each. A record larger than
 * the next block gets a block of its own size. */
struct markdown_core_map_block {
    union {
        markdown_core_map_block *next;
        long double alignment;
        int64_t integer_alignment;
    } head;
};

#define MAP_BLOCK_MIN_BYTES ((size_t)1024)
#define MAP_BLOCK_MAX_BYTES ((size_t)16 * 1024)

void *markdown_core_map_carve(markdown_core_map *map, size_t size) {
    const size_t align = sizeof(markdown_core_map_block);
    if (size > SIZE_MAX - align) {
        return NULL;
    }
    size = (size + align - 1) / align * align;
    if (!map->blocks || map->block_size - map->block_used < size) {
        size_t capacity = map->block_size ? map->block_size * 2 : MAP_BLOCK_MIN_BYTES;
        if (capacity > MAP_BLOCK_MAX_BYTES) {
            capacity = MAP_BLOCK_MAX_BYTES;
        }
        if (capacity < size) {
            capacity = size;
        }
        if (capacity > SIZE_MAX - sizeof(markdown_core_map_block)) {
            return NULL;
        }
        markdown_core_map_block *block =
            (markdown_core_map_block *)markdown_core_realloc(NULL, sizeof(markdown_core_map_block) + capacity);
        if (!block) {
            return NULL;
        }
        block->head.next = map->blocks;
        map->blocks = block;
        map->block_size = capacity;
        map->block_used = 0;
    }
    void *storage = (unsigned char *)(map->blocks + 1) + map->block_used;
    map->block_used += size;
    return storage;
}

void markdown_core_map_free(markdown_core_map *map) {
    markdown_core_map_record *record;

    if (map == NULL) {
        return;
    }

    /* The map's holder goes; a resource some node still reads through stays
     * with that node, which is how the tree outlives the parser. */
    for (record = map->records; record; record = record->next) {
        markdown_core_resource_release(record->resource);
    }
    while (map->blocks) {
        markdown_core_map_block *next = map->blocks->head.next;
        markdown_core_free(map->blocks);
        map->blocks = next;
    }

    markdown_core_key_index_free(&map->index);
    markdown_core_strbuf_free(&map->label_buffer);
    markdown_core_free(map);
}

markdown_core_map *markdown_core_map_new(void) {
    markdown_core_map *map = (markdown_core_map *)markdown_core_alloc(1, sizeof(markdown_core_map));
    if (!map) {
        return NULL;
    }
    map->label_buffer = (markdown_core_strbuf)MARKDOWN_CORE_BUF_INIT();
    return map;
}
