#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "alloc.h"
#include "labels.h"
#include "parser.h"
#include "utf8.h"
#include "pieces.h"

/* THE INDEX'S HASH of a key, its space then its name: an ANCHOR's is its
 * measure's, as a name is filed (map.h, KEYS UNDER A HASH THEIR USER GIVES),
 * so that a heading's name, kept in pieces, is looked up whole
 * (markdown_core_labels_find_name); any other space's is its bounds' read in
 * parts (markdown_core_key_stream), so that a reader looks up a space and a
 * name without joining them (markdown_core_labels_seek). */
static inline uint64_t S_name_hash(markdown_core_label_space space, const unsigned char *name, bufsize_t length) {
    const unsigned char head = (unsigned char)space;
    markdown_core_key_stream stream;
    markdown_core_key_stream_start(&stream);
    markdown_core_key_stream_add(&stream, &head, 1);
    markdown_core_key_stream_add(&stream, name, (size_t)length);
    return markdown_core_key_stream_end(&stream);
}

static inline uint64_t S_hash(const unsigned char *key, bufsize_t length) {
    return key[0] == (unsigned char)MARKDOWN_CORE_LABEL_ANCHOR
               ? markdown_core_key_index_hash(markdown_core_measure_of(key, (size_t)length))
               : S_name_hash(key[0], key + 1, length - 1);
}

/* THE SEQUENCES' TREES: a place moves with an edit before it, and tells
 * its mention where it is. */
static size_t S_place_start(const void *place) { return ((const markdown_core_mention_place *)place)->start; }

static void S_place_shift(void *place, int64_t delta) {
    markdown_core_mention_place *at = place;
    at->start = (uint32_t)((int64_t)at->start + delta);
}

static void S_place_moved(const markdown_core_tree *tree, void *place, markdown_core_tree_leaf *leaf, uint32_t slot) {
    (void)tree;
    ((markdown_core_mention_place *)place)->mention->at = (markdown_core_tree_handle){leaf, slot};
}

static const markdown_core_tree_ops S_places_ops = {
    sizeof(markdown_core_mention_place), S_place_start, NULL, S_place_shift, S_place_moved, NULL};

MARKDOWN_CORE_TREE_OPS(S_places, markdown_core_mention_place)

static inline markdown_core_tree *S_places(const markdown_core_labels *labels, const markdown_core_mention *mention) {
    return &labels->sequences[mention->sequence]->places;
}

/* The rank of a mention in its sequence. */
static inline size_t S_rank(const markdown_core_labels *labels, const markdown_core_mention *mention) {
    return markdown_core_tree_rank(S_places(labels, mention), mention->at);
}

uint32_t markdown_core_labels_located(const markdown_core_labels *labels, const markdown_core_mention *mention,
                                      bool *back) {
    const markdown_core_tree *places = S_places(labels, mention);
    markdown_core_mention_place place;
    markdown_core_tree_read(places, mention->at, &place);
    *back = markdown_core_tree_rank(places, mention->at) >= places->front;
    return place.start;
}

/* THE NAMES IN ORDER: the defined labels by key. */
static bool S_names_before(const void *a, const void *b) { return markdown_core_key_before(a, b); }

static const markdown_core_tree_ops S_names_ops = {
    sizeof(markdown_core_key_entry), NULL, S_names_before, NULL, NULL, NULL};

static inline markdown_core_key_entry S_ordered(const markdown_core_label *label) {
    return (markdown_core_key_entry){markdown_core_key_head(label->key, label->key_len), label->key, label->key_len, 0,
                                     label};
}

/* The label, defined now, takes its place in order. False when storage
 * could not be had. */
static bool S_order_in(markdown_core_labels *labels, const markdown_core_label *label) {
    const markdown_core_key_entry mention = S_ordered(label);
    return markdown_core_tree_insert(&labels->names, markdown_core_tree_seek(&labels->names, &mention), &mention,
                                     false) != NULL;
}

/* The label, defined no more, leaves it. */
static void S_order_out(markdown_core_labels *labels, const markdown_core_label *label) {
    const markdown_core_key_entry mention = S_ordered(label);
    const size_t rank = markdown_core_tree_seek(&labels->names, &mention);
    assert(rank < labels->names.count &&
           ((const markdown_core_key_entry *)markdown_core_tree_peek(&labels->names, rank))->value == label);
    markdown_core_tree_erase(&labels->names, rank, 1);
}

