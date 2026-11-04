#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "alloc.h"
#include "roster.h"
#include "parser.h"
#include "utf8.h"

/* THE LOG'S RECORD STORAGE. A record lives until the roster is freed, a
 * rollback pops it or a compaction copies it, and pops come newest first,
 * so records are carved one after another from blocks the roster owns: a
 * rollback releases the blocks opened after its mark and resumes the mark's
 * block where the mark left it. A block starts small, so a parse that
 * declares a few labels costs one small allocation, and doubles up to a cap,
 * so a document with many definitions pays for its records in a few
 * allocations rather than one each. A record larger than the next block gets
 * a block of its own size. */
struct markdown_core_roster_block {
    union {
        markdown_core_roster_block *next;
        long double alignment;
        int64_t integer_alignment;
    } head;
};

#define ROSTER_BLOCK_MIN_BYTES ((size_t)1024)
#define ROSTER_BLOCK_MAX_BYTES ((size_t)16 * 1024)

/* The bytes a record of a key of `length` bytes takes in its block. */
static inline size_t S_record_size(bufsize_t length) {
    const size_t align = sizeof(markdown_core_roster_block);
    const size_t size = sizeof(markdown_core_roster_record) + (size_t)length + 2;
    return (size + align - 1) / align * align;
}

static void *S_carve(markdown_core_roster *roster, size_t size) {
    if (!roster->blocks || roster->block_size - roster->block_used < size) {
        size_t capacity = roster->block_size ? roster->block_size * 2 : ROSTER_BLOCK_MIN_BYTES;
        if (capacity > ROSTER_BLOCK_MAX_BYTES) {
            capacity = ROSTER_BLOCK_MAX_BYTES;
        }
        if (capacity < size) {
            capacity = size;
        }
        if (capacity > SIZE_MAX - sizeof(markdown_core_roster_block)) {
            return NULL;
        }
        markdown_core_roster_block *block =
            (markdown_core_roster_block *)markdown_core_realloc(NULL, sizeof(markdown_core_roster_block) + capacity);
        if (!block) {
            return NULL;
        }
        block->head.next = roster->blocks;
        roster->blocks = block;
        roster->block_size = capacity;
        roster->block_used = 0;
    }
    void *storage = (unsigned char *)(roster->blocks + 1) + roster->block_used;
    roster->block_used += size;
    return storage;
}

static void S_free_blocks(markdown_core_roster_block *blocks) {
    while (blocks) {
        markdown_core_roster_block *next = blocks->head.next;
        markdown_core_free(blocks);
        blocks = next;
    }
}

/* THE PLACES' ZIPPER: an entry turned about the end turns where it starts,
 * and an instance has nothing about its front to let go. */
static inline markdown_core_roster_place S_place_flip(markdown_core_roster_place place, size_t end) {
    place.start = (uint32_t)(end - place.start);
    return place;
}

static inline size_t S_place_start(const markdown_core_roster_place *place) { return place->start; }

static inline void S_places_leave(markdown_core_roster *roster, size_t first) {
    (void)roster;
    (void)first;
}

MARKDOWN_CORE_ZIPPER_OPS(S_places, markdown_core_roster_places, markdown_core_roster_place, S_place_flip, S_place_start,
                         S_places_leave, markdown_core_roster)

void markdown_core_roster_init(markdown_core_roster *roster) {
    *roster = (markdown_core_roster){0};
    markdown_core_strbuf_init(&roster->key, 0);
    markdown_core_strbuf_init(&roster->lost, 0);
}

void markdown_core_roster_free(markdown_core_roster *roster) {
    S_free_blocks(roster->blocks);
    markdown_core_free(roster->records);
    markdown_core_key_index_free(&roster->index);
    markdown_core_strbuf_free(&roster->key);
    markdown_core_strbuf_free(&roster->lost);
    for (size_t kind = 0; kind < sizeof(roster->spaces) / sizeof(*roster->spaces); kind++) {
        markdown_core_array_free(&roster->spaces[kind].places.front);
        S_places_release_back(&roster->spaces[kind].places);
    }
    markdown_core_roster_init(roster);
}

/* THE KEY'S LIST. Two declarations of a key in the order of where they
 * start: the front's before the back's, the front's by where they start, and
 * the back's by how far back from the end, the farthest first. A
 * declaration after another that starts where it does goes after it. */
