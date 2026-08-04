#include "source.h"

#include <string.h>

// Persistent stored-byte substrate. The tree is an AVL-balanced rope whose
// leaves are windows into refcounted immutable buffers. Everything is
// immutable after construction: an apply builds new nodes along the edit
// paths and shares every other subtree, so predecessors stay readable at
// zero copying cost. Reference convention: constructors named *_own consume
// the node references they are given, on failure too; borrowing helpers say
// so. Buffers are retained explicitly wherever a leaf takes a window.
//
// Balance invariant: sibling heights differ by at most one. The join used
// by concat descends the taller side until the height gap closes, so a
// freshly made interior sees a gap of at most two, which one rotation
// repairs. That keeps every descent, split, and append path O(log n).

typedef struct source_buffer {
    size_t refcount;
    size_t capacity; // payload bytes that follow this header
    uint8_t bytes[]; // C99 flexible member; allocation sizes it
} source_buffer;

typedef struct source_node {
    size_t refcount;
    size_t length; // subtree stored bytes
    size_t height; // leaf height is 1
    struct source_node *left;  // both NULL for a leaf
    struct source_node *right; // never exactly one NULL
    source_buffer *buffer;     // leaf only
    size_t offset;             // window start within buffer (leaf only)
} source_node;

struct markdown_core_source {
    size_t refcount;
    markdown_core_mem *mem;
    markdown_core_source_profile profile;
    source_node *root; // NULL when the source is empty
};

static void stats_add(markdown_core_source_stats *stats, size_t copied, size_t buffers, size_t nodes) {
    stats->bytes_copied += copied;
    stats->buffers_created += buffers;
    stats->nodes_created += nodes;
}

// --- buffers ---------------------------------------------------------------

static source_buffer *buffer_new(markdown_core_mem *mem, size_t capacity) {
    source_buffer *buffer = (source_buffer *)mem->calloc(mem, 1, sizeof(source_buffer) + capacity);
    if (!buffer) {
        return NULL;
    }
    buffer->refcount = 1;
    buffer->capacity = capacity;
    return buffer;
}

static void buffer_retain(source_buffer *buffer) { buffer->refcount++; }

static void buffer_release(markdown_core_mem *mem, source_buffer *buffer) {
    if (--buffer->refcount == 0) {
        mem->free(mem, buffer);
    }
}

// --- nodes -----------------------------------------------------------------

static source_node *node_retain(source_node *node) {
    node->refcount++;
    return node;
}

static void node_release(markdown_core_mem *mem, source_node *node) {
    // Iterative on the left spine, recursive on the right: recursion depth
    // is bounded by the AVL height, O(log n).
    while (node && --node->refcount == 0) {
        source_node *left = node->left;
        if (node->right) {
            node_release(mem, node->right);
        }
        if (node->buffer) {
            buffer_release(mem, node->buffer);
        }
        mem->free(mem, node);
        node = left;
    }
}

// Borrows `buffer` (retains internally). Returns a fresh leaf or NULL.
static source_node *leaf_new(
    markdown_core_mem *mem,
    source_buffer *buffer,
    size_t offset,
    size_t length,
    markdown_core_source_stats *stats
) {
    source_node *leaf = (source_node *)mem->calloc(mem, 1, sizeof(source_node));
    if (!leaf) {
        return NULL;
    }
    leaf->refcount = 1;
    leaf->length = length;
    leaf->height = 1;
    buffer_retain(buffer);
    leaf->buffer = buffer;
    leaf->offset = offset;
    stats_add(stats, 0, 0, 1);
    return leaf;
}

// Copies `length` caller bytes into a fresh right-sized buffer and leaf.
static source_node *leaf_from_bytes(
    markdown_core_mem *mem,
    const uint8_t *bytes,
    size_t length,
    markdown_core_source_stats *stats
) {
    source_buffer *buffer = buffer_new(mem, length);
    source_node *leaf;
    if (!buffer) {
        return NULL;
    }
    memcpy(buffer->bytes, bytes, length);
    leaf = leaf_new(mem, buffer, 0, length, stats);
    buffer_release(mem, buffer); // leaf_new retained on success
    stats_add(stats, length, 1, 0);
    return leaf;
}

