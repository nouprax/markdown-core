#include "map.h"
#include "utf8.h"
#include "parser.h"

#define KEY_INDEX_MIN_CAPACITY 16
#define KEY_INDEX_MAX_PROBES 64

static uint64_t hash_key(const unsigned char *key, markdown_core_bufsize key_len) {
    uint64_t hash = UINT64_C(1469598103934665603);
    markdown_core_bufsize i;
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

static markdown_core_key_index_slot *find_key_slot(
    markdown_core_key_index_slot *slots,
    size_t capacity,
    uint64_t hash,
    const unsigned char *key,
    markdown_core_bufsize key_len
) {
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
    slots = (markdown_core_key_index_slot *)index->mem->calloc(index->mem, capacity, sizeof(*slots));
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
            index->mem->free(index->mem, slots);
            return 0;
        }
        *destination = *source;
    }
    index->mem->free(index->mem, index->slots);
    index->slots = slots;
    index->capacity = capacity;
    return 1;
}

int markdown_core_key_index_init(markdown_core_key_index *index, markdown_core_mem *mem) {
    memset(index, 0, sizeof(*index));
    index->mem = mem;
    index->slots = (markdown_core_key_index_slot *)mem->calloc(mem, KEY_INDEX_MIN_CAPACITY, sizeof(*index->slots));
    if (!index->slots) {
        return 0;
    }
    index->capacity = KEY_INDEX_MIN_CAPACITY;
    return 1;
}

void markdown_core_key_index_free(markdown_core_key_index *index) {
    if (index->slots) {
        index->mem->free(index->mem, index->slots);
    }
    memset(index, 0, sizeof(*index));
}