static inline bool S_precedes(const markdown_core_roster_record *left, const markdown_core_roster_record *right) {
    if (left->back != right->back) {
        return !left->back;
    }
    return left->back ? left->start > right->start : left->start < right->start;
}

/* A list's last declaration, which its first holds in `before`. */
static inline markdown_core_roster_record *S_last(const markdown_core_roster_record *first) { return first->before; }

/* The declaration before `at` in the list that starts at `first`, or NULL. */
static inline markdown_core_roster_record *S_prior(const markdown_core_roster_record *first,
                                                   markdown_core_roster_record *at) {
    return at == first ? NULL : at->before;
}

/* The declaration joins its key's list in `slot`, an entry of the index
 * that `markdown_core_key_index_entry` found. Its search begins at the
 * list's hint, the declaration the list last took or let go near, and walks
 * whichever way its place lies: a parse declares and withdraws a key's
 * declarations mostly in the order of the text, and an edit's reading its
 * own past those before its rest point, so the walk is short. */
static void S_link(markdown_core_roster *roster, markdown_core_key_index_slot *slot,
                   markdown_core_roster_record *record) {
    record->linked = true;
    record->after = NULL;
    if (!slot->key) {
        record->before = record->hint = record;
        markdown_core_key_index_commit(&roster->index, slot, record->key);
        slot->value = (size_t)record;
        slot->extra = 1;
        return;
    }
    markdown_core_roster_record *first = (markdown_core_roster_record *)slot->value;
    markdown_core_roster_record *at = first->hint ? first->hint : S_last(first);
    if (!S_precedes(record, at)) {
        while (at->after && !S_precedes(record, at->after)) {
            at = at->after;
        }
    } else {
        while (at && S_precedes(record, at)) {
            at = S_prior(first, at);
        }
    }
    slot->extra++;
    record->hint = NULL;
    if (!at) {
        record->after = first;
        record->before = S_last(first);
        first->before = record;
        first->hint = NULL;
        slot->value = (size_t)record;
        slot->key = record->key;
        first = record;
    } else {
        record->before = at;
        record->after = at->after;
        if (at->after) {
            at->after->before = record;
        } else {
            first->before = record;
        }
        at->after = record;
    }
    first->hint = record;
}

/* The declaration leaves its key's list. */
static void S_unlink(markdown_core_roster *roster, markdown_core_roster_record *record) {
    markdown_core_key_index_slot *slot = markdown_core_key_index_find(&roster->index, record->key, record->key_len);
    assert(slot && record->linked);
    record->linked = false;
    markdown_core_roster_record *first = (markdown_core_roster_record *)slot->value;
    if (!--slot->extra) {
        markdown_core_key_index_remove(&roster->index, slot);
        return;
    }
    /* The next search of the list is likely near the one that went. */
    markdown_core_roster_record *prior = S_prior(first, record);
    first->hint = prior ? prior : record->after;
    if (record == first) {
        markdown_core_roster_record *next = first->after;
        next->before = S_last(first);
        next->hint = first->hint;
        slot->value = (size_t)next;
        slot->key = next->key;
        return;
    }
    prior->after = record->after;
    if (record->after) {
        record->after->before = prior;
    } else {
        first->before = prior;
    }
}

static inline bool S_append(markdown_core_roster *roster, markdown_core_roster_kind kind, const unsigned char *label,
                            bufsize_t length, uint32_t start, bool withdraws) {
    void *records =
        markdown_core_reserve(roster->records, &roster->capacity, roster->count + 1, sizeof(*roster->records));
    if (!records) {
        return false;
    }
    roster->records = records;
    const size_t size = S_record_size(length);
    markdown_core_roster_record *record = S_carve(roster, size);
    if (!record) {
        return false;
    }
    record->key[0] = (unsigned char)kind;
    memcpy(record->key + 1, label, (size_t)length);
    record->key[length + 1] = '\0';
    record->key_len = length + 1;
    record->start = start;
    record->before = record->after = record->hint = NULL;
    record->live = 0;
    record->withdraws = withdraws;
    record->linked = record->withdrawn = record->back = record->gone = false;
    roster->records[roster->count++] = record;
    roster->spaces[kind].records++;
    roster->bytes += size;
    return true;
}

/* The front's place for a declaration that starts at `start`: after every
 * one that starts there or before. */
static inline size_t S_front_after(const markdown_core_roster_places *places, uint32_t start) {
    return S_places_front_from(places, (size_t)start + 1);
}

