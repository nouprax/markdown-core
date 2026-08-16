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
            /* The key bytes match, but they are borrowed from the losing
             * value's storage; repoint them with the value so the slot
             * never mixes one owner's key bytes with another's value. */
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

/* Removes `key`, if present, by backward shift: the slots after it in its
 * probe run move up over the hole when their home lies at or before it, so
 * no run is broken and no tombstone is left to borrow freed key bytes.
 * Answers whether it was present. */
int markdown_core_key_index_remove(
    markdown_core_key_index *index,
    const unsigned char *key,
    markdown_core_bufsize key_len
) {
    uint64_t hash = hash_key(key, key_len);
    size_t mask = index->capacity - 1;
    size_t hole;
    size_t probe;
    markdown_core_key_index_slot *slot = NULL;
    size_t position = (size_t)hash & mask;
    for (probe = 0; probe < KEY_INDEX_MAX_PROBES; probe++) {
        markdown_core_key_index_slot *candidate = &index->slots[position];
        if (!candidate->key) {
            return 0;
        }
        if (candidate->hash == hash && candidate->key_len == key_len &&
            memcmp(candidate->key, key, (size_t)key_len) == 0) {
            slot = candidate;
            break;
        }
        position = (position + 1) & mask;
    }
    if (!slot) {
        return 0;
    }
    hole = position;
    for (;;) {
        size_t next = (position + 1) & mask;
        size_t home;
        markdown_core_key_index_slot *mover = &index->slots[next];
        if (!mover->key) {
            break;
        }
        home = (size_t)mover->hash & mask;
        /* The mover may fill the hole only if its home is not in the open
         * interval (hole, next] — cyclically. */
        if (hole <= next ? (hole < home && home <= next) : (hole < home || home <= next)) {
            position = next;
            continue;
        }
        index->slots[hole] = *mover;
        hole = next;
        position = next;
    }
    memset(&index->slots[hole], 0, sizeof(index->slots[hole]));
    index->size--;
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

/* Hash path: label -> winner. The live chain is newest-first with monotonic
 * orders, so every entry this walk offers is OLDER than every definition of
 * its label already indexed — a new minimum, so it takes the slot
 * unconditionally (replace=1). */
static int index_map(markdown_core_map *map) {
    markdown_core_map_entry *ref;
    if (!markdown_core_key_index_init(&map->index, map->mem)) {
        return 0;
    }
    for (ref = map->refs; ref; ref = ref->next) {
        markdown_core_bufsize label_len = (markdown_core_bufsize)strlen((char *)ref->label);
        if (!markdown_core_key_index_insert(&map->index, ref->label, label_len, ref, 1, NULL)) {
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

uint64_t markdown_core_map_label_hash(const unsigned char *label) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (; *label; label++) {
        h = (h ^ *label) * UINT64_C(0x100000001b3);
    }
    return h ? h : 1;
}

int markdown_core_map_entry_wins(markdown_core_map *map, markdown_core_map_entry *entry) {
    /* Through the prepared lookup: the winner of the label is what a lookup
     * answers. A map that cannot be prepared (an allocation lost, and the
     * sticky bit set) answers yes — a flip too many is a refine, not a
     * wrong answer. */
    if (!prepare_map(map)) {
        map->oom = 1;
        return 1;
    }
    if (map->indexed) {
        return markdown_core_key_index_lookup(
                   &map->index,
                   entry->label,
                   (markdown_core_bufsize)strlen((char *)entry->label)
               ) == entry;
    }
    return sorted_winner(map, entry->label) == entry;
}

void markdown_core_map_truncate(markdown_core_map *map, size_t size) {
    if (!map || map->size <= size) {
        return;
    }
    while (map->size > size) {
        markdown_core_map_entry *head = map->refs;
        map->refs = head->next;
        map->size--;
        map->next_order--;
        /* The hash path forgets the entry in place: it is in the index only
         * if it won its label, and then no older definition of that label
         * exists to take its slot. The sorted path cannot absorb removals. */
        if (map->prepared) {
            if (map->indexed) {
                if (markdown_core_key_index_lookup(
                        &map->index,
                        head->label,
                        (markdown_core_bufsize)strlen((char *)head->label)
                    ) == head) {
                    markdown_core_key_index_remove(
                        &map->index,
                        head->label,
                        (markdown_core_bufsize)strlen((char *)head->label)
                    );
                }
            } else {
                unprepare_map(map);
            }
        }
        map->free(map, head);
    }
}

markdown_core_map_entry *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label) {
    return markdown_core_map_lookup_probe(map, label, NULL);
}

markdown_core_map_entry *markdown_core_map_lookup_probe(
    markdown_core_map *map,
    markdown_core_chunk *label,
    uint64_t *hash
) {
    markdown_core_map_entry *r = NULL;
    unsigned char *norm;

    if (hash) {
        *hash = 0;
    }
    /* Labels over the scanner cap can never match any definition. */
    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH) {
        return NULL;
    }

    if (map == NULL) {
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
    if (hash) {
        *hash = markdown_core_map_label_hash(norm);
    }
    if (!map->size) {
        map->mem->free(map->mem, norm);
        return NULL;
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
    entry->next = map->refs;
    map->refs = entry;
    map->size++;

    if (!map->prepared) {
        return;
    }
    /* The new entry's order is the largest ever stamped, so it wins its
     * label only when the label is new: replace=0 leaves an existing winner
     * untouched and still reports success. */
    if (!map->indexed || !markdown_core_key_index_insert(
                             &map->index,
                             entry->label,
                             (markdown_core_bufsize)strlen((char *)entry->label),
                             entry,
                             0,
                             NULL
                         )) {
        /* The sorted array cannot absorb inserts, and a failed index insert
         * left the new label invisible to the hash path; drop the structures
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

/* --- the probe index ---------------------------------------------------- */

markdown_core_probe_index *markdown_core_probe_index_new(markdown_core_mem *mem) {
    markdown_core_probe_index *index = (markdown_core_probe_index *)mem->calloc(mem, 1, sizeof(*index));
    if (!index) {
        return NULL;
    }
    index->mem = mem;
    index->bucket_count = 64;
    index->buckets = (struct markdown_core_probe_link **)mem->calloc(mem, index->bucket_count, sizeof(*index->buckets));
    if (!index->buckets) {
        mem->free(mem, index);
        return NULL;
    }
    return index;
}

static void probe_index_free(markdown_core_probe_index *index) {
    markdown_core_mem *mem = index->mem;
    mem->free(mem, index->buckets);
    mem->free(mem, index);
}

void markdown_core_probe_index_release(markdown_core_probe_index *index) {
    if (!index) {
        return;
    }
    index->orphaned = 1;
    if (index->link_count == 0) {
        probe_index_free(index);
    }
}

static void probe_link_thread(markdown_core_probe_index *index, struct markdown_core_probe_link *link) {
    struct markdown_core_probe_link **bucket = &index->buckets[link->hash % index->bucket_count];
    link->next = *bucket;
    link->pprev = bucket;
    if (*bucket) {
        (*bucket)->pprev = &link->next;
    }
    *bucket = link;
}

static void probe_link_unthread(struct markdown_core_probe_link *link) {
    *link->pprev = link->next;
    if (link->next) {
        link->next->pprev = link->pprev;
    }
    link->next = NULL;
    link->pprev = NULL;
}

/* Twice the links per bucket on average is where the chains are rehashed
 * into twice the buckets: every link is rethreaded, so the walk is the
 * links'. */
static int probe_index_grow(markdown_core_probe_index *index) {
    markdown_core_mem *mem = index->mem;
    size_t old_count = index->bucket_count;
    struct markdown_core_probe_link **old = index->buckets;
    struct markdown_core_probe_link **grown;
    size_t i;
    if (index->link_count < old_count * 2) {
        return 1;
    }
    grown = (struct markdown_core_probe_link **)mem->calloc(mem, old_count * 2, sizeof(*grown));
    if (!grown) {
        /* Not a loss: the chains are longer than they would be, and every
         * answer is still right. */
        return 1;
    }
    index->buckets = grown;
    index->bucket_count = old_count * 2;
    for (i = 0; i < old_count; i++) {
        struct markdown_core_probe_link *link = old[i];
        while (link) {
            struct markdown_core_probe_link *next = link->next;
            probe_link_thread(index, link);
            link = next;
        }
    }
    mem->free(mem, old);
    return 1;
}

struct markdown_core_probes *markdown_core_probes_attach(
    markdown_core_probe_index *index,
    struct markdown_core_node *unit,
    const uint64_t *hashes,
    size_t count
) {
    markdown_core_mem *mem = index->mem;
    struct markdown_core_probes *probes;
    size_t i;
    if (count == 0) {
        return NULL;
    }
    probes =
        (struct markdown_core_probes *)mem->calloc(mem, 1, sizeof(*probes) + (count - 1) * sizeof(probes->links[0]));
    if (!probes) {
        return NULL;
    }
    probes->index = index;
    probes->count = count;
    index->link_count += count;
    probe_index_grow(index);
    for (i = 0; i < count; i++) {
        probes->links[i].hash = hashes[i];
        probes->links[i].unit = unit;
        probe_link_thread(index, &probes->links[i]);
    }
    return probes;
}

void markdown_core_probes_free(struct markdown_core_probes *probes) {
    markdown_core_probe_index *index;
    size_t i;
    if (!probes) {
        return;
    }
    index = probes->index;
    for (i = 0; i < probes->count; i++) {
        probe_link_unthread(&probes->links[i]);
    }
    index->link_count -= probes->count;
    index->mem->free(index->mem, probes);
    if (index->orphaned && index->link_count == 0) {
        probe_index_free(index);
    }
}

int markdown_core_probe_index_units(
    const markdown_core_probe_index *index,
    uint64_t hash,
    markdown_core_mem *mem,
    struct markdown_core_node ***units,
    size_t *count,
    size_t *capacity
) {
    const struct markdown_core_probe_link *link;
    if (!index) {
        return 1;
    }
    for (link = index->buckets[hash % index->bucket_count]; link; link = link->next) {
        if (link->hash != hash) {
            continue;
        }
        if (*count == *capacity) {
            size_t grown_capacity = *capacity ? *capacity * 2 : 16;
            struct markdown_core_node **grown =
                (struct markdown_core_node **)mem->realloc(mem, *units, grown_capacity * sizeof(*grown));
            if (!grown) {
                return 0;
            }
            *units = grown;
            *capacity = grown_capacity;
        }
        (*units)[(*count)++] = link->unit;
    }
    return 1;
}
