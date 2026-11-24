#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "alloc.h"
#include "roster.h"
#include "parser.h"
#include "utf8.h"
#include "pieces.h"

/* THE INDEX'S HASH of a record's key, its kind then its label: an ANCHOR's
 * is its measure's, as a name is filed (map.h, KEYS UNDER A HASH THEIR USER
 * GIVES), so that a heading's name, kept in pieces, is looked up whole
 * (markdown_core_roster_find_name); any other kind's is its bytes' read in
 * parts (markdown_core_key_stream), so that a reader looks up a kind and a
 * label without joining them (markdown_core_roster_seek). */
static inline uint64_t S_label_hash(markdown_core_roster_kind kind, const unsigned char *label, bufsize_t length) {
    const unsigned char byte = (unsigned char)kind;
    markdown_core_key_stream stream;
    markdown_core_key_stream_start(&stream);
    markdown_core_key_stream_add(&stream, &byte, 1);
    markdown_core_key_stream_add(&stream, label, (size_t)length);
    return markdown_core_key_stream_end(&stream);
}

static inline uint64_t S_hash(const unsigned char *key, bufsize_t length) {
    return key[0] == (unsigned char)MARKDOWN_CORE_ROSTER_ANCHOR
               ? markdown_core_key_index_hash(markdown_core_measure_of(key, (size_t)length))
               : S_label_hash(key[0], key + 1, length - 1);
}

static inline markdown_core_key_index_slot *S_index_find(markdown_core_roster *roster, const unsigned char *key,
                                                         bufsize_t length) {
    return markdown_core_key_index_find_hashed(&roster->index, S_hash(key, length), key, length);
}

static inline markdown_core_key_index_slot *S_slot(markdown_core_roster *roster, const unsigned char *key,
                                                   bufsize_t length) {
    return markdown_core_key_index_entry_hashed(&roster->index, S_hash(key, length), key, length);
}

/* THE LOG'S RECORD STORAGE. A record lives until the roster is freed or a
 * compaction copies it, so records are carved one after another from
 * blocks the roster owns. A block starts small, so a parse that
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
        ((const markdown_core_roster_shelf *)(const void *)((const char *)tree -
                                                            offsetof(markdown_core_roster_shelf, places)))
            ->roster;
    *markdown_core_array_write(&roster->where, record->id) = (markdown_core_tree_handle){leaf, slot};
}

static const markdown_core_tree_ops S_places_ops = {
    sizeof(markdown_core_roster_place), S_place_start, NULL, S_place_shift, S_place_moved, NULL};

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

/* The tree that places the record. */
static inline markdown_core_tree *S_places(const markdown_core_roster *roster,
                                           const markdown_core_roster_record *record) {
    return &roster->shelves[record->shelf]->places;
}

/* The rank of a placed declaration's entry in its shelf's tree. */
static inline size_t S_rank(const markdown_core_roster *roster, const markdown_core_roster_record *record) {
    return markdown_core_tree_rank(S_places(roster, record), roster->where.values[record->id]);
}

uint32_t markdown_core_roster_placed(const markdown_core_roster *roster, const markdown_core_roster_record *record,
                                     bool *back) {
    const markdown_core_tree *places = S_places(roster, record);
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
    return true;
}

void markdown_core_roster_free(markdown_core_roster *roster) {
    S_free_blocks(roster->blocks);
    markdown_core_free(roster->records);
    markdown_core_key_index_free(&roster->index);
    markdown_core_strbuf_free(&roster->key);
    markdown_core_strbuf_free(&roster->lost);
    for (size_t i = 0; i < roster->shelf_count; i++) {
        markdown_core_tree_free(&roster->shelves[i]->places);
        markdown_core_free(roster->shelves[i]);
    }
    markdown_core_free(roster->shelves);
    markdown_core_free(roster->spaces);
    markdown_core_array_free(&roster->where);
    *roster = (markdown_core_roster){0};
}

/* THE KEY'S LIST. Two declarations of a key in the order of where they
 * start, the front's before the back's: of one shelf by their entries'
 * ranks, so that a declaration after another that starts where it does
 * goes after it, and of two by their parts and starts (THE PLACES). */
