#include <stdlib.h>

#include "session_internal.h"

// Deltas are caller-owned plain data: they stay valid after the session
// advances or is freed, so they use the system allocator like the facade's
// error and dump buffers do.

bool markdown_core_id_array_push(markdown_core_id_array *array, markdown_core_node_id id) {
    if (array->count == array->capacity) {
        size_t capacity = array->capacity ? array->capacity * 2 : 16;
        markdown_core_node_id *grown = (markdown_core_node_id *)realloc(array->ids, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        array->ids = grown;
        array->capacity = capacity;
    }
    array->ids[array->count++] = id;
    return true;
}

bool markdown_core_id_array_reserve(markdown_core_id_array *array, size_t extra) {
    size_t needed = array->count + extra;
    markdown_core_node_id *grown;
    size_t capacity;
    if (needed <= array->capacity) {
        return true;
    }
    capacity = array->capacity ? array->capacity : 16;
    while (capacity < needed) {
        capacity *= 2;
    }
    grown = (markdown_core_node_id *)realloc(array->ids, capacity * sizeof(*grown));
    if (!grown) {
        return false;
    }
    array->ids = grown;
    array->capacity = capacity;
    return true;
}

void markdown_core_delta_revisions(const markdown_core_delta *changes, uint64_t *before, uint64_t *after) {
    if (before) {
        *before = changes ? changes->before : 0;
    }
    if (after) {
        *after = changes ? changes->after : 0;
    }
}

static size_t array_view(const markdown_core_id_array *array, const markdown_core_node_id **ids) {
    if (ids) {
        *ids = array->ids;
    }
    return array->count;
}

size_t markdown_core_delta_added(const markdown_core_delta *changes, const markdown_core_node_id **ids) {
    if (!changes) {
        if (ids) {
            *ids = NULL;
        }
        return 0;
    }
    return array_view(&changes->added, ids);
}

size_t markdown_core_delta_removed(const markdown_core_delta *changes, const markdown_core_node_id **ids) {
    if (!changes) {
        if (ids) {
            *ids = NULL;
        }
        return 0;
    }
    return array_view(&changes->removed, ids);
}

size_t markdown_core_delta_changed(const markdown_core_delta *changes, const markdown_core_node_id **ids) {
    if (!changes) {
        if (ids) {
            *ids = NULL;
        }
        return 0;
    }
    return array_view(&changes->changed, ids);
}

size_t markdown_core_delta_bubbled(const markdown_core_delta *changes, const markdown_core_node_id **ids) {
    if (!changes) {
        if (ids) {
            *ids = NULL;
        }
        return 0;
    }
    return array_view(&changes->bubbled, ids);
}

typedef struct ordered_delta_slot {
    markdown_core_node_id id;
    size_t touched_index;
} ordered_delta_slot;

typedef struct ordered_delta_work {
    const markdown_core_node *node;
    markdown_core_node_id parent;
    size_t parent_index;
    size_t remaining_children;
    size_t next_ready;
    markdown_core_delta_change_kind change;
} ordered_delta_work;

/* One 50%-loaded table indexes all four arrays at once. It rejects duplicate
 * ids across and within verdicts while mapping each surviving id to its work
 * row; removed ids deliberately map to SIZE_MAX. */
static bool checked_add(size_t *total, size_t count) {
    if (count > SIZE_MAX - *total) {
        return false;
    }
    *total += count;
    return true;
}

static bool ordered_table_capacity(size_t count, size_t *capacity) {
    size_t target;
    size_t value = 1;
    if (count > SIZE_MAX / 2) {
        return false;
    }
    target = count * 2;
    while (value < target) {
        if (value > SIZE_MAX / 2) {
            return false;
        }
        value *= 2;
    }
    *capacity = value;
    return true;
}

static ordered_delta_slot *ordered_slot_find(ordered_delta_slot *slots, size_t capacity, markdown_core_node_id id) {
    size_t slot;
    if (id == 0) {
        return NULL;
    }
    slot = (size_t)markdown_core_mix64(id) & (capacity - 1);
    while (slots[slot].id != 0 && slots[slot].id != id) {
        slot = (slot + 1) & (capacity - 1);
    }
    return &slots[slot];
}

static bool ordered_insert_removed(
    ordered_delta_slot *slots,
    size_t capacity,
    const markdown_core_node_id *ids,
    size_t count
) {
    size_t index;
    if (count != 0 && ids == NULL) {
        return false;
    }
    for (index = 0; index < count; ++index) {
        ordered_delta_slot *slot = ordered_slot_find(slots, capacity, ids[index]);
        if (!slot || slot->id != 0) {
            return false;
        }
        slot->id = ids[index];
        slot->touched_index = SIZE_MAX;
    }
    return true;
}

static bool ordered_collect_touched(
    ordered_delta_slot *slots,
    size_t capacity,
    ordered_delta_work *work,
    size_t *cursor,
    const markdown_core_session *session,
    const markdown_core_node_id *ids,
    size_t count,
    markdown_core_delta_change_kind change,
    uint64_t revision
) {
    size_t index;
    if (count != 0 && ids == NULL) {
        return false;
    }
    for (index = 0; index < count; ++index) {
        ordered_delta_slot *slot = ordered_slot_find(slots, capacity, ids[index]);
        size_t touched_index = *cursor;
        const markdown_core_node *node;
        if (!slot || slot->id != 0) {
            return false;
        }
        slot->id = ids[index];
        node = markdown_core_session_node_by_id(session, ids[index]);
        if (!node || markdown_core_node_get_id(node) != ids[index] ||
            markdown_core_node_get_revision(node) != revision) {
            return false;
        }
        slot->touched_index = touched_index;
        work[touched_index].node = node;
        work[touched_index].parent = 0;
        work[touched_index].parent_index = SIZE_MAX;
        work[touched_index].remaining_children = 0;
        work[touched_index].next_ready = SIZE_MAX;
        work[touched_index].change = change;
        ++*cursor;
    }
    return true;
}

static bool ordered_link_parents(ordered_delta_slot *slots, size_t capacity, ordered_delta_work *work, size_t count) {
    size_t index;
    size_t roots = 0;
    for (index = 0; index < count; ++index) {
        const markdown_core_node *parent = markdown_core_node_get_parent(work[index].node);
        ordered_delta_slot *parent_slot;
        if (!parent) {
            ++roots;
            continue;
        }
        work[index].parent = markdown_core_node_get_id(parent);
        parent_slot = ordered_slot_find(slots, capacity, work[index].parent);
        if (!parent_slot || parent_slot->id != work[index].parent || parent_slot->touched_index == SIZE_MAX ||
            parent_slot->touched_index == index || work[parent_slot->touched_index].remaining_children == SIZE_MAX) {
            return false;
        }
        work[index].parent_index = parent_slot->touched_index;
        ++work[parent_slot->touched_index].remaining_children;
    }
    return roots == 1;
}

static void ordered_enqueue(ordered_delta_work *work, size_t entry, size_t *head, size_t *tail) {
    if (*tail == SIZE_MAX) {
        *head = entry;
    } else {
        work[*tail].next_ready = entry;
    }
    *tail = entry;
}

static bool ordered_emit(ordered_delta_work *work, size_t count, markdown_core_delta_entry *entries) {
    size_t head = SIZE_MAX;
    size_t tail = SIZE_MAX;
    size_t index;
    size_t written = 0;

    /* The FIFO is seeded in delta-array order. Unlocking a parent appends it
     * after every already-ready leaf, giving deterministic Kahn order while
     * the child counters enforce the one-pass mirror dependency. */
    for (index = 0; index < count; ++index) {
        if (work[index].remaining_children == 0) {
            ordered_enqueue(work, index, &head, &tail);
        }
    }
    while (head != SIZE_MAX) {
        size_t entry_index = head;
        ordered_delta_work *entry = &work[entry_index];
        size_t parent_index = entry->parent_index;
        head = entry->next_ready;
        if (head == SIZE_MAX) {
            tail = SIZE_MAX;
        }

        entries[written].id = markdown_core_node_get_id(entry->node);
        entries[written].parent = entry->parent;
        entries[written].change = entry->change;
        ++written;

        if (parent_index != SIZE_MAX) {
            if (work[parent_index].remaining_children == 0) {
                return false;
            }
            --work[parent_index].remaining_children;
            if (work[parent_index].remaining_children == 0) {
                ordered_enqueue(work, parent_index, &head, &tail);
            }
        }
    }
    return written == count;
}

bool markdown_core_session_ordered_delta_entries(
    const markdown_core_session *session,
    const markdown_core_delta *changes,
    markdown_core_delta_entry **output,
    size_t *count,
    markdown_core_error **error
) {
    const markdown_core_node_id *arrays[4];
    size_t counts[4];
    size_t all_count = 0;
    size_t touched_count = 0;
    size_t capacity;
    size_t cursor = 0;
    ordered_delta_slot *slots = NULL;
    ordered_delta_work *work = NULL;
    markdown_core_delta_entry *entries = NULL;

    if (error) {
        *error = NULL;
    }
    if (output) {
        *output = NULL;
    }
    if (count) {
        *count = 0;
    }
    if (!session || !changes || !output || !count) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "session, delta, output, and count must not be null"
        );
        return false;
    }
    if (changes->lineage != session->lineage || changes->after != session->revision) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "session must originate the delta and remain at its committed revision"
        );
        return false;
    }

    counts[0] = array_view(&changes->added, &arrays[0]);
    counts[1] = array_view(&changes->removed, &arrays[1]);
    counts[2] = array_view(&changes->changed, &arrays[2]);
    counts[3] = array_view(&changes->bubbled, &arrays[3]);
    if (!checked_add(&all_count, counts[0]) || !checked_add(&all_count, counts[1]) ||
        !checked_add(&all_count, counts[2]) || !checked_add(&all_count, counts[3]) ||
        !checked_add(&touched_count, counts[0]) || !checked_add(&touched_count, counts[2]) ||
        !checked_add(&touched_count, counts[3]) || !ordered_table_capacity(all_count, &capacity) ||
        capacity > SIZE_MAX / sizeof(*slots) || touched_count > SIZE_MAX / sizeof(*work) ||
        touched_count > SIZE_MAX / sizeof(*entries)) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "delta is too large to order");
        return false;
    }
    if (all_count == 0) {
        return true;
    }

    slots = (ordered_delta_slot *)calloc(capacity, sizeof(*slots));
    if (touched_count != 0) {
        work = (ordered_delta_work *)malloc(touched_count * sizeof(*work));
        entries = (markdown_core_delta_entry *)malloc(touched_count * sizeof(*entries));
    }
    if (!slots || (touched_count != 0 && (!work || !entries))) {
        goto allocation_failed;
    }
    if (!ordered_collect_touched(
            slots,
            capacity,
            work,
            &cursor,
            session,
            arrays[0],
            counts[0],
            MARKDOWN_CORE_DELTA_CHANGE_ADDED,
            changes->after
        ) ||
        !ordered_insert_removed(slots, capacity, arrays[1], counts[1]) ||
        !ordered_collect_touched(
            slots,
            capacity,
            work,
            &cursor,
            session,
            arrays[2],
            counts[2],
            MARKDOWN_CORE_DELTA_CHANGE_CHANGED,
            changes->after
        ) ||
        !ordered_collect_touched(
            slots,
            capacity,
            work,
            &cursor,
            session,
            arrays[3],
            counts[3],
            MARKDOWN_CORE_DELTA_CHANGE_BUBBLED,
            changes->after
        ) ||
        cursor != touched_count ||
        (touched_count != 0 && (!ordered_link_parents(slots, capacity, work, touched_count) ||
                                !ordered_emit(work, touched_count, entries)))) {
        goto invalid_delta;
    }

    free(slots);
    free(work);
    *output = entries;
    *count = touched_count;
    return true;

allocation_failed:
    free(slots);
    free(work);
    free(entries);
    markdown_core_ast_set_error(
        error,
        MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
        "could not allocate ordered delta entries"
    );
    return false;

invalid_delta:
    free(slots);
    free(work);
    free(entries);
    markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INTERNAL, "delta does not form one complete touched tree");
    return false;
}

void markdown_core_delta_entries_free(markdown_core_delta_entry *output) { free(output); }

void markdown_core_delta_free(markdown_core_delta *changes) {
    if (!changes) {
        return;
    }
    free(changes->added.ids);
    free(changes->removed.ids);
    free(changes->changed.ids);
    free(changes->bubbled.ids);
    free(changes);
}
