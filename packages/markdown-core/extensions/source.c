#include "source.h"

#include <stdint.h>
#include <string.h>

// The stored bytes, in one growable buffer.
//
// This was an AVL-balanced rope of windows into refcounted immutable buffers,
// so that applying an edit built a successor sharing every untouched subtree
// and predecessors stayed readable at zero copying cost. Nothing needed
// predecessors: a session publishes one document, reusing its view in place at
// every commit, so no caller can hold one (incremental-canonical-ast.md 4.2),
// and a consumer that wants a revision to outlive its commit takes a value
// copy, which the bindings already do.
//
// What is left is what the engine ever asked of this file: give me the bytes,
// and apply an edit. Nineteen call sites, all of them one of those two.

struct markdown_core_source {
    markdown_core_mem *mem;
    uint8_t *bytes;
    size_t length;
    size_t capacity;
};

static void stats_add(markdown_core_source_stats *stats, size_t copied, size_t buffers) {
    stats->bytes_copied += copied;
    stats->buffers_created += buffers;
}

/* Makes room for `needed` bytes, growing geometrically so that a trace of N
 * appends copies O(N) bytes rather than O(N^2). */
static bool reserve(markdown_core_source *source, size_t needed, markdown_core_source_stats *stats) {
    size_t capacity = source->capacity;
    uint8_t *grown;
    if (needed <= capacity) {
        return true;
    }
    if (needed > (size_t)PTRDIFF_MAX) {
        /* Not a C object, so not an allocation — refused here rather than in
         * the doubling loop below, which is what makes that loop's overflow
         * arm unreachable and lets it be deleted. `apply` rejects the same
         * ceiling on its own before it moves a byte; this guard is what
         * markdown_core_source_new reaches. */
        return false;
    }
    if (capacity == 0) {
        capacity = 64;
    }
    while (capacity < needed) {
        capacity *= 2;
    }
    grown = (uint8_t *)source->mem->realloc(source->mem, source->bytes, capacity);
    if (!grown) {
        return false;
    }
    source->bytes = grown;
    source->capacity = capacity;
    stats_add(stats, 0, 1);
    return true;
}

markdown_core_source *markdown_core_source_new(
    markdown_core_mem *mem,
    const uint8_t *bytes,
    size_t length,
    markdown_core_source_stats *stats,
    markdown_core_source_status *status
) {
    markdown_core_source *source = (markdown_core_source *)mem->calloc(mem, 1, sizeof(markdown_core_source));
    *status = MARKDOWN_CORE_SOURCE_OK;
    if (!source) {
        *status = MARKDOWN_CORE_SOURCE_NO_MEMORY;
        return NULL;
    }
    source->mem = mem;
    if (length) {
        if (!reserve(source, length, stats)) {
            mem->free(mem, source);
            *status = MARKDOWN_CORE_SOURCE_NO_MEMORY;
            return NULL;
        }
        memcpy(source->bytes, bytes, length);
        source->length = length;
        stats_add(stats, length, 0);
    }
    return source;
}

void markdown_core_source_release(markdown_core_source *source) {
    markdown_core_mem *mem;
    if (!source) {
        return;
    }
    mem = source->mem;
    if (source->bytes) {
        mem->free(mem, source->bytes);
    }
    mem->free(mem, source);
}

size_t markdown_core_source_length(const markdown_core_source *source) { return source->length; }

