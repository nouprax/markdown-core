#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "alloc.h"
#include "registry.h"
#include "parser.h"
#include "utf8.h"

/* THE LOG'S RECORD STORAGE. A record lives until the registry is freed, a
 * rollback pops it or a compaction copies it, and pops come newest first,
 * so records are carved one after another from blocks the registry owns: a
 * rollback releases the blocks opened after its mark and resumes the mark's
 * block where the mark left it. A block starts small, so a parse that
 * declares a few labels costs one small allocation, and doubles up to a cap,
 * so a document with many definitions pays for its records in a few
 * allocations rather than one each. A record larger than the next block gets
 * a block of its own size. */
struct markdown_core_registry_block {
    union {
        markdown_core_registry_block *next;
        long double alignment;
        int64_t integer_alignment;
    } head;
};

#define REGISTRY_BLOCK_MIN_BYTES ((size_t)1024)
#define REGISTRY_BLOCK_MAX_BYTES ((size_t)16 * 1024)

/* The bytes a record of a key of `length` bytes takes in its block. */
static inline size_t S_record_size(bufsize_t length) {
    const size_t align = sizeof(markdown_core_registry_block);
    const size_t size = sizeof(markdown_core_registry_record) + (size_t)length + 2;
    return (size + align - 1) / align * align;
}

static void *S_carve(markdown_core_registry *registry, size_t size) {
    if (!registry->blocks || registry->block_size - registry->block_used < size) {
        size_t capacity = registry->block_size ? registry->block_size * 2 : REGISTRY_BLOCK_MIN_BYTES;
        if (capacity > REGISTRY_BLOCK_MAX_BYTES) {
            capacity = REGISTRY_BLOCK_MAX_BYTES;
        }
        if (capacity < size) {
            capacity = size;
        }
        if (capacity > SIZE_MAX - sizeof(markdown_core_registry_block)) {
            return NULL;
        }
        markdown_core_registry_block *block = (markdown_core_registry_block *)markdown_core_realloc(
            NULL, sizeof(markdown_core_registry_block) + capacity);
        if (!block) {
            return NULL;
        }
        block->head.next = registry->blocks;
        registry->blocks = block;
        registry->block_size = capacity;
        registry->block_used = 0;
    }
    void *storage = (unsigned char *)(registry->blocks + 1) + registry->block_used;
    registry->block_used += size;
    return storage;
}

static void S_free_blocks(markdown_core_registry_block *blocks) {
    while (blocks) {
        markdown_core_registry_block *next = blocks->head.next;
        markdown_core_free(blocks);
        blocks = next;
    }
}

/* THE PLACES' ZIPPER: an entry turned about the end turns where it starts,
 * and an instance has nothing about its front to let go. */
static inline markdown_core_registry_place S_place_flip(markdown_core_registry_place place, size_t end) {
    place.start = (uint32_t)(end - place.start);
    return place;
}

static inline size_t S_place_start(const markdown_core_registry_place *place) { return place->start; }

static inline void S_places_leave(markdown_core_registry *registry, size_t first) {
    (void)registry;
    (void)first;
}

MARKDOWN_CORE_ZIPPER_OPS(S_places, markdown_core_registry_places, markdown_core_registry_place, S_place_flip,
                         S_place_start, S_places_leave, markdown_core_registry)

void markdown_core_registry_init(markdown_core_registry *registry) {
    *registry = (markdown_core_registry){0};
    markdown_core_strbuf_init(&registry->key, 0);
    markdown_core_strbuf_init(&registry->lost, 0);
}

void markdown_core_registry_free(markdown_core_registry *registry) {
    S_free_blocks(registry->blocks);
    markdown_core_free(registry->records);
    markdown_core_key_index_free(&registry->index);
    markdown_core_strbuf_free(&registry->key);
    markdown_core_strbuf_free(&registry->lost);
    for (size_t kind = 0; kind < sizeof(registry->spaces) / sizeof(*registry->spaces); kind++) {
        markdown_core_array_free(&registry->spaces[kind].places.front);
        S_places_release_back(&registry->spaces[kind].places);
    }
    markdown_core_registry_init(registry);
}

