#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "alloc.h"
#include "roster.h"
#include "parser.h"
#include "utf8.h"

static inline markdown_core_key_index_slot *S_index_find(markdown_core_roster *roster, const unsigned char *key,
                                                         bufsize_t length) {
    return markdown_core_key_index_find(&roster->index, key, length);
}

static inline markdown_core_key_index_slot *S_slot(markdown_core_roster *roster, const unsigned char *key,
                                                   bufsize_t length) {
    return markdown_core_key_index_entry(&roster->index, key, length);
}

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

/* THE PLACES' TREES: an entry moves with an edit before it, and tells its
 * record's number where it is. */
static size_t S_place_start(const void *place) { return ((const markdown_core_roster_place *)place)->start; }

static void S_place_shift(void *entry, int64_t delta) {
    markdown_core_roster_place *place = entry;
    place->start = (uint32_t)((int64_t)place->start + delta);
}

static void S_place_moved(const markdown_core_tree *tree, void *entry, markdown_core_tree_leaf *leaf, uint32_t slot) {
    const markdown_core_roster_record *record = ((const markdown_core_roster_place *)entry)->record;
    markdown_core_roster *roster =
        ((const markdown_core_roster_space *)(const void *)((const char *)tree -
                                                            offsetof(markdown_core_roster_space, places)))
            ->roster;
    *markdown_core_array_write(&roster->where, record->id) = (markdown_core_tree_handle){leaf, slot};
}

static const markdown_core_tree_ops S_places_ops = {sizeof(markdown_core_roster_place), S_place_start, NULL,
                                                    S_place_shift, S_place_moved};

MARKDOWN_CORE_TREE_OPS(S_places, markdown_core_roster_place)

/* The record numbered, or false when storage could not be had. */
static bool S_number(markdown_core_roster *roster, markdown_core_roster_record *record) {
    if (roster->free) {
        record->id = roster->free - 1;
        roster->free = roster->where.values[record->id].slot;
        return true;
    }
    if (!markdown_core_array_reserve(&roster->where, roster->where.count + 1)) {
        return false;
    }
    record->id = (uint32_t)roster->where.count++;
    roster->where.values[record->id] = (markdown_core_tree_handle){NULL, 0};
    return true;
}

/* The record's number free again. */
static void S_unnumber(markdown_core_roster *roster, markdown_core_roster_record *record) {
    *markdown_core_array_write(&roster->where, record->id) = (markdown_core_tree_handle){NULL, roster->free};
    roster->free = record->id + 1;
    record->id = MARKDOWN_CORE_ROSTER_UNPLACED;
}

/* The rank of a placed declaration's entry in its kind's tree. */
static inline size_t S_rank(const markdown_core_roster *roster, const markdown_core_roster_record *record) {
    return markdown_core_tree_rank(&roster->spaces[record->key[0]].places, roster->where.values[record->id]);
}

uint32_t markdown_core_roster_placed(const markdown_core_roster *roster, const markdown_core_roster_record *record,
                                     bool *back) {
    const markdown_core_tree *places = &roster->spaces[record->key[0]].places;
    const markdown_core_tree_handle where = roster->where.values[record->id];
    markdown_core_roster_place place;
    markdown_core_tree_read(places, where, &place);
    *back = markdown_core_tree_rank(places, where) >= places->front;
    return place.start;
}

bool markdown_core_roster_init(markdown_core_roster *roster, size_t kinds) {
    *roster = (markdown_core_roster){0};
    markdown_core_strbuf_init(&roster->key, 0);
    markdown_core_strbuf_init(&roster->lost, 0);
    roster->spaces = markdown_core_alloc(kinds, sizeof(*roster->spaces));
    if (!roster->spaces) {
        return false;
    }
    roster->kinds = kinds;
    for (size_t kind = 0; kind < kinds; kind++) {
        roster->spaces[kind].places = markdown_core_tree_make(&S_places_ops);
        roster->spaces[kind].roster = roster;
    }
    return true;
}

void markdown_core_roster_free(markdown_core_roster *roster) {
    S_free_blocks(roster->blocks);
    markdown_core_free(roster->records);
    markdown_core_key_index_free(&roster->index);
    markdown_core_strbuf_free(&roster->key);
    markdown_core_strbuf_free(&roster->lost);
    for (size_t kind = 0; kind < roster->kinds; kind++) {
        markdown_core_tree_free(&roster->spaces[kind].places);
    }
    markdown_core_free(roster->spaces);
    markdown_core_array_free(&roster->where);
    *roster = (markdown_core_roster){0};
}

/* THE KEY'S LIST. Two declarations of a key in the order of where they
 * start: by their entries' ranks once the kinds hold places, the front's
 * before the back's; a declaration after another that starts where it
 * does goes after it. */