bool markdown_core_source_apply(
    markdown_core_source *source,
    const markdown_core_source_edit *edits,
    size_t edit_count,
    markdown_core_source_stats *stats,
    markdown_core_source_status *status
) {
    size_t i;
    size_t length = source->length;
    size_t grown = length;

    *status = MARKDOWN_CORE_SOURCE_OK;
    /* Every span is checked before any byte moves, so a rejected batch leaves
     * the source exactly as it was — which is what "nothing is published on
     * failure" costs here, now that there is no successor to discard. */
    for (i = 0; i < edit_count; i++) {
        const markdown_core_span *span = &edits[i].span;
        if (span->start > span->end || span->end > length) {
            *status = MARKDOWN_CORE_SOURCE_INVALID_SPAN;
            return false;
        }
        if (i && span->start < edits[i - 1].span.end) {
            *status = MARKDOWN_CORE_SOURCE_INVALID_SPAN;
            return false;
        }
        grown -= span->end - span->start;
        if (edits[i].replacement_length > (size_t)PTRDIFF_MAX - grown) {
            /* The resulting document would not be a C object: no allocation
             * may exceed PTRDIFF_MAX, since a difference of two pointers into
             * it has to be representable. Refused before a byte of the
             * (impossible) replacement is read, and before the number reaches
             * an allocator — the ceiling is PTRDIFF_MAX rather than SIZE_MAX
             * because a size between the two is arithmetically fine and still
             * cannot be allocated, which is what ASan reports when realloc is
             * handed one. The session passes this number straight through from
             * public API, which is what makes the guard reachable at all. */
            *status = MARKDOWN_CORE_SOURCE_NO_MEMORY;
            return false;
        }
        grown += edits[i].replacement_length;
    }
    /* ONE EDIT SPLICES IN PLACE; a batch is built beside the source.
     *
     * In place, the direction is forced and it is not a preference: growing,
     * the splice runs from the END backwards so a write always lands at or
     * above the byte it is reading; shrinking, from the FRONT forwards so it
     * always lands at or below. Either way nothing is written over a byte that
     * has not been read.
     *
     * A BATCH CANNOT DO THAT. An edit's final position depends on the net
     * shift of every edit before it, so a batch that grows overall while an
     * earlier edit shrinks makes the write cursor cross the read cursor
     * mid-splice, whichever direction it runs. Back-to-front alone looked
     * right and the randomized oracle caught it in round twenty. A batch
     * therefore copies into a fresh buffer, in order, which is what the
     * oracle itself does. The session sends one edit per call. */
    if (edit_count <= 1) {
        /* Reserved before anything moves, so the splice below cannot fail
         * halfway: the only failure this primitive has is an allocation, and
         * it happens while the source is still untouched. */
        if (!reserve(source, grown > length ? grown : length, stats)) {
            *status = MARKDOWN_CORE_SOURCE_NO_MEMORY;
            return false;
        }
        if (grown >= length) {
            size_t write = grown;
            size_t read = length;
            i = edit_count;
            while (i--) {
                size_t tail = read - edits[i].span.end;
                write -= tail;
                read -= tail;
                if (tail) {
                    memmove(source->bytes + write, source->bytes + read, tail);
                    stats_add(stats, tail, 0);
                }
                write -= edits[i].replacement_length;
                if (edits[i].replacement_length) {
                    memcpy(source->bytes + write, edits[i].replacement, edits[i].replacement_length);
                    stats_add(stats, edits[i].replacement_length, 0);
                }
                read = edits[i].span.start;
            }
        } else {
            size_t write = 0;
            size_t read = 0;
            for (i = 0; i < edit_count; i++) {
                size_t head = edits[i].span.start - read;
                if (head) {
                    memmove(source->bytes + write, source->bytes + read, head);
                    stats_add(stats, head, 0);
                }
                write += head;
                if (edits[i].replacement_length) {
                    memcpy(source->bytes + write, edits[i].replacement, edits[i].replacement_length);
                    stats_add(stats, edits[i].replacement_length, 0);
                }
                write += edits[i].replacement_length;
                read = edits[i].span.end;
            }
            if (length > read) {
                memmove(source->bytes + write, source->bytes + read, length - read);
                stats_add(stats, length - read, 0);
            }
        }
    } else {
        uint8_t *built = grown ? (uint8_t *)source->mem->calloc(source->mem, 1, grown) : NULL;
        size_t write = 0;
        size_t read = 0;
        if (grown && !built) {
            *status = MARKDOWN_CORE_SOURCE_NO_MEMORY;
            return false;
        }
        for (i = 0; i < edit_count; i++) {
            size_t head = edits[i].span.start - read;
            if (head) {
                memcpy(built + write, source->bytes + read, head);
            }
            write += head;
            if (edits[i].replacement_length) {
                memcpy(built + write, edits[i].replacement, edits[i].replacement_length);
            }
            write += edits[i].replacement_length;
            read = edits[i].span.end;
        }
        if (length > read) {
            memcpy(built + write, source->bytes + read, length - read);
        }
        stats_add(stats, grown, 1);
        if (source->bytes) {
            source->mem->free(source->mem, source->bytes);
        }
        source->bytes = built;
        source->capacity = grown;
    }
    source->length = grown;
    return true;
}

void markdown_core_source_copy_bytes(const markdown_core_source *source, size_t offset, size_t length, uint8_t *out) {
    if (length) {
        memcpy(out, source->bytes + offset, length);
    }
}

const uint8_t *markdown_core_source_run_at(const markdown_core_source *source, size_t offset, size_t *run_length) {
    *run_length = source->length - offset;
    return source->bytes + offset;
}

uint8_t markdown_core_source_byte_at(const markdown_core_source *source, size_t offset) {
    return source->bytes[offset];
}
