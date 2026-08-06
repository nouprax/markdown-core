#ifndef MARKDOWN_CORE_SOURCE_H
#define MARKDOWN_CORE_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <markdown-core.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Persistent stored-byte substrate (incremental-canonical-ast.md 7.1, 8.1).
 *
 * A markdown_core_source is an immutable value: a frozen profile plus the
 * exact committed bytes, held as a persistent balanced tree whose leaves are
 * windows into refcounted immutable buffers. Applying an edit batch builds a
 * new source that shares every unedited subtree with its predecessor; the
 * old source stays valid and readable until released, however many
 * successors exist. Nothing here is public API, but it is no longer dark:
 * the session owns its bytes here, and the contiguous store this replaced is
 * gone. (An earlier revision of this comment scheduled that adoption for the
 * M7 flip. Nothing else did — no gate, no contract clause, and not the
 * delivery plan's M7 section — and M3 needs it, because the ordered indices
 * only reduce to queries over a sequence that is persistent.)
 *
 * Two bounds are declared here because acceptance gates assert them:
 *
 * - Appending at end-of-source path-copies O(log n) tree nodes and copies at
 *   most MARKDOWN_CORE_SOURCE_LEAF_MERGE_LIMIT stored bytes beyond the
 *   replacement itself; the retained prefix is never copied (14.3.5).
 * - The buffer bytes a source retains never exceed
 *   MARKDOWN_CORE_SOURCE_MAX_AMPLIFICATION times its logical length: a leaf
 *   window that would reference less than 1/MAX_AMPLIFICATION of its buffer
 *   is copied into a right-sized buffer instead of sharing (14.3.5). Tree
 *   node overhead is additional and proportional to the leaf count, never to
 *   the bytes of any predecessor.
 *
 * Under MARKDOWN_CORE_SOURCE_STRICT_UTF8 the stored bytes are valid UTF-8
 * except for at most one truncated final code point — a well-formed prefix
 * at end-of-source that a continuation byte would complete (7.1). The
 * exception is positional: the same prefix followed by any further byte is a
 * profile violation. There is no pending or awaiting-continuation state; a
 * document ending mid-character is a legal final document. Validation is
 * incremental: an apply examines only the bytes of each edit plus a bounded
 * neighbourhood of its junctions, never the whole document, and reports the
 * bytes it examined so a gate can hold it to that.
 *
 * Ownership: sources are refcounted values confined to one thread, like the
 * session state they will serve. All allocation goes through the
 * markdown_core_mem the source was created with; allocation failure surfaces
 * as NULL with MARKDOWN_CORE_SOURCE_NO_MEMORY and never publishes a partial
 * source (8.1).
 *
 * Arithmetic note: interior lengths are sums over simultaneously live
 * allocations, so their sum cannot wrap size_t on any platform where those
 * objects fit in the address space, and they carry no overflow guards — a
 * branch no input can take is untestable, and the coverage gate treats
 * untestable branches as defects.
 *
 * A replacement length is not such a sum. It is a caller's number, and the
 * session hands it straight through from public API, so a caller may name a
 * length no allocation could ever have produced — and it becomes one buffer,
 * sized as a header plus that payload. `markdown_core_source_apply` therefore
 * refuses a replacement the buffer header cannot be added to, before reading
 * any byte, reporting NO_MEMORY because no allocator can satisfy the request
 * and leaving the predecessor retained and readable. It is checked per
 * replacement and not over the batch: a running total cannot reach SIZE_MAX
 * without the buffers it counts having been allocated first. This is the one
 * guard here that is reachable, and the pinning corpus reaches it.
 */

/** Which byte sequences a source may store (7.1). Frozen at creation. */
typedef enum markdown_core_source_profile {
    MARKDOWN_CORE_SOURCE_STRICT_UTF8,
    MARKDOWN_CORE_SOURCE_PERMISSIVE_BYTES
} markdown_core_source_profile;

/** Why a constructor or apply returned NULL (or that it succeeded). */
typedef enum markdown_core_source_status {
    MARKDOWN_CORE_SOURCE_OK,
    MARKDOWN_CORE_SOURCE_INVALID_SPAN,
    MARKDOWN_CORE_SOURCE_INVALID_UTF8,
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
    size_t bytes_copied;    /* payload bytes memcpy'd into fresh buffers */
    size_t buffers_created; /* immutable buffers allocated */
    size_t nodes_created;   /* tree nodes allocated */
    size_t validated_bytes; /* bytes examined by profile validation */
} markdown_core_source_stats;

typedef struct markdown_core_source markdown_core_source;

/** Creates a source owning a copy of `bytes[0..length)`. Under STRICT_UTF8
 * the bytes must be valid UTF-8 except for an optional truncated final code
 * point; a violation returns NULL with MARKDOWN_CORE_SOURCE_INVALID_UTF8.
 * `bytes` may be NULL when `length` is 0. `stats` and `status` are required:
 * a caller that does not care passes a scratch struct, and the constructor
 * never branches on their presence. */
markdown_core_source *markdown_core_source_new(
    markdown_core_mem *mem,
    markdown_core_source_profile profile,
    const uint8_t *bytes,
    size_t length,
    markdown_core_source_stats *stats,
    markdown_core_source_status *status
);

void markdown_core_source_retain(markdown_core_source *source);
void markdown_core_source_release(markdown_core_source *source);

markdown_core_source_profile markdown_core_source_profile_of(const markdown_core_source *source);
size_t markdown_core_source_length(const markdown_core_source *source);

/** Applies `edits` to `source` and returns the successor source, sharing
 * every unedited subtree. `source` is not modified and stays readable.
 *
 * The edit list must be canonical: ascending by span start and
 * non-overlapping (edits[i].span.end <= edits[i+1].span.start), each span
 * half-open with start <= end <= length. Anything else fails with
 * MARKDOWN_CORE_SOURCE_INVALID_SPAN. Normalizing a convenience batch into
 * this form is the caller's job (8.1); the primitive stays deterministic.
 *
 * Under STRICT_UTF8 the candidate bytes are validated at every junction; a
 * violation fails with MARKDOWN_CORE_SOURCE_INVALID_UTF8. On any failure
 * nothing is published: the return is NULL and `source` is untouched. */
markdown_core_source *markdown_core_source_apply(
    const markdown_core_source *source,
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
 * A run never spans two leaves, so a caller that needs more bytes calls
 * again at offset + *run_length. O(log n) per call: there are no parent
 * pointers, so successive runs re-descend from the root. */
const uint8_t *markdown_core_source_run_at(const markdown_core_source *source, size_t offset, size_t *run_length);

/** The stored byte at `offset`, which must be inside the source. */
uint8_t markdown_core_source_byte_at(const markdown_core_source *source, size_t offset);

/** Sums the capacities of the distinct buffers the source retains — the
 * physical bytes the amplification bound above is stated over. Diagnostic
 * accessor for gates; cost is quadratic in the leaf count. Returns false on
 * allocation failure of its scratch set. */
bool markdown_core_source_retained_bytes(const markdown_core_source *source, markdown_core_mem *mem, size_t *out);

/** The declared bounds the gates assert; see the header comment. */
#define MARKDOWN_CORE_SOURCE_MAX_AMPLIFICATION 4
#define MARKDOWN_CORE_SOURCE_LEAF_MERGE_LIMIT 128

#ifdef __cplusplus
}
#endif

#endif