/* THE KEY'S LIST. Two declarations of a key in the order of where they
 * start: the front's before the back's, the front's by where they start, and
 * the back's by how far back from the end, the farthest first. A
 * declaration after another that starts where it does goes after it. */
static inline bool S_precedes(const markdown_core_registry_record *left, const markdown_core_registry_record *right) {
    if (left->back != right->back) {
        return !left->back;
    }
    return left->back ? left->start > right->start : left->start < right->start;
}

/* The declaration joins its key's list in `slot`, an entry of the index
 * that `markdown_core_key_index_entry` found. Its search begins at the
 * list's hint, the declaration the list last took or let go near, and walks
 * whichever way its place lies: a parse declares and withdraws a key's
 * declarations mostly in the order of the text, and an edit's reading its
 * own past those before its rest point, so the walk is short. */
static void S_link(markdown_core_registry *registry, markdown_core_key_index_slot *slot,
                   markdown_core_registry_record *record) {
    record->linked = true;
    record->before = record->after = NULL;
    if (!slot->key) {
        record->last = record->hint = record;
        record->bound = record->back ? NULL : record;
        markdown_core_key_index_commit(&registry->index, slot, record->key);
        slot->value = (size_t)record;
        slot->extra = 1;
        return;
    }
    markdown_core_registry_record *first = (markdown_core_registry_record *)slot->value;
    markdown_core_registry_record *at = first->hint ? first->hint : first->last;
    if (!S_precedes(record, at)) {
        while (at->after && !S_precedes(record, at->after)) {
            at = at->after;
        }
    } else {
        while (at && S_precedes(record, at)) {
            at = at->before;
        }
    }
    slot->extra++;
    record->bound = record->hint = NULL;
    if (!at) {
        record->after = first;
        record->last = first->last;
        record->bound = first->bound;
        first->before = record;
        first->last = first->bound = first->hint = NULL;
        slot->value = (size_t)record;
        slot->key = record->key;
        first = record;
    } else {
        record->last = NULL;
        record->before = at;
        record->after = at->after;
        if (at->after) {
            at->after->before = record;
        } else {
            first->last = record;
        }
        at->after = record;
    }
    first->hint = record;
    if (!record->back && (!record->after || record->after->back)) {
        first->bound = record;
    }
}

/* The declaration leaves its key's list. */
static void S_unlink(markdown_core_registry *registry, markdown_core_registry_record *record) {
    markdown_core_key_index_slot *slot = markdown_core_key_index_find(&registry->index, record->key, record->key_len);
    assert(slot && record->linked);
    record->linked = false;
    markdown_core_registry_record *first = (markdown_core_registry_record *)slot->value;
    if (!--slot->extra) {
        markdown_core_key_index_remove(&registry->index, slot);
        return;
    }
    if (first->bound == record) {
        first->bound = record->before;
    }
    /* The next search of the list is likely near the one that went. */
    first->hint = record->before ? record->before : record->after;
    if (record == first) {
        markdown_core_registry_record *next = first->after;
        next->before = NULL;
        next->last = first->last;
        next->bound = first->bound;
        next->hint = first->hint;
        slot->value = (size_t)next;
        slot->key = next->key;
        return;
    }
    record->before->after = record->after;
    if (record->after) {
        record->after->before = record->before;
    } else {
        first->last = record->before;
    }
}

static inline bool S_append(markdown_core_registry *registry, markdown_core_registry_kind kind,
                            const unsigned char *label, bufsize_t length, uint32_t start, bool withdraws) {
    void *records =
        markdown_core_reserve(registry->records, &registry->capacity, registry->count + 1, sizeof(*registry->records));
    if (!records) {
        return false;
    }
    registry->records = records;
    const size_t size = S_record_size(length);
    markdown_core_registry_record *record = S_carve(registry, size);
    if (!record) {
        return false;
    }
    record->key[0] = (unsigned char)kind;
    memcpy(record->key + 1, label, (size_t)length);
    record->key[length + 1] = '\0';
    record->key_len = length + 1;
    record->start = start;
    record->before = record->after = record->last = NULL;
    record->live = 0;
    record->withdraws = withdraws;
    record->linked = record->withdrawn = record->back = record->gone = false;
    registry->records[registry->count++] = record;
    registry->spaces[kind].records++;
    registry->bytes += size;
    return true;
}

