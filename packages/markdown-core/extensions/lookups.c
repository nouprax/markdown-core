#include <string.h>

#include "session_internal.h"

#include "directive.h"

#include <node.h>

// Reference-lookup records: which inline-owning unit depends on which
// normalized labels. The map's lookup sink feeds a per-commit recording
// (units keyed by node pointer while ids are still unassigned); after id
// adoption the recording is bundled into per-unit records and installed in
// the session's persistent table. A commit that changes a label's winning
// definition consults the table to re-refine exactly the dependent units
// (incremental.c) instead of falling back to a full reparse.

static uint64_t lookup_mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// --- recording (one staged parse) -------------------------------------------

void markdown_core_lookup_recording_init(markdown_core_lookup_recording *recording, markdown_core_mem *mem) {
    memset(recording, 0, sizeof(*recording));
    recording->mem = mem;
}

void markdown_core_lookup_recording_release(markdown_core_lookup_recording *recording) {
    size_t i;
    for (i = 0; i < recording->count; i++) {
        if (recording->items[i].label) {
            recording->mem->free(recording->mem, recording->items[i].label);
        }
    }
    if (recording->items) {
        recording->mem->free(recording->mem, recording->items);
    }
    recording->items = NULL;
    recording->count = 0;
    recording->capacity = 0;
}

/* The map's lookup sink. Events of one unit arrive consecutively (the inline
 * phase parses each unit's content, nested owners included, before moving
 * on), so deduplication only scans the trailing same-unit run. A lost
 * observation poisons the recording: a commit that cannot trust its records
 * must not commit incrementally. */
void markdown_core_lookup_recording_sink(void *context, void *unit_pointer, const unsigned char *label) {
    markdown_core_lookup_recording *recording = (markdown_core_lookup_recording *)context;
    markdown_core_node *unit = (markdown_core_node *)unit_pointer;
    size_t label_size;
    unsigned char *copy;
    size_t i;

    /* Directive-label wrappers are facade-invisible and cannot anchor a
     * dependency; the directive they label can. */
    if (unit && unit->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL && unit->parent) {
        unit = unit->parent;
    }
    if (!unit) {
        recording->lost = true;
        return;
    }

    for (i = recording->count; i > 0 && recording->items[i - 1].unit == unit; i--) {
        if (strcmp((const char *)recording->items[i - 1].label, (const char *)label) == 0) {
            return;
        }
    }

    if (recording->count == recording->capacity) {
        size_t capacity = recording->capacity ? recording->capacity * 2 : 32;
        markdown_core_lookup_event *grown =
            (markdown_core_lookup_event *)
                recording->mem->realloc(recording->mem, recording->items, capacity * sizeof(*grown));
        if (!grown) {
            recording->lost = true;
            return;
        }
        recording->items = grown;
        recording->capacity = capacity;
    }

    label_size = strlen((const char *)label) + 1;
    copy = (unsigned char *)recording->mem->calloc(recording->mem, 1, label_size);
    if (!copy) {
        recording->lost = true;
        return;
    }
    memcpy(copy, label, label_size);
    recording->items[recording->count].unit = unit;
    recording->items[recording->count].label = copy;
    recording->count++;
}

/* Groups the recording's consecutive same-unit runs into per-unit records,
 * moving label ownership out of the recording. On failure the recording
 * still owns whatever was not yet moved, so releasing both structures never
 * double-frees. */
bool markdown_core_lookup_recording_bundle(
    markdown_core_lookup_recording *recording,
    markdown_core_unit_lookups **out,
    size_t *out_count
) {
    markdown_core_mem *mem = recording->mem;
    markdown_core_unit_lookups *bundles = NULL;
    size_t bundle_count = 0;
    size_t i;

    *out = NULL;
    *out_count = 0;
    if (recording->count == 0) {
        return true;
    }

    for (i = 0; i < recording->count; i++) {
        if (i == 0 || recording->items[i].unit != recording->items[i - 1].unit) {
            bundle_count++;
        }
    }
    bundles = (markdown_core_unit_lookups *)mem->calloc(mem, bundle_count, sizeof(*bundles));
    if (!bundles) {
        return false;
    }

    {
        size_t filled = 0;
        size_t run_start = 0;
        for (i = 1; i <= recording->count; i++) {
            if (i == recording->count || recording->items[i].unit != recording->items[run_start].unit) {
                size_t run_length = i - run_start;
                markdown_core_unit_lookups *bundle = &bundles[filled];
                bundle->unit = recording->items[run_start].unit;
                bundle->record.labels = (unsigned char **)mem->calloc(mem, run_length, sizeof(unsigned char *));
                bundle->record.positions = (size_t *)mem->calloc(mem, run_length, sizeof(size_t));
                if (!bundle->record.labels || !bundle->record.positions) {
                    markdown_core_unit_lookups_free(mem, bundles, bundle_count);
                    return false;
                }
                bundle->record.count = run_length;
                for (size_t k = 0; k < run_length; k++) {
                    bundle->record.labels[k] = recording->items[run_start + k].label;
                    recording->items[run_start + k].label = NULL; // moved
                }
                filled++;
                run_start = i;
            }
        }
    }

    *out = bundles;
    *out_count = bundle_count;
    return true;
}