static inline bool S_precedes(const markdown_core_roster *roster, const markdown_core_roster_record *left,
                              const markdown_core_roster_record *right) {
    if (left->shelf == right->shelf) {
        return S_rank(roster, left) < S_rank(roster, right);
    }
    bool left_back, right_back;
    const uint32_t left_start = markdown_core_roster_placed(roster, left, &left_back);
    const uint32_t right_start = markdown_core_roster_placed(roster, right, &right_back);
    if (left_back != right_back) {
        return right_back;
    }
    return left_start != right_start ? left_start < right_start : left->shelf < right->shelf;
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
                            bufsize_t length, uint32_t start, bool withdraws, const markdown_core_node *node) {
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
    if (length) {
        memcpy(record->key + 1, label, (size_t)length);
    }
    record->key[length + 1] = '\0';
    record->key_len = length + 1;
    record->start = start;
    record->id = MARKDOWN_CORE_ROSTER_UNPLACED;
    record->before = record->after = record->hint = NULL;
    record->node = node;
    record->shelf = 0;
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

/* The shelf of `kind` that nodes of kind `node` declare in, made when
 * there is none, as its number, or -1 when storage could not be had. */
static int S_shelf(markdown_core_roster *roster, markdown_core_roster_kind kind, uint16_t node) {
    for (size_t i = 0; i < roster->shelf_count; i++) {
        if (roster->shelves[i]->kind == kind && roster->shelves[i]->node == node) {
            return (int)i;
        }
    }
    if (roster->shelf_count == MARKDOWN_CORE_ROSTER_SHELVES) {
        return -1;
    }
    markdown_core_roster_shelf **shelves =
        markdown_core_realloc(roster->shelves, (roster->shelf_count + 1) * sizeof(*shelves));
    if (!shelves) {
        return -1;
    }
    roster->shelves = shelves;
    markdown_core_roster_shelf *shelf = markdown_core_alloc(1, sizeof(*shelf));
    if (!shelf) {
        return -1;
    }
    *shelf = (markdown_core_roster_shelf){markdown_core_tree_make(&S_places_ops), roster, kind, node};
    shelves[roster->shelf_count] = shelf;
    return (int)roster->shelf_count++;
}

bool markdown_core_roster_declare(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                  const unsigned char *label, bufsize_t length, uint32_t start,
                                  const markdown_core_node *node) {
    if (length < 0 || (!length && !node)) {
        return true;
    }
    /* Each declaration is placed as it is declared, in its shelf. */
    const int shelf = S_shelf(roster, kind, node ? node->kind : (uint16_t)MARKDOWN_CORE_NODE_NONE);
    if (shelf < 0 || !S_append(roster, kind, label, length, start, false, node)) {
        return false;
    }
    markdown_core_tree *places = &roster->shelves[shelf]->places;
    markdown_core_roster_record *record = roster->records[roster->count - 1];
    record->shelf = (uint8_t)shelf;
    if (S_number(roster, record) &&
        S_places_insert(places, S_front_after(places, start), (markdown_core_roster_place){start, record}, true)) {
        return true;
    }
    /* The log as it was; the record's bytes stay carved until a
     * compaction. */
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
        /* A declaration of no label is a node's place alone. */
        if (!markdown_core_roster_stands(record) || record->key_len == 1) {
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

/* Whether the ANCHOR record `slot` holds is of the name `probe`. */
static bool S_same_name(const void *probe, const markdown_core_key_index_slot *slot) {
    return slot->key[0] == (unsigned char)MARKDOWN_CORE_ROSTER_ANCHOR &&
           markdown_core_pieces_are(probe, slot->key + 1, slot->key_len - 1);
}

const markdown_core_roster_record *markdown_core_roster_find_name(markdown_core_roster *roster,
                                                                  const markdown_core_pieces *name,
                                                                  markdown_core_measure measure, bool *failed) {
    if (!name->size || !roster->spaces[MARKDOWN_CORE_ROSTER_ANCHOR].records) {
        return NULL;
    }
    if (!S_index_space(roster, MARKDOWN_CORE_ROSTER_ANCHOR)) {
        *failed = true;
        return NULL;
    }
    const unsigned char kind = (unsigned char)MARKDOWN_CORE_ROSTER_ANCHOR;
    const uint64_t hash =
        markdown_core_key_index_hash(markdown_core_measure_join(markdown_core_measure_of(&kind, 1), measure));
    return S_seen(roster, markdown_core_key_index_seek(&roster->index, hash, name->size + 1, S_same_name, name));
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
    if (!S_append(roster, kind, label, length, start, true, NULL)) {
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

bool markdown_core_roster_index(markdown_core_roster *roster, markdown_core_roster_kind kind) {
    return S_index_space(roster, kind);
}

/* Whether the slot holds the key of the kind and label `probe` names. */
typedef struct {
    markdown_core_roster_kind kind;
    const unsigned char *label;
    bufsize_t length;
} S_probe;

static bool S_same_label(const void *probe, const markdown_core_key_index_slot *slot) {
    const S_probe *key = probe;
    return slot->key[0] == (unsigned char)key->kind && !memcmp(slot->key + 1, key->label, (size_t)key->length);
}

const markdown_core_roster_record *markdown_core_roster_seek(const markdown_core_roster *roster,
                                                             markdown_core_roster_kind kind, const unsigned char *label,
                                                             bufsize_t length) {
    assert(!roster->hidden && roster->spaces[kind].indexed == roster->spaces[kind].records);
    if (length <= 0 || length == INT32_MAX || !roster->spaces[kind].records) {
        return NULL;
    }
    const S_probe probe = {kind, label, length};
    const markdown_core_key_index_slot *slot = markdown_core_key_index_seek(
        &roster->index, S_label_hash(kind, label, length), length + 1, S_same_label, &probe);
    return slot ? (const markdown_core_roster_record *)slot->value : NULL;
}

const markdown_core_roster_shelf *markdown_core_roster_shelf_of(const markdown_core_roster *roster,
                                                                markdown_core_roster_kind kind, uint16_t node) {
    for (size_t i = 0; i < roster->shelf_count; i++) {
        if (roster->shelves[i]->kind == kind && roster->shelves[i]->node == node) {
            return roster->shelves[i];
        }
    }
    return NULL;
}

const markdown_core_roster_record *markdown_core_roster_shelf_at(const markdown_core_roster_shelf *shelf,
                                                                 size_t index) {
    return ((const markdown_core_roster_place *)markdown_core_tree_peek(&shelf->places, index))->record;
}

void markdown_core_roster_moved(markdown_core_roster *roster) {
    for (size_t i = 0; i < roster->shelf_count; i++) {
        roster->shelves[i]->roster = roster;
    }
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

bool markdown_core_roster_reopen(markdown_core_roster *roster, uint32_t start) {
    if (roster->dead > roster->bytes - roster->dead + 4096 && !markdown_core_roster_compact(roster)) {
        return false;
    }
    for (size_t i = 0; i < roster->shelf_count; i++) {
        markdown_core_tree *places = &roster->shelves[i]->places;
        const bool back = places->front < places->count && S_places_at(places, places->front)->start < start;
        markdown_core_tree_focus(places, back ? markdown_core_tree_from(places, start, places->front, places->count)
                                              : markdown_core_tree_from(places, start, 0, places->front));
    }
    roster->hidden = true;
    roster->from = start;
    markdown_core_strbuf_clear(&roster->lost);
    return true;
}

void markdown_core_roster_splice(markdown_core_roster *roster, size_t place, int64_t delta) {
    for (size_t i = 0; i < roster->shelf_count; i++) {
        markdown_core_tree *places = &roster->shelves[i]->places;
        const size_t front = places->front;
        const size_t gone = markdown_core_tree_from(places, place, front, places->count) - front;
        for (size_t k = 0; k < gone; k++) {
            markdown_core_roster_record *record = S_places_at(places, front + k)->record;
            if (markdown_core_roster_stands(record) && record->key_len > 1) {
                markdown_core_strbuf_put_key(&roster->lost, record->key, record->key_len);
            }
            if (markdown_core_roster_stands(record)) {
                roster->dead += S_record_size(record->key_len - 1);
            }
            if (record->linked) {
                S_unlink(roster, record);
            }
            record->gone = true;
        }
        for (size_t k = 0; k < gone; k++) {
            S_unnumber(roster, S_places_at(places, front + k)->record);
        }
        markdown_core_tree_erase(places, front, gone);
        markdown_core_tree_shift(places, front, delta);
    }
    roster->hidden = false;
}

/* A key at `key`, `size` bytes. */
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

/* The `count` keys of `keys`, sorted, each once, into the list of keys
 * `labels` (buffer.h). */
static bool S_put_keys(S_key *keys, size_t count, markdown_core_strbuf *labels) {
    if (count) {
        qsort(keys, count, sizeof(*keys), S_key_order);
    }
    markdown_core_strbuf_clear(labels);
    for (size_t i = 0; i < count; i++) {
        if (!i || S_key_order(&keys[i], &keys[i - 1])) {
            markdown_core_strbuf_put_key(labels, keys[i].key, keys[i].size);
        }
    }
    return !labels->oom;
}

bool markdown_core_roster_made(const markdown_core_roster *roster, markdown_core_strbuf *labels) {
    const uint32_t start = roster->from;
    size_t count = 0;
    for (size_t k = 0; k < roster->shelf_count; k++) {
        const markdown_core_tree *places = &roster->shelves[k]->places;
        if (roster->shelves[k]->kind != MARKDOWN_CORE_ROSTER_ANCHOR) {
            count += places->front - markdown_core_tree_from(places, start, 0, places->front);
        }
    }
    S_key *keys = count ? markdown_core_alloc(count, sizeof(*keys)) : NULL;
    if (count && !keys) {
        return false;
    }
    count = 0;
    for (size_t k = 0; k < roster->shelf_count; k++) {
        markdown_core_tree *places = &roster->shelves[k]->places;
        if (roster->shelves[k]->kind == MARKDOWN_CORE_ROSTER_ANCHOR) {
            continue;
        }
        for (size_t i = markdown_core_tree_from(places, start, 0, places->front); i < places->front; i++) {
            const markdown_core_roster_record *record = S_places_at(places, i)->record;
            if (markdown_core_roster_stands(record) && record->key_len > 1) {
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
    bufsize_t size;
    for (bufsize_t at = 0; markdown_core_strbuf_key(&roster->lost, &at, &size);) {
        count++;
    }
    S_key *keys = count ? markdown_core_alloc(count, sizeof(*keys)) : NULL;
    if (count && !keys) {
        return false;
    }
    count = 0;
    for (bufsize_t at = 0;;) {
        const unsigned char *key = markdown_core_strbuf_key(&roster->lost, &at, &size);
        if (!key) {
            break;
        }
        if (key[0] != MARKDOWN_CORE_ROSTER_ANCHOR) {
            keys[count++] = (S_key){key, size};
        }
    }
    const bool ok = S_put_keys(keys, count, labels);
    markdown_core_free(keys);
    return ok;
}

bool markdown_core_roster_touched(const markdown_core_roster *roster, markdown_core_roster_kind kind,
                                  markdown_core_strbuf *keys) {
    markdown_core_strbuf_clear(keys);
    for (size_t k = 0; k < roster->shelf_count; k++) {
        markdown_core_tree *places = &roster->shelves[k]->places;
        if (roster->shelves[k]->kind != kind) {
            continue;
        }
        for (size_t i = markdown_core_tree_from(places, roster->from, 0, places->front); i < places->front; i++) {
            const markdown_core_roster_record *record = S_places_at(places, i)->record;
            if (markdown_core_roster_stands(record) && record->key_len > 1) {
                markdown_core_strbuf_put_key(keys, record->key, record->key_len);
            }
        }
    }
    bufsize_t size;
    for (bufsize_t at = 0;;) {
        const unsigned char *key = markdown_core_strbuf_key(&roster->lost, &at, &size);
        if (!key) {
            break;
        }
        if (key[0] == (unsigned char)kind) {
            markdown_core_strbuf_put_key(keys, key, size);
        }
    }
    return !keys->oom && !roster->lost.oom;
}

bool markdown_core_roster_anchors(const markdown_core_roster *roster, uint32_t start, markdown_core_strbuf *labels) {
    markdown_core_strbuf_clear(labels);
    for (size_t k = 0; k < roster->shelf_count; k++) {
        markdown_core_tree *places = &roster->shelves[k]->places;
        if (roster->shelves[k]->kind != MARKDOWN_CORE_ROSTER_ANCHOR) {
            continue;
        }
        const size_t end = roster->hidden ? places->count : places->front;
        for (size_t i = markdown_core_tree_from(places, start, 0, places->front); i < end; i++) {
            const markdown_core_roster_record *record = S_places_at(places, i)->record;
            markdown_core_strbuf_put_key(labels, record->key + 1, record->key_len - 1);
        }
    }
    bufsize_t size;
    for (bufsize_t at = 0;;) {
        const unsigned char *key = markdown_core_strbuf_key(&roster->lost, &at, &size);
        if (!key) {
            break;
        }
        if (key[0] == MARKDOWN_CORE_ROSTER_ANCHOR) {
            markdown_core_strbuf_put_key(labels, key + 1, size - 1);
        }
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

/* A record copied in a compaction, which the old one forwards to in
 * `hint` (markdown_core_roster_compact), or NULL. */
static inline markdown_core_roster_record *S_forward(const markdown_core_roster_record *old) {
    return old ? old->hint : NULL;
}

bool markdown_core_roster_compact(markdown_core_roster *roster) {
    /* Every kind is indexed first, so that the copies keep the index as it
     * is, and a reader finds them in it (markdown_core_roster_seek). */
    for (size_t kind = 0; kind < roster->kinds; kind++) {
        if (!S_index_space(roster, (markdown_core_roster_kind)kind)) {
            return false;
        }
    }
    /* The places keep only the declarations that stand. */
    size_t count = 0;
    for (size_t k = 0; k < roster->shelf_count; k++) {
        markdown_core_tree *places = &roster->shelves[k]->places;
        for (size_t i = places->count; i-- > 0;) {
            markdown_core_roster_record *record = S_places_at(places, i)->record;
            if (!markdown_core_roster_stands(record)) {
                S_unnumber(roster, record);
                markdown_core_tree_erase(places, i, 1);
            }
        }
        count += places->count;
    }
    /* Then each is copied into new storage, a shelf's in the order of where
     * they start, which the log then holds. */
    markdown_core_roster fresh = {0};
    fresh.records = markdown_core_alloc(count ? count : 1, sizeof(*fresh.records));
    if (!fresh.records) {
        return false;
    }
    fresh.capacity = count ? count : 1;
    for (size_t k = 0; k < roster->shelf_count; k++) {
        markdown_core_tree *places = &roster->shelves[k]->places;
        for (size_t i = 0; i < places->count; i++) {
            const size_t size = S_record_size(S_places_at(places, i)->record->key_len - 1);
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
    /* Each old record forwards to its copy, which the lists and the index
     * then name in its place. */
    count = 0;
    for (size_t k = 0; k < roster->shelf_count; k++) {
        markdown_core_tree *places = &roster->shelves[k]->places;
        for (size_t i = 0; i < places->count; i++) {
            markdown_core_roster_place *place = S_places_at(places, i);
            markdown_core_roster_record *copy = fresh.records[count++];
            memcpy(copy, place->record, sizeof(*copy) + (size_t)place->record->key_len + 1);
            place->record->hint = copy;
            place->record = copy;
        }
    }
    for (size_t i = 0; i < fresh.count; i++) {
        markdown_core_roster_record *copy = fresh.records[i];
        copy->before = S_forward(copy->before);
        copy->after = S_forward(copy->after);
        copy->hint = S_forward(copy->hint);
    }
    for (size_t i = 0; i < roster->index.capacity; i++) {
        markdown_core_key_index_slot *slot = &roster->index.slots[i];
        if (slot->key) {
            markdown_core_roster_record *first = S_forward((markdown_core_roster_record *)slot->value);
            slot->value = (size_t)first;
            slot->key = first->key;
        }
    }
    for (size_t kind = 0; kind < roster->kinds; kind++) {
        roster->spaces[kind].records = roster->spaces[kind].indexed = 0;
    }
    for (size_t i = 0; i < fresh.count; i++) {
        markdown_core_roster_space *space = &roster->spaces[fresh.records[i]->key[0]];
        space->records++;
        space->indexed++;
    }
    for (size_t kind = 0; kind < roster->kinds; kind++) {
        roster->spaces[kind].cursor = fresh.count;
    }
    S_free_blocks(roster->blocks);
    markdown_core_free(roster->records);
    roster->records = fresh.records;
    roster->count = fresh.count;
    roster->capacity = fresh.capacity;
    roster->blocks = fresh.blocks;
    roster->block_used = fresh.block_used;
    roster->block_size = fresh.block_size;
    roster->bytes = fresh.bytes;
    roster->dead = 0;
    return true;
}