static inline bool S_precedes(const markdown_core_roster *roster, const markdown_core_roster_record *left,
                              const markdown_core_roster_record *right) {
    return roster->placed ? S_rank(roster, left) < S_rank(roster, right) : left->start < right->start;
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
    if (!S_precedes(roster, record, at)) {
        while (at->after && !S_precedes(roster, record, at->after)) {
            at = at->after;
        }
    } else {
        while (at && S_precedes(roster, record, at)) {
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
    markdown_core_key_index_slot *slot = S_index_find(roster, record->key, record->key_len);
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
    record->id = MARKDOWN_CORE_ROSTER_UNPLACED;
    record->before = record->after = record->hint = NULL;
    record->live = 0;
    record->withdraws = withdraws;
    record->linked = record->withdrawn = record->gone = false;
    roster->records[roster->count++] = record;
    roster->spaces[kind].records++;
    roster->bytes += size;
    return true;
}

/* The front's place for a declaration that starts at `start`: after every
 * one that starts there or before. */
static inline size_t S_front_after(const markdown_core_tree *places, uint32_t start) {
    return markdown_core_tree_from(places, (size_t)start + 1, 0, places->front);
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
    markdown_core_tree *places = &roster->spaces[kind].places;
    if (!S_append(roster, kind, label, length, start, false)) {
        return false;
    }
    markdown_core_roster_record *record = roster->records[roster->count - 1];
    if (S_number(roster, record) &&
        S_places_insert(places, S_front_after(places, start), (markdown_core_roster_place){start, record}, true)) {
        return true;
    }
    /* The log as it was; the record's bytes stay carved until a rollback
     * or a compaction. */
    if (record->id != MARKDOWN_CORE_ROSTER_UNPLACED) {
        S_unnumber(roster, record);
    }
    roster->count--;
    roster->spaces[kind].records--;
    roster->bytes -= S_record_size(length);
    return false;
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
        markdown_core_key_index_slot *slot = S_slot(roster, record->key, record->key_len);
        if (!slot) {
            return false;
        }
        S_link(roster, slot, record);
    }
    space->cursor = roster->count;
    return true;
}

/* The first declaration of the key `slot` holds, of none, that stands and
 * a lookup sees. */
static inline markdown_core_roster_record *S_seen(const markdown_core_roster *roster,
                                                  const markdown_core_key_index_slot *slot) {
    markdown_core_roster_record *first = slot ? (markdown_core_roster_record *)slot->value : NULL;
    /* Every declaration of the front comes before any of the back's. */
    return first && !(roster->hidden && markdown_core_roster_back(roster, first)) ? first : NULL;
}

/* The first declaration of the key the roster assembled that stands and
 * a lookup sees. */
static markdown_core_roster_record *S_first(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                            bool *failed) {
    if (roster->key.oom || !S_index_space(roster, kind)) {
        *failed = true;
        return NULL;
    }
    return S_seen(roster, S_index_find(roster, roster->key.ptr, roster->key.size));
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
    const markdown_core_key_index_slot *slot = S_index_find(roster, roster->key.ptr, roster->key.size);
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
    const markdown_core_key_index_slot *slot = S_index_find(roster, roster->key.ptr, roster->key.size);
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
    markdown_core_tree *places = (markdown_core_tree *)&roster->spaces[kind].places;
    const size_t high = back ? places->count : places->front;
    for (size_t i = markdown_core_tree_from(places, start, back ? places->front : 0, high);
         i < high && S_places_at(places, i)->start == start; i++) {
        const markdown_core_roster_record *record = S_places_at(places, i)->record;
        if (S_holds(record, kind, label, length)) {
            return record;
        }
    }
    return NULL;
}

const markdown_core_roster_record *markdown_core_roster_back_first(const markdown_core_roster *roster,
                                                                   markdown_core_roster_record *first) {
    /* The walk begins at the hint (S_link), which it leaves at the front's
     * last for the next. */
    markdown_core_roster_record *at = first->hint ? first->hint : S_last(first);
    while (at && markdown_core_roster_back(roster, at)) {
        at = S_prior(first, at);
    }
    while (at && at->after && !markdown_core_roster_back(roster, at->after)) {
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
static void S_unplace(markdown_core_roster *roster, markdown_core_roster_record *record) {
    markdown_core_tree *places = &roster->spaces[record->key[0]].places;
    const size_t at = S_rank(roster, record);
    assert(at < places->front);
    S_unnumber(roster, record);
    markdown_core_tree_erase(places, at, 1);
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
            markdown_core_key_index_slot *slot = S_slot(roster, withdrawn->key, withdrawn->key_len);
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
    for (size_t kind = 0; kind < roster->kinds; kind++) {
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

/* A key at `key`, its NUL after it. */
typedef struct {
    const unsigned char *key;
    bufsize_t size;
} S_key;
