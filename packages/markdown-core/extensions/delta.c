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