// Takes a window of `buffer`, sharing it when the window is a large enough
// fraction and copying it out otherwise. This single rule is the declared
// amplification bound: every leaf window covers at least
// 1/MAX_AMPLIFICATION of its buffer, so the distinct buffer capacities a
// source retains sum to at most MAX_AMPLIFICATION times its length.
static source_node *leaf_window(
    markdown_core_mem *mem,
    source_buffer *buffer,
    size_t offset,
    size_t length,
    markdown_core_source_stats *stats
) {
    if (length * MARKDOWN_CORE_SOURCE_MAX_AMPLIFICATION >= buffer->capacity) {
        return leaf_new(mem, buffer, offset, length, stats);
    }
    return leaf_from_bytes(mem, buffer->bytes + offset, length, stats);
}

// Consumes `left` and `right` (non-NULL, heights within one). No rotation.
static source_node *interior_raw(
    markdown_core_mem *mem,
    source_node *left,
    source_node *right,
    markdown_core_source_stats *stats
) {
    source_node *node = (source_node *)mem->calloc(mem, 1, sizeof(source_node));
    if (!node) {
        node_release(mem, left);
        node_release(mem, right);
        return NULL;
    }
    node->refcount = 1;
    node->length = left->length + right->length;
    node->height = 1 + (left->height > right->height ? left->height : right->height);
    node->left = left;
    node->right = right;
    stats_add(stats, 0, 0, 1);
    return node;
}

// Consumes `left` and `right` (non-NULL). Repairs a height gap of exactly
// two with one single or double rotation; the join in concat guarantees the
// gap is never wider.
static source_node *interior_balanced(
    markdown_core_mem *mem,
    source_node *left,
    source_node *right,
    markdown_core_source_stats *stats
) {
    if (left->height > right->height + 1) {
        // Left-heavy: left is interior (height >= 3), so its children exist.
        if (left->left->height >= left->right->height) {
            // Single right rotation: (ll, lr) r -> ll (lr r).
            source_node *lower = interior_raw(mem, node_retain(left->right), right, stats);
            source_node *result;
            if (!lower) {
                node_release(mem, left);
                return NULL;
            }
            result = interior_raw(mem, node_retain(left->left), lower, stats);
            node_release(mem, left);
            return result;
        }
        // Double rotation through left->right: ll (lrl, lrr) r
        // -> (ll lrl) (lrr r).
        {
            source_node *pivot = left->right;
            source_node *low_left = interior_raw(mem, node_retain(left->left), node_retain(pivot->left), stats);
            source_node *low_right;
            source_node *result;
            if (!low_left) {
                node_release(mem, left);
                node_release(mem, right);
                return NULL;
            }
            low_right = interior_raw(mem, node_retain(pivot->right), right, stats);
            if (!low_right) {
                node_release(mem, low_left);
                node_release(mem, left);
                return NULL;
            }
            result = interior_raw(mem, low_left, low_right, stats);
            node_release(mem, left);
            return result;
        }
    }
    if (right->height > left->height + 1) {
        // Right-heavy mirror.
        if (right->right->height >= right->left->height) {
            source_node *lower = interior_raw(mem, left, node_retain(right->left), stats);
            source_node *result;
            if (!lower) {
                node_release(mem, right);
                return NULL;
            }
            result = interior_raw(mem, lower, node_retain(right->right), stats);
            node_release(mem, right);
            return result;
        }
        {
            source_node *pivot = right->left;
            source_node *low_left = interior_raw(mem, left, node_retain(pivot->left), stats);
            source_node *low_right;
            source_node *result;
            if (!low_left) {
                node_release(mem, right);
                return NULL;
            }
            low_right = interior_raw(mem, node_retain(pivot->right), node_retain(right->right), stats);
            if (!low_right) {
                node_release(mem, low_left);
                node_release(mem, right);
                return NULL;
            }
            result = interior_raw(mem, low_left, low_right, stats);
            node_release(mem, right);
            return result;
        }
    }
    return interior_raw(mem, left, right, stats);
}