size_t markdown_core_labels_names_of(const markdown_core_labels *labels, markdown_core_label_space space,
                                     size_t *count) {
    const unsigned char bounds[2] = {(unsigned char)space, (unsigned char)(space + 1)};
    const markdown_core_key_entry from = {markdown_core_key_head(bounds, 1), bounds, 1, 0, NULL};
    const markdown_core_key_entry past = {markdown_core_key_head(bounds + 1, 1), bounds + 1, 1, 0, NULL};
    const size_t first = markdown_core_tree_seek(&labels->names, &from);
    *count = markdown_core_tree_seek(&labels->names, &past) - first;
    return first;
}

void markdown_core_labels_init(markdown_core_labels *labels) {
    *labels = (markdown_core_labels){0};
    labels->uses = -1;
    labels->names = markdown_core_tree_make(&S_names_ops);
    markdown_core_strbuf_init(&labels->key, 0);
}

void markdown_core_labels_free(markdown_core_labels *labels) {
    /* Every mention is in a sequence. */
    for (size_t i = 0; i < labels->sequence_count; i++) {
        markdown_core_tree *places = &labels->sequences[i]->places;
        for (size_t k = 0; k < places->count; k++) {
            markdown_core_free(((const markdown_core_mention_place *)markdown_core_tree_peek(places, k))->mention);
        }
        markdown_core_tree_free(places);
        markdown_core_free(labels->sequences[i]);
    }
    markdown_core_free(labels->sequences);
    /* Every label is in the index. */
    for (size_t i = 0; i < labels->index.capacity; i++) {
        if (labels->index.slots[i].key) {
            markdown_core_free((void *)labels->index.slots[i].value);
        }
    }
    markdown_core_key_index_free(&labels->index);
    markdown_core_tree_free(&labels->names);
    markdown_core_strbuf_free(&labels->key);
    markdown_core_array_free(&labels->altered);
    markdown_core_array_free(&labels->left);
    *labels = (markdown_core_labels){0};
}

/* THE LABEL'S DEFINITIONS. Two definitions in the order of their
 * locations, the front's before the back's: of one sequence by their ranks,
 * so that a definition after another located where it is goes after it, and
 * of two by their parts and locations (THE SEQUENCES). */
static inline bool S_precedes(const markdown_core_labels *labels, const markdown_core_mention *one,
                              const markdown_core_mention *other) {
    if (one->sequence == other->sequence) {
        return S_rank(labels, one) < S_rank(labels, other);
    }
    bool one_back, other_back;
    const uint32_t one_start = markdown_core_labels_located(labels, one, &one_back);
    const uint32_t other_start = markdown_core_labels_located(labels, other, &other_back);
    if (one_back != other_back) {
        return other_back;
    }
    return one_start != other_start ? one_start < other_start : one->sequence < other->sequence;
}

/* The definition before `at` in the label's list, or NULL. */
static inline markdown_core_mention *S_prior(const markdown_core_label *label, markdown_core_mention *at) {
    return at == label->first ? NULL : at->before;
}

/* A label that holds no mention and no transaction lists goes. */
static void S_vacate(markdown_core_labels *labels, markdown_core_label *label) {
    if (label->first || label->uses || label->listed) {
        return;
    }
    markdown_core_key_index_slot *slot = markdown_core_key_index_find_hashed(
        &labels->index, S_hash(label->key, label->key_len), label->key, label->key_len);
    assert(slot && (markdown_core_label *)slot->value == label);
    markdown_core_key_index_remove(&labels->index, slot);
    markdown_core_free(label);
}

/* THE ALTERED LABELS start empty as a transaction begins after the last one
 * closed: at an edit's reopen, or at its first change, the labels the last
 * one left with no mention going. */
static void S_renew(markdown_core_labels *labels) {
    if (!labels->closed) {
        return;
    }
    labels->closed = false;
    for (size_t i = 0; i < labels->altered.count; i++) {
        markdown_core_label *label = labels->altered.values[i];
        label->listed = false;
        S_vacate(labels, label);
    }
    labels->altered.count = 0;
}

/* The label the transaction is about to change listed, once, with whether
 * it has a definition before the change. False when storage could not be
 * had. */
