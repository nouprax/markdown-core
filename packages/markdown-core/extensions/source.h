#ifndef MARKDOWN_CORE_SOURCE_H
#define MARKDOWN_CORE_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <markdown-core.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The stored bytes.
 *
 * One source holds one CHAIN's text: bytes only ever arrive at the end, and
 * every document on the chain is a length watermark into them (the living
 * tree, docs/reviews/2026-08-13-living-tree-plan.md §2). Growth is
 * geometric, so a chain of appends totalling N bytes copies O(N) bytes over
 * its whole life rather than O(N) per tick.
 *
 * Nothing outside this module holds a pointer into the buffer across a
 * mutation: the parse copies what it keeps into the tree, so growing the
 * buffer — which may move it — is invisible to every document already
 * built.
 *
 * Ownership: a source is confined to one thread, like the chain it serves.
 * All allocation goes through the markdown_core_mem it was created with,
 * which for a chain's source is the chain's base allocator rather than any
 * document's arena — the bytes outlive every document that describes them.
 */

typedef struct markdown_core_source markdown_core_source;

/** Creates an empty source, or returns NULL on allocation failure. */
markdown_core_source *markdown_core_source_new(markdown_core_mem *mem);

void markdown_core_source_release(markdown_core_source *source);

/** Guarantees room for `additional` bytes beyond the stored length, so the
 * commit of that many cannot fail. Reserving is where growth — and its only
 * failure — happens, which is what lets a caller take the bytes only after
 * everything else about the mutation has succeeded.
 *
 * A total length above PTRDIFF_MAX is refused as an allocation failure
 * before a byte is copied: the buffer would not be a C object, since a
 * difference of two pointers into it has to be representable. */
bool markdown_core_source_reserve(markdown_core_source *source, size_t additional);

/** Copies `bytes[0..length)` onto the end. The caller must have reserved at
 * least `length` and not committed since; that is what makes this
 * infallible, and a mutation that cannot fail here is a mutation that can
 * publish its bytes last. */
void markdown_core_source_commit(markdown_core_source *source, const uint8_t *bytes, size_t length);

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