/* The front's place for a declaration that starts at `start`: after every
 * one that starts there or before. */
static inline size_t S_front_after(const markdown_core_registry_places *places, uint32_t start) {
    return S_places_front_from(places, (size_t)start + 1);
}

bool markdown_core_registry_declare(markdown_core_registry *registry, markdown_core_registry_kind kind,
                                    const unsigned char *label, bufsize_t length, uint32_t start) {
    if (length <= 0) {
        return true;
    }
    if (!registry->placed) {
        return S_append(registry, kind, label, length, start, false);
    }
    /* A parse an edit reads places it as it declares it. */
    markdown_core_registry_places *places = &registry->spaces[kind].places;
    if (!markdown_core_array_reserve(&places->front, places->front.count + 1) ||
        !S_append(registry, kind, label, length, start, false)) {
        return false;
    }
    const markdown_core_registry_place place = {start, registry->records[registry->count - 1]};
    (void)S_places_front_insert(places, S_front_after(places, start), place, registry);
    return true;
}

/* The index takes the kind's records the log holds beyond its cursor, in log
 * order, with room made for all of them first: each declaration that stands
 * joins its key's list. */
static bool S_index_space(markdown_core_registry *registry, markdown_core_registry_kind kind) {
    markdown_core_registry_space *space = &registry->spaces[kind];
    if (space->indexed < space->records &&
        !markdown_core_key_index_reserve(&registry->index, registry->index.size + space->records - space->indexed)) {
        return false;
    }
    for (; space->indexed < space->records; space->cursor++) {
        markdown_core_registry_record *record = registry->records[space->cursor];
        if (record->key[0] != (unsigned char)kind) {
            continue;
        }
        space->indexed++;
        if (!markdown_core_registry_stands(record)) {
            continue;
        }
        markdown_core_key_index_slot *slot =
            markdown_core_key_index_entry(&registry->index, record->key, record->key_len);
        if (!slot) {
            return false;
        }
        S_link(registry, slot, record);
    }
    space->cursor = registry->count;
    return true;
}

/* The first declaration of the key the registry assembled that stands and
 * a lookup sees. */
static markdown_core_registry_record *S_first(markdown_core_registry *registry, markdown_core_registry_kind kind,
                                              bool *failed) {
    if (registry->key.oom || !S_index_space(registry, kind)) {
        *failed = true;
        return NULL;
    }
    const markdown_core_key_index_slot *slot =
        markdown_core_key_index_find(&registry->index, registry->key.ptr, registry->key.size);
    markdown_core_registry_record *first = slot ? (markdown_core_registry_record *)slot->value : NULL;
    /* Every declaration of the front comes before any of the back's. */
    return first && !(first->back && registry->hidden) ? first : NULL;
}

static inline void S_assemble(markdown_core_registry *registry, markdown_core_registry_kind kind,
                              const unsigned char *label, bufsize_t length) {
    markdown_core_strbuf_clear(&registry->key);
    markdown_core_strbuf_putc(&registry->key, (int)kind);
    markdown_core_strbuf_put(&registry->key, label, length);
}