bool markdown_core_roster_declare(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                  const unsigned char *label, bufsize_t length, uint32_t start) {
    if (length <= 0) {
        return true;
    }
    if (!roster->placed) {
        return S_append(roster, kind, label, length, start, false);
    }
    /* A parse an edit reads places it as it declares it. */
    markdown_core_roster_places *places = &roster->spaces[kind].places;
    if (!markdown_core_array_reserve(&places->front, places->front.count + 1) ||
        !S_append(roster, kind, label, length, start, false)) {
        return false;
    }
    const markdown_core_roster_place place = {start, roster->records[roster->count - 1]};
    (void)S_places_front_insert(places, S_front_after(places, start), place, roster);
    return true;
}

/* The index takes the kind's records the log holds beyond its cursor, in log
 * order, with room made for all of them first: each declaration that stands
 * joins its key's list. */
static bool S_index_space(markdown_core_roster *roster, markdown_core_roster_kind kind) {
    markdown_core_roster_space *space = &roster->spaces[kind];
    if (space->indexed < space->records &&
        !markdown_core_key_index_reserve(&roster->index, roster->index.size + space->records - space->indexed)) {
        return false;
    }
    for (; space->indexed < space->records; space->cursor++) {
        markdown_core_roster_record *record = roster->records[space->cursor];
        if (record->key[0] != (unsigned char)kind) {
            continue;
        }
        space->indexed++;
        if (!markdown_core_roster_stands(record)) {
            continue;
        }
        markdown_core_key_index_slot *slot =
            markdown_core_key_index_entry(&roster->index, record->key, record->key_len);
        if (!slot) {
            return false;
        }
        S_link(roster, slot, record);
    }
    space->cursor = roster->count;
    return true;
}

/* The first declaration of the key the roster assembled that stands and
 * a lookup sees. */
static markdown_core_roster_record *S_first(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                            bool *failed) {
    if (roster->key.oom || !S_index_space(roster, kind)) {
        *failed = true;
        return NULL;
    }
    const markdown_core_key_index_slot *slot =
        markdown_core_key_index_find(&roster->index, roster->key.ptr, roster->key.size);
    markdown_core_roster_record *first = slot ? (markdown_core_roster_record *)slot->value : NULL;
    /* Every declaration of the front comes before any of the back's. */
    return first && !(first->back && roster->hidden) ? first : NULL;
}

static inline void S_assemble(markdown_core_roster *roster, markdown_core_roster_kind kind, const unsigned char *label,
                              bufsize_t length) {
    markdown_core_strbuf_clear(&roster->key);
    markdown_core_strbuf_putc(&roster->key, (int)kind);
    markdown_core_strbuf_put(&roster->key, label, length);
}

bool markdown_core_roster_withdraw(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                   const unsigned char *label, bufsize_t length, uint32_t start) {
    assert(length > 0);
    if (!S_index_space(roster, kind)) {
        return false;
    }
    S_assemble(roster, kind, label, length);
    if (roster->key.oom) {
        return false;
    }
    const markdown_core_key_index_slot *slot =
        markdown_core_key_index_find(&roster->index, roster->key.ptr, roster->key.size);
    assert(slot);
    /* The declaration that starts where the withdrawal does, the last of
     * them; or, when none does, the key's last. The list is in order of
     * where they start: the search walks from its hint (S_link). */
    markdown_core_roster_record *first = (markdown_core_roster_record *)slot->value;
    markdown_core_roster_record *at = first->hint ? first->hint : S_last(first);
    if (markdown_core_roster_start(roster, at) <= start) {
        while (at->after && markdown_core_roster_start(roster, at->after) <= start) {
            at = at->after;
        }
    } else {
        while (at && markdown_core_roster_start(roster, at) > start) {
            at = S_prior(first, at);
        }
    }
    markdown_core_roster_record *withdrawn = at && markdown_core_roster_start(roster, at) == start ? at : S_last(first);
    const uint32_t live = (uint32_t)slot->extra - 1;
    if (!S_append(roster, kind, label, length, start, true)) {
        return false;
    }
    markdown_core_roster_record *record = roster->records[roster->count - 1];
    record->before = withdrawn;
    record->live = live;
    S_unlink(roster, withdrawn);
    withdrawn->withdrawn = true;
    roster->dead += S_record_size(record->key_len - 1) + S_record_size(withdrawn->key_len - 1);
    roster->spaces[kind].indexed++;
    roster->spaces[kind].cursor = roster->count;
    return true;
}

