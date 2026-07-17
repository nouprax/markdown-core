#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

#include <iterator.h>
#include <map.h>
#include <node.h>
#include <parser.h>

// Session footnote index (decision #5, revised 2026-07-16): the tree is
// source-faithful — definitions never move, references always carry their
// label — and numbering, first-use order, resolution state, and
// back-reference ordinals are computed here per commit. Labels match
// case-folded with collapsed whitespace via the same normalization the
// reference map uses; the earliest definition of a label in document order
// wins. Reference labels longer than MAX_LINK_LABEL_LENGTH never resolve,
// exactly like link-reference lookups.

typedef struct {
    markdown_core_node **items;
    size_t count;
    size_t capacity;
} node_list;

static bool node_list_push(markdown_core_mem *mem, node_list *list, markdown_core_node *node) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        markdown_core_node **grown = (markdown_core_node **)mem->realloc(list->items, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = node;
    return true;
}

// Per-label accumulator. `winner` is the earliest definition in document
// order; `number` is the 1-based first-use ordinal, assigned when the first
// reference of a defined label is met; `seen` runs the per-reference
// ordinals.
typedef struct {
    unsigned char *normalized; // owned
    markdown_core_node *winner;
    uint64_t number;
    uint64_t references;
    uint64_t seen;
} label_state;

typedef struct {
    label_state *items;
    size_t count;
    size_t capacity;
} label_list;

// Resolves `label` to its accumulator slot, creating one on first sight.
// Returns SIZE_MAX with *failed set on allocation loss; SIZE_MAX without it
// when the label normalizes to nothing and can never participate.
static size_t label_slot(markdown_core_mem *mem, label_list *labels, markdown_core_key_index *by_label,
                         const markdown_core_chunk *label, bool *failed) {
    markdown_core_chunk copy = *label;
    int lost = 0;
    unsigned char *normalized = markdown_core_map_normalize_label(mem, &copy, &lost);
    bufsize_t normalized_len;
    void *existing;

    if (!normalized) {
        *failed = lost != 0;
        return SIZE_MAX;
    }
    normalized_len = (bufsize_t)strlen((char *)normalized);

    existing = markdown_core_key_index_lookup(by_label, normalized, normalized_len);
    if (existing) {
        mem->free(normalized);
        return (size_t)((uintptr_t)existing - 1);
    }

    if (labels->count == labels->capacity) {
        size_t capacity = labels->capacity ? labels->capacity * 2 : 16;
        label_state *grown = (label_state *)mem->realloc(labels->items, capacity * sizeof(*grown));
        if (!grown) {
            mem->free(normalized);
            *failed = true;
            return SIZE_MAX;
        }
        labels->items = grown;
        labels->capacity = capacity;
    }

    memset(&labels->items[labels->count], 0, sizeof(labels->items[0]));
    labels->items[labels->count].normalized = normalized;
    if (!markdown_core_key_index_insert(by_label, normalized, normalized_len, (void *)(uintptr_t)(labels->count + 1), 0,
                                        NULL)) {
        mem->free(normalized);
        *failed = true;
        return SIZE_MAX;
    }
    return labels->count++;
}

static int record_id_compare(const void *a, const void *b) {
    const markdown_core_footnote_record *ra = (const markdown_core_footnote_record *)a;
    const markdown_core_footnote_record *rb = (const markdown_core_footnote_record *)b;
    if (ra->node->id != rb->node->id) {
        return ra->node->id < rb->node->id ? -1 : 1;
    }
    return 0;
}

void markdown_core_footnote_index_release(markdown_core_mem *mem, markdown_core_footnote_index *index) {
    if (index->records) {
        mem->free(index->records);
    }
    if (index->in_use) {
        mem->free(index->in_use);
    }
    if (index->references) {
        mem->free(index->references);
    }
    if (index->reference_offsets) {
        mem->free(index->reference_offsets);
    }
    memset(index, 0, sizeof(*index));
}

