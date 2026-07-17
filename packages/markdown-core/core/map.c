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
            /* The key bytes match, but the stored pointer may belong to an
             * entry that is about to be removed; repoint it at the caller's
             * storage together with the value. */
            slot->key = key;
            slot->key_len = key_len;
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

int markdown_core_key_index_remove(markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len) {
    uint64_t hash = hash_key(key, key_len);
    size_t mask;
    size_t position;
    size_t probe;
    size_t gap;
    size_t scan;
    int found = 0;

    if (!index->capacity) {
        return 0;
    }
    mask = index->capacity - 1;
    position = (size_t)hash & mask;
    for (probe = 0; probe < KEY_INDEX_MAX_PROBES; probe++) {
        markdown_core_key_index_slot *slot = &index->slots[position];
        if (!slot->key) {
            return 0;
        }
        if (slot->hash == hash && slot->key_len == key_len && memcmp(slot->key, key, (size_t)key_len) == 0) {
            found = 1;
            break;
        }
        position = (position + 1) & mask;
    }
    if (!found) {
        return 0;
    }

    /* Backward-shift deletion: walk the collision run after the gap and pull
     * every entry whose home slot lies at or before the gap into it. The
     * load-factor bound guarantees an empty slot, so the walk terminates. */
    gap = position;
    scan = (gap + 1) & mask;
    while (index->slots[scan].key) {
        size_t home = (size_t)index->slots[scan].hash & mask;
        if (((scan - home) & mask) >= ((scan - gap) & mask)) {
            index->slots[gap] = index->slots[scan];
            gap = scan;
        }
        scan = (scan + 1) & mask;
    }
    memset(&index->slots[gap], 0, sizeof(index->slots[gap]));
    index->size--;
    return 1;
}

// normalize map label:  collapse internal whitespace to single space,
// remove leading/trailing whitespace, case fold
// Return NULL if the label is actually empty (i.e. composed solely from
// whitespace)
unsigned char *markdown_core_map_normalize_label(markdown_core_mem *mem, markdown_core_chunk *ref, int *lost) {
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
    if (res) {
        return res;
    }
    if (r1->order != r2->order) {
        return r1->order < r2->order ? -1 : 1;
    }
    return 0;
}

static int refsearch(const void *label, const void *p2) {
    markdown_core_map_entry *ref = *(markdown_core_map_entry **)p2;
    return labelcmp((const unsigned char *)label, ref->label);
}

/* Drops the prepared lookup structures. Lossless: the live chain still holds
 * every entry, so the next lookup rebuilds. */
static void unprepare_map(markdown_core_map *map) {
    if (map->sorted) {
        map->mem->free(map->sorted);
        map->sorted = NULL;
    }
    markdown_core_key_index_free(&map->index);
    map->prepared = 0;
    map->indexed = 0;
}

/* Sorted fallback: every entry, duplicates included, ordered by (label,
 * document order). The winner for a label is the first entry of its run. */
static int sort_map(markdown_core_map *map) {
    size_t i = 0, size = map->size;
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

    map->sorted = sorted;
    map->prepared = 1;
    map->indexed = 0;
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

/* Splices `entry` into its label bucket in ascending document order and
 * keeps the index slot pointing at the bucket head (the winner). Returns 0
 * when the index could not take the label. */
static int bucket_attach(markdown_core_map *map, markdown_core_map_entry *entry) {
    bufsize_t label_len = (bufsize_t)strlen((char *)entry->label);
    markdown_core_map_entry *head =
        (markdown_core_map_entry *)markdown_core_key_index_lookup(&map->index, entry->label, label_len);
    markdown_core_map_entry *cur;

    if (!head) {
        entry->bucket_next = NULL;
        return markdown_core_key_index_insert(&map->index, entry->label, label_len, entry, 0, NULL);
    }
    if (entry->order <= head->order) {
        entry->bucket_next = head;
        return markdown_core_key_index_insert(&map->index, entry->label, label_len, entry, 1, NULL);
    }
    cur = head;
    while (cur->bucket_next && cur->bucket_next->order < entry->order) {
        cur = cur->bucket_next;
    }
    entry->bucket_next = cur->bucket_next;
    cur->bucket_next = entry;
    return 1;
}

/* Hash path: label -> bucket head. The live chain is newest-first with
 * monotonic orders, so the descending traversal hits bucket_attach's prepend
 * fast path and the build stays O(entries) even when one label repeats. */
static int index_map(markdown_core_map *map) {
    markdown_core_map_entry *ref;
    if (!markdown_core_key_index_init(&map->index, map->mem, map_index_expected_size(map))) {
        return 0;
    }
    for (ref = map->refs; ref; ref = ref->next) {
        if (!bucket_attach(map, ref)) {
            markdown_core_key_index_free(&map->index);
            return 0;
        }
    }
    map->prepared = 1;
    map->indexed = 1;
    return 1;
}

static int prepare_map(markdown_core_map *map) { return map->prepared || index_map(map) || sort_map(map); }

/* Leftmost entry of the label's run in the sorted array: the winner. */
static markdown_core_map_entry *sorted_winner(markdown_core_map *map, const unsigned char *label) {
    markdown_core_map_entry **ref = (markdown_core_map_entry **)bsearch(label, map->sorted, map->size,
                                                                        sizeof(markdown_core_map_entry *), refsearch);
    if (!ref) {
        return NULL;
    }
    while (ref > map->sorted && labelcmp(ref[-1]->label, label) == 0) {
        ref--;
    }
    return *ref;
}

markdown_core_map_entry *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label) {
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
        norm = markdown_core_map_normalize_label(map->mem, label, &lost);
        if (norm == NULL) {
            if (lost) {
                map->oom = 1;
            }
            return NULL;
        }
    }

    if (!prepare_map(map)) {
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
        r = sorted_winner(map, norm);
    }
    map->mem->free(norm);

    if (r != NULL) {
        /* Check for expansion limit */
        if (r->size > map->max_ref_size - map->ref_size) {
            return NULL;
        }
        map->ref_size += r->size;
    }

    return r;
}