const markdown_core_roster_record *markdown_core_roster_find(markdown_core_roster *roster,
                                                             markdown_core_roster_kind kind, const unsigned char *label,
                                                             bufsize_t length, bool *failed) {
    if (length <= 0 || !roster->spaces[kind].records) {
        return NULL;
    }
    S_assemble(roster, kind, label, length);
    return S_first(roster, kind, failed);
}

markdown_core_roster_record *markdown_core_roster_first(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                                        const unsigned char *label, bufsize_t length, bool *failed) {
    if (length <= 0 || !roster->spaces[kind].records) {
        return NULL;
    }
    S_assemble(roster, kind, label, length);
    if (roster->key.oom || !S_index_space(roster, kind)) {
        *failed = true;
        return NULL;
    }
    const markdown_core_key_index_slot *slot =
        markdown_core_key_index_find(&roster->index, roster->key.ptr, roster->key.size);
    return slot ? (markdown_core_roster_record *)slot->value : NULL;
}

/* Whether `record` is a declaration of `kind` for `label` the index holds. */
static inline bool S_holds(const markdown_core_roster_record *record, markdown_core_roster_kind kind,
                           const unsigned char *label, bufsize_t length) {
    return record->linked && record->key[0] == (unsigned char)kind && record->key_len == length + 1 &&
           !memcmp(record->key + 1, label, (size_t)length);
}

const markdown_core_roster_record *markdown_core_roster_held_at(const markdown_core_roster *roster,
                                                                markdown_core_roster_kind kind,
                                                                const unsigned char *label, bufsize_t length, bool back,
                                                                uint32_t start) {
    if (!roster->placed) {
        return NULL;
    }
    const markdown_core_roster_places *places = &roster->spaces[kind].places;
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

const markdown_core_roster_record *markdown_core_roster_back_first(markdown_core_roster_record *first) {
    /* The walk begins at the hint (S_link), which it leaves at the front's
     * last for the next. */
    markdown_core_roster_record *at = first->hint ? first->hint : S_last(first);
    while (at && at->back) {
        at = S_prior(first, at);
    }
    while (at && at->after && !at->after->back) {
        at = at->after;
    }
    first->hint = at ? at : first;
    return at ? at->after : first;
}

const markdown_core_roster_record *markdown_core_roster_find_label(markdown_core_roster *roster,
                                                                   markdown_core_roster_kind kind,
                                                                   const markdown_core_chunk *label, bool *failed) {
    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH || !roster->spaces[kind].records) {
        return NULL;
    }
    markdown_core_strbuf_clear(&roster->key);
    markdown_core_strbuf_putc(&roster->key, (int)kind);
    markdown_core_utf8proc_normalize_label(&roster->key, label->data, label->len);
    if (roster->key.size == 1 && !roster->key.oom) {
        return NULL;
    }
    return S_first(roster, kind, failed);
}

bool markdown_core_roster_normal_label(markdown_core_roster *roster, markdown_core_node_pool *pool,
                                       const markdown_core_chunk *label, markdown_core_chunk *normal) {
    *normal = (markdown_core_chunk){0};
    markdown_core_strbuf_clear(&roster->key);
    if (label->len > 0) {
        markdown_core_utf8proc_normalize_label(&roster->key, label->data, label->len);
    }
    if (roster->key.oom) {
        return false;
    }
    const bufsize_t length = roster->key.size;
    if (!length) {
        return true;
    }
    unsigned char *stored = markdown_core_node_pool_bytes(pool, (size_t)length + 1);
    if (!stored) {
        return false;
    }
    memcpy(stored, roster->key.ptr, (size_t)length);
    stored[length] = '\0';
    *normal = (markdown_core_chunk){stored, length, 0};
    return true;
}

markdown_core_roster_mark markdown_core_roster_mark_now(const markdown_core_roster *roster) {
    return (markdown_core_roster_mark){roster->count,      roster->blocks, roster->block_used,
                                       roster->block_size, roster->bytes,  roster->dead};
}

/* The front's entry of the declaration `record`, which an edit's reading
 * declared, goes. */
