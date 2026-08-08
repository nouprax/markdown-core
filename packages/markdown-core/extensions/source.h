#ifndef MARKDOWN_CORE_SOURCE_H
#define MARKDOWN_CORE_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <markdown-core.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The stored bytes (incremental-canonical-ast.md 7.1, 8.1).
 *
 * A markdown_core_source owns one growable buffer holding the session's exact
 * bytes. It is mutable and singly owned: applying an edit splices the buffer
 * in place.
 *
 * It was a persistent AVL rope of windows into refcounted immutable buffers,
 * so that an apply built a successor sharing every unedited subtree and
 * predecessors stayed readable however many successors existed. Nothing
 * needed a predecessor. A session publishes one document — the view it reuses
 * in place at every commit — so no caller can hold one (4.2), and a consumer
 * that wants a revision to outlive its commit takes a value copy, which the
 * bindings already do. The two bounds declared here for that design, an
 * amplification limit on retained buffer bytes and a leaf-merge limit on
 * append copying, described sharing that no longer happens.
 *
 * What replaces them is one property, and it is the one an editor cares
 * about: **a trace of N appends copies O(N) bytes, not O(N^2)**, because the
 * buffer grows geometrically. A splice in the middle moves the tail after it.
 * That is a walk, and 11.1 no longer treats a size-dependent term as a
 * violation on its own.
 *
 * Ownership: a source is confined to one thread, like the session state it
 * serves. All allocation goes through the markdown_core_mem it was created
 * with. Allocation failure surfaces before any byte moves — an apply reserves
 * what it needs while the source is still untouched — so a failed apply
 * leaves the source exactly as it was, which is what 8.1's "nothing is
 * published on failure" costs now that there is no successor to discard.
 */

/* A source stored any byte sequence under one of two profiles, and the
 * profile selected whether it validated UTF-8. Both are gone: UTF-8 is
 * assumed and never validated (incremental-canonical-ast.md 7.1), so there
 * was one behaviour under two names and a validator no caller reached. */

/** Why a constructor or apply returned NULL (or that it succeeded). */
typedef enum markdown_core_source_status {
    MARKDOWN_CORE_SOURCE_OK,
    MARKDOWN_CORE_SOURCE_INVALID_SPAN,
    MARKDOWN_CORE_SOURCE_NO_MEMORY
} markdown_core_source_status;

/** A half-open run of stored bytes in the source an edit is applied to
 * (8.1). Offsets are stored-byte offsets, never projected coordinates: the
 * projection profiles of 7.2 do not exist at this layer, so a projected
 * coordinate cannot be fed back in as an edit by construction. */
typedef struct markdown_core_span {
    size_t start;
    size_t end;
} markdown_core_span;

/** One byte-level edit: the span to replace and the bytes that take its
 * place (8.1). `replacement` may be NULL when `replacement_length` is 0. */
typedef struct markdown_core_source_edit {
    markdown_core_span span;
    const uint8_t *replacement;
    size_t replacement_length;
} markdown_core_source_edit;

/** Deterministic work counters, accumulated (never reset) into the struct a
 * caller passes. They exist so the storage bounds above are gates instead of
 * prose: a gate zeroes a struct, runs a trace, and asserts the counts. */
typedef struct markdown_core_source_stats {
    size_t bytes_copied;    /* payload bytes copied or moved */
    size_t buffers_created; /* buffer growths */
} markdown_core_source_stats;

typedef struct markdown_core_source markdown_core_source;

/** Creates a source owning a copy of `bytes[0..length)`. `bytes` may be NULL
 * when `length` is 0. `stats` and `status` are required: a caller that does
 * not care passes a scratch struct, and the constructor never branches on
 * their presence. */
markdown_core_source *markdown_core_source_new(
    markdown_core_mem *mem,
    const uint8_t *bytes,
    size_t length,
    markdown_core_source_stats *stats,
    markdown_core_source_status *status
);

void markdown_core_source_release(markdown_core_source *source);

size_t markdown_core_source_length(const markdown_core_source *source);

/** Applies `edits` to `source` in place.
 *
 * The edit list must be canonical: ascending by span start and
 * non-overlapping (edits[i].span.end <= edits[i+1].span.start), each span
 * half-open with start <= end <= length. Anything else fails with
 * MARKDOWN_CORE_SOURCE_INVALID_SPAN. Normalizing a convenience batch into
 * this form is the caller's job (8.1); the primitive stays deterministic.
 *
 * On failure it returns false having moved no byte: every span is validated
 * and the buffer reserved before the splice, so the only failure after that
 * point would be one no allocator can produce. */
bool markdown_core_source_apply(
    markdown_core_source *source,
    const markdown_core_source_edit *edits,
    size_t edit_count,
    markdown_core_source_stats *stats,
    markdown_core_source_status *status
);

/** Copies stored bytes `[offset, offset + length)` into `out`. The range
 * must lie inside the source, like memcpy the behaviour is otherwise
 * undefined; every caller here materializes ranges it just measured. */
void markdown_core_source_copy_bytes(const markdown_core_source *source, size_t offset, size_t length, uint8_t *out);

/** Random-access and streaming reads.
 *
 * `run_at` returns a pointer to the contiguous run of stored bytes that
 * begins at `offset`, and `*run_length` its length. `offset` must be inside
 * the source; there is no out-of-range arm, because no caller has one to
 * exercise and an unreachable branch is a defect here rather than caution.
 * The bytes are contiguous, so one call answers to the end of the source and
 * a caller that loops finds nothing left to ask for. The loop is kept at the
 * call sites: it costs nothing here and it is what a chunked substrate would
 * need if one ever came back. */
const uint8_t *markdown_core_source_run_at(const markdown_core_source *source, size_t offset, size_t *run_length);

/** The stored byte at `offset`, which must be inside the source. */
uint8_t markdown_core_source_byte_at(const markdown_core_source *source, size_t offset);

#ifdef __cplusplus
}
#endif

#endif