bool markdown_core_registry_withdraw(markdown_core_registry *registry, markdown_core_registry_kind kind,
                                     const unsigned char *label, bufsize_t length, uint32_t start) {
    assert(length > 0);
    if (!S_index_space(registry, kind)) {
        return false;
    }
    S_assemble(registry, kind, label, length);
    if (registry->key.oom) {
        return false;
    }
    const markdown_core_key_index_slot *slot =
        markdown_core_key_index_find(&registry->index, registry->key.ptr, registry->key.size);
    assert(slot);
    /* The declaration that starts where the withdrawal does, the last of
     * them; or, when none does, the key's last. The list is in order of
     * where they start: the search walks from its hint (S_link). */
    markdown_core_registry_record *first = (markdown_core_registry_record *)slot->value;
    markdown_core_registry_record *at = first->hint ? first->hint : first->last;
    if (markdown_core_registry_start(registry, at) <= start) {
        while (at->after && markdown_core_registry_start(registry, at->after) <= start) {
            at = at->after;
        }
    } else {
        while (at && markdown_core_registry_start(registry, at) > start) {
            at = at->before;
        }
    }
    markdown_core_registry_record *withdrawn =
        at && markdown_core_registry_start(registry, at) == start ? at : first->last;
    const uint32_t live = (uint32_t)slot->extra - 1;
    if (!S_append(registry, kind, label, length, start, true)) {
        return false;
    }
    markdown_core_registry_record *record = registry->records[registry->count - 1];
    record->before = withdrawn;
    record->live = live;
    S_unlink(registry, withdrawn);
    withdrawn->withdrawn = true;
    registry->dead += S_record_size(record->key_len - 1) + S_record_size(withdrawn->key_len - 1);
    registry->spaces[kind].indexed++;
    registry->spaces[kind].cursor = registry->count;
    return true;
}

const markdown_core_registry_record *markdown_core_registry_find(markdown_core_registry *registry,
                                                                 markdown_core_registry_kind kind,
                                                                 const unsigned char *label, bufsize_t length,
                                                                 bool *failed) {
    if (length <= 0 || !registry->spaces[kind].records) {
        return NULL;
    }
    S_assemble(registry, kind, label, length);
    return S_first(registry, kind, failed);
}

markdown_core_registry_record *markdown_core_registry_first(markdown_core_registry *registry,
                                                            markdown_core_registry_kind kind,
                                                            const unsigned char *label, bufsize_t length,
                                                            bool *failed) {
    if (length <= 0 || !registry->spaces[kind].records) {
        return NULL;
    }
    S_assemble(registry, kind, label, length);
    if (registry->key.oom || !S_index_space(registry, kind)) {
        *failed = true;
        return NULL;
    }
    const markdown_core_key_index_slot *slot =
        markdown_core_key_index_find(&registry->index, registry->key.ptr, registry->key.size);
    return slot ? (markdown_core_registry_record *)slot->value : NULL;
}

/* Whether `record` is a declaration of `kind` for `label` the index holds. */
static inline bool S_holds(const markdown_core_registry_record *record, markdown_core_registry_kind kind,
                           const unsigned char *label, bufsize_t length) {
    return record->linked && record->key[0] == (unsigned char)kind && record->key_len == length + 1 &&
           !memcmp(record->key + 1, label, (size_t)length);
}

const markdown_core_registry_record *markdown_core_registry_held_at(const markdown_core_registry *registry,
                                                                    markdown_core_registry_kind kind,
                                                                    const unsigned char *label, bufsize_t length,
                                                                    bool back, uint32_t start) {
    if (!registry->placed) {
        return NULL;
    }
    const markdown_core_registry_places *places = &registry->spaces[kind].places;
    if (!back) {
        for (size_t i = S_places_front_from(places, start);
             i < places->front.count && places->front.values[i].start == start; i++) {
            if (S_holds(places->front.values[i].record, kind, label, length)) {
                return places->front.values[i].record;
            }
        }
        return NULL;
    }
    /* The back's entries nearer the focus are further from the end. */
    size_t low = 0, high = places->back_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (places->back[middle].start < start) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    for (size_t i = low; i < places->back_count && places->back[i].start == start; i++) {
        if (S_holds(places->back[i].record, kind, label, length)) {
            return places->back[i].record;
        }
    }
    return NULL;
}

