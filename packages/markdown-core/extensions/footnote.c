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

bool markdown_core_footnote_site_push(
    markdown_core_mem *mem,
    markdown_core_footnote_site_list *list,
    markdown_core_footnote_site site
) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        markdown_core_footnote_site *grown =
            (markdown_core_footnote_site *)mem->realloc(list->items, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = site;
    return true;
}

void markdown_core_footnote_site_list_release(markdown_core_mem *mem, markdown_core_footnote_site_list *list) {
    if (list->items) {
        mem->free(list->items);
    }
    memset(list, 0, sizeof(*list));
}

/* The document child whose subtree holds `node` (the node itself when it is
 * one). `root` is the walked subtree's root, so a NULL-parent stop can never
 * escape into a containing tree. */
static markdown_core_node *site_anchor(markdown_core_node *root, markdown_core_node *node) {
    markdown_core_node *top = node;
    while (top->parent && top->parent != root) {
        top = top->parent;
    }
    return top;
}

/* The inline-owning leaf that parsed a reference: its nearest block
 * ancestor. */
static markdown_core_node *site_unit(markdown_core_node *node) {
    markdown_core_node *parent = node->parent;
    while (parent && !MARKDOWN_CORE_NODE_BLOCK_P(parent)) {
        parent = parent->parent;
    }
    return parent;
}