// Consumes `left` (which may be NULL) and `right` (never NULL: every call
// site joins a possibly-empty prefix with a piece it just built). Joins two
// ropes, merging small boundary leaves so byte-wise append traces coalesce
// into MERGE_LIMIT-sized buffers instead of one leaf per byte.
static source_node *concat(
    markdown_core_mem *mem,
    source_node *left,
    source_node *right,
    markdown_core_source_stats *stats
) {
    if (!left) {
        return right;
    }
    if (!left->left && !right->left && left->length + right->length <= MARKDOWN_CORE_SOURCE_LEAF_MERGE_LIMIT) {
        // Two small leaves: copy both windows into one fresh buffer.
        source_buffer *buffer = buffer_new(mem, left->length + right->length);
        source_node *leaf;
        if (!buffer) {
            node_release(mem, left);
            node_release(mem, right);
            return NULL;
        }
        memcpy(buffer->bytes, left->buffer->bytes + left->offset, left->length);
        memcpy(buffer->bytes + left->length, right->buffer->bytes + right->offset, right->length);
        leaf = leaf_new(mem, buffer, 0, left->length + right->length, stats);
        buffer_release(mem, buffer);
        stats_add(stats, left->length + right->length, 1, 0);
        node_release(mem, left);
        node_release(mem, right);
        return leaf;
    }
    if (left->height > right->height + 1) {
        // Descend the right spine of the taller left side.
        source_node *joined = concat(mem, node_retain(left->right), right, stats);
        source_node *result;
        if (!joined) {
            node_release(mem, left);
            return NULL;
        }
        result = interior_balanced(mem, node_retain(left->left), joined, stats);
        node_release(mem, left);
        return result;
    }
    if (right->height > left->height + 1) {
        source_node *joined = concat(mem, left, node_retain(right->left), stats);
        source_node *result;
        if (!joined) {
            node_release(mem, right);
            return NULL;
        }
        result = interior_balanced(mem, joined, node_retain(right->right), stats);
        node_release(mem, right);
        return result;
    }
    return interior_raw(mem, left, right, stats);
}

// Borrows `node`; returns a new reference to the byte range [start, end),
// sharing whole subtrees and windowing boundary leaves.
static source_node *slice(
    markdown_core_mem *mem,
    source_node *node,
    size_t start,
    size_t end,
    markdown_core_source_stats *stats
) {
    if (start == 0 && end == node->length) {
        return node_retain(node);
    }
    if (!node->left) {
        return leaf_window(mem, node->buffer, node->offset + start, end - start, stats);
    }
    if (end <= node->left->length) {
        return slice(mem, node->left, start, end, stats);
    }
    if (start >= node->left->length) {
        return slice(mem, node->right, start - node->left->length, end - node->left->length, stats);
    }
    {
        source_node *left = slice(mem, node->left, start, node->left->length, stats);
        source_node *right;
        if (!left) {
            return NULL;
        }
        right = slice(mem, node->right, 0, end - node->left->length, stats);
        if (!right) {
            node_release(mem, left);
            return NULL;
        }
        return concat(mem, left, right, stats);
    }
}

// Borrows `node`; copies the byte range [start, end) into `out`.
static void node_copy_bytes(const source_node *node, size_t start, size_t end, uint8_t *out) {
    while (node->left) {
        if (end <= node->left->length) {
            node = node->left;
        } else if (start >= node->left->length) {
            start -= node->left->length;
            end -= node->left->length;
            node = node->right;
        } else {
            node_copy_bytes(node->left, start, node->left->length, out);
            out += node->left->length - start;
            start = 0;
            end -= node->left->length;
            node = node->right;
        }
    }
    memcpy(out, node->buffer->bytes + node->offset + start, end - start);
}