static void lookup_record_release(markdown_core_mem *mem, markdown_core_lookup_record *record) {
    size_t i;
    for (i = 0; i < record->count; i++) {
        if (record->labels[i]) {
            mem->free(mem, record->labels[i]);
        }
    }
    if (record->labels) {
        mem->free(mem, record->labels);
    }
    if (record->positions) {
        mem->free(mem, record->positions);
    }
    record->labels = NULL;
    record->positions = NULL;
    record->count = 0;
}

// --- postings ----------------------------------------------------------------

static markdown_core_lookup_posting *posting_of(const markdown_core_lookup_table *table, const unsigned char *label) {
    void *value =
        markdown_core_key_index_lookup(&table->postings.by_label, label, (bufsize_t)strlen((const char *)label));
    if (!value) {
        return NULL;
    }
    return &table->postings.items[(size_t)(uintptr_t)value - 1];
}

const markdown_core_lookup_posting *
markdown_core_lookup_postings_find(const markdown_core_lookup_table *table, const unsigned char *label) {
    if (table->postings.by_label.capacity == 0) {
        return NULL;
    }
    return posting_of(table, label);
}

/* Finds the posting for `label`, creating an empty one (owned label copy,
 * index entry) when the table has never seen it. Fallible; runs only from
 * the pre-splice reserve step. */
