#ifndef MARKDOWN_CORE_KOTLIN_BRIDGE_H
#define MARKDOWN_CORE_KOTLIN_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every payload-producing call returns false only when the payload itself
 * could not be allocated; structured failures travel inside the MKC5 payload
 * as an error record. Payloads are caller-owned and released with
 * markdown_core_kotlin_free.
 *
 * A document handle is the native document's address as a uint64_t, handed
 * back INSIDE the payload rather than through a second call: one crossing per
 * operation, and a handle that cannot be observed without the tree it belongs
 * to. Zero is never a valid handle. */

bool markdown_core_kotlin_open(const uint8_t *source, size_t length, uint32_t options_mask,
                               uint8_t **output, size_t *output_length);

/* Hands `handle` new text, exactly as markdown_core_document_edit does: the
 * successor supersedes the receiver on success, the receiver's handle stays
 * releasable either way, and the payload carries the successor's own. A
 * failed edit supersedes nothing. The edit payload never prunes — an edit
 * can shift a node's positions without touching its revision, so nothing a
 * caller decoded before it is guaranteed current. */
bool markdown_core_kotlin_edit(uint64_t handle, const uint8_t *source, size_t length,
                               uint8_t **output, size_t *output_length);

/* Hands `handle` a trailing chunk, exactly as markdown_core_document_append
 * does: the successor supersedes the receiver on success, the receiver's
 * handle stays releasable either way, and a failure past the argument guards
 * poisons the chain. The error shape is the edit entry's.
 *
 * `baseline` prunes the tree body: a non-root subtree is emitted as one
 * REUSE record instead of its records when its revision is <= baseline AND
 * its extent provably did not move (it ends strictly before the receiver's
 * last content coordinate — the revision vouches for content only, because
 * a trailing construct can absorb its terminating newline into its scope
 * end without a revision bump). Pass the receiver's root revision: the
 * caller already holds every such subtree decoded from the receiver. Every
 * revision minted by this append is strictly above any the receiver held,
 * so no reusable subtree is ever new — a baseline of 0 still reuses
 * revision-0 subtrees, which only a non-empty open mints; a chain opened
 * empty gets a full payload at 0. */
bool markdown_core_kotlin_append(uint64_t handle, const uint8_t *chunk, size_t length,
                                 uint64_t baseline, uint8_t **output, size_t *output_length);

void markdown_core_kotlin_release(uint64_t handle);
void markdown_core_kotlin_free(uint8_t *output);

#endif