// --- profile validation ----------------------------------------------------

// RFC 3629 acceptance, expressed as the continuation bytes still owed plus
// the admissible range of the next one; the E0/ED/F0/F4 rows are what
// reject overlongs, surrogates, and anything above U+10FFFF.
typedef struct utf8_state {
    size_t need;
    uint8_t lo;
    uint8_t hi;
} utf8_state;

// Returns false on a byte no UTF-8 document admits at this position.
static bool utf8_step(utf8_state *state, uint8_t byte) {
    if (state->need > 0) {
        if (byte < state->lo || byte > state->hi) {
            return false;
        }
        state->need--;
        state->lo = 0x80;
        state->hi = 0xBF;
        return true;
    }
    if (byte < 0x80) {
        return true;
    }
    if (byte >= 0xC2 && byte <= 0xDF) {
        state->need = 1;
        state->lo = 0x80;
        state->hi = 0xBF;
        return true;
    }
    if (byte >= 0xE0 && byte <= 0xEF) {
        state->need = 2;
        state->lo = byte == 0xE0 ? 0xA0 : 0x80;
        state->hi = byte == 0xED ? 0x9F : 0xBF;
        return true;
    }
    if (byte >= 0xF0 && byte <= 0xF4) {
        state->need = 3;
        state->lo = byte == 0xF0 ? 0x90 : 0x80;
        state->hi = byte == 0xF4 ? 0x8F : 0xBF;
        return true;
    }
    return false; // 0x80..0xC1 at a boundary, or 0xF5..0xFF anywhere
}

// Validates a contiguous run that starts at a character boundary and ends at
// end-of-source. A trailing incomplete character is exactly the truncated
// final code point 7.1 admits, so it is accepted; everything else invalid
// is not.
static bool utf8_valid_prefix_run(const uint8_t *bytes, size_t length, markdown_core_source_stats *stats) {
    utf8_state state = {0, 0, 0};
    size_t i;
    stats->validated_bytes += length;
    for (i = 0; i < length; i++) {
        if (!utf8_step(&state, bytes[i])) {
            return false;
        }
    }
    return true;
}

static bool is_continuation(uint8_t byte) { return byte >= 0x80 && byte <= 0xBF; }

typedef struct dirty_range {
    size_t start;
    size_t end;
} dirty_range;

// Validates one dirty range of the candidate tree under STRICT_UTF8. The
// scan starts at the character boundary at or before `start` (backing up
// over at most three continuation bytes plus their lead — a valid
// predecessor guarantees a boundary that close) and runs until it is past
// `end` on a boundary, hits end-of-source (where a dangling prefix is the
// admitted truncated final code point), or rejects.
static bool validate_range(
    const source_node *root,
    size_t total,
    size_t start,
    size_t end,
    markdown_core_source_stats *stats
) {
    utf8_state state = {0, 0, 0};
    uint8_t window[8];
    size_t back = 0;
    size_t position;
    while (back < 3 && start > 0) {
        uint8_t byte;
        node_copy_bytes(root, start - 1, start, &byte);
        if (!is_continuation(byte)) {
            break;
        }
        start--;
        back++;
    }
    if (start > 0) {
        uint8_t byte;
        node_copy_bytes(root, start - 1, start, &byte);
        if (byte >= 0xC0) {
            start--;
        }
    }
    position = start;
    while (position < total) {
        // Pull the window in small chunks; ranges are edit-sized, so the
        // chunking only bounds the stack, not the asymptotics.
        size_t chunk = total - position;
        size_t i;
        if (chunk > sizeof(window)) {
            chunk = sizeof(window);
        }
        node_copy_bytes(root, position, position + chunk, window);
        stats->validated_bytes += chunk;
        for (i = 0; i < chunk; i++) {
            if (!utf8_step(&state, window[i])) {
                return false;
            }
            position++;
            if (position >= end && state.need == 0) {
                return true;
            }
        }
    }
    return true; // end-of-source: any owed continuation is the truncated final
}