static bool S_touch(markdown_core_labels *labels, markdown_core_label *label) {
    S_renew(labels);
    if (label->listed) {
        return true;
    }
    if (!markdown_core_array_push(&labels->altered, label)) {
        return false;
    }
    label->listed = true;
    label->was = label->first != NULL;
    return true;
}

/* The definition joins its label's list. The search begins where the list
 * was last searched, and walks whichever way the place lies: a parse
 * defines a label's definitions mostly in the order of the text, and an
 * edit's reading its own past those before its rest point, so the walk is
 * short. */
static void S_link(markdown_core_labels *labels, markdown_core_mention *mention) {
    markdown_core_label *label = mention->label;
    mention->after = NULL;
    label->count++;
    markdown_core_mention *first = label->first;
    if (!first) {
        mention->before = mention;
        label->first = label->near = mention;
        return;
    }
    markdown_core_mention *at = label->near ? label->near : first->before;
    if (!S_precedes(labels, mention, at)) {
        while (at->after && !S_precedes(labels, mention, at->after)) {
            at = at->after;
        }
    } else {
        while (at && S_precedes(labels, mention, at)) {
            at = S_prior(label, at);
        }
    }
    if (!at) {
        mention->after = first;
        mention->before = first->before;
        first->before = mention;
        label->first = mention;
    } else {
        mention->before = at;
        mention->after = at->after;
        if (at->after) {
            at->after->before = mention;
        } else {
            first->before = mention;
        }
        at->after = mention;
    }
    label->near = mention;
}

/* The definition leaves its label's list. */
static void S_unlink(markdown_core_labels *labels, markdown_core_mention *mention) {
    markdown_core_label *label = mention->label;
    assert(label->count);
    if (!--label->count) {
        label->first = label->near = NULL;
        S_order_out(labels, label);
        return;
    }
    /* The next search of the list is likely near the one that went. */
    markdown_core_mention *prior = S_prior(label, mention);
    label->near = prior ? prior : mention->after;
    if (mention == label->first) {
        markdown_core_mention *next = mention->after;
        next->before = mention->before;
        label->first = next;
        return;
    }
    prior->after = mention->after;
    if (mention->after) {
        mention->after->before = prior;
    } else {
        label->first->before = prior;
    }
}

/* The reference joins its label's references, first, and leaves them. */
static void S_use_link(markdown_core_mention *mention) {
    markdown_core_label *label = mention->label;
    mention->before = NULL;
    mention->after = label->uses;
    if (label->uses) {
        label->uses->before = mention;
    }
    label->uses = mention;
}

static void S_use_unlink(markdown_core_mention *mention) {
    markdown_core_label *label = mention->label;
    if (mention->before) {
        mention->before->after = mention->after;
    } else {
        label->uses = mention->after;
    }
    if (mention->after) {
        mention->after->before = mention->before;
    }
}

/* The label of the key assembled, made when there is none, or NULL when
 * storage could not be had. */
static markdown_core_label *S_label(markdown_core_labels *labels) {
    const unsigned char *key = (const unsigned char *)labels->key.ptr;
    const bufsize_t length = labels->key.size;
    markdown_core_key_index_slot *slot =
        markdown_core_key_index_entry_hashed(&labels->index, S_hash(key, length), key, length);
    if (!slot) {
        return NULL;
    }
    if (slot->key) {
        return (markdown_core_label *)slot->value;
    }
    markdown_core_label *label = markdown_core_realloc(NULL, sizeof(*label) + (size_t)length + 1);
    if (!label) {
        return NULL;
    }
    *label = (markdown_core_label){0};
    memcpy(label->key, key, (size_t)length);
    label->key[length] = '\0';
    label->key_len = length;
    markdown_core_key_index_commit(&labels->index, slot, label->key);
    slot->value = (size_t)label;
    return label;
}

/* The sequence of `space` that nodes of kind `node` define in, made when
 * there is none, as its number, or -1 when storage could not be had. */
