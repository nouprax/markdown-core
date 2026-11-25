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

/* THE STORAGE of labels and records. Each lives until the roster is freed
 * or a compaction copies it, so they are carved one after another from
 * blocks the roster owns. A block starts small, so a parse that declares a
 * few labels costs one small allocation, and doubles up to a cap, so a
 * document with many definitions pays for them in a few allocations rather
 * than one each. A label larger than the next block gets a block of its own
 * size. */
struct markdown_core_roster_block {
    union {
        markdown_core_roster_block *next;
        long double alignment;
        int64_t integer_alignment;
    } head;
};

#define ROSTER_BLOCK_MIN_BYTES ((size_t)1024)
#define ROSTER_BLOCK_MAX_BYTES ((size_t)16 * 1024)

/* The bytes `size` bytes take in their block. */
static inline size_t S_aligned(size_t size) {
    const size_t align = sizeof(markdown_core_roster_block);
    return (size + align - 1) / align * align;
}

/* The bytes a label of a key of `length` bytes takes. */
static inline size_t S_label_size(bufsize_t length) {
    return S_aligned(sizeof(markdown_core_roster_label) + (size_t)length + 1);
}

#define S_RECORD_SIZE S_aligned(sizeof(markdown_core_roster_record))

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

/* THE LABELS IN ORDER: the declared labels by key. */
static bool S_labels_before(const void *a, const void *b) { return markdown_core_key_before(a, b); }

static const markdown_core_tree_ops S_labels_ops = {
    sizeof(markdown_core_key_entry), NULL, S_labels_before, NULL, NULL, NULL};

static inline markdown_core_key_entry S_label_entry(const markdown_core_roster_label *label) {
    return (markdown_core_key_entry){markdown_core_key_head(label->key, label->key_len), label->key, label->key_len, 0,
                                     label};
}

/* The label, declared now, takes its place in order. False when storage
 * could not be had. */
static bool S_order_in(markdown_core_roster *roster, const markdown_core_roster_label *label) {
    const markdown_core_key_entry entry = S_label_entry(label);
    return markdown_core_tree_insert(&roster->labels, markdown_core_tree_seek(&roster->labels, &entry), &entry,
                                     false) != NULL;
}

/* The label, declared no more, leaves it. */
static void S_order_out(markdown_core_roster *roster, const markdown_core_roster_label *label) {
    const markdown_core_key_entry entry = S_label_entry(label);
    const size_t rank = markdown_core_tree_seek(&roster->labels, &entry);
    assert(rank < roster->labels.count &&
           ((const markdown_core_key_entry *)markdown_core_tree_peek(&roster->labels, rank))->value == label);
    markdown_core_tree_erase(&roster->labels, rank, 1);
}

size_t markdown_core_roster_labels_of(const markdown_core_roster *roster, markdown_core_roster_kind kind,
                                      size_t *count) {
    const unsigned char bytes[2] = {(unsigned char)kind, (unsigned char)(kind + 1)};
    const markdown_core_key_entry from = {markdown_core_key_head(bytes, 1), bytes, 1, 0, NULL};
    const markdown_core_key_entry past = {markdown_core_key_head(bytes + 1, 1), bytes + 1, 1, 0, NULL};
    const size_t first = markdown_core_tree_seek(&roster->labels, &from);
    *count = markdown_core_tree_seek(&roster->labels, &past) - first;
    return first;
}

const markdown_core_roster_label *markdown_core_roster_label_at(const markdown_core_roster *roster, size_t rank) {
    return ((const markdown_core_key_entry *)markdown_core_tree_peek(&roster->labels, rank))->value;
}

bool markdown_core_roster_init(markdown_core_roster *roster, size_t kinds) {
    *roster = (markdown_core_roster){0};
    roster->use_shelf = -1;
    roster->labels = markdown_core_tree_make(&S_labels_ops);
    markdown_core_strbuf_init(&roster->key, 0);
    roster->made = markdown_core_alloc(kinds, sizeof(*roster->made));
    if (!roster->made) {
        return false;
    }
    roster->kinds = kinds;
    return true;
}

void markdown_core_roster_free(markdown_core_roster *roster) {
    S_free_blocks(roster->blocks);
    markdown_core_key_index_free(&roster->index);
    markdown_core_tree_free(&roster->labels);
    markdown_core_strbuf_free(&roster->key);
    for (size_t i = 0; i < roster->shelf_count; i++) {
        markdown_core_tree_free(&roster->shelves[i]->places);
        markdown_core_free(roster->shelves[i]);
    }
    markdown_core_free(roster->shelves);
    markdown_core_free(roster->made);
    markdown_core_array_free(&roster->where);
    markdown_core_array_free(&roster->journal);
    *roster = (markdown_core_roster){0};
}

