#ifndef MARKDOWN_CORE_SOURCE_H
#define MARKDOWN_CORE_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include <markdown-core.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The stored bytes.
 *
 * A markdown_core_source owns one buffer holding a document's exact text,
 * filled at construction and read-only from then on. The engine's one
 * mutation, append, builds a whole successor document rather than growing a
 * predecessor's buffer, so a chain of N appends copies O(N^2) bytes over its
 * lifetime; the living tree (docs/reviews/2026-08-13-living-tree-plan.md §2)
 * retires that by making the chain own one growable buffer, and this module
 * is where that buffer will live.
 *
 * Ownership: a source is confined to one thread, like the document it
 * serves. All allocation goes through the markdown_core_mem it was created
 * with.
 */

typedef struct markdown_core_source markdown_core_source;

/** Creates a source owning a copy of `bytes[0..length)`, or returns NULL on
 * allocation failure — the constructor's only failure. `bytes` may be NULL
 * when `length` is 0. A length above PTRDIFF_MAX is refused as an allocation
 * failure before a byte is read: the buffer would not be a C object, since a
 * difference of two pointers into it has to be representable. */
markdown_core_source *markdown_core_source_new(markdown_core_mem *mem, const uint8_t *bytes, size_t length);

void markdown_core_source_release(markdown_core_source *source);

size_t markdown_core_source_length(const markdown_core_source *source);

/** A pointer to the contiguous run of stored bytes beginning at `offset`,
 * with `*run_length` its length. `offset` must be inside the source; there
 * is no out-of-range arm, because no caller has one to exercise and an
 * unreachable branch is a defect here rather than caution. The bytes are
 * contiguous, so one call answers to the end of the source; the loop is kept
 * at the call sites, where it costs nothing and is what a chunked substrate
 * would need if one ever came back. */
const uint8_t *markdown_core_source_run_at(const markdown_core_source *source, size_t offset, size_t *run_length);

#ifdef __cplusplus
}
#endif

#endif