static int S_sequence(markdown_core_labels *labels, markdown_core_label_space space, uint16_t node) {
    for (size_t i = 0; i < labels->sequence_count; i++) {
        if (labels->sequences[i]->space == space && labels->sequences[i]->node == node) {
            return (int)i;
        }
    }
    if (labels->sequence_count == MARKDOWN_CORE_SEQUENCES) {
        return -1;
    }
    markdown_core_sequence **sequences =
        markdown_core_realloc(labels->sequences, (labels->sequence_count + 1) * sizeof(*sequences));
    if (!sequences) {
        return -1;
    }
    labels->sequences = sequences;
    markdown_core_sequence *sequence = markdown_core_alloc(1, sizeof(*sequence));
    if (!sequence) {
        return -1;
    }
    *sequence = (markdown_core_sequence){markdown_core_tree_make(&S_places_ops), space, node};
    sequences[labels->sequence_count] = sequence;
    return (int)labels->sequence_count++;
}

const markdown_core_sequence *markdown_core_labels_sequence(const markdown_core_labels *labels,
                                                            markdown_core_label_space space, uint16_t node) {
    for (size_t i = 0; i < labels->sequence_count; i++) {
        if (labels->sequences[i]->space == space && labels->sequences[i]->node == node) {
            return labels->sequences[i];
        }
    }
    return NULL;
}

/* `owner` owns `mention`, first of its mentions. */
static inline void S_own(markdown_core_mention *mention, markdown_core_node *owner) {
    mention->owner = owner;
    mention->owned = owner->mentions;
    owner->mentions = mention;
}

/* The mention leaves its owner's mentions. */
static void S_disown(markdown_core_mention *mention) {
    markdown_core_mention **link = &mention->owner->mentions;
    while (*link != mention) {
        link = &(*link)->owned;
    }
    *link = mention->owned;
    mention->owner = NULL;
    mention->owned = NULL;
}

/* The mention at rank `at` of `places` goes with the text it was in. False
 * when THE ALTERED LABELS could not take its label. */
static bool S_drop(markdown_core_labels *labels, markdown_core_tree *places, size_t at) {
    markdown_core_mention *mention = S_places_at(places, at)->mention;
    bool ok = true;
    if (mention->owner) {
        S_disown(mention);
    }
    markdown_core_label *label = mention->label;
    if ((int)mention->sequence == labels->uses) {
        S_use_unlink(mention);
    } else if (label) {
        ok = S_touch(labels, label);
        S_unlink(labels, mention);
    }
    markdown_core_tree_erase(places, at, 1);
    labels->count--;
    markdown_core_free(mention);
    if (label) {
        S_vacate(labels, label);
    }
    return ok;
}

/* A fresh mention of `label` in sequence `sequence`, located at `start`
 * after every mention located there or before, or NULL when storage could not
 * be had. */
static markdown_core_mention *S_place(markdown_core_labels *labels, int sequence, markdown_core_label *label,
                                      const markdown_core_node *node, uint32_t start) {
    markdown_core_mention *mention = sequence < 0 ? NULL : markdown_core_alloc(1, sizeof(*mention));
    if (!mention) {
        return NULL;
    }
    *mention = (markdown_core_mention){.label = label, .node = node, .sequence = (uint8_t)sequence};
    markdown_core_tree *places = &labels->sequences[sequence]->places;
    const markdown_core_mention_place place = {start, mention};
    /* A parse defines and refers mostly in the order of the text. */
    const size_t front = places->front;
    const size_t at = front && S_places_at(places, front - 1)->start > start
                          ? markdown_core_tree_from(places, (size_t)start + 1, 0, front)
                          : front;
    if (at == places->count) {
        markdown_core_mention_place *pushed = S_places_push(places);
        if (pushed) {
            *pushed = place;
            mention->at = (markdown_core_tree_handle){places->last, places->last->count - 1};
        }
        if (!pushed) {
            markdown_core_free(mention);
            return NULL;
        }
    } else if (!S_places_insert(places, at, place, true)) {
        markdown_core_free(mention);
        return NULL;
    }
    labels->count++;
    return mention;
}

static inline void S_assemble(markdown_core_labels *labels, markdown_core_label_space space, const unsigned char *name,
                              bufsize_t length) {
    markdown_core_strbuf_clear(&labels->key);
    markdown_core_strbuf_putc(&labels->key, (int)space);
    markdown_core_strbuf_put(&labels->key, name, length);
}