// --- public (internal-header) API ------------------------------------------

markdown_core_source *markdown_core_source_new(
    markdown_core_mem *mem,
    markdown_core_source_profile profile,
    const uint8_t *bytes,
    size_t length,
    markdown_core_source_stats *stats,
    markdown_core_source_status *status
) {
    markdown_core_source *source;
    source_node *root = NULL;
    *status = MARKDOWN_CORE_SOURCE_OK;
    if (profile == MARKDOWN_CORE_SOURCE_STRICT_UTF8 && !utf8_valid_prefix_run(bytes, length, stats)) {
        *status = MARKDOWN_CORE_SOURCE_INVALID_UTF8;
        return NULL;
    }
    if (length > 0) {
        root = leaf_from_bytes(mem, bytes, length, stats);
        if (!root) {
            *status = MARKDOWN_CORE_SOURCE_NO_MEMORY;
            return NULL;
        }
    }
    source = (markdown_core_source *)mem->calloc(mem, 1, sizeof(markdown_core_source));
    if (!source) {
        if (root) {
            node_release(mem, root);
        }
        *status = MARKDOWN_CORE_SOURCE_NO_MEMORY;
        return NULL;
    }
    source->refcount = 1;
    source->mem = mem;
    source->profile = profile;
    source->root = root;
    return source;
}

void markdown_core_source_retain(markdown_core_source *source) { source->refcount++; }

void markdown_core_source_release(markdown_core_source *source) {
    if (--source->refcount == 0) {
        markdown_core_mem *mem = source->mem;
        if (source->root) {
            node_release(mem, source->root);
        }
        mem->free(mem, source);
    }
}

markdown_core_source_profile markdown_core_source_profile_of(const markdown_core_source *source) {
    return source->profile;
}

size_t markdown_core_source_length(const markdown_core_source *source) {
    return source->root ? source->root->length : 0;
}