/* THE LABEL'S LIST. Two declarations of a label in the order of where they
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

/* THE JOURNAL starts empty as a transaction begins after the last one
 * flushed: at an edit's reopen, or at its first change. It takes the label
 * the transaction is about to change, once, with whether it is declared
 * before the change. False when storage could not be had. */
static inline void S_begin(markdown_core_roster *roster) {
    if (roster->closed) {
        roster->closed = false;
        roster->epoch++;
        roster->journal.count = 0;
    }
}

static bool S_touch(markdown_core_roster *roster, markdown_core_roster_label *label) {
    S_begin(roster);
    if (label->listed == roster->epoch + 1) {
        return true;
    }
    if (!markdown_core_array_push(&roster->journal, label)) {
        return false;
    }
    label->listed = roster->epoch + 1;
    label->was = label->first != NULL;
    return true;
}

/* The declaration joins its label's list. Its search begins at the list's
 * hint, the declaration the list last took or let go near, and walks
 * whichever way its place lies: a parse declares a label's declarations
 * mostly in the order of the text, and an edit's reading its own past
 * those before its rest point, so the walk is short. */
static void S_link(markdown_core_roster *roster, markdown_core_roster_record *record) {
    markdown_core_roster_label *label = record->label;
    record->linked = true;
    record->after = NULL;
    record->hint = NULL;
    label->count++;
    markdown_core_roster_record *first = label->first;
    if (!first) {
        record->before = record->hint = record;
        label->first = record;
        if (!label->uses) {
            roster->dead -= S_label_size(label->key_len);
        }
        return;
    }
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
    if (!at) {
        record->after = first;
        record->before = S_last(first);
        first->before = record;
        first->hint = NULL;
        label->first = first = record;
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

/* The declaration leaves its label's list; a label none declares or uses
 * any more is dead bytes until a compaction. */
static void S_unlink(markdown_core_roster *roster, markdown_core_roster_record *record) {
    markdown_core_roster_label *label = record->label;
    assert(record->linked && label->count);
    record->linked = false;
    markdown_core_roster_record *first = label->first;
    if (!--label->count) {
        label->first = NULL;
        S_order_out(roster, label);
        if (!label->uses) {
            roster->dead += S_label_size(label->key_len);
        }
        return;
    }
    /* The next search of the list is likely near the one that went. */
    markdown_core_roster_record *prior = S_prior(first, record);
    first->hint = prior ? prior : record->after;
    if (record == first) {
        markdown_core_roster_record *next = first->after;
        next->before = S_last(first);
        next->hint = first->hint;
        label->first = next;
        return;
    }
    prior->after = record->after;
    if (record->after) {
        record->after->before = prior;
    } else {
        first->before = prior;
    }
}

/* The use joins its label's uses, first, and leaves them (THE USES). */
static void S_use_link(markdown_core_roster *roster, markdown_core_roster_record *record) {
    markdown_core_roster_label *label = record->label;
    if (!label->uses && !label->first) {
        roster->dead -= S_label_size(label->key_len);
    }
    record->linked = true;
    record->before = NULL;
    record->after = label->uses;
    if (label->uses) {
        label->uses->before = record;
    }
    label->uses = record;
}

static void S_use_unlink(markdown_core_roster *roster, markdown_core_roster_record *record) {
    markdown_core_roster_label *label = record->label;
    assert(record->linked);
    record->linked = false;
    if (record->before) {
        record->before->after = record->after;
    } else {
        label->uses = record->after;
    }
    if (record->after) {
        record->after->before = record->before;
    }
    if (!label->uses && !label->first) {
        roster->dead += S_label_size(label->key_len);
    }
}

/* The label of the key the roster assembled, made when there is none, or
 * NULL when storage could not be had. */
static markdown_core_roster_label *S_label(markdown_core_roster *roster) {
    const unsigned char *key = (const unsigned char *)roster->key.ptr;
    const bufsize_t length = roster->key.size;
    markdown_core_key_index_slot *slot = S_slot(roster, key, length);
    if (!slot) {
        return NULL;
    }
    if (slot->key) {
        return (markdown_core_roster_label *)slot->value;
    }
    const size_t size = S_label_size(length);
    markdown_core_roster_label *label = S_carve(roster, size);
    if (!label) {
        return NULL;
    }
    *label = (markdown_core_roster_label){0};
    memcpy(label->key, key, (size_t)length);
    label->key[length] = '\0';
    label->key_len = length;
    markdown_core_key_index_commit(&roster->index, slot, label->key);
    slot->value = (size_t)label;
    roster->bytes += size;
    /* None declares it yet (S_link). */
    roster->dead += size;
    return label;
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

static inline void S_assemble(markdown_core_roster *roster, markdown_core_roster_kind kind, const unsigned char *label,
                              bufsize_t length) {
    markdown_core_strbuf_clear(&roster->key);
    markdown_core_strbuf_putc(&roster->key, (int)kind);
    markdown_core_strbuf_put(&roster->key, label, length);
}

bool markdown_core_roster_declare(markdown_core_roster *roster, markdown_core_roster_kind kind,
                                  const unsigned char *label, bufsize_t length, uint32_t start,
                                  const markdown_core_node *node) {
    if (length < 0 || (!length && !node)) {
        return true;
    }
    markdown_core_roster_label *named = NULL;
    if (length) {
        S_assemble(roster, kind, label, length);
        if (roster->key.oom || !(named = S_label(roster)) || !S_touch(roster, named)) {
            return false;
        }
    }
    /* Each declaration is placed as it is declared, in its shelf. */
    const int shelf = S_shelf(roster, kind, node ? node->kind : (uint16_t)MARKDOWN_CORE_NODE_NONE);
    markdown_core_roster_record *record = shelf < 0 ? NULL : S_carve(roster, S_RECORD_SIZE);
    if (!record) {
        return false;
    }
    *record = (markdown_core_roster_record){
        .label = named, .node = node, .id = MARKDOWN_CORE_ROSTER_UNPLACED, .shelf = (uint8_t)shelf};
    roster->bytes += S_RECORD_SIZE;
    markdown_core_tree *places = &roster->shelves[shelf]->places;
    /* A label declared now takes its place in order (THE LABELS IN ORDER). */
    const bool fresh = named && !named->first;
    const bool numbered = S_number(roster, record), ordered = numbered && (!fresh || S_order_in(roster, named));
    if (!ordered ||
        !S_places_insert(places, S_front_after(places, start), (markdown_core_roster_place){start, record}, true)) {
        /* The record's bytes stay carved, dead, until a compaction. */
        if (ordered && fresh) {
            S_order_out(roster, named);
        }
        if (numbered) {
            S_unnumber(roster, record);
        }
        roster->dead += S_RECORD_SIZE;
        return false;
    }
    roster->made[kind]++;
    if (named) {
        S_link(roster, record);
    }
    return true;
}

/* The first declaration of the label `slot` holds, of none, that stands and
 * a lookup sees. */
static inline markdown_core_roster_record *S_seen(const markdown_core_roster *roster,
                                                  const markdown_core_key_index_slot *slot) {
    const markdown_core_roster_label *label = slot ? (const markdown_core_roster_label *)slot->value : NULL;
    return label && markdown_core_roster_declared(roster, label) ? label->first : NULL;
}

/* Whether the ANCHOR label `slot` holds is of the name `probe`. */
static bool S_same_name(const void *probe, const markdown_core_key_index_slot *slot) {
    return slot->key[0] == (unsigned char)MARKDOWN_CORE_ROSTER_ANCHOR &&
           markdown_core_pieces_are(probe, slot->key + 1, slot->key_len - 1);
}

const markdown_core_roster_record *markdown_core_roster_find_name(markdown_core_roster *roster,
                                                                  const markdown_core_pieces *name,
                                                                  markdown_core_measure measure, bool *failed) {
    (void)failed;
    if (!name->size || !roster->made[MARKDOWN_CORE_ROSTER_ANCHOR]) {
        return NULL;
    }
    const unsigned char kind = (unsigned char)MARKDOWN_CORE_ROSTER_ANCHOR;
    const uint64_t hash =
        markdown_core_key_index_hash(markdown_core_measure_join(markdown_core_measure_of(&kind, 1), measure));
    return S_seen(roster, markdown_core_key_index_seek(&roster->index, hash, name->size + 1, S_same_name, name));
}

static bool S_key(markdown_core_roster *roster, markdown_core_roster_kind kind, const markdown_core_chunk *label,
                  bool folds, bool *failed);

const markdown_core_roster_record *markdown_core_roster_find(markdown_core_roster *roster,
                                                             markdown_core_roster_kind kind, const unsigned char *label,
                                                             bufsize_t length, bool *failed) {
    const markdown_core_chunk text = {(unsigned char *)label, length, 0};
    if (!roster->made[kind] || !S_key(roster, kind, &text, false, failed)) {
        return NULL;
    }
    return S_seen(roster, S_index_find(roster, (const unsigned char *)roster->key.ptr, roster->key.size));
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
    if (length <= 0 || length == INT32_MAX || !roster->made[kind]) {
        return NULL;
    }
    const S_probe probe = {kind, label, length};
    const markdown_core_key_index_slot *slot = markdown_core_key_index_seek(
        &roster->index, S_label_hash(kind, label, length), length + 1, S_same_label, &probe);
    return slot ? ((const markdown_core_roster_label *)slot->value)->first : NULL;
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

/* The key of `label` of `kind`, as written, assembled: in the
 * reference-label normal form when the kind `folds`. False when it names
 * nothing (markdown_core_roster_use) or, with `*failed` set, when storage
 * could not be had. */
static bool S_key(markdown_core_roster *roster, markdown_core_roster_kind kind, const markdown_core_chunk *label,
                  bool folds, bool *failed) {
    if (folds ? label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH : label->len <= 0) {
        return false;
    }
    markdown_core_strbuf_clear(&roster->key);
    markdown_core_strbuf_putc(&roster->key, (int)kind);
    if (folds) {
        markdown_core_utf8proc_normalize_label(&roster->key, label->data, label->len);
    } else {
        markdown_core_strbuf_put(&roster->key, label->data, label->len);
    }
    if (roster->key.oom) {
        *failed = true;
        return false;
    }
    return roster->key.size > 1;
}

const markdown_core_roster_record *markdown_core_roster_find_label(markdown_core_roster *roster,
                                                                   markdown_core_roster_kind kind,
                                                                   const markdown_core_chunk *label, bool *failed) {
    if (!roster->made[kind] || !S_key(roster, kind, label, true, failed)) {
        return NULL;
    }
    return S_seen(roster, S_index_find(roster, (const unsigned char *)roster->key.ptr, roster->key.size));
}

/* The uses' shelf, made at the first use, or NULL when storage could not be
 * had. */
static inline markdown_core_tree *S_uses(markdown_core_roster *roster) {
    if (roster->use_shelf < 0) {
        roster->use_shelf = S_shelf(roster, 0, (uint16_t)MARKDOWN_CORE_NODE_NONE);
    }
    return roster->use_shelf < 0 ? NULL : &roster->shelves[roster->use_shelf]->places;
}

static inline const markdown_core_tree *S_uses_of(const markdown_core_roster *roster) {
    return roster->use_shelf < 0 ? NULL : &roster->shelves[roster->use_shelf]->places;
}

const markdown_core_roster_record *markdown_core_roster_use(markdown_core_roster *roster,
                                                            markdown_core_roster_kind kind,
                                                            const markdown_core_chunk *label, bool folds,
                                                            uint32_t start, const markdown_core_node *node,
                                                            markdown_core_roster_placing *placed, bool *failed) {
    *placed = MARKDOWN_CORE_ROSTER_UNUSED;
    if (!S_key(roster, kind, label, folds, failed)) {
        return NULL;
    }
    markdown_core_roster_label *named = S_label(roster);
    markdown_core_tree *places = named ? S_uses(roster) : NULL;
    markdown_core_roster_record *record = places ? S_carve(roster, S_RECORD_SIZE) : NULL;
    if (!record) {
        *failed = true;
        return NULL;
    }
    *record = (markdown_core_roster_record){
        .label = named, .node = node, .id = MARKDOWN_CORE_ROSTER_UNPLACED, .shelf = (uint8_t)roster->use_shelf};
    roster->bytes += S_RECORD_SIZE;
    /* A completion looks up in the order of the text, mostly, and the
     * roots complete in the order of where they start. */
    const size_t front = places->front;
    const size_t at = front && S_places_at(places, front - 1)->start > start ? S_front_after(places, start) : front;
    if (!S_number(roster, record)) {
        roster->dead += S_RECORD_SIZE;
        *failed = true;
        return NULL;
    }
    const markdown_core_roster_place place = {start, record};
    if (at == places->count) {
        markdown_core_roster_place *pushed = S_places_push(places);
        if (pushed) {
            *pushed = place;
            *markdown_core_array_write(&roster->where, record->id) =
                (markdown_core_tree_handle){places->last, places->last->count - 1};
        }
        record->linked = pushed != NULL;
    } else {
        record->linked = S_places_insert(places, at, place, true) != NULL;
    }
    if (!record->linked) {
        S_unnumber(roster, record);
        roster->dead += S_RECORD_SIZE;
        *failed = true;
        return NULL;
    }
    *placed = at == front ? MARKDOWN_CORE_ROSTER_APPENDED : MARKDOWN_CORE_ROSTER_AMONG;
    S_use_link(roster, record);
    record->found = markdown_core_roster_declared(roster, named);
    return record->found ? named->first : NULL;
}

size_t markdown_core_roster_uses_from(const markdown_core_roster *roster, size_t place) {
    const markdown_core_tree *uses = S_uses_of(roster);
    if (!uses) {
        return 0;
    }
    if (uses->front < uses->count &&
        ((const markdown_core_roster_place *)markdown_core_tree_peek(uses, uses->front))->start < place) {
        return markdown_core_tree_from(uses, place, uses->front, uses->count);
    }
    return markdown_core_tree_from(uses, place, 0, uses->front);
}

size_t markdown_core_roster_uses_front(const markdown_core_roster *roster) {
    const markdown_core_tree *uses = S_uses_of(roster);
    return uses ? uses->front : 0;
}

size_t markdown_core_roster_uses_back(const markdown_core_roster *roster) {
    const markdown_core_tree *uses = S_uses_of(roster);
    return uses ? uses->count - uses->front : 0;
}

/* The records of `places` from rank `at`, `count` of them, go with the
 * text they were in. False when the journal could not take a label. */
static bool S_drop(markdown_core_roster *roster, markdown_core_tree *places, size_t at, size_t count) {
    bool ok = true;
    for (size_t k = 0; k < count; k++) {
        markdown_core_roster_record *record = S_places_at(places, at + k)->record;
        if (record->shelf == roster->use_shelf) {
            S_use_unlink(roster, record);
        } else {
            if (record->label) {
                ok = S_touch(roster, record->label) && ok;
            }
            if (record->linked) {
                S_unlink(roster, record);
            }
        }
        record->gone = true;
        roster->dead += S_RECORD_SIZE;
    }
    for (size_t k = 0; k < count; k++) {
        S_unnumber(roster, S_places_at(places, at + k)->record);
    }
    markdown_core_tree_erase(places, at, count);
    return ok;
}

void markdown_core_roster_unuse(markdown_core_roster *roster, size_t at, size_t count) {
    if (count) {
        (void)S_drop(roster, S_uses(roster), at, count);
    }
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

bool markdown_core_roster_reopen(markdown_core_roster *roster, uint32_t start, uint32_t uses) {
    /* An edit begins a transaction (THE JOURNAL). */
    S_begin(roster);
    if (roster->dead > roster->bytes - roster->dead + 4096 && !markdown_core_roster_compact(roster)) {
        return false;
    }
    for (size_t i = 0; i < roster->shelf_count; i++) {
        markdown_core_tree *places = &roster->shelves[i]->places;
        const uint32_t at = (int)i == roster->use_shelf ? uses : start;
        const bool back = places->front < places->count && S_places_at(places, places->front)->start < at;
        markdown_core_tree_focus(places, back ? markdown_core_tree_from(places, at, places->front, places->count)
                                              : markdown_core_tree_from(places, at, 0, places->front));
    }
    return true;
}

bool markdown_core_roster_splice(markdown_core_roster *roster, size_t place, int64_t delta, size_t held) {
    bool ok = true;
    for (size_t i = 0; i < roster->shelf_count; i++) {
        markdown_core_tree *places = &roster->shelves[i]->places;
        const size_t front = places->front;
        const size_t kept = (int)i == roster->use_shelf ? held : 0;
        const size_t gone = markdown_core_tree_from(places, place, front, places->count) - front;
        if (gone > kept) {
            ok = S_drop(roster, places, front + kept, gone - kept) && ok;
        }
        markdown_core_tree_shift(places, front, delta);
    }
    return ok;
}

/* An entry of the labels in order, or a copy of one, names the label's
 * copy (THE LABELS IN ORDER). */
static void S_label_moved(void *entry, void *context) {
    (void)context;
    markdown_core_key_entry *held = entry;
    const markdown_core_roster_label *copy = ((const markdown_core_roster_label *)held->value)->copy;
    held->key = copy->key;
    held->value = copy;
}

/* The copy a compaction made of the old record, which forwards to it in
 * `hint`, or NULL. */
static inline markdown_core_roster_record *S_forward(const markdown_core_roster_record *old) {
    return old ? old->hint : NULL;
}

bool markdown_core_roster_compact(markdown_core_roster *roster) {
    /* The declarations that stand are the shelves' places; the labels to
     * keep are those they declare. */
    size_t size = 0, labels = 0;
    for (size_t k = 0; k < roster->shelf_count; k++) {
        markdown_core_tree *places = &roster->shelves[k]->places;
        for (size_t i = 0; i < places->count; i++) {
            markdown_core_roster_label *label = S_places_at(places, i)->record->label;
            size += S_RECORD_SIZE;
            if (label && !label->copy) {
                label->copy = label;
                size += S_label_size(label->key_len);
                labels++;
            }
        }
    }
    /* All the storage first, so that a failure leaves the roster as it
     * was. */
    markdown_core_key_index index = {0};
    markdown_core_roster_block *block =
        size ? (markdown_core_roster_block *)markdown_core_realloc(NULL, sizeof(markdown_core_roster_block) + size)
             : NULL;
    if ((size && !block) || !markdown_core_key_index_init(&index, labels)) {
        markdown_core_free(block);
        for (size_t k = 0; k < roster->shelf_count; k++) {
            markdown_core_tree *places = &roster->shelves[k]->places;
            for (size_t i = 0; i < places->count; i++) {
                markdown_core_roster_label *label = S_places_at(places, i)->record->label;
                if (label) {
                    label->copy = NULL;
                }
            }
        }
        return false;
    }
    unsigned char *at = block ? (unsigned char *)(block + 1) : NULL;
    /* Each is copied, and the old forwards to its copy, which the lists and
     * the places then name in its place. */
    for (size_t k = 0; k < roster->shelf_count; k++) {
        markdown_core_tree *places = &roster->shelves[k]->places;
        for (size_t i = 0; i < places->count; i++) {
            markdown_core_roster_place *place = S_places_at(places, i);
            markdown_core_roster_record *record = place->record;
            markdown_core_roster_label *label = record->label;
            if (label && label->copy == label) {
                markdown_core_roster_label *copy = (markdown_core_roster_label *)(void *)at;
                memcpy(copy, label, sizeof(*label) + (size_t)label->key_len + 1);
                copy->copy = NULL;
                copy->listed = 0;
                label->copy = copy;
                at += S_label_size(label->key_len);
                markdown_core_key_index_slot *slot = markdown_core_key_index_entry_hashed(
                    &index, S_hash(copy->key, copy->key_len), copy->key, copy->key_len);
                assert(slot && !slot->key);
                markdown_core_key_index_commit(&index, slot, copy->key);
                slot->value = (size_t)copy;
            }
            markdown_core_roster_record *copy = (markdown_core_roster_record *)(void *)at;
            *copy = *record;
            copy->label = label ? label->copy : NULL;
            if (label && label->first == record) {
                label->copy->first = copy;
            }
            if (label && label->uses == record) {
                label->copy->uses = copy;
            }
            record->hint = copy;
            place->record = copy;
            at += S_RECORD_SIZE;
        }
    }
    for (size_t k = 0; k < roster->shelf_count; k++) {
        markdown_core_tree *places = &roster->shelves[k]->places;
        for (size_t i = 0; i < places->count; i++) {
            markdown_core_roster_record *copy = S_places_at(places, i)->record;
            copy->before = S_forward(copy->before);
            copy->after = S_forward(copy->after);
            copy->hint = S_forward(copy->hint);
        }
    }
    /* The labels in order name the copies, which keep their keys. */
    markdown_core_tree_rewrite(&roster->labels, S_label_moved, NULL);
    S_free_blocks(roster->blocks);
    markdown_core_key_index_free(&roster->index);
    roster->index = index;
    if (block) {
        block->head.next = NULL;
    }
    roster->blocks = block;
    roster->block_used = roster->block_size = size;
    roster->bytes = size;
    roster->dead = 0;
    /* The journal named the old labels: it starts empty. */
    roster->journal.count = 0;
    roster->epoch++;
    roster->closed = false;
    return true;
}