const markdown_core_registry_record *markdown_core_registry_back_first(markdown_core_registry_record *first) {
    /* The bound is near the front's last (S_link), which it is made again. */
    markdown_core_registry_record *at = first->bound ? first->bound : (first->back ? NULL : first);
    while (at && at->back) {
        at = at->before;
    }
    while (at && at->after && !at->after->back) {
        at = at->after;
    }
    first->bound = at;
    return at ? at->after : first;
}

const markdown_core_registry_record *markdown_core_registry_find_label(markdown_core_registry *registry,
                                                                       markdown_core_registry_kind kind,
                                                                       const markdown_core_chunk *label, bool *failed) {
    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH || !registry->spaces[kind].records) {
        return NULL;
    }
    markdown_core_strbuf_clear(&registry->key);
    markdown_core_strbuf_putc(&registry->key, (int)kind);
    markdown_core_utf8proc_normalize_label(&registry->key, label->data, label->len);
    if (registry->key.size == 1 && !registry->key.oom) {
        return NULL;
    }
    return S_first(registry, kind, failed);
}

bool markdown_core_registry_normal_label(markdown_core_registry *registry, markdown_core_node_pool *pool,
                                         const markdown_core_chunk *label, markdown_core_chunk *normal) {
    *normal = (markdown_core_chunk){0};
    markdown_core_strbuf_clear(&registry->key);
    if (label->len > 0) {
        markdown_core_utf8proc_normalize_label(&registry->key, label->data, label->len);
    }
    if (registry->key.oom) {
        return false;
    }
    const bufsize_t length = registry->key.size;
    if (!length) {
        return true;
    }
    unsigned char *stored = markdown_core_node_pool_bytes(pool, (size_t)length + 1);
    if (!stored) {
        return false;
    }
    memcpy(stored, registry->key.ptr, (size_t)length);
    stored[length] = '\0';
    *normal = (markdown_core_chunk){stored, length, 0};
    return true;
}

markdown_core_registry_mark markdown_core_registry_mark_now(const markdown_core_registry *registry) {
    return (markdown_core_registry_mark){registry->count,      registry->blocks, registry->block_used,
                                         registry->block_size, registry->bytes,  registry->dead};
}

/* The front's entry of the declaration `record`, which an edit's reading
 * declared, goes. */
static void S_unplace(markdown_core_registry *registry, const markdown_core_registry_record *record) {
    markdown_core_registry_places *places = &registry->spaces[record->key[0]].places;
    assert(!record->back);
    size_t at = S_places_front_from(places, record->start);
    while (places->front.values[at].record != record) {
        at++;
        assert(at < places->front.count);
    }
    S_places_front_erase(places, at, 1, registry);
}

void markdown_core_registry_rollback(markdown_core_registry *registry, markdown_core_registry_mark mark) {
    assert(mark.count <= registry->count);
    while (registry->count > mark.count) {
        markdown_core_registry_record *record = registry->records[--registry->count];
        const markdown_core_registry_kind kind = (markdown_core_registry_kind)record->key[0];
        markdown_core_registry_space *space = &registry->spaces[kind];
        space->records--;
        assert(!record->gone);
        /* A record the index never took leaves only the log. */
        if (registry->count < space->cursor) {
            space->indexed--;
        }
        if (record->withdraws) {
            /* The declaration it withdrew stands again, where it stood. */
            markdown_core_registry_record *withdrawn = record->before;
            withdrawn->withdrawn = false;
            markdown_core_key_index_slot *slot =
                markdown_core_key_index_entry(&registry->index, withdrawn->key, withdrawn->key_len);
            /* A removal leaves room for what it removed. */
            assert(slot);
            S_link(registry, slot, withdrawn);
            continue;
        }
        if (record->linked) {
            S_unlink(registry, record);
        }
        if (registry->placed) {
            S_unplace(registry, record);
        }
    }
    for (size_t kind = 0; kind < sizeof(registry->spaces) / sizeof(*registry->spaces); kind++) {
        if (registry->spaces[kind].cursor > registry->count) {
            registry->spaces[kind].cursor = registry->count;
        }
    }
    while (registry->blocks != mark.blocks) {
        markdown_core_registry_block *block = registry->blocks;
        registry->blocks = block->head.next;
        markdown_core_free(block);
    }
    registry->block_used = mark.block_used;
    registry->block_size = mark.block_size;
    registry->bytes = mark.bytes;
    registry->dead = mark.dead;
}