static markdown_core_lookup_posting *
posting_find_or_create(markdown_core_mem *mem, markdown_core_lookup_table *table, const unsigned char *label) {
    markdown_core_lookup_postings *postings = &table->postings;
    markdown_core_lookup_posting *posting = postings->by_label.capacity ? posting_of(table, label) : NULL;
    size_t length;
    unsigned char *copy;
    if (posting) {
        return posting;
    }
    if (postings->by_label.capacity == 0 && !markdown_core_key_index_init(&postings->by_label, mem, 16)) {
        return NULL;
    }
    if (postings->count == postings->capacity) {
        size_t grown_capacity = postings->capacity ? postings->capacity * 2 : 16;
        markdown_core_lookup_posting *grown =
            (markdown_core_lookup_posting *)mem->realloc(mem, postings->items, grown_capacity * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        postings->items = grown;
        postings->capacity = grown_capacity;
    }
    length = strlen((const char *)label);
    copy = (unsigned char *)mem->calloc(mem, 1, length + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, label, length + 1);
    posting = &postings->items[postings->count];
    posting->label = copy;
    posting->items = NULL;
    posting->count = 0;
    posting->capacity = 0;
    posting->staged = 0;
    // The index keys the owned copy: it must outlive the slot, and it does —
    // posting slots persist for the table's lifetime.
    if (!markdown_core_key_index_insert(
            &postings->by_label,
            copy,
            (bufsize_t)length,
            (void *)(uintptr_t)(postings->count + 1),
            0,
            NULL
        )) {
        mem->free(mem, copy);
        posting->label = NULL;
        return NULL;
    }
    postings->count++;
    return posting;
}

/* Zeroes the staged tallies of every bundle label; failure cleanup so a
 * retried reserve starts from an honest count. */
static void postings_clear_staged(
    markdown_core_lookup_table *table,
    const markdown_core_unit_lookups *bundles,
    size_t bundle_count
) {
    size_t i;
    if (table->postings.by_label.capacity == 0) {
        return;
    }
    for (i = 0; i < bundle_count; i++) {
        size_t k;
        for (k = 0; k < bundles[i].record.count; k++) {
            markdown_core_lookup_posting *posting = posting_of(table, bundles[i].record.labels[k]);
            if (posting) {
                posting->staged = 0;
            }
        }
    }
}

bool markdown_core_lookup_postings_reserve(
    markdown_core_mem *mem,
    markdown_core_lookup_table *table,
    const markdown_core_unit_lookups *bundles,
    size_t bundle_count
) {
    size_t i;
    // Pass 1: find or create every label's posting and tally this commit's
    // appends per label (the same label may repeat within and across
    // bundles; one slot per occurrence).
    for (i = 0; i < bundle_count; i++) {
        size_t k;
        for (k = 0; k < bundles[i].record.count; k++) {
            markdown_core_lookup_posting *posting = posting_find_or_create(mem, table, bundles[i].record.labels[k]);
            if (!posting) {
                postings_clear_staged(table, bundles, bundle_count);
                return false;
            }
            posting->staged++;
        }
    }
    // Pass 2: one growth per touched posting (the first occurrence sees the
    // full tally and zeroes it, later occurrences skip).
    for (i = 0; i < bundle_count; i++) {
        size_t k;
        for (k = 0; k < bundles[i].record.count; k++) {
            markdown_core_lookup_posting *posting = posting_of(table, bundles[i].record.labels[k]);
            size_t needed;
            if (posting->staged == 0) {
                continue;
            }
            needed = posting->count + posting->staged;
            if (needed > posting->capacity) {
                size_t grown_capacity = posting->capacity ? posting->capacity : 4;
                markdown_core_lookup_posting_entry *grown;
                while (grown_capacity < needed) {
                    grown_capacity *= 2;
                }
                grown = (markdown_core_lookup_posting_entry *)
                            mem->realloc(mem, posting->items, grown_capacity * sizeof(*grown));
                if (!grown) {
                    postings_clear_staged(table, bundles, bundle_count);
                    return false;
                }
                posting->items = grown;
                posting->capacity = grown_capacity;
            }
            posting->staged = 0;
        }
    }
    return true;
}

/* Appends the (unit, ordinal) entry to `label`'s posting. Capacity was
 * reserved; the posting must exist. */
static void posting_append(
    markdown_core_lookup_table *table,
    const unsigned char *label,
    markdown_core_node_id unit,
    size_t ordinal,
    size_t *position
) {
    markdown_core_lookup_posting *posting = posting_of(table, label);
    posting->items[posting->count].unit = unit;
    posting->items[posting->count].ordinal = ordinal;
    *position = posting->count;
    posting->count++;
}

static markdown_core_lookup_record *lookup_table_find(markdown_core_lookup_table *table, markdown_core_node_id id);

/* Swap-removes one posting entry and repoints the moved entry's owner
 * record at its new position. Infallible. */
static void posting_remove(markdown_core_lookup_table *table, const unsigned char *label, size_t position) {
    markdown_core_lookup_posting *posting = posting_of(table, label);
    size_t last = posting->count - 1;
    if (position != last) {
        markdown_core_lookup_posting_entry moved = posting->items[last];
        markdown_core_lookup_record *owner = lookup_table_find(table, moved.unit);
        posting->items[position] = moved;
        owner->positions[moved.ordinal] = position;
    }
    posting->count = last;
}

static void postings_release(markdown_core_mem *mem, markdown_core_lookup_postings *postings) {
    size_t i;
    for (i = 0; i < postings->count; i++) {
        if (postings->items[i].label) {
            mem->free(mem, postings->items[i].label);
        }
        if (postings->items[i].items) {
            mem->free(mem, postings->items[i].items);
        }
    }
    if (postings->items) {
        mem->free(mem, postings->items);
    }
    if (postings->by_label.capacity) {
        markdown_core_key_index_free(&postings->by_label);
    }
    memset(postings, 0, sizeof(*postings));
}

void markdown_core_unit_lookups_free(markdown_core_mem *mem, markdown_core_unit_lookups *bundles, size_t count) {
    size_t i;
    if (!bundles) {
        return;
    }
    for (i = 0; i < count; i++) {
        lookup_record_release(mem, &bundles[i].record);
    }
    mem->free(mem, bundles);
}

// --- persistent table --------------------------------------------------------

void markdown_core_lookup_table_release(markdown_core_mem *mem, markdown_core_lookup_table *table) {
    size_t i;
    for (i = 0; i < table->capacity; i++) {
        if (table->keys[i] != 0) {
            lookup_record_release(mem, &table->records[i]);
        }
    }
    if (table->keys) {
        mem->free(mem, table->keys);
    }
    if (table->records) {
        mem->free(mem, table->records);
    }
    table->keys = NULL;
    table->records = NULL;
    table->capacity = 0;
    table->count = 0;
    postings_release(mem, &table->postings);
}

static markdown_core_lookup_record *lookup_table_find(markdown_core_lookup_table *table, markdown_core_node_id id) {
    size_t mask = table->capacity - 1;
    size_t slot = (size_t)lookup_mix64(id) & mask;
    while (table->keys[slot] != id) {
        if (table->keys[slot] == 0) {
            return NULL;
        }
        slot = (slot + 1) & mask;
    }
    return &table->records[slot];
}

/* Unlinks every posting entry of `record` (called before the record leaves
 * the table). Infallible. */
static void record_postings_remove(markdown_core_lookup_table *table, const markdown_core_lookup_record *record) {
    size_t i;
    for (i = 0; i < record->count; i++) {
        posting_remove(table, record->labels[i], record->positions[i]);
    }
}

/* Links every label of `record` into its posting (capacity reserved).
 * Infallible; fills record->positions. */
static void record_postings_add(
    markdown_core_lookup_table *table,
    markdown_core_lookup_record *record,
    markdown_core_node_id unit
) {
    size_t i;
    for (i = 0; i < record->count; i++) {
        posting_append(table, record->labels[i], unit, i, &record->positions[i]);
    }
}

/* Raw slot install: postings are the caller's concern (the rehash moves
 * records without touching them; the public put links them afterwards). */
static void lookup_table_insert(
    markdown_core_mem *mem,
    markdown_core_lookup_table *table,
    markdown_core_node_id id,
    markdown_core_lookup_record record
) {
    size_t mask = table->capacity - 1;
    size_t slot = (size_t)lookup_mix64(id) & mask;
    while (table->keys[slot] != 0) {
        if (table->keys[slot] == id) {
            record_postings_remove(table, &table->records[slot]);
            lookup_record_release(mem, &table->records[slot]);
            table->records[slot] = record;
            return;
        }
        slot = (slot + 1) & mask;
    }
    table->keys[slot] = id;
    table->records[slot] = record;
    table->count++;
}

bool markdown_core_lookup_table_reserve(markdown_core_mem *mem, markdown_core_lookup_table *table, size_t extra) {
    markdown_core_lookup_table grown = {NULL, NULL, 0, 0, {NULL, 0, 0, {NULL, NULL, 0, 0}}};
    size_t needed = table->count + extra;
    size_t capacity = 16;
    size_t i;

    if (table->capacity && needed * 2 <= table->capacity) {
        return true;
    }
    while (capacity < needed * 2) {
        capacity *= 2;
    }
    grown.keys = (markdown_core_node_id *)mem->calloc(mem, capacity, sizeof(*grown.keys));
    grown.records = (markdown_core_lookup_record *)mem->calloc(mem, capacity, sizeof(*grown.records));
    if (!grown.keys || !grown.records) {
        if (grown.keys) {
            mem->free(mem, grown.keys);
        }
        if (grown.records) {
            mem->free(mem, grown.records);
        }
        return false;
    }
    grown.capacity = capacity;
    for (i = 0; i < table->capacity; i++) {
        if (table->keys[i] != 0) {
            lookup_table_insert(mem, &grown, table->keys[i], table->records[i]);
        }
    }
    if (table->keys) {
        mem->free(mem, table->keys);
    }
    if (table->records) {
        mem->free(mem, table->records);
    }
    // The postings reference units by id, not by slot, so the rehash moves
    // them wholesale; entries and stored positions stay valid.
    grown.postings = table->postings;
    *table = grown;
    return true;
}

void markdown_core_lookup_table_put(
    markdown_core_mem *mem,
    markdown_core_lookup_table *table,
    markdown_core_node_id id,
    markdown_core_lookup_record record
) {
    lookup_table_insert(mem, table, id, record);
    // Link after install so the positions land in the stored record; the
    // postings reserve ran before the splice, so appends cannot fail.
    record_postings_add(table, lookup_table_find(table, id), id);
}

void markdown_core_lookup_table_remove(
    markdown_core_mem *mem,
    markdown_core_lookup_table *table,
    markdown_core_node_id id
) {
    size_t mask;
    size_t slot;
    size_t gap;
    size_t scan;

    if (table->capacity == 0 || id == 0) {
        return;
    }
    mask = table->capacity - 1;
    slot = (size_t)lookup_mix64(id) & mask;
    while (table->keys[slot] != id) {
        if (table->keys[slot] == 0) {
            return;
        }
        slot = (slot + 1) & mask;
    }
    record_postings_remove(table, &table->records[slot]);
    lookup_record_release(mem, &table->records[slot]);

    // Backward-shift deletion, mirroring the session id table.
    gap = slot;
    scan = (gap + 1) & mask;
    while (table->keys[scan] != 0) {
        size_t home = (size_t)lookup_mix64(table->keys[scan]) & mask;
        if (((scan - home) & mask) >= ((scan - gap) & mask)) {
            table->keys[gap] = table->keys[scan];
            table->records[gap] = table->records[scan];
            gap = scan;
        }
        scan = (scan + 1) & mask;
    }
    table->keys[gap] = 0;
    memset(&table->records[gap], 0, sizeof(table->records[gap]));
    table->count--;
}