bool markdown_core_labels_define(markdown_core_labels *labels, markdown_core_label_space space,
                                 const unsigned char *name, bufsize_t length, uint32_t start,
                                 const markdown_core_node *node, markdown_core_node *owner) {
    if (length < 0 || (!length && !node)) {
        return true;
    }
    const int sequence = S_sequence(labels, space, node ? node->kind : (uint16_t)MARKDOWN_CORE_NODE_NONE);
    if (sequence < 0) {
        return false;
    }
    /* The owner defines in a sequence once: a definition again replaces its
     * earlier one there. */
    for (markdown_core_mention *mine = owner->mentions; mine; mine = mine->owned) {
        if (mine->sequence == (uint8_t)sequence) {
            if (!S_drop(labels, S_places(labels, mine), S_rank(labels, mine))) {
                return false;
            }
            break;
        }
    }
    markdown_core_label *label = NULL;
    if (length) {
        S_assemble(labels, space, name, length);
        if (labels->key.oom || !(label = S_label(labels)) || !S_touch(labels, label)) {
            return false;
        }
        /* A label defined now takes its place in order (THE NAMES IN
         * ORDER). */
        if (!label->first && !S_order_in(labels, label)) {
            return false;
        }
    }
    markdown_core_mention *mention = S_place(labels, sequence, label, node, start);
    if (!mention) {
        if (label && !label->first) {
            S_order_out(labels, label);
        }
        return false;
    }
    if (label) {
        S_link(labels, mention);
    }
    S_own(mention, owner);
    return true;
}

/* The key of `name` of `space`, as written, assembled: in the
 * reference-label normal form when the space `folds`. False when it names
 * nothing (markdown_core_labels_refer) or, with `*failed` set, when
 * storage could not be had. */
static bool S_compose(markdown_core_labels *labels, markdown_core_label_space space, const markdown_core_chunk *name,
                      bool folds, bool *failed) {
    if (folds ? name->len < 1 || name->len > MAX_LINK_LABEL_LENGTH : name->len <= 0) {
        return false;
    }
    markdown_core_strbuf_clear(&labels->key);
    markdown_core_strbuf_putc(&labels->key, (int)space);
    if (folds) {
        markdown_core_utf8proc_normalize_label(&labels->key, name->data, name->len);
    } else {
        markdown_core_strbuf_put(&labels->key, name->data, name->len);
    }
    if (labels->key.oom) {
        *failed = true;
        return false;
    }
    return labels->key.size > 1;
}

/* The first definition of the label of the key assembled, or NULL. */
static inline const markdown_core_mention *S_first(const markdown_core_labels *labels) {
    const unsigned char *key = (const unsigned char *)labels->key.ptr;
    const markdown_core_key_index_slot *slot =
        markdown_core_key_index_find_hashed(&labels->index, S_hash(key, labels->key.size), key, labels->key.size);
    return slot ? ((const markdown_core_label *)slot->value)->first : NULL;
}

const markdown_core_mention *markdown_core_labels_find(markdown_core_labels *labels, markdown_core_label_space space,
                                                       const unsigned char *name, bufsize_t length, bool *failed) {
    const markdown_core_chunk text = {(unsigned char *)name, length, 0};
    return S_compose(labels, space, &text, false, failed) ? S_first(labels) : NULL;
}

const markdown_core_mention *markdown_core_labels_find_label(markdown_core_labels *labels,
                                                             markdown_core_label_space space,
                                                             const markdown_core_chunk *name, bool *failed) {
    return S_compose(labels, space, name, true, failed) ? S_first(labels) : NULL;
}

/* Whether the ANCHOR label `slot` holds is of the name `probe`. */
static bool S_same_name(const void *probe, const markdown_core_key_index_slot *slot) {
    return slot->key[0] == (unsigned char)MARKDOWN_CORE_LABEL_ANCHOR &&
           markdown_core_pieces_are(probe, slot->key + 1, slot->key_len - 1);
}

const markdown_core_mention *markdown_core_labels_find_name(const markdown_core_labels *labels,
                                                            const markdown_core_pieces *name,
                                                            markdown_core_measure measure) {
    if (!name->size || !labels->index.size) {
        return NULL;
    }
    const unsigned char space = (unsigned char)MARKDOWN_CORE_LABEL_ANCHOR;
    const uint64_t hash =
        markdown_core_key_index_hash(markdown_core_measure_join(markdown_core_measure_of(&space, 1), measure));
    const markdown_core_key_index_slot *slot =
        markdown_core_key_index_seek(&labels->index, hash, name->size + 1, S_same_name, name);
    return slot ? ((const markdown_core_label *)slot->value)->first : NULL;
}