/* Two places in order of where they start, then of the log, which holds
 * the declarations of one start in the order the parse made them. */
static int S_place_order(const void *a, const void *b) {
    const markdown_core_registry_place *left = a, *right = b;
    if (left->start != right->start) {
        return left->start < right->start ? -1 : 1;
    }
    return (left->record > right->record) - (left->record < right->record);
}

/* THE FIRST EDIT places each kind's declarations that stand, all in the
 * front, in the order of where they start. False, with the registry as it
 * was, when storage could not be had. */
static bool S_place(markdown_core_registry *registry) {
    size_t counts[sizeof(registry->spaces) / sizeof(*registry->spaces)] = {0};
    for (size_t i = 0; i < registry->count; i++) {
        counts[registry->records[i]->key[0]] += markdown_core_registry_stands(registry->records[i]);
    }
    for (size_t kind = 0; kind < sizeof(counts) / sizeof(*counts); kind++) {
        if (!markdown_core_array_reserve(&registry->spaces[kind].places.front, counts[kind])) {
            return false;
        }
    }
    for (size_t i = 0; i < registry->count; i++) {
        markdown_core_registry_record *record = registry->records[i];
        if (markdown_core_registry_stands(record)) {
            const markdown_core_registry_place place = {record->start, record};
            (void)markdown_core_array_push(&registry->spaces[record->key[0]].places.front, place);
        }
    }
    for (size_t kind = 0; kind < sizeof(counts) / sizeof(*counts); kind++) {
        markdown_core_registry_places *places = &registry->spaces[kind].places;
        /* A record's address orders the declarations of one start as the
         * log does only within a block; the parse makes most in order of
         * where they start, which the sort then keeps. */
        markdown_core_ahead_unsettle(&places->front.ahead, 0);
        if (places->front.count > 1) {
            qsort(places->front.values, places->front.count, sizeof(*places->front.values), S_place_order);
        }
    }
    registry->placed = true;
    return true;
}

bool markdown_core_registry_reopen(markdown_core_registry *registry, uint32_t start, size_t size) {
    if (!registry->placed && !S_place(registry)) {
        return false;
    }
    if (registry->dead > registry->bytes - registry->dead + 4096 && !markdown_core_registry_compact(registry)) {
        return false;
    }
    const size_t kinds = sizeof(registry->spaces) / sizeof(*registry->spaces);
    for (size_t kind = 0; kind < kinds; kind++) {
        if (!S_places_ready(&registry->spaces[kind].places, start, size)) {
            return false;
        }
    }
    for (size_t kind = 0; kind < kinds; kind++) {
        markdown_core_registry_places *places = &registry->spaces[kind].places;
        const size_t front = places->front.count, back = places->back_count;
        (void)S_places_move(places, start, size, registry);
        /* Each record the move took to the other part stands where its
         * entry does. */
        for (size_t i = back; i < places->back_count; i++) {
            places->back[i].record->start = places->back[i].start;
            places->back[i].record->back = true;
        }
        for (size_t i = front; i < places->front.count; i++) {
            places->front.values[i].record->start = places->front.values[i].start;
            places->front.values[i].record->back = false;
        }
    }
    registry->hidden = true;
    markdown_core_strbuf_clear(&registry->lost);
    return true;
}