bool markdown_core_footnote_collect_sites(
    markdown_core_mem *mem,
    markdown_core_node *root,
    markdown_core_node *anchor,
    markdown_core_footnote_site_list *defs,
    markdown_core_footnote_site_list *refs
) {
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev;
    bool ok = true;

    if (!iter) {
        return false;
    }
    while (ok && (ev = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node;
        markdown_core_footnote_site site;
        if (ev != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        node = markdown_core_iter_get_node(iter);
        if (node->type != MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION &&
            node->type != MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE) {
            continue;
        }
        site.node = node;
        site.anchor = anchor ? anchor : site_anchor(root, node);
        site.label = SIZE_MAX; // interned later by label_sites
        site.group_pos = 0;
        if (node->type == MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION) {
            site.unit = NULL;
            ok = markdown_core_footnote_site_push(mem, defs, site);
        } else {
            site.unit = site_unit(node);
            ok = markdown_core_footnote_site_push(mem, refs, site);
        }
    }
    markdown_core_iter_free(iter);
    return ok;
}

// --- persistent label interning ----------------------------------------------

void markdown_core_footnote_labels_release(markdown_core_mem *mem, markdown_core_footnote_labels *labels) {
    size_t i;
    for (i = 0; i < labels->count; i++) {
        mem->free(labels->normalized[i]);
    }
    if (labels->normalized) {
        mem->free(labels->normalized);
    }
    markdown_core_key_index_free(&labels->by_label); // slots-NULL safe
    memset(labels, 0, sizeof(*labels));
}

size_t
markdown_core_session_footnote_label(markdown_core_session *session, const markdown_core_chunk *label, bool *failed) {
    markdown_core_mem *mem = session->mem;
    markdown_core_footnote_labels *labels = &session->footnote_labels;
    markdown_core_chunk copy = *label;
    int lost = 0;
    unsigned char *normalized;
    bufsize_t normalized_len;
    void *existing;

    *failed = false;
    normalized = markdown_core_map_normalize_label(mem, &copy, &lost);
    if (!normalized) {
        *failed = lost != 0;
        return SIZE_MAX;
    }
    normalized_len = (bufsize_t)strlen((char *)normalized);

    // A failed init leaves `mem` set with no slots; probe the slots so the
    // next call retries instead of walking a zero-capacity table.
    if (labels->by_label.slots == NULL && !markdown_core_key_index_init(&labels->by_label, mem, 16)) {
        mem->free(normalized);
        *failed = true;
        return SIZE_MAX;
    }
    existing = markdown_core_key_index_lookup(&labels->by_label, normalized, normalized_len);
    if (existing) {
        mem->free(normalized);
        return (size_t)((uintptr_t)existing - 1);
    }

    if (labels->count == labels->capacity) {
        size_t capacity = labels->capacity ? labels->capacity * 2 : 16;
        unsigned char **grown = (unsigned char **)mem->realloc(labels->normalized, capacity * sizeof(*grown));
        if (!grown) {
            mem->free(normalized);
            *failed = true;
            return SIZE_MAX;
        }
        labels->normalized = grown;
        labels->capacity = capacity;
    }
    if (!markdown_core_key_index_insert(
            &labels->by_label,
            normalized,
            normalized_len,
            (void *)(uintptr_t)(labels->count + 1),
            0,
            NULL
        )) {
        mem->free(normalized);
        *failed = true;
        return SIZE_MAX;
    }
    labels->normalized[labels->count] = normalized;
    return labels->count++;
}

bool markdown_core_session_footnote_label_sites(
    markdown_core_session *session,
    markdown_core_footnote_site_list *defs,
    markdown_core_footnote_site_list *refs
) {
    size_t i;
    for (i = 0; i < defs->count; i++) {
        bool failed = false;
        defs->items[i].label = markdown_core_session_footnote_label(session, &defs->items[i].node->as.literal, &failed);
        if (failed) {
            return false;
        }
    }
    for (i = 0; i < refs->count; i++) {
        markdown_core_node *ref = refs->items[i].node;
        bool failed = false;
        refs->items[i].label = SIZE_MAX;
        if (ref->as.literal.len >= 1 && ref->as.literal.len <= MAX_LINK_LABEL_LENGTH) {
            refs->items[i].label = markdown_core_session_footnote_label(session, &ref->as.literal, &failed);
        }
        if (failed) {
            return false;
        }
    }
    return true;
}

// --- id -> record table ------------------------------------------------------

#define FOOTNOTE_TOMBSTONE UINT64_MAX

static uint64_t footnote_mix64(uint64_t x) {
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}

markdown_core_footnote_record *
markdown_core_footnote_table_find(const markdown_core_footnote_table *table, markdown_core_node_id id) {
    size_t mask;
    size_t slot;
    if (table->capacity == 0) {
        return NULL;
    }
    mask = table->capacity - 1;
    slot = (size_t)footnote_mix64(id) & mask;
    while (table->keys[slot] != 0) {
        if (table->keys[slot] == id) {
            return &table->records[slot];
        }
        slot = (slot + 1) & mask;
    }
    return NULL;
}

void markdown_core_footnote_table_put(markdown_core_footnote_table *table, markdown_core_footnote_record record) {
    size_t mask = table->capacity - 1;
    size_t slot = (size_t)footnote_mix64(record.node->id) & mask;
    while (table->keys[slot] != 0 && table->keys[slot] != FOOTNOTE_TOMBSTONE) {
        slot = (slot + 1) & mask;
    }
    if (table->keys[slot] != FOOTNOTE_TOMBSTONE) {
        table->occupied++;
    }
    table->keys[slot] = record.node->id;
    table->records[slot] = record;
    table->count++;
}

void markdown_core_footnote_table_remove(markdown_core_footnote_table *table, markdown_core_node_id id) {
    size_t mask;
    size_t slot;
    if (table->capacity == 0) {
        return;
    }
    mask = table->capacity - 1;
    slot = (size_t)footnote_mix64(id) & mask;
    while (table->keys[slot] != 0) {
        if (table->keys[slot] == id) {
            table->keys[slot] = FOOTNOTE_TOMBSTONE;
            table->count--;
            return;
        }
        slot = (slot + 1) & mask;
    }
}

static void footnote_table_release(markdown_core_mem *mem, markdown_core_footnote_table *table) {
    if (table->keys) {
        mem->free(table->keys);
    }
    if (table->records) {
        mem->free(table->records);
    }
    memset(table, 0, sizeof(*table));
}

bool markdown_core_footnote_table_reserve(markdown_core_mem *mem, markdown_core_footnote_table *table, size_t extra) {
    markdown_core_footnote_table grown;
    size_t needed = table->count + extra;
    size_t capacity = 16;
    size_t i;

    // Tombstones count against the probe budget, so the trigger is occupancy.
    if (table->capacity && (table->occupied + extra) * 2 <= table->capacity) {
        return true;
    }
    memset(&grown, 0, sizeof(grown));
    while (capacity < needed * 2) {
        capacity *= 2;
    }
    grown.keys = (markdown_core_node_id *)mem->calloc(capacity, sizeof(*grown.keys));
    grown.records = (markdown_core_footnote_record *)mem->calloc(capacity, sizeof(*grown.records));
    if (!grown.keys || !grown.records) {
        footnote_table_release(mem, &grown);
        return false;
    }
    grown.capacity = capacity;
    for (i = 0; i < table->capacity; i++) {
        if (table->keys[i] != 0 && table->keys[i] != FOOTNOTE_TOMBSTONE) {
            markdown_core_footnote_table_put(&grown, table->records[i]);
        }
    }
    footnote_table_release(mem, table);
    *table = grown;
    return true;
}

// Per-label accumulator, indexed by the session's interned label slot.
// `winner` is the earliest definition in document order; `number` is the
// 1-based first-use ordinal, assigned when the first reference of a defined
// label is met; `seen` runs the per-reference ordinals.
typedef struct {
    markdown_core_node *winner;
    uint64_t number;
    uint64_t references;
    uint64_t seen;
} label_scratch;

void markdown_core_footnote_index_release(markdown_core_mem *mem, markdown_core_footnote_index *index) {
    markdown_core_footnote_site_list_release(mem, &index->defs);
    markdown_core_footnote_site_list_release(mem, &index->refs);
    footnote_table_release(mem, &index->records);
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

bool markdown_core_footnote_index_build_sites(
    markdown_core_mem *mem,
    markdown_core_footnote_site_list *defs,
    markdown_core_footnote_site_list *refs,
    markdown_core_footnote_index *index
) {
    label_scratch *labels = NULL;
    size_t label_bound = 0;
    size_t *cursors = NULL;
    size_t def_count;
    size_t ref_count;
    bool ok = false;
    size_t i;

    memset(index, 0, sizeof(*index));
    index->defs = *defs;
    index->refs = *refs;
    memset(defs, 0, sizeof(*defs));
    memset(refs, 0, sizeof(*refs));
    def_count = index->defs.count;
    ref_count = index->refs.count;

    if (def_count == 0 && ref_count == 0) {
        return true;
    }

    for (i = 0; i < def_count; i++) {
        size_t slot = index->defs.items[i].label;
        if (slot != SIZE_MAX && slot + 1 > label_bound) {
            label_bound = slot + 1;
        }
    }
    for (i = 0; i < ref_count; i++) {
        size_t slot = index->refs.items[i].label;
        if (slot != SIZE_MAX && slot + 1 > label_bound) {
            label_bound = slot + 1;
        }
    }
    labels = (label_scratch *)mem->calloc(label_bound ? label_bound : 1, sizeof(*labels));
    if (!labels) {
        goto done;
    }

    for (i = 0; i < def_count; i++) {
        size_t slot = index->defs.items[i].label;
        if (slot != SIZE_MAX && !labels[slot].winner) {
            labels[slot].winner = index->defs.items[i].node;
        }
    }
    for (i = 0; i < ref_count; i++) {
        size_t slot = index->refs.items[i].label;
        if (slot != SIZE_MAX) {
            labels[slot].references++;
        }
    }

    // First-use numbering over the resolved labels, in reference document
    // order.
    index->in_use = (markdown_core_node_id *)mem->calloc(label_bound ? label_bound : 1, sizeof(*index->in_use));
    if (!index->in_use) {
        goto done;
    }
    for (i = 0; i < ref_count; i++) {
        size_t slot = index->refs.items[i].label;
        label_scratch *label = slot == SIZE_MAX ? NULL : &labels[slot];
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
    for (i = 0; i < label_bound; i++) {
        if (labels[i].number) {
            index->reference_offsets[labels[i].number] = (size_t)labels[i].references;
        }
    }
    for (i = 1; i <= index->in_use_count; i++) {
        index->reference_offsets[i] += index->reference_offsets[i - 1];
    }
    index->references = (markdown_core_node_id *)mem->calloc(
        index->reference_offsets[index->in_use_count] ? index->reference_offsets[index->in_use_count] : 1,
        sizeof(*index->references)
    );
    if (!index->references) {
        goto done;
    }
    for (i = 0; i < ref_count; i++) {
        size_t slot = index->refs.items[i].label;
        label_scratch *label = slot == SIZE_MAX ? NULL : &labels[slot];
        if (label && label->number) {
            size_t group = (size_t)(label->number - 1);
            index->references[index->reference_offsets[group] + cursors[group]++] = index->refs.items[i].node->id;
        }
    }

    // One record per footnote node, keyed by id. `group_pos` mirrors each
    // reference's position inside its label's group run so a
    // sequence-preserving commit can patch a churned id in place.
    if (!markdown_core_footnote_table_reserve(mem, &index->records, def_count + ref_count)) {
        goto done;
    }
    for (i = 0; i < def_count; i++) {
        markdown_core_footnote_record record;
        size_t slot = index->defs.items[i].label;
        label_scratch *label = slot == SIZE_MAX ? NULL : &labels[slot];
        memset(&record, 0, sizeof(record));
        record.node = index->defs.items[i].node;
        record.group = SIZE_MAX;
        if (label) {
            record.info.definition = label->winner->id;
            record.info.number = label->number;
            record.info.reference_count = label->references;
            if (label->winner == record.node && label->number) {
                record.group = (size_t)(label->number - 1);
            }
        }
        markdown_core_footnote_table_put(&index->records, record);
    }
    for (i = 0; i < ref_count; i++) {
        markdown_core_footnote_record record;
        size_t slot = index->refs.items[i].label;
        label_scratch *label = slot == SIZE_MAX ? NULL : &labels[slot];
        memset(&record, 0, sizeof(record));
        record.node = index->refs.items[i].node;
        record.group = SIZE_MAX;
        if (label) {
            record.info.reference_ordinal = ++label->seen;
            record.info.reference_count = label->references;
            index->refs.items[i].group_pos = (size_t)(record.info.reference_ordinal - 1);
            if (label->winner) {
                record.info.definition = label->winner->id;
                record.info.number = label->number;
            }
        }
        markdown_core_footnote_table_put(&index->records, record);
    }

    ok = true;

done:
    if (labels) {
        mem->free(labels);
    }
    if (cursors) {
        mem->free(cursors);
    }
    if (!ok) {
        markdown_core_footnote_index_release(mem, index);
    }
    return ok;
}

bool markdown_core_footnote_index_build(
    markdown_core_session *session,
    markdown_core_node *root,
    markdown_core_footnote_index *index
) {
    markdown_core_mem *mem = session->mem;
    markdown_core_footnote_site_list defs = {NULL, 0, 0};
    markdown_core_footnote_site_list refs = {NULL, 0, 0};

    memset(index, 0, sizeof(*index));
    if (!markdown_core_footnote_collect_sites(mem, root, NULL, &defs, &refs) ||
        !markdown_core_session_footnote_label_sites(session, &defs, &refs)) {
        markdown_core_footnote_site_list_release(mem, &defs);
        markdown_core_footnote_site_list_release(mem, &refs);
        return false;
    }
    return markdown_core_footnote_index_build_sites(mem, &defs, &refs, index);
}

static const markdown_core_footnote_record *
find_record(const markdown_core_footnote_index *index, markdown_core_node_id id) {
    return markdown_core_footnote_table_find(&index->records, id);
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

bool markdown_core_footnote_index_diff(
    markdown_core_mem *mem,
    const markdown_core_footnote_index *previous,
    const markdown_core_footnote_index *next,
    uint64_t new_rev,
    markdown_core_delta *changes
) {
    // Two phases so the diff can run against a session's live tree: phase 1
    // collects the affected nodes without touching them (every allocation
    // happens here or in the reserve step), phase 2 applies revision bumps
    // and records ids infallibly. A failed diff therefore leaves every
    // committed node exactly as it was.
    node_list changed = {NULL, 0, 0};
    node_list bubbled = {NULL, 0, 0};
    bool ok = false;
    size_t i;

    for (i = 0; i < next->records.capacity; i++) {
        markdown_core_node *node;
        const markdown_core_footnote_record *old;
        markdown_core_node *parent;
        if (next->records.keys[i] == 0 || next->records.keys[i] == FOOTNOTE_TOMBSTONE) {
            continue;
        }
        node = next->records.records[i].node;
        if (node->last_changed_rev == new_rev || node_list_holds(&changed, node) || node_list_holds(&bubbled, node)) {
            continue; // already reported by the adoption walk or this diff
        }
        old = find_record(previous, node->id);
        if (old && info_equal(&old->info, &next->records.records[i].info)) {
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

bool markdown_core_session_footnote_info(
    const markdown_core_session *session,
    markdown_core_node_id id,
    markdown_core_footnote_info *info
) {
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

size_t markdown_core_session_footnote_references(
    const markdown_core_session *session,
    markdown_core_node_id definition,
    const markdown_core_node_id **ids
) {
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