void markdown_core_map_add(markdown_core_map *map, markdown_core_map_entry *entry) {
    entry->order = ++map->next_order;
    entry->owner = map->pending_owner;
    entry->bucket_next = NULL;
    entry->next = map->refs;
    map->refs = entry;
    map->size++;

    if (!map->prepared) {
        return;
    }
    if (!map->indexed || !bucket_attach(map, entry)) {
        /* The sorted array cannot absorb inserts (and a failed index attach
         * must not leave the label partially visible); drop the structures
         * and let the next lookup rebuild them. */
        unprepare_map(map);
    }
}

/* Unlinks `entry` from its bucket and keeps the index slot on the winner.
 * Returns 0 when the index state could not be kept coherent. */
static int bucket_detach(markdown_core_map *map, markdown_core_map_entry *entry) {
    bufsize_t label_len = (bufsize_t)strlen((char *)entry->label);
    markdown_core_map_entry *head =
        (markdown_core_map_entry *)markdown_core_key_index_lookup(&map->index, entry->label, label_len);
    markdown_core_map_entry *cur;

    if (!head) {
        return 0;
    }
    if (head == entry) {
        if (!markdown_core_key_index_remove(&map->index, entry->label, label_len)) {
            return 0;
        }
        if (!entry->bucket_next) {
            return 1;
        }
        /* Re-elect the next-oldest definition; its label bytes key the slot
         * from now on. The freshly vacated run guarantees room, and the
         * insert's own growth path covers the remaining corner cases. */
        return markdown_core_key_index_insert(&map->index, entry->bucket_next->label,
                                              (bufsize_t)strlen((char *)entry->bucket_next->label), entry->bucket_next,
                                              0, NULL);
    }
    cur = head;
    while (cur->bucket_next && cur->bucket_next != entry) {
        cur = cur->bucket_next;
    }
    if (cur->bucket_next != entry) {
        return 0;
    }
    cur->bucket_next = entry->bucket_next;
    return 1;
}

void markdown_core_map_remove_until(markdown_core_map *map, markdown_core_map_entry *until) {
    if (map == NULL) {
        return;
    }
    while (map->refs && map->refs != until) {
        markdown_core_map_entry *entry = map->refs;
        map->refs = entry->next;
        if (map->prepared) {
            if (!map->indexed || !bucket_detach(map, entry)) {
                unprepare_map(map);
            }
        }
        map->size--;
        map->free(map, entry);
    }
}

void markdown_core_map_remove_owned(markdown_core_map *map, uint64_t owner) {
    markdown_core_map_entry **link;

    if (map == NULL) {
        return;
    }

    link = &map->refs;
    while (*link) {
        markdown_core_map_entry *entry = *link;
        if (entry->owner != owner) {
            link = &entry->next;
            continue;
        }
        *link = entry->next;
        if (map->prepared) {
            if (!map->indexed || !bucket_detach(map, entry)) {
                unprepare_map(map);
            }
        }
        map->size--;
        map->free(map, entry);
    }
}

markdown_core_map_entry **markdown_core_map_winners(markdown_core_map *map, size_t *count) {
    markdown_core_map_entry **winners;
    size_t filled = 0;

    *count = 0;
    if (map == NULL || !map->size) {
        return NULL;
    }
    if (!prepare_map(map)) {
        map->oom = 1;
        return NULL;
    }

    if (map->indexed) {
        size_t slot;
        winners = (markdown_core_map_entry **)map->mem->calloc(map->index.size, sizeof(*winners));
        if (!winners) {
            map->oom = 1;
            return NULL;
        }
        for (slot = 0; slot < map->index.capacity; slot++) {
            if (map->index.slots[slot].key) {
                winners[filled++] = (markdown_core_map_entry *)map->index.slots[slot].value;
            }
        }
    } else {
        size_t i;
        size_t unique = 0;
        for (i = 0; i < map->size; i++) {
            if (i == 0 || labelcmp(map->sorted[i]->label, map->sorted[i - 1]->label) != 0) {
                unique++;
            }
        }
        winners = (markdown_core_map_entry **)map->mem->calloc(unique, sizeof(*winners));
        if (!winners) {
            map->oom = 1;
            return NULL;
        }
        for (i = 0; i < map->size; i++) {
            if (i == 0 || labelcmp(map->sorted[i]->label, map->sorted[i - 1]->label) != 0) {
                winners[filled++] = map->sorted[i];
            }
        }
    }

    *count = filled;
    return winners;
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

markdown_core_map *markdown_core_map_new(markdown_core_mem *mem, markdown_core_map_free_func free) {
    markdown_core_map *map = (markdown_core_map *)mem->calloc(1, sizeof(markdown_core_map));
    if (!map) {
        return NULL;
    }
    map->mem = mem;
    map->free = free;
    map->max_ref_size = UINT_MAX;
    return map;
}