void markdown_core_registry_splice(markdown_core_registry *registry, size_t place, int64_t delta) {
    for (size_t kind = 0; kind < sizeof(registry->spaces) / sizeof(*registry->spaces); kind++) {
        markdown_core_registry_places *places = &registry->spaces[kind].places;
        const size_t gone = S_places_back_from(places, place);
        for (size_t k = 0; k < gone; k++) {
            markdown_core_registry_record *record = places->back[places->back_count - 1 - k].record;
            if (markdown_core_registry_stands(record)) {
                if (kind != MARKDOWN_CORE_REGISTRY_ANCHOR) {
                    markdown_core_strbuf_put(&registry->lost, record->key, record->key_len + 1);
                }
                registry->dead += S_record_size(record->key_len - 1);
            }
            if (record->linked) {
                S_unlink(registry, record);
            }
            record->gone = true;
        }
        (void)S_places_cut(places, place);
        places->end = (size_t)((int64_t)places->end + delta);
    }
    registry->hidden = false;
}

/* A key at `key`, its NUL after it. */
typedef struct {
    const unsigned char *key;
    bufsize_t size;
} S_key;

/* Two keys in byte order, the shorter first on a common prefix. */
static int S_key_order(const void *a, const void *b) {
    const S_key *left = a, *right = b;
    const bufsize_t common = left->size < right->size ? left->size : right->size;
    const int order = memcmp(left->key, right->key, (size_t)common);
    return order ? order : (left->size > right->size) - (left->size < right->size);
}

/* The `count` keys of `keys`, sorted, each once and with its NUL, into
 * `labels`. */
static bool S_put_keys(S_key *keys, size_t count, markdown_core_strbuf *labels) {
    if (count) {
        qsort(keys, count, sizeof(*keys), S_key_order);
    }
    markdown_core_strbuf_clear(labels);
    for (size_t i = 0; i < count; i++) {
        if (!i || S_key_order(&keys[i], &keys[i - 1])) {
            markdown_core_strbuf_put(labels, keys[i].key, keys[i].size + 1);
        }
    }
    return !labels->oom;
}

bool markdown_core_registry_made(const markdown_core_registry *registry, uint32_t start, markdown_core_strbuf *labels) {
    size_t count = 0;
    for (size_t kind = 0; kind < MARKDOWN_CORE_REGISTRY_ANCHOR; kind++) {
        const markdown_core_registry_places *places = &registry->spaces[kind].places;
        count += places->front.count - S_places_front_from(places, start);
    }
    S_key *keys = count ? markdown_core_alloc(count, sizeof(*keys)) : NULL;
    if (count && !keys) {
        return false;
    }
    count = 0;
    for (size_t kind = 0; kind < MARKDOWN_CORE_REGISTRY_ANCHOR; kind++) {
        const markdown_core_registry_places *places = &registry->spaces[kind].places;
        for (size_t i = S_places_front_from(places, start); i < places->front.count; i++) {
            const markdown_core_registry_record *record = places->front.values[i].record;
            if (markdown_core_registry_stands(record)) {
                keys[count++] = (S_key){record->key, record->key_len};
            }
        }
    }
    const bool ok = S_put_keys(keys, count, labels);
    markdown_core_free(keys);
    return ok;
}

bool markdown_core_registry_lost(const markdown_core_registry *registry, markdown_core_strbuf *labels) {
    /* A key the splice could not keep is one the answer would miss. */
    if (registry->lost.oom) {
        return false;
    }
    size_t count = 0;
    for (bufsize_t at = 0; at < registry->lost.size;
         at += (bufsize_t)strlen((const char *)registry->lost.ptr + at) + 1) {
        count++;
    }
    S_key *keys = count ? markdown_core_alloc(count, sizeof(*keys)) : NULL;
    if (count && !keys) {
        return false;
    }
    count = 0;
    for (bufsize_t at = 0; at < registry->lost.size;) {
        const bufsize_t size = (bufsize_t)strlen((const char *)registry->lost.ptr + at);
        keys[count++] = (S_key){registry->lost.ptr + at, size};
        at += size + 1;
    }
    const bool ok = S_put_keys(keys, count, labels);
    markdown_core_free(keys);
    return ok;
}

