#include "map.h"
#include "utf8.h"
#include "parser.h"

#define KEY_INDEX_MIN_CAPACITY 16
#define KEY_INDEX_MAX_PROBES 64

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
    for (probe = 0; probe < KEY_INDEX_MAX_PROBES; probe++) {
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
    slot = find_key_slot(index->slots, index->capacity, hash, key, key_len);
    if (!slot) {
        /* A full probe run below the load-factor bound means the keys cluster
         * in one bucket window. Doubling once disperses honest clusters via
         * the extra mask bit; engineered identical hashes stay clustered and
         * still fail here, which callers turn into the sorted fallback. */
        if (!grow_key_index(index)) {
            return 0;
        }
        slot = find_key_slot(index->slots, index->capacity, hash, key, key_len);
        if (!slot) {
            return 0;
        }
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
    uint64_t hash = hash_key(key, key_len);
    size_t position = (size_t)hash & (index->capacity - 1);
    size_t probe;
    for (probe = 0; probe < KEY_INDEX_MAX_PROBES; probe++) {
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

static int labelcmp(const unsigned char *a, const unsigned char *b) { return strcmp((const char *)a, (const char *)b); }

static int refcmp(const void *p1, const void *p2) {
    markdown_core_map_entry *r1 = *(markdown_core_map_entry **)p1;
    markdown_core_map_entry *r2 = *(markdown_core_map_entry **)p2;
    int res = labelcmp(r1->label, r2->label);
    return res ? res : ((int)r1->age - (int)r2->age);
}

static int refsearch(const void *label, const void *p2) {
    markdown_core_map_entry *ref = *(markdown_core_map_entry **)p2;
    return labelcmp((const unsigned char *)label, ref->label);
}

static int sort_map(markdown_core_map *map) {
    size_t i = 0, last = 0, size = map->size;
    markdown_core_map_entry *r = map->refs, **sorted = NULL;

    sorted = (markdown_core_map_entry **)map->mem->calloc(size, sizeof(markdown_core_map_entry *));
    if (!sorted) {
        return 0;
    }
    while (r) {
        sorted[i++] = r;
        r = r->next;
    }

    qsort(sorted, size, sizeof(markdown_core_map_entry *), refcmp);

    for (i = 1; i < size; i++) {
        if (labelcmp(sorted[i]->label, sorted[last]->label) != 0) {
            sorted[++last] = sorted[i];
        }
    }

    map->sorted = sorted;
    map->size = last + 1;
    map->prepared = 1;
    return 1;
}

/* Duplicate-heavy definition lists should not pre-size the table by every
 * source occurrence. Sample up to 1024 entries; a unique-heavy sample keeps
 * the flat total-count allocation, a duplicate-heavy one starts at the
 * sampled unique count and relies on amortized growth. */
static size_t map_index_expected_size(markdown_core_map *map) {
    const size_t sample_limit = 1024;
    markdown_core_key_index sample;
    markdown_core_map_entry *ref;
    size_t sampled = 0;
    size_t unique;
    if (map->size <= sample_limit) {
        return map->size;
    }
    if (!markdown_core_key_index_init(&sample, map->mem, sample_limit)) {
        return map->size;
    }
    for (ref = map->refs; ref && sampled < sample_limit; ref = ref->next, sampled++) {
        if (!markdown_core_key_index_insert(&sample, ref->label, (bufsize_t)strlen((char *)ref->label), ref, 0, NULL)) {
            markdown_core_key_index_free(&sample);
            return map->size;
        }
    }
    unique = sample.size;
    markdown_core_key_index_free(&sample);
    return unique > sampled / 2 ? map->size : unique;
}

static int index_map(markdown_core_map *map) {
    markdown_core_map_entry *ref;
    if (!markdown_core_key_index_init(&map->index, map->mem, map_index_expected_size(map))) {
        return 0;
    }
    /* Entries are linked newest-first. Replacing while traversing therefore
     * leaves the oldest (first source) definition in each slot. */
    for (ref = map->refs; ref; ref = ref->next) {
        if (!markdown_core_key_index_insert(&map->index, ref->label, (bufsize_t)strlen((char *)ref->label), ref, 1,
                                            NULL)) {
            markdown_core_key_index_free(&map->index);
            return 0;
        }
    }
    map->size = map->index.size;
    map->prepared = 1;
    map->indexed = 1;
    return 1;
}

markdown_core_map_entry *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label) {
    markdown_core_map_entry **ref = NULL;
    markdown_core_map_entry *r = NULL;
    unsigned char *norm;

    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH) {
        return NULL;
    }

    if (map == NULL || !map->size) {
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

    if (!map->prepared && !index_map(map) && !sort_map(map)) {
        /* Neither preparation path could allocate; report a miss and leave
         * the map unprepared so a later lookup can retry. */
        map->oom = 1;
        map->mem->free(norm);
        return NULL;
    }

    if (map->indexed) {
        r = (markdown_core_map_entry *)markdown_core_key_index_lookup(&map->index, norm,
                                                                      (bufsize_t)strlen((char *)norm));
    } else {
        ref = (markdown_core_map_entry **)bsearch(norm, map->sorted, map->size, sizeof(markdown_core_map_entry *),
                                                  refsearch);
    }
    map->mem->free(norm);

    if (r != NULL || ref != NULL) {
        if (!r) {
            r = ref[0];
        }
        /* Check for expansion limit */
        if (r->size > map->max_ref_size - map->ref_size) {
            return NULL;
        }
        map->ref_size += r->size;
    }

    return r;
}

void markdown_core_map_free(markdown_core_map *map) {
    markdown_core_map_entry *ref;

    if (map == NULL) {
        return;
    }

    ref = map->refs;
    while (ref) {
        markdown_core_map_entry *next = ref->next;
        map->free(map, ref);
        ref = next;
    }

    map->mem->free(map->sorted);
    markdown_core_key_index_free(&map->index);
    map->mem->free(map);
}

markdown_core_map *markdown_core_map_new(markdown_core_mem *mem, markdown_core_map_free_f free) {
    markdown_core_map *map = (markdown_core_map *)mem->calloc(1, sizeof(markdown_core_map));
    if (!map) {
        return NULL;
    }
    map->mem = mem;
    map->free = free;
    map->max_ref_size = UINT_MAX;
    return map;
}