static void S_unplace(markdown_core_roster *roster, const markdown_core_roster_record *record) {
    markdown_core_roster_places *places = &roster->spaces[record->key[0]].places;
    assert(!record->back);
    size_t at = S_places_front_from(places, record->start);
    while (places->front.values[at].record != record) {
        at++;
        assert(at < places->front.count);
    }
    S_places_front_erase(places, at, 1, roster);
}

void markdown_core_roster_rollback(markdown_core_roster *roster, markdown_core_roster_mark mark) {
    assert(mark.count <= roster->count);
    while (roster->count > mark.count) {
        markdown_core_roster_record *record = roster->records[--roster->count];
        const markdown_core_roster_kind kind = (markdown_core_roster_kind)record->key[0];
        markdown_core_roster_space *space = &roster->spaces[kind];
        space->records--;
        assert(!record->gone);
        /* A record the index never took leaves only the log. */
        if (roster->count < space->cursor) {
            space->indexed--;
        }
        if (record->withdraws) {
            /* The declaration it withdrew stands again, where it stood. */
            markdown_core_roster_record *withdrawn = record->before;
            withdrawn->withdrawn = false;
            markdown_core_key_index_slot *slot =
                markdown_core_key_index_entry(&roster->index, withdrawn->key, withdrawn->key_len);
            /* A removal leaves room for what it removed. */
            assert(slot);
            S_link(roster, slot, withdrawn);
            continue;
        }
        if (record->linked) {
            S_unlink(roster, record);
        }
        if (roster->placed) {
            S_unplace(roster, record);
        }
    }
    for (size_t kind = 0; kind < sizeof(roster->spaces) / sizeof(*roster->spaces); kind++) {
        if (roster->spaces[kind].cursor > roster->count) {
            roster->spaces[kind].cursor = roster->count;
        }
    }
    while (roster->blocks != mark.blocks) {
        markdown_core_roster_block *block = roster->blocks;
        roster->blocks = block->head.next;
        markdown_core_free(block);
    }
    roster->block_used = mark.block_used;
    roster->block_size = mark.block_size;
    roster->bytes = mark.bytes;
    roster->dead = mark.dead;
}

/* Two places in order of where they start, then of the log, which holds
 * the declarations of one start in the order the parse made them. */
static int S_place_order(const void *a, const void *b) {
    const markdown_core_roster_place *left = a, *right = b;
    if (left->start != right->start) {
        return left->start < right->start ? -1 : 1;
    }
    return (left->record > right->record) - (left->record < right->record);
}

/* THE FIRST EDIT places each kind's declarations that stand, all in the
 * front, in the order of where they start. False, with the roster as it
 * was, when storage could not be had. */
static bool S_place(markdown_core_roster *roster) {
    size_t counts[sizeof(roster->spaces) / sizeof(*roster->spaces)] = {0};
    for (size_t i = 0; i < roster->count; i++) {
        counts[roster->records[i]->key[0]] += markdown_core_roster_stands(roster->records[i]);
    }
    for (size_t kind = 0; kind < sizeof(counts) / sizeof(*counts); kind++) {
        if (!markdown_core_array_reserve(&roster->spaces[kind].places.front, counts[kind])) {
            return false;
        }
    }
    for (size_t i = 0; i < roster->count; i++) {
        markdown_core_roster_record *record = roster->records[i];
        if (markdown_core_roster_stands(record)) {
            const markdown_core_roster_place place = {record->start, record};
            (void)markdown_core_array_push(&roster->spaces[record->key[0]].places.front, place);
        }
    }
    for (size_t kind = 0; kind < sizeof(counts) / sizeof(*counts); kind++) {
        markdown_core_roster_places *places = &roster->spaces[kind].places;
        /* A record's address orders the declarations of one start as the
         * log does only within a block; the parse makes most in order of
         * where they start, which the sort then keeps. */
        markdown_core_ahead_unsettle(&places->front.ahead, 0);
        if (places->front.count > 1) {
            qsort(places->front.values, places->front.count, sizeof(*places->front.values), S_place_order);
        }
    }
    roster->placed = true;
    return true;
}