markdown_core_source *markdown_core_source_apply(
    const markdown_core_source *source,
    const markdown_core_source_edit *edits,
    size_t edit_count,
    markdown_core_source_stats *stats,
    markdown_core_source_status *status
) {
    markdown_core_mem *mem = source->mem;
    size_t length = markdown_core_source_length(source);
    source_node *candidate = NULL;
    markdown_core_source *result;
    size_t consumed = 0; // old bytes already placed into the candidate
    size_t i;
    *status = MARKDOWN_CORE_SOURCE_OK;
    for (i = 0; i < edit_count; i++) {
        size_t floor = i > 0 ? edits[i - 1].span.end : 0;
        if (edits[i].span.start < floor || edits[i].span.end < edits[i].span.start || edits[i].span.end > length) {
            *status = MARKDOWN_CORE_SOURCE_INVALID_SPAN;
            return NULL;
        }
    }
    for (i = 0; i < edit_count; i++) {
        if (edits[i].span.start > consumed) {
            source_node *kept = slice(mem, source->root, consumed, edits[i].span.start, stats);
            if (!kept) {
                goto no_memory;
            }
            candidate = concat(mem, candidate, kept, stats);
            if (!candidate) {
                goto no_memory;
            }
        }
        if (edits[i].replacement_length > 0) {
            source_node *piece = leaf_from_bytes(mem, edits[i].replacement, edits[i].replacement_length, stats);
            if (!piece) {
                goto no_memory;
            }
            candidate = concat(mem, candidate, piece, stats);
            if (!candidate) {
                goto no_memory;
            }
        }
        consumed = edits[i].span.end;
    }
    if (length > consumed) {
        source_node *kept = slice(mem, source->root, consumed, length, stats);
        if (!kept) {
            goto no_memory;
        }
        candidate = concat(mem, candidate, kept, stats);
        if (!candidate) {
            goto no_memory;
        }
    }
    if (source->profile == MARKDOWN_CORE_SOURCE_STRICT_UTF8 && candidate) {
        // Validate each edit's neighbourhood in candidate coordinates,
        // merging ranges that sit close enough to interact (a character is
        // at most four bytes, so six bytes of separation isolates them).
        // Ascending non-overlapping spans keep the coordinate shift simple:
        // every already-processed removal lies left of the next start.
        size_t removed = 0;
        size_t inserted = 0;
        size_t open = 0;
        size_t close = 0;
        bool pending = false;
        for (i = 0; i < edit_count; i++) {
            size_t start = edits[i].span.start - removed + inserted;
            size_t end = start + edits[i].replacement_length;
            removed += edits[i].span.end - edits[i].span.start;
            inserted += edits[i].replacement_length;
            if (pending && start > close + 6) {
                if (!validate_range(candidate, candidate->length, open, close, stats)) {
                    goto invalid_utf8;
                }
                pending = false;
            }
            if (!pending) {
                open = start;
                pending = true;
            }
            close = end;
        }
        if (pending && !validate_range(candidate, candidate->length, open, close, stats)) {
            goto invalid_utf8;
        }
    }
    result = (markdown_core_source *)mem->calloc(mem, 1, sizeof(markdown_core_source));
    if (!result) {
        goto no_memory;
    }
    result->refcount = 1;
    result->mem = mem;
    result->profile = source->profile;
    result->root = candidate;
    return result;

invalid_utf8:
    node_release(mem, candidate);
    *status = MARKDOWN_CORE_SOURCE_INVALID_UTF8;
    return NULL;

no_memory:
    if (candidate) {
        node_release(mem, candidate);
    }
    *status = MARKDOWN_CORE_SOURCE_NO_MEMORY;
    return NULL;
}

void markdown_core_source_copy_bytes(
    const markdown_core_source *source,
    size_t offset,
    size_t length,
    uint8_t *out
) {
    if (length > 0) {
        node_copy_bytes(source->root, offset, offset + length, out);
    }
}

// --- diagnostics -----------------------------------------------------------

typedef struct buffer_set {
    source_buffer **items;
    size_t count;
    size_t capacity;
} buffer_set;

static bool buffer_set_add(markdown_core_mem *mem, buffer_set *set, source_buffer *buffer, size_t *sum) {
    size_t i;
    for (i = 0; i < set->count; i++) {
        if (set->items[i] == buffer) {
            return true;
        }
    }
    if (set->count == set->capacity) {
        size_t grown = set->capacity == 0 ? 8 : set->capacity * 2;
        source_buffer **items = (source_buffer **)mem->realloc(mem, set->items, grown * sizeof(*items));
        if (!items) {
            return false;
        }
        set->items = items;
        set->capacity = grown;
    }
    set->items[set->count++] = buffer;
    *sum += buffer->capacity;
    return true;
}

static bool sum_buffers(markdown_core_mem *mem, const source_node *node, buffer_set *set, size_t *sum) {
    while (node->left) {
        if (!sum_buffers(mem, node->left, set, sum)) {
            return false;
        }
        node = node->right;
    }
    return buffer_set_add(mem, set, node->buffer, sum);
}

bool markdown_core_source_retained_bytes(
    const markdown_core_source *source,
    markdown_core_mem *mem,
    size_t *out
) {
    buffer_set set = {NULL, 0, 0};
    size_t sum = 0;
    bool ok = true;
    if (source->root) {
        ok = sum_buffers(mem, source->root, &set, &sum);
    }
    if (set.items) {
        mem->free(mem, set.items);
    }
    if (ok) {
        *out = sum;
    }
    return ok;
}
