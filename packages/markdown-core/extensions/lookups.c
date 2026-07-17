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
            recording->mem->free(recording->items[i].label);
        }
    }
    if (recording->items) {
        recording->mem->free(recording->items);
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
            (markdown_core_lookup_event *)recording->mem->realloc(recording->items, capacity * sizeof(*grown));
        if (!grown) {
            recording->lost = true;
            return;
        }
        recording->items = grown;
        recording->capacity = capacity;
    }

    label_size = strlen((const char *)label) + 1;
    copy = (unsigned char *)recording->mem->calloc(1, label_size);
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
bool markdown_core_lookup_recording_bundle(markdown_core_lookup_recording *recording, markdown_core_unit_lookups **out,
                                           size_t *out_count) {
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
    bundles = (markdown_core_unit_lookups *)mem->calloc(bundle_count, sizeof(*bundles));
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
                bundle->record.labels = (unsigned char **)mem->calloc(run_length, sizeof(unsigned char *));
                if (!bundle->record.labels) {
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
            mem->free(record->labels[i]);
        }
    }
    if (record->labels) {
        mem->free(record->labels);
    }
    record->labels = NULL;
    record->count = 0;
}

void markdown_core_unit_lookups_free(markdown_core_mem *mem, markdown_core_unit_lookups *bundles, size_t count) {
    size_t i;
    if (!bundles) {
        return;
    }
    for (i = 0; i < count; i++) {
        lookup_record_release(mem, &bundles[i].record);
    }
    mem->free(bundles);
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
        mem->free(table->keys);
    }
    if (table->records) {
        mem->free(table->records);
    }
    table->keys = NULL;
    table->records = NULL;
    table->capacity = 0;
    table->count = 0;
}

static void lookup_table_insert(markdown_core_mem *mem, markdown_core_lookup_table *table, markdown_core_node_id id,
                                markdown_core_lookup_record record) {
    size_t mask = table->capacity - 1;
    size_t slot = (size_t)lookup_mix64(id) & mask;
    while (table->keys[slot] != 0) {
        if (table->keys[slot] == id) {
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
    markdown_core_lookup_table grown = {NULL, NULL, 0, 0};
    size_t needed = table->count + extra;
    size_t capacity = 16;
    size_t i;

    if (table->capacity && needed * 2 <= table->capacity) {
        return true;
    }
    while (capacity < needed * 2) {
        capacity *= 2;
    }
    grown.keys = (markdown_core_node_id *)mem->calloc(capacity, sizeof(*grown.keys));
    grown.records = (markdown_core_lookup_record *)mem->calloc(capacity, sizeof(*grown.records));
    if (!grown.keys || !grown.records) {
        if (grown.keys) {
            mem->free(grown.keys);
        }
        if (grown.records) {
            mem->free(grown.records);
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
        mem->free(table->keys);
    }
    if (table->records) {
        mem->free(table->records);
    }
    *table = grown;
    return true;
}

void markdown_core_lookup_table_put(markdown_core_mem *mem, markdown_core_lookup_table *table, markdown_core_node_id id,
                                    markdown_core_lookup_record record) {
    lookup_table_insert(mem, table, id, record);
}

void markdown_core_lookup_table_remove(markdown_core_mem *mem, markdown_core_lookup_table *table,
                                       markdown_core_node_id id) {
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