bool markdown_core_roster_reopen(markdown_core_roster *roster, uint32_t start, size_t size) {
    if (!roster->placed && !S_place(roster)) {
        return false;
    }
    if (roster->dead > roster->bytes - roster->dead + 4096 && !markdown_core_roster_compact(roster)) {
        return false;
    }
    const size_t kinds = sizeof(roster->spaces) / sizeof(*roster->spaces);
    for (size_t kind = 0; kind < kinds; kind++) {
        if (!S_places_ready(&roster->spaces[kind].places, start, size)) {
            return false;
        }
    }
    for (size_t kind = 0; kind < kinds; kind++) {
        markdown_core_roster_places *places = &roster->spaces[kind].places;
        const size_t front = places->front.count, back = places->back_count;
        (void)S_places_move(places, start, size, roster);
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
    roster->hidden = true;
    markdown_core_strbuf_clear(&roster->lost);
    return true;
}

void markdown_core_roster_splice(markdown_core_roster *roster, size_t place, int64_t delta) {
    for (size_t kind = 0; kind < sizeof(roster->spaces) / sizeof(*roster->spaces); kind++) {
        markdown_core_roster_places *places = &roster->spaces[kind].places;
        const size_t gone = S_places_back_from(places, place);
        for (size_t k = 0; k < gone; k++) {
            markdown_core_roster_record *record = places->back[places->back_count - 1 - k].record;
            if (markdown_core_roster_stands(record)) {
                markdown_core_strbuf_put(&roster->lost, record->key, record->key_len + 1);
                roster->dead += S_record_size(record->key_len - 1);
            }
            if (record->linked) {
                S_unlink(roster, record);
            }
            record->gone = true;
        }
        (void)S_places_cut(places, place);
        places->end = (size_t)((int64_t)places->end + delta);
    }
    roster->hidden = false;
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

bool markdown_core_roster_made(const markdown_core_roster *roster, uint32_t start, markdown_core_strbuf *labels) {
    size_t count = 0;
    for (size_t kind = 0; kind < MARKDOWN_CORE_ROSTER_ANCHOR; kind++) {
        const markdown_core_roster_places *places = &roster->spaces[kind].places;
        count += places->front.count - S_places_front_from(places, start);
    }
    S_key *keys = count ? markdown_core_alloc(count, sizeof(*keys)) : NULL;
    if (count && !keys) {
        return false;
    }
    count = 0;
    for (size_t kind = 0; kind < MARKDOWN_CORE_ROSTER_ANCHOR; kind++) {
        const markdown_core_roster_places *places = &roster->spaces[kind].places;
        for (size_t i = S_places_front_from(places, start); i < places->front.count; i++) {
            const markdown_core_roster_record *record = places->front.values[i].record;
            if (markdown_core_roster_stands(record)) {
                keys[count++] = (S_key){record->key, record->key_len};
            }
        }
    }
    const bool ok = S_put_keys(keys, count, labels);
    markdown_core_free(keys);
    return ok;
}

bool markdown_core_roster_lost(const markdown_core_roster *roster, markdown_core_strbuf *labels) {
    /* A key the splice could not keep is one the answer would miss. */
    if (roster->lost.oom) {
        return false;
    }
    size_t count = 0;
    for (bufsize_t at = 0; at < roster->lost.size; at += (bufsize_t)strlen((const char *)roster->lost.ptr + at) + 1) {
        count++;
    }
    S_key *keys = count ? markdown_core_alloc(count, sizeof(*keys)) : NULL;
    if (count && !keys) {
        return false;
    }
    count = 0;
    for (bufsize_t at = 0; at < roster->lost.size;) {
        const bufsize_t size = (bufsize_t)strlen((const char *)roster->lost.ptr + at);
        if (roster->lost.ptr[at] != MARKDOWN_CORE_ROSTER_ANCHOR) {
            keys[count++] = (S_key){roster->lost.ptr + at, size};
        }
        at += size + 1;
    }
    const bool ok = S_put_keys(keys, count, labels);
    markdown_core_free(keys);
    return ok;
}

bool markdown_core_roster_anchors(const markdown_core_roster *roster, uint32_t start, markdown_core_strbuf *labels) {
    const markdown_core_roster_places *places = &roster->spaces[MARKDOWN_CORE_ROSTER_ANCHOR].places;
    markdown_core_strbuf_clear(labels);
    for (size_t i = S_places_front_from(places, start); i < places->front.count; i++) {
        const markdown_core_roster_record *record = places->front.values[i].record;
        markdown_core_strbuf_put(labels, record->key + 1, record->key_len);
    }
    for (size_t i = 0; roster->hidden && i < places->back_count; i++) {
        const markdown_core_roster_record *record = places->back[i].record;
        markdown_core_strbuf_put(labels, record->key + 1, record->key_len);
    }
    for (bufsize_t at = 0; at < roster->lost.size;) {
        const bufsize_t size = (bufsize_t)strlen((const char *)roster->lost.ptr + at);
        if (roster->lost.ptr[at] == MARKDOWN_CORE_ROSTER_ANCHOR) {
            markdown_core_strbuf_put(labels, roster->lost.ptr + at + 1, size);
        }
        at += size + 1;
    }
    return !labels->oom && !roster->lost.oom;
}

bool markdown_core_roster_declared_outside(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                           const unsigned char *label, bufsize_t length, uint32_t from, uint32_t to,
                                           bool *failed) {
    if (length <= 0 || !roster->spaces[kind].records) {
        return false;
    }
    S_assemble(roster, kind, label, length);
    const markdown_core_roster_record *first = S_first(roster, kind, failed);
    return first && (markdown_core_roster_start(roster, first) < from ||
                     markdown_core_roster_start(roster, S_last(first)) >= to);
}

bool markdown_core_roster_compact(markdown_core_roster *roster) {
    if (!roster->placed) {
        return true;
    }
    const size_t kinds = sizeof(roster->spaces) / sizeof(*roster->spaces);
    /* The places keep only the declarations that stand. */
    size_t count = 0;
    for (size_t kind = 0; kind < kinds; kind++) {
        markdown_core_roster_places *places = &roster->spaces[kind].places;
        size_t kept = 0;
        for (size_t i = 0; i < places->front.count; i++) {
            if (markdown_core_roster_stands(places->front.values[i].record)) {
                places->front.values[kept++] = places->front.values[i];
            }
        }
        markdown_core_ahead_unsettle(&places->front.ahead, 0);
        markdown_core_array_truncate(&places->front, kept);
        kept = 0;
        for (size_t i = 0; i < places->back_count; i++) {
            if (markdown_core_roster_stands(places->back[i].record)) {
                places->back[kept++] = places->back[i];
            }
        }
        places->back_count = kept;
        count += places->front.count + places->back_count;
    }
    /* Then each is copied into new storage, a kind's in the order of where
     * they start, which the log then holds. */
    markdown_core_roster fresh;
    markdown_core_roster_init(&fresh);
    fresh.records = markdown_core_alloc(count ? count : 1, sizeof(*fresh.records));
    if (!fresh.records) {
        return false;
    }
    fresh.capacity = count ? count : 1;
    for (size_t kind = 0; kind < kinds; kind++) {
        markdown_core_roster_places *places = &roster->spaces[kind].places;
        for (size_t i = 0; i < places->front.count + places->back_count; i++) {
            markdown_core_roster_place *place = i < places->front.count
                                                    ? &places->front.values[i]
                                                    : &places->back[places->back_count - 1 - (i - places->front.count)];
            const size_t size = S_record_size(place->record->key_len - 1);
            markdown_core_roster_record *copy = S_carve(&fresh, size);
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
        markdown_core_roster_space *space = &roster->spaces[kind];
        markdown_core_roster_places *places = &space->places;
        for (size_t i = 0; i < places->front.count + places->back_count; i++) {
            markdown_core_roster_place *place = i < places->front.count
                                                    ? &places->front.values[i]
                                                    : &places->back[places->back_count - 1 - (i - places->front.count)];
            markdown_core_roster_record *copy = fresh.records[count++];
            memcpy(copy, place->record, sizeof(*copy) + (size_t)place->record->key_len + 1);
            copy->before = copy->after = copy->hint = NULL;
            copy->linked = false;
            place->record = copy;
        }
        space->records = places->front.count + places->back_count;
        space->indexed = space->cursor = 0;
        markdown_core_array_shrink(&places->front);
    }
    S_free_blocks(roster->blocks);
    markdown_core_free(roster->records);
    markdown_core_key_index_free(&roster->index);
    roster->index = (markdown_core_key_index){0};
    roster->records = fresh.records;
    roster->count = fresh.count;
    roster->capacity = fresh.capacity;
    roster->blocks = fresh.blocks;
    roster->block_used = fresh.block_used;
    roster->block_size = fresh.block_size;
    roster->bytes = fresh.bytes;
    roster->dead = 0;
    markdown_core_strbuf_free(&fresh.key);
    markdown_core_strbuf_free(&fresh.lost);
    return true;
}