bool markdown_core_footnote_index_build(markdown_core_mem *mem, markdown_core_node *root,
                                        markdown_core_footnote_index *index) {
    node_list defs = {NULL, 0, 0};
    node_list refs = {NULL, 0, 0};
    label_list labels = {NULL, 0, 0};
    markdown_core_key_index by_label = {NULL, NULL, 0, 0};
    size_t *def_labels = NULL;
    size_t *ref_labels = NULL;
    size_t *cursors = NULL;
    markdown_core_iter *iter;
    markdown_core_event_type ev;
    bool ok = false;
    bool indexed = false;
    size_t i;

    memset(index, 0, sizeof(*index));

    iter = markdown_core_iter_new(root);
    if (!iter) {
        return false;
    }
    while ((ev = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node;
        if (ev != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        node = markdown_core_iter_get_node(iter);
        if (node->type == MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION) {
            if (!node_list_push(mem, &defs, node)) {
                goto done;
            }
        } else if (node->type == MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE) {
            if (!node_list_push(mem, &refs, node)) {
                goto done;
            }
        }
    }
    markdown_core_iter_free(iter);
    iter = NULL;

    if (defs.count == 0 && refs.count == 0) {
        ok = true;
        goto done;
    }

    if (!markdown_core_key_index_init(&by_label, mem, defs.count + refs.count)) {
        goto done;
    }
    indexed = true;

    def_labels = (size_t *)mem->calloc(defs.count ? defs.count : 1, sizeof(*def_labels));
    ref_labels = (size_t *)mem->calloc(refs.count ? refs.count : 1, sizeof(*ref_labels));
    if (!def_labels || !ref_labels) {
        goto done;
    }

    for (i = 0; i < defs.count; i++) {
        bool failed = false;
        size_t slot = label_slot(mem, &labels, &by_label, &defs.items[i]->as.literal, &failed);
        if (failed) {
            goto done;
        }
        def_labels[i] = slot;
        if (slot != SIZE_MAX && !labels.items[slot].winner) {
            labels.items[slot].winner = defs.items[i];
        }
    }

    for (i = 0; i < refs.count; i++) {
        bool failed = false;
        size_t slot = SIZE_MAX;
        if (refs.items[i]->as.literal.len >= 1 && refs.items[i]->as.literal.len <= MAX_LINK_LABEL_LENGTH) {
            slot = label_slot(mem, &labels, &by_label, &refs.items[i]->as.literal, &failed);
        }
        if (failed) {
            goto done;
        }
        ref_labels[i] = slot;
        if (slot != SIZE_MAX) {
            labels.items[slot].references++;
        }
    }

    // First-use numbering over the resolved labels, in reference document
    // order.
    index->in_use = (markdown_core_node_id *)mem->calloc(labels.count ? labels.count : 1, sizeof(*index->in_use));
    if (!index->in_use) {
        goto done;
    }
    for (i = 0; i < refs.count; i++) {
        label_state *label = ref_labels[i] == SIZE_MAX ? NULL : &labels.items[ref_labels[i]];
        if (label && label->winner && !label->number) {
            label->number = ++index->in_use_count;
            index->in_use[label->number - 1] = label->winner->id;
        }
    }

    // Back-reference targets: reference ids grouped by first-use number.
    index->reference_offsets = (size_t *)mem->calloc(index->in_use_count + 1, sizeof(*index->reference_offsets));
    cursors = (size_t *)mem->calloc(index->in_use_count + 1, sizeof(*cursors));
    if (!index->reference_offsets || !cursors) {
        goto done;
    }
    for (i = 0; i < labels.count; i++) {
        if (labels.items[i].number) {
            index->reference_offsets[labels.items[i].number] = (size_t)labels.items[i].references;
        }
    }
    for (i = 1; i <= index->in_use_count; i++) {
        index->reference_offsets[i] += index->reference_offsets[i - 1];
    }
    index->references = (markdown_core_node_id *)mem->calloc(
        index->reference_offsets[index->in_use_count] ? index->reference_offsets[index->in_use_count] : 1,
        sizeof(*index->references));
    if (!index->references) {
        goto done;
    }
    for (i = 0; i < refs.count; i++) {
        label_state *label = ref_labels[i] == SIZE_MAX ? NULL : &labels.items[ref_labels[i]];
        if (label && label->number) {
            size_t group = (size_t)(label->number - 1);
            index->references[index->reference_offsets[group] + cursors[group]++] = refs.items[i]->id;
        }
    }

    // One record per footnote node, sorted by id for the query bsearch.
    index->records = (markdown_core_footnote_record *)mem->calloc(defs.count + refs.count, sizeof(*index->records));
    if (!index->records) {
        goto done;
    }
    for (i = 0; i < defs.count; i++) {
        markdown_core_footnote_record *record = &index->records[index->record_count++];
        label_state *label = def_labels[i] == SIZE_MAX ? NULL : &labels.items[def_labels[i]];
        record->node = defs.items[i];
        record->group = SIZE_MAX;
        if (label) {
            record->info.definition = label->winner->id;
            record->info.number = label->number;
            record->info.reference_count = label->references;
            if (label->winner == defs.items[i] && label->number) {
                record->group = (size_t)(label->number - 1);
            }
        }
    }
    for (i = 0; i < refs.count; i++) {
        markdown_core_footnote_record *record = &index->records[index->record_count++];
        label_state *label = ref_labels[i] == SIZE_MAX ? NULL : &labels.items[ref_labels[i]];
        record->node = refs.items[i];
        record->group = SIZE_MAX;
        if (label) {
            record->info.reference_ordinal = ++label->seen;
            record->info.reference_count = label->references;
            if (label->winner) {
                record->info.definition = label->winner->id;
                record->info.number = label->number;
            }
        }
    }
    qsort(index->records, index->record_count, sizeof(*index->records), record_id_compare);

    ok = true;

done:
    if (iter) {
        markdown_core_iter_free(iter);
    }
    for (i = 0; i < labels.count; i++) {
        mem->free(labels.items[i].normalized);
    }
    if (labels.items) {
        mem->free(labels.items);
    }
    if (indexed) {
        markdown_core_key_index_free(&by_label);
    }
    if (def_labels) {
        mem->free(def_labels);
    }
    if (ref_labels) {
        mem->free(ref_labels);
    }
    if (cursors) {
        mem->free(cursors);
    }
    if (defs.items) {
        mem->free(defs.items);
    }
    if (refs.items) {
        mem->free(refs.items);
    }
    if (!ok) {
        markdown_core_footnote_index_release(mem, index);
    }
    return ok;
}

static const markdown_core_footnote_record *find_record(const markdown_core_footnote_index *index,
                                                        markdown_core_node_id id) {
    size_t lo = 0;
    size_t hi = index->record_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (index->records[mid].node->id == id) {
            return &index->records[mid];
        }
        if (index->records[mid].node->id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static bool info_equal(const markdown_core_footnote_info *a, const markdown_core_footnote_info *b) {
    return a->definition == b->definition && a->number == b->number && a->reference_ordinal == b->reference_ordinal &&
           a->reference_count == b->reference_count;
}

static bool node_list_holds(const node_list *list, const markdown_core_node *node) {
    size_t i;
    for (i = 0; i < list->count; i++) {
        if (list->items[i] == node) {
            return true;
        }
    }
    return false;
}

bool markdown_core_footnote_index_diff(markdown_core_mem *mem, const markdown_core_footnote_index *previous,
                                       const markdown_core_footnote_index *next, uint64_t new_rev,
                                       markdown_core_changeset *changes) {
    // Two phases so the diff can run against a session's live tree: phase 1
    // collects the affected nodes without touching them (every allocation
    // happens here or in the reserve step), phase 2 applies revision bumps
    // and records ids infallibly. A failed diff therefore leaves every
    // committed node exactly as it was.
    node_list changed = {NULL, 0, 0};
    node_list bubbled = {NULL, 0, 0};
    bool ok = false;
    size_t i;

    for (i = 0; i < next->record_count; i++) {
        markdown_core_node *node = next->records[i].node;
        const markdown_core_footnote_record *old;
        markdown_core_node *parent;
        if (node->last_changed_rev == new_rev || node_list_holds(&changed, node) || node_list_holds(&bubbled, node)) {
            continue; // already reported by the adoption walk or this diff
        }
        old = find_record(previous, node->id);
        if (old && info_equal(&old->info, &next->records[i].info)) {
            continue;
        }
        if (!node_list_push(mem, &changed, node)) {
            goto done;
        }
        // Ancestors already carrying (or collected for) their own bump
        // covered the rest of the chain.
        for (parent = node->parent; parent && parent->last_changed_rev != new_rev; parent = parent->parent) {
            if (node_list_holds(&bubbled, parent) || node_list_holds(&changed, parent)) {
                break;
            }
            if (!node_list_push(mem, &bubbled, parent)) {
                goto done;
            }
        }
    }

    if (changes && (!markdown_core_id_array_reserve(&changes->changed, changed.count) ||
                    !markdown_core_id_array_reserve(&changes->bubbled, bubbled.count))) {
        goto done;
    }

    for (i = 0; i < changed.count; i++) {
        changed.items[i]->last_changed_rev = new_rev;
        if (changes) {
            markdown_core_id_array_push(&changes->changed, changed.items[i]->id);
        }
    }
    for (i = 0; i < bubbled.count; i++) {
        bubbled.items[i]->last_changed_rev = new_rev;
        if (changes) {
            markdown_core_id_array_push(&changes->bubbled, bubbled.items[i]->id);
        }
    }
    ok = true;

done:
    if (changed.items) {
        mem->free(changed.items);
    }
    if (bubbled.items) {
        mem->free(bubbled.items);
    }
    return ok;
}

// --- public queries ----------------------------------------------------------

bool markdown_core_session_footnote_info(const markdown_core_session *session, markdown_core_node_id id,
                                         markdown_core_footnote_info *info) {
    const markdown_core_footnote_record *record;
    if (info) {
        memset(info, 0, sizeof(*info));
    }
    if (!session || !info || id == 0) {
        return false;
    }
    record = find_record(&session->footnotes, id);
    if (!record) {
        return false;
    }
    *info = record->info;
    return true;
}

size_t markdown_core_session_footnotes(const markdown_core_session *session, const markdown_core_node_id **ids) {
    if (ids) {
        *ids = session ? session->footnotes.in_use : NULL;
    }
    return session ? session->footnotes.in_use_count : 0;
}

size_t markdown_core_session_footnote_references(const markdown_core_session *session, markdown_core_node_id definition,
                                                 const markdown_core_node_id **ids) {
    const markdown_core_footnote_record *record;
    if (ids) {
        *ids = NULL;
    }
    if (!session || definition == 0) {
        return 0;
    }
    record = find_record(&session->footnotes, definition);
    if (!record || record->group == SIZE_MAX) {
        return 0;
    }
    if (ids) {
        *ids = session->footnotes.references + session->footnotes.reference_offsets[record->group];
    }
    return session->footnotes.reference_offsets[record->group + 1] -
           session->footnotes.reference_offsets[record->group];
}