int markdown_core_key_index_insert(
    markdown_core_key_index *index,
    const unsigned char *key,
    markdown_core_bufsize key_len,
    void *value,
    int replace,
    void **existing
) {
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

void *markdown_core_key_index_lookup(
    const markdown_core_key_index *index,
    const unsigned char *key,
    markdown_core_bufsize key_len
) {
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

int markdown_core_key_index_remove(
    markdown_core_key_index *index,
    const unsigned char *key,
    markdown_core_bufsize key_len
) {
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
        mem->free(mem, result);
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

/* Drops the prepared lookup structures. Lossless: the live chain still holds
 * every entry, so the next lookup rebuilds. */
static void unprepare_map(markdown_core_map *map) {
    if (map->sorted) {
        map->mem->free(map->mem, map->sorted);
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

    sorted = (markdown_core_map_entry **)map->mem->calloc(map->mem, size, sizeof(markdown_core_map_entry *));
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

/* Splices `entry` in front of the bucket head, which for a circular list is
 * the tail position. O(1), and the only splice there is: nothing here
 * searches for a slot, because no caller ever has a middle one to offer. */
static void bucket_splice(markdown_core_map_entry *head, markdown_core_map_entry *entry) {
    entry->bucket_prev = head->bucket_prev;
    entry->bucket_next = head;
    head->bucket_prev->bucket_next = entry;
    head->bucket_prev = entry;
}

static void bucket_alone(markdown_core_map_entry *entry) {
    entry->bucket_next = entry;
    entry->bucket_prev = entry;
}

/* `entry` is OLDER than every definition of its label, so it becomes the
 * winner and the index slot moves to it. The full index build is the caller:
 * it walks the live chain newest-first, so every entry it offers is a new
 * minimum. Returns 0 when the index could not take the label. */
static int bucket_prepend(markdown_core_map *map, markdown_core_map_entry *entry) {
    markdown_core_bufsize label_len = (markdown_core_bufsize)strlen((char *)entry->label);
    markdown_core_map_entry *head =
        (markdown_core_map_entry *)markdown_core_key_index_lookup(&map->index, entry->label, label_len);

    if (!head) {
        bucket_alone(entry);
        return markdown_core_key_index_insert(&map->index, entry->label, label_len, entry, 0, NULL);
    }
    bucket_splice(head, entry);
    return markdown_core_key_index_insert(&map->index, entry->label, label_len, entry, 1, NULL);
}

/* `entry` is NEWER than every definition of its label, so it becomes the
 * tail and the winner does not change — which is why this touches the index
 * only when the label is new. The incremental add is the caller: its order is
 * the largest ever stamped. Returns 0 when the index could not take the
 * label. */
static int bucket_append(markdown_core_map *map, markdown_core_map_entry *entry) {
    markdown_core_bufsize label_len = (markdown_core_bufsize)strlen((char *)entry->label);
    markdown_core_map_entry *head =
        (markdown_core_map_entry *)markdown_core_key_index_lookup(&map->index, entry->label, label_len);

    if (!head) {
        bucket_alone(entry);
        return markdown_core_key_index_insert(&map->index, entry->label, label_len, entry, 0, NULL);
    }
    bucket_splice(head, entry);
    return 1;
}

/* Hash path: label -> bucket head. The live chain is newest-first with
 * monotonic orders, so every entry this offers is a new minimum for its
 * label — which is the claim bucket_prepend is named after, and which is now
 * carried by the choice of function rather than by a branch inside one. */
static int index_map(markdown_core_map *map) {
    markdown_core_map_entry *ref;
    if (!markdown_core_key_index_init(&map->index, map->mem)) {
        return 0;
    }
    for (ref = map->refs; ref; ref = ref->next) {
        if (!bucket_prepend(map, ref)) {
            markdown_core_key_index_free(&map->index);
            return 0;
        }
    }
    map->prepared = 1;
    map->indexed = 1;
    return 1;
}

static int prepare_map(markdown_core_map *map) { return map->prepared || index_map(map) || sort_map(map); }

/* Leftmost entry of the label's run in the sorted array: the winner.
 * A lower-bound binary search keeps duplicate-heavy fallback lookups at
 * O(log n) instead of walking the run linearly. */
static markdown_core_map_entry *sorted_winner(markdown_core_map *map, const unsigned char *label) {
    size_t lo = 0;
    size_t hi = map->size;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (labelcmp(map->sorted[mid]->label, label) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == map->size || labelcmp(map->sorted[lo]->label, label) != 0) {
        return NULL;
    }
    return map->sorted[lo];
}

markdown_core_map_entry *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label) {
    markdown_core_map_entry *r = NULL;
    unsigned char *norm;

    /* Labels over the scanner cap can never match any definition. */
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
        map->mem->free(map->mem, norm);
        return NULL;
    }

    if (map->indexed) {
        r = (markdown_core_map_entry *)
            markdown_core_key_index_lookup(&map->index, norm, (markdown_core_bufsize)strlen((char *)norm));
    } else {
        r = sorted_winner(map, norm);
    }
    map->mem->free(map->mem, norm);

    return r;
}

void markdown_core_map_add(markdown_core_map *map, markdown_core_map_entry *entry) {
    entry->order = ++map->next_order;
    entry->bucket_next = NULL;
    entry->bucket_prev = NULL;
    entry->next = map->refs;
    map->refs = entry;
    map->size++;

    if (!map->prepared) {
        return;
    }
    if (!map->indexed || !bucket_append(map, entry)) {
        /* The sorted array cannot absorb inserts (and a failed index attach
         * must not leave the label partially visible); drop the structures
         * and let the next lookup rebuild them. */
        unprepare_map(map);
    }
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

    map->mem->free(map->mem, map->sorted);
    markdown_core_key_index_free(&map->index);
    map->mem->free(map->mem, map);
}

markdown_core_map *markdown_core_map_new(markdown_core_mem *mem, markdown_core_map_free_func free) {
    markdown_core_map *map = (markdown_core_map *)mem->calloc(mem, 1, sizeof(markdown_core_map));
    if (!map) {
        return NULL;
    }
    map->mem = mem;
    map->free = free;
    return map;
}