/* Whether the slot holds the key of the space and name `probe` names. */
typedef struct {
    markdown_core_label_space space;
    const unsigned char *name;
    bufsize_t length;
} S_probe;

static bool S_same_label(const void *probe, const markdown_core_key_index_slot *slot) {
    const S_probe *key = probe;
    return slot->key[0] == (unsigned char)key->space && !memcmp(slot->key + 1, key->name, (size_t)key->length);
}

const markdown_core_mention *markdown_core_labels_seek(const markdown_core_labels *labels,
                                                       markdown_core_label_space space, const unsigned char *name,
                                                       bufsize_t length) {
    if (length <= 0 || length == INT32_MAX || !labels->index.size) {
        return NULL;
    }
    const S_probe probe = {space, name, length};
    const markdown_core_key_index_slot *slot = markdown_core_key_index_seek(
        &labels->index, S_name_hash(space, name, length), length + 1, S_same_label, &probe);
    return slot ? ((const markdown_core_label *)slot->value)->first : NULL;
}

const markdown_core_mention *markdown_core_labels_refer(markdown_core_labels *labels, markdown_core_label_space space,
                                                        const markdown_core_chunk *name, bool folds, uint32_t start,
                                                        markdown_core_node *root, bool *failed) {
    if (!S_compose(labels, space, name, folds, failed)) {
        return NULL;
    }
    if (labels->uses < 0) {
        labels->uses = S_sequence(labels, 0, (uint16_t)MARKDOWN_CORE_NODE_NONE);
    }
    markdown_core_label *label = labels->uses < 0 ? NULL : S_label(labels);
    markdown_core_mention *mention = label ? S_place(labels, labels->uses, label, root, start) : NULL;
    if (!mention) {
        if (label) {
            S_vacate(labels, label);
        }
        *failed = true;
        return NULL;
    }
    S_use_link(mention);
    mention->found = label->first != NULL;
    mention->owed = true;
    S_own(mention, root);
    return label->first;
}

void markdown_core_labels_own(markdown_core_mention *mention, markdown_core_node *owner) {
    assert(mention->owed);
    mention->owed = false;
    if (owner != mention->owner) {
        S_disown(mention);
        S_own(mention, owner);
    }
}

void markdown_core_labels_release(markdown_core_labels *labels, markdown_core_node *node) {
    while (node->mentions) {
        markdown_core_mention *mention = node->mentions;
        (void)S_drop(labels, S_places(labels, mention), S_rank(labels, mention));
    }
}

bool markdown_core_labels_normal_label(markdown_core_labels *labels, markdown_core_node_pool *pool,
                                       const markdown_core_chunk *name, markdown_core_chunk *normal) {
    *normal = (markdown_core_chunk){0};
    markdown_core_strbuf_clear(&labels->key);
    if (name->len > 0) {
        markdown_core_utf8proc_normalize_label(&labels->key, name->data, name->len);
    }
    if (labels->key.oom) {
        return false;
    }
    const bufsize_t length = labels->key.size;
    if (!length) {
        return true;
    }
    unsigned char *stored = markdown_core_node_pool_bytes(pool, (size_t)length + 1);
    if (!stored) {
        return false;
    }
    memcpy(stored, labels->key.ptr, (size_t)length);
    stored[length] = '\0';
    *normal = (markdown_core_chunk){stored, length, 0};
    return true;
}

void markdown_core_labels_reopen(markdown_core_labels *labels, uint32_t start) {
    /* An edit begins a transaction (THE ALTERED LABELS). */
    S_renew(labels);
    for (size_t i = 0; i < labels->sequence_count; i++) {
        markdown_core_tree *places = &labels->sequences[i]->places;
        const bool back = places->front < places->count && S_places_at(places, places->front)->start < start;
        markdown_core_tree_focus(places, back ? markdown_core_tree_from(places, start, places->front, places->count)
                                              : markdown_core_tree_from(places, start, 0, places->front));
    }
}

void markdown_core_labels_shift(markdown_core_labels *labels, int64_t delta) {
    for (size_t i = 0; i < labels->sequence_count; i++) {
        markdown_core_tree *places = &labels->sequences[i]->places;
        markdown_core_tree_shift(places, places->front, delta);
    }
}