bool markdown_core_registry_declared_outside(markdown_core_registry *registry, markdown_core_registry_kind kind,
                                             const unsigned char *label, bufsize_t length, uint32_t from, uint32_t to,
                                             bool *failed) {
    if (length <= 0 || !registry->spaces[kind].records) {
        return false;
    }
    S_assemble(registry, kind, label, length);
    const markdown_core_registry_record *first = S_first(registry, kind, failed);
    return first && (markdown_core_registry_start(registry, first) < from ||
                     markdown_core_registry_start(registry, first->last) >= to);
}

bool markdown_core_registry_compact(markdown_core_registry *registry) {
    if (!registry->placed) {
        return true;
    }
    const size_t kinds = sizeof(registry->spaces) / sizeof(*registry->spaces);
    /* The places keep only the declarations that stand. */
    size_t count = 0;
    for (size_t kind = 0; kind < kinds; kind++) {
        markdown_core_registry_places *places = &registry->spaces[kind].places;
        size_t kept = 0;
        for (size_t i = 0; i < places->front.count; i++) {
            if (markdown_core_registry_stands(places->front.values[i].record)) {
                places->front.values[kept++] = places->front.values[i];
            }
        }
        markdown_core_ahead_unsettle(&places->front.ahead, 0);
        markdown_core_array_truncate(&places->front, kept);
        kept = 0;
        for (size_t i = 0; i < places->back_count; i++) {
            if (markdown_core_registry_stands(places->back[i].record)) {
                places->back[kept++] = places->back[i];
            }
        }
        places->back_count = kept;
        count += places->front.count + places->back_count;
    }
    /* Then each is copied into new storage, a kind's in the order of where
     * they start, which the log then holds. */
    markdown_core_registry fresh;
    markdown_core_registry_init(&fresh);
    fresh.records = markdown_core_alloc(count ? count : 1, sizeof(*fresh.records));
    if (!fresh.records) {
        return false;
    }
    fresh.capacity = count ? count : 1;
    for (size_t kind = 0; kind < kinds; kind++) {
        markdown_core_registry_places *places = &registry->spaces[kind].places;
        for (size_t i = 0; i < places->front.count + places->back_count; i++) {
            markdown_core_registry_place *place =
                i < places->front.count ? &places->front.values[i]
                                        : &places->back[places->back_count - 1 - (i - places->front.count)];
            const size_t size = S_record_size(place->record->key_len - 1);
            markdown_core_registry_record *copy = S_carve(&fresh, size);
            if (!copy) {
                S_free_blocks(fresh.blocks);
                markdown_core_free(fresh.records);
                return false;
            }
            fresh.records[fresh.count++] = copy;
            fresh.bytes += size;
        }
    }
    count = 0;
    for (size_t kind = 0; kind < kinds; kind++) {
        markdown_core_registry_space *space = &registry->spaces[kind];
        markdown_core_registry_places *places = &space->places;
        for (size_t i = 0; i < places->front.count + places->back_count; i++) {
            markdown_core_registry_place *place =
                i < places->front.count ? &places->front.values[i]
                                        : &places->back[places->back_count - 1 - (i - places->front.count)];
            markdown_core_registry_record *copy = fresh.records[count++];
            memcpy(copy, place->record, sizeof(*copy) + (size_t)place->record->key_len + 1);
            copy->before = copy->after = copy->last = NULL;
            copy->linked = false;
            place->record = copy;
        }
        space->records = places->front.count + places->back_count;
        space->indexed = space->cursor = 0;
        markdown_core_array_shrink(&places->front);
    }
    S_free_blocks(registry->blocks);
    markdown_core_free(registry->records);
    markdown_core_key_index_free(&registry->index);
    registry->index = (markdown_core_key_index){0};
    registry->records = fresh.records;
    registry->count = fresh.count;
    registry->capacity = fresh.capacity;
    registry->blocks = fresh.blocks;
    registry->block_used = fresh.block_used;
    registry->block_size = fresh.block_size;
    registry->bytes = fresh.bytes;
    registry->dead = 0;
    markdown_core_strbuf_free(&fresh.key);
    markdown_core_strbuf_free(&fresh.lost);
    return true;
}
