#ifndef MARKDOWN_CORE_KOTLIN_BRIDGE_H
#define MARKDOWN_CORE_KOTLIN_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every payload-producing call returns false only when the payload itself
 * could not be allocated; structured failures travel inside the MKC4 payload
 * as an error record. Payloads are caller-owned and released with
 * markdown_core_kotlin_free.
 *
 * A document handle is the native document's address as a uint64_t, handed
 * back INSIDE the payload rather than through a second call: one crossing per
 * operation, and a handle that cannot be observed without the tree it belongs
 * to. Zero is never a valid handle. */

bool markdown_core_kotlin_open(const uint8_t *source, size_t length, uint32_t options_mask,
                               uint8_t **output, size_t *output_length);

/* Hands `handle` new text. Reads it and takes nothing, exactly as
 * markdown_core_document_edit does: the handle stays valid and releasable,
 * and the payload carries the successor's own. */
bool markdown_core_kotlin_edit(uint64_t handle, const uint8_t *source, size_t length,
                               uint8_t **output, size_t *output_length);

void markdown_core_kotlin_release(uint64_t handle);
void markdown_core_kotlin_free(uint8_t *output);

#endif
