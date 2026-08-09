#include <stdlib.h>

#include "document_internal.h"

// Deltas are caller-owned plain data: they stay valid after the session
// advances or is freed, so they use the system allocator like the facade's
// error and dump buffers do.
//
// This file used to be 430 lines. 321 of them reconstructed tree position for
// a delta that was four UNORDERED id sets: a 50%-loaded hash table over all
// four arrays at once, a work row per surviving id, a parent-link pass, and a
// Kahn queue to emit children before parents -- plus, for every id, a lookup
// through a document-wide id->node index that had to be built on every commit
// just to answer it. `diffs` is emitted in postorder now (document.c), so a
// row's position IS its position and none of that has a question to answer.

bool markdown_core_delta_push(markdown_core_delta *changes, markdown_core_node_id id, uint32_t parts) {
    if (changes->count == changes->capacity) {
        size_t capacity = changes->capacity ? changes->capacity * 2 : 16;
        markdown_core_diff *grown = (markdown_core_diff *)realloc(changes->diffs, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        changes->diffs = grown;
        changes->capacity = capacity;
    }
    changes->diffs[changes->count].markup = id;
    changes->diffs[changes->count].parts = parts;
    changes->count++;
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

size_t markdown_core_delta_diffs(const markdown_core_delta *changes, const markdown_core_diff **diffs) {
    if (diffs) {
        *diffs = changes ? changes->diffs : NULL;
    }
    return changes ? changes->count : 0;
}

void markdown_core_delta_free(markdown_core_delta *changes) {
    if (!changes) {
        return;
    }
    free(changes->diffs);
    free(changes);
}
